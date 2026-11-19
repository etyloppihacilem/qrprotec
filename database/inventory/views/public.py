# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          views/public.py
#       -\-    _|__
#        |\___/  . \        Created on 16 Nov. 2026 at 19:45
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Vues publiques (disponibles sur les deux API) : vérif, fiches, connexion, suivi, notifications web, signalements."""

from .common import *  # noqa: F401,F403
from .common import _checklist_call


# Vues publiques (disponibles sur les deux API)
# ----------------------------------------------------------------------------------------------------------------------

@api_view(['GET'])
def health(request):
    return Response({
        'status': 'ok',
        'api': 'local' if is_front(request) else 'public',
        # utilisateur reconnu par le jeton de session du poste (vide : personne de connecte)
        'user': front_user(request).matricule if front_user(request) else '',
        'today': timezone.localdate().isoformat(),
        'public_base_url': qrprotec_setting('PUBLIC_BASE_URL'),
        'remote_scanner': scanner_hub.enabled,
        # front distant (API distante, cle X-QRProtec-Key) : nom de sa cle
        'front': getattr(request, 'qrprotec_front', ''),
        # poste principal (API locale, ou cle marquee poste) : impressions en attente, ordinateurs connectes
        'poste': is_main_poste(request),
        # operations sur un lot possibles sans badge, sous un nom declare (reglage du serveur)
        'declared_identity': ServerSettings.get().declared_identity,
        # volume de la douchette quand personne n'est connecte (et des utilisateurs sans volume choisi)
        'scanner_volume_default': ServerSettings.get().scanner_volume_default,
        # lecture des codes-barres par le front web : le poste l'active a la premiere impression d'un code-barres
        'web_barcodes': ServerSettings.get().web_barcodes,
        # nom de l'antenne (case « Certifié par le TECH ... » des rapports d'etat de lot)
        'antenna_name': ServerSettings.get().antenna_name,
        # reassort : le lot passe en verif recommandee (reglage du serveur, coche par defaut)
        'restock_verif_recommended': ServerSettings.get().restock_verif_recommended,
    })


PIN_RESET_SALT = 'qrprotec.pin-reset'


def pin_reset_token(user):
    """Jeton du lien de deblocage (signature courte, pour un QR code lisible) : lie au blocage en cours, il ne sert
    qu'une fois."""
    return signing.Signer(salt=PIN_RESET_SALT).signature(f'{user.matricule}:{user.pin_blocked.timestamp():.6f}')


def pin_reset_token_valid(user, token):
    if user.pin_blocked is None or not isinstance(token, str):
        return False
    return hmac.compare_digest(pin_reset_token(user).encode(), token.encode())


def pin_contact_name(user):
    """Admin a contacter : celui choisi pour l'utilisateur, sinon l'admin par defaut (vide : un administrateur)."""
    contact = user.contact_admin()
    return short_name(contact.matricule) if contact else ''


def apply_default_contact(user, data):
    """Case « admin a contacter par defaut » : un seul admin actif a la fois."""
    if 'default_contact' in data:
        user.default_contact = bool(data['default_contact'])
    if user.role != Role.ADMIN or not user.active:
        user.default_contact = False
    if user.default_contact:
        Secouristes.objects.filter(default_contact=True).exclude(matricule=user.matricule).update(default_contact=False)


def pin_reset_info(user):
    """Lien de deblocage (et son QR code sur le telephone) a envoyer a un admin."""
    return {
        'url': ser.public_url('pinreset', m=user.matricule, t=pin_reset_token(user)),
        'contact': pin_contact_name(user),
        'matricule': user.matricule,
        'name': short_name(user.matricule),
        'forgotten': user.pin_forgotten,
    }


def pin_blocked_error(user):
    """PIN bloque : previent les admins (une seule fois par blocage) et renvoie le lien de deblocage."""
    info = pin_reset_info(user)
    info['notified'] = notifications.notify_pin_blocked(user, info['url'])
    message = ("Code PIN oublié : le TECH doit le réinitialiser" if user.pin_forgotten
               else "Code PIN bloqué après trop d'essais : le TECH doit le réinitialiser")
    return ApiError(message, status.HTTP_403_FORBIDDEN, pin_required=True, pin_blocked=True, pin_reset=info)


def pin_setup_extra(user):
    """Champs de l'erreur « choisissez votre code » : la case « Demander a chaque connexion » est cochee par defaut
    quand le code est obligatoire, et imposee pour un admin."""
    return {'pin_setup_required': True, 'pin_at_login_default': user.pin_mandatory,
            'pin_at_login_locked': user.pin_at_login_locked}


def verify_pin(user, data):
    """Controle du PIN a la connexion (obligatoire pour un admin, ou si l'utilisateur en a un).

    Un admin sans PIN (compte cree avant les PIN, ou par createadmin) le choisit a sa connexion
    avec `new_pin`. Les erreurs portent pin_required / pin_setup_required pour que le front demande le PIN.
    """
    if not user.pin_required:
        return
    if user.pin_blocked:
        raise pin_blocked_error(user)
    if user.pin_locked():
        minutes = max(1, int((user.pin_locked_until - timezone.now()).total_seconds() // 60) + 1)
        raise ApiError(f"Trop d'essais : PIN bloqué pendant {minutes} min", status.HTTP_403_FORBIDDEN,
                       pin_required=True, pin_locked=True)
    if not user.has_pin:
        new_pin = str(data.get('new_pin', '') or '')
        if not new_pin:
            raise ApiError("Choisissez votre code PIN (4 à 8 chiffres)", status.HTTP_403_FORBIDDEN,
                           **pin_setup_extra(user))
        try:
            user.set_pin(new_pin)
        except ValueError as exc:
            raise ApiError(str(exc), status.HTTP_400_BAD_REQUEST, **pin_setup_extra(user))
        # case « Demander a chaque connexion » de la fenetre de choix du code (absente : demande, comme avant)
        user.set_pin_at_login(data.get('pin_at_login', True))
        user.save(update_fields=['pin_hash', 'pin_failures', 'pin_failures_total', 'pin_locked_until', 'pin_blocked',
                                 'pin_reset_required', 'pin_forgotten', 'pin_reset_notified', 'pin_at_login'])
        return
    pin = str(data.get('pin', '') or '')
    if not pin:
        raise ApiError("Code PIN requis", status.HTTP_403_FORBIDDEN, pin_required=True)
    if not user.check_pin(pin):
        if user.pin_blocked:
            raise pin_blocked_error(user)
        raise ApiError("PIN bloqué 5 min après trop d'essais" if user.pin_locked() else "Code PIN incorrect",
                       status.HTTP_403_FORBIDDEN, pin_required=True, pin_locked=user.pin_locked())


@api_view(['POST'])
@handle_errors
def auth(request):
    """Confirme l'identite d'un secouriste : cle du badge (valable un an), puis PIN si besoin.

    La reponse contient un jeton `session` (12 h) que le front web renvoie pour les lectures reservees
    (etat des stocks) au lieu de redemander le PIN.
    """
    matricule = str(request.data.get('matricule', ''))
    if not request.data.get('key'):
        # matricule seul (code sans l'URL du badge, ou tape) : rien ne prouve le badge, le PIN est obligatoire
        return Response(login_response(request, matricule_login(request, matricule)))
    user = Secouristes.objects.filter(matricule=matricule).first()
    if user is None or not user.check_key(request.data.get('key')):
        if user is not None and user.guest and keys_match(user.key_hash, hash_key(str(request.data.get('key') or ''))):
            if user.guest_activated is not None:  # deja activee par un invite : jamais pour une autre personne
                raise ApiError(guest_used_message(user), status.HTTP_403_FORBIDDEN, guest=True, guest_used=True,
                               prenom=user.prenom, antenna=user.guest_antenna)
            raise ApiError(guest_refusal(user), status.HTTP_403_FORBIDDEN, guest=True)
        raise ApiError("Badge invalide ou expire", status.HTTP_403_FORBIDDEN)
    if user.guest:
        activate_guest_card(request, user)
    verify_pin(user, request.data)
    if user.name_login_failures or user.name_login_locked_until:
        # le badge prouve que c'est bien lui : la connexion par nom et PIN redevient possible
        user.name_login_failures = 0
        user.name_login_locked_until = None
        user.save(update_fields=['name_login_failures', 'name_login_locked_until'])
    return Response(login_response(request, user))


def guest_used_message(card):
    """Carte d'invite deja activee qui ne marche plus (expiree ou desactivee) : elle n'est qu'a son invite. Prenom
    seul (jamais de nom de famille sur le web)."""
    antenna = f' de {card.guest_antenna}' if card.guest_antenna else ''
    return (f"Cette carte a déjà été activée par {card.prenom}{antenna}, elle n'est plus utilisable par une autre "
            "personne. Utilisez une autre carte d'invité.")


def guest_refusal(card):
    """Message d'une carte d'invite qui ne connecte plus personne."""
    state = card.guest_state()
    if state == 'disabled':
        return "Carte d'invité désactivée : demandez une autre carte au TECH"
    if state == 'expired_unused':
        return "Carte d'invité pas activée à temps : demandez une autre carte au TECH"
    return "Carte d'invité expirée : demandez une autre carte au TECH"


def guest_card_action(card, action, data, admin, by, web=False):
    """Reprise du numero d'une carte jamais activee (`reuse`) ou nouvelle periode pour un invite qui revient
    (`reactivate` {"guest_hours", "new_card"}), notees au journal. Depuis le web, toujours une nouvelle cle (QR code a
    l'ecran) et 24 h pour activer une carte reprise."""
    try:
        if action == 'reuse':
            guests.reuse(card, guests.WEB_ACTIVATION_HOURS if web else None)
            journal.record(OperationKind.USER, by, f"carte d'invité {card.matricule} reprise par {admin} "
                           "(jamais activée : nouvelle carte, l'ancienne ne marche plus)", matricule=card.matricule)
        else:
            hours = data.get('guest_hours') or card.guest_hours
            guests.reactivate(card, hours, new_card=web or bool(data.get('new_card')))
            # une reactivation ouvre une nouvelle periode : l'annuler annule ce que l'invite a fait depuis
            journal.record(OperationKind.USER, by, f"carte d'invité {card.matricule} ({card}) réactivée par {admin} "
                           f"pour {card.guest_hours} h" + (', nouvelle carte imprimée' if getattr(card, 'new_key', None)
                                                           else ''),
                           matricule=card.matricule, guest_activation=True)
    except ValueError as exc:
        raise ApiError(str(exc))


def activate_guest_card(request, card):
    """Premiere connexion avec une carte d'invite : {"guest_nom", "guest_prenom"} la nomment et lancent sa duree de
    vie. Sans eux, erreur `guest_name_required` : le front demande le nom et le prenom puis renvoie la connexion."""
    if card.guest_activated is not None:
        return
    nom = request.data.get('guest_nom')
    prenom = request.data.get('guest_prenom')
    antenna = request.data.get('guest_antenne')
    if not any(str(value or '').strip() for value in (nom, prenom, antenna)):
        raise ApiError("Carte d'invité : indiquez votre prénom, votre nom et votre antenne", status.HTTP_403_FORBIDDEN,
                       guest_name_required=True, guest_hours=card.guest_hours)
    try:
        activated = guests.activate(card, nom, prenom, antenna)
    except ValueError as exc:
        raise ApiError(str(exc), status.HTTP_400_BAD_REQUEST, guest_name_required=True, guest_hours=card.guest_hours)
    if activated:
        # guest_activation : annuler cette ligne du journal annule aussi tout ce que l'invite a fait ensuite (undo.py)
        journal.record(OperationKind.USER, f'{SOURCE_VERIFIED}:{card.matricule}',
                       f"carte d'invité {card.matricule} activée par {card.prenom} {card.nom} "
                       f"(antenne {card.guest_antenna}), valable {card.guest_hours} h",
                       matricule=card.matricule, guest_activation=True)


def login_response(request, user, temporary=False):
    data = ser.user_dict(user)
    if is_front(request):
        data['nom'] = user.nom  # le poste l'affiche ; jamais sur le front web
    data['session'] = session_token(user, temporary)
    if temporary:
        data['temporary'] = True  # connexion de depannage : la session du telephone vaut 24 h
        data['session_hours'] = TEMP_SESSION_MAX_AGE // 3600
    # reglages des notifications web affiches seulement si l'utilisateur peut en recevoir
    data['notifications'] = bool(user.push_types()) and webpush.available()
    data['scanner_volume'] = user.effective_scanner_volume
    return data


NAME_LOGIN_ERROR = "Nom, prénom ou code PIN incorrect"
MATRICULE_LOGIN_ERROR = "Matricule ou code PIN incorrect"
NAME_LOGIN_LOCK_EVERY = 5                    # essais faux consecutifs avant une pause
NAME_LOGIN_LOCK_DURATION = timedelta(minutes=15)
NAME_LOGIN_MAX_FAILURES = 20                 # puis connexion par nom coupee jusqu'a une connexion avec le badge
NAME_LOGIN_ADDRESS_LIMIT = 10                # essais faux par adresse et par periode
NAME_LOGIN_ADDRESS_PERIOD = 15 * 60


@functools.cache
def dummy_pin_hash():
    """Hache d'un PIN quelconque : meme temps de reponse que le nom existe ou non."""
    return make_password('0000')


def client_address(request):
    """Adresse du client ; derriere le reverse proxy local (Caddy), celle qu'il transmet."""
    address = request.META.get('REMOTE_ADDR', '') or ''
    forwarded = request.META.get('HTTP_X_FORWARDED_FOR', '')
    if forwarded and address in ('127.0.0.1', '::1'):
        return forwarded.split(',')[0].strip()[:64]
    return address[:64]


def login_name_key(name):
    """Forme d'un nom pour la connexion : name_key sans aucun separateur (espaces, tirets, apostrophes de toutes
    sortes). « Jean-Pierre », « jean pierre », « Jean–Pierre » et « jeanpierre » sont le meme prenom ; le PIN
    reste exige, donc pas de risque de confondre deux secouristes."""
    return ''.join(character for character in name_key(name) if character.isalnum())


def name_login_candidates(nom, prenom):
    """Secouristes de ce nom (sans accents, casse ni separateurs) qui peuvent se connecter par nom et PIN."""
    key = (login_name_key(nom), login_name_key(prenom))
    return [user for user in name_login_candidates_queryset()
            if (login_name_key(user.nom), login_name_key(user.prenom)) == key]


def name_login_candidates_queryset():
    """Secouristes qui peuvent se connecter sans badge (nom ou matricule seul, puis PIN) : PIN defini et utilisable,
    badge valide, connexion sans badge pas en pause."""
    queryset = Secouristes.objects.filter(active=True, guest=False, key_expires__gte=timezone.localdate()).exclude(pin_hash='')
    return [user for user in queryset if name_login_allowed(user)]


@api_view(['POST'])
@handle_errors
def auth_name(request):
    """Connexion sans badge : {"nom", "prenom", "pin"}, pour un secouriste qui a un code PIN. Meme reponse que
    /api/auth/ (jeton `session` a renvoyer a la place de la cle du badge).

    Un refus ne dit jamais si le nom existe (les noms sont anonymises sur l'API publique). Les essais faux sont
    comptes a part du PIN du badge : pause de 15 min tous les 5 essais, connexion par nom coupee apres 20 (une
    connexion avec le badge la retablit), et 10 essais faux au plus par adresse et par quart d'heure.
    """
    data = request.data
    check_login_address(request)
    nom, prenom, pin = (str(data.get(field, '') or '').strip() for field in ('nom', 'prenom', 'pin'))
    if not nom or not prenom or not PIN_RE.match(pin):
        raise ApiError("Saisissez votre nom, votre prénom et votre code PIN (4 à 8 chiffres)")
    user = pin_login(request, name_login_candidates(nom, prenom), pin, NAME_LOGIN_ERROR)
    return Response(login_response(request, user))


@api_view(['POST'])
@handle_errors
def auth_temp(request):
    """Connexion de depannage : {"matricule", "token"} d'un QR code temporaire montre par un admin (outil de debug)
    a un secouriste qui a oublie sa carte. Le QR code vaut 1 h ; la session ouverte (sans cle de badge ni PIN, l'admin
    s'en porte garant) vaut 24 h."""
    check_login_address(request)
    user = temp_login_user(request.data.get('matricule'), request.data.get('token'))
    if user is None or user.role == Role.ADMIN:
        reserve_login_attempt(login_address_key(request))
        raise ApiError("QR code de connexion expiré ou invalide : demandez-en un nouveau au TECH",
                       status.HTTP_403_FORBIDDEN)
    journal.record(OperationKind.USER, f'{SOURCE_VERIFIED}:{user.matricule}',
                   f'connexion de dépannage de {user} ({user.matricule}) par QR code temporaire',
                   matricule=user.matricule)
    return Response(login_response(request, user, temporary=True))


def check_login_address(request):
    if cache.get(login_address_key(request), 0) >= NAME_LOGIN_ADDRESS_LIMIT:
        raise ApiError("Trop d'essais : réessayez dans quelques minutes", status.HTTP_429_TOO_MANY_REQUESTS)


def login_address_key(request):
    return 'qrprotec.name-login.' + client_address(request)


def reserve_login_attempt(address_key):
    """Compte l'essai pour l'adresse avant de verifier le PIN (et non apres un echec) : des essais envoyes en meme
    temps ne passent pas tous le controle. L'essai est rendu s'il reussit. cache.incr est atomique."""
    cache.add(address_key, 0, NAME_LOGIN_ADDRESS_PERIOD)
    try:
        attempts = cache.incr(address_key)
    except ValueError:  # entree expiree entre add et incr
        cache.set(address_key, 1, NAME_LOGIN_ADDRESS_PERIOD)
        attempts = 1
    if attempts > NAME_LOGIN_ADDRESS_LIMIT:
        raise ApiError("Trop d'essais : réessayez dans quelques minutes", status.HTTP_429_TOO_MANY_REQUESTS)


def release_login_attempt(address_key):
    try:
        cache.decr(address_key)
    except ValueError:
        pass


def matricule_login(request, matricule):
    """Connexion par matricule seul et PIN (sans la cle du badge) : memes regles que la connexion par nom (compteurs
    d'essais a part du PIN du badge, limite par adresse, refus qui ne dit pas si le matricule existe). Sans PIN
    envoye, toujours « PIN requis », que le matricule existe ou non."""
    check_login_address(request)
    pin = str(request.data.get('pin', '') or '').strip()
    if not pin:
        raise ApiError("Code PIN requis", status.HTTP_403_FORBIDDEN, pin_required=True)
    if not PIN_RE.match(pin):
        raise ApiError(MATRICULE_LOGIN_ERROR, status.HTTP_403_FORBIDDEN, pin_required=True)
    candidates = [user for user in name_login_candidates_queryset() if user.matricule == matricule]
    return pin_login(request, candidates, pin, MATRICULE_LOGIN_ERROR, pin_required=True)


def name_login_allowed(user):
    """Le candidat peut-il encore se connecter sans badge (meme filtre que name_login_candidates_queryset) ?"""
    now = timezone.now()
    return (user.active and bool(user.pin_hash) and user.pin_blocked is None and not user.pin_locked()
            and user.pin_mode != PinMode.DISABLED
            and user.name_login_failures < NAME_LOGIN_MAX_FAILURES
            and (user.name_login_locked_until is None or user.name_login_locked_until <= now))


NAME_LOGIN_STATE_FIELDS = ['active', 'pin_hash', 'pin_blocked', 'pin_locked_until', 'pin_mode',
                           'name_login_failures', 'name_login_locked_until']


def pin_login(request, candidates, pin, error, **extra):
    """Connexion sans badge parmi des candidats (nom ou matricule) : PIN verifie, essais faux comptes.

    Essais simultanes : l'adresse est comptee avant le controle, et chaque candidat est relu sous verrou
    (transaction IMMEDIATE) pour compter les echecs un par un et refuser un essai arrive apres une pause posee
    par un essai voisin."""
    address_key = login_address_key(request)
    reserve_login_attempt(address_key)
    if not candidates:
        check_password(pin, dummy_pin_hash())
    hashes = {candidate.pk: candidate.pin_hash for candidate in candidates}
    user = next((candidate for candidate in candidates if check_password(pin, candidate.pin_hash)), None)
    if user is not None:
        with transaction.atomic():
            user.refresh_from_db(fields=NAME_LOGIN_STATE_FIELDS, from_queryset=Secouristes.objects.select_for_update())
            if name_login_allowed(user) and user.pin_hash == hashes[user.pk]:
                user.name_login_failures = 0
                user.name_login_locked_until = None
                user.save(update_fields=['name_login_failures', 'name_login_locked_until'])
                release_login_attempt(address_key)
                return user
        raise ApiError(error, status.HTTP_403_FORBIDDEN, **extra)  # pause ou blocage pose entre-temps : non compte
    for candidate in candidates:
        with transaction.atomic():
            candidate.refresh_from_db(fields=['name_login_failures', 'name_login_locked_until'],
                                      from_queryset=Secouristes.objects.select_for_update())
            candidate.name_login_failures += 1
            if candidate.name_login_failures % NAME_LOGIN_LOCK_EVERY == 0:
                candidate.name_login_locked_until = timezone.now() + NAME_LOGIN_LOCK_DURATION
            candidate.save(update_fields=['name_login_failures', 'name_login_locked_until'])
    raise ApiError(error, status.HTTP_403_FORBIDDEN, **extra)


@api_view(['POST'])
@handle_errors
def pin_forgot(request):
    """Code oublie : {"matricule", "key"} (badge) -> PIN bloque jusqu'a sa reinitialisation par un admin.

    Meme reponse qu'apres 50 essais faux (403 pin_blocked + lien de deblocage) ; les admins sont prevenus
    une seule fois par blocage, quel que soit le nombre de demandes.
    """
    user = Secouristes.objects.filter(matricule=str(request.data.get('matricule', ''))).first()
    if user is None or not user.check_key(request.data.get('key')):
        raise ApiError("Badge invalide ou expire", status.HTTP_403_FORBIDDEN)
    if not user.has_pin:
        raise ApiError("Pas de code PIN à réinitialiser : choisissez-en un à la connexion", pin_setup_required=True)
    if user.pin_blocked is None:
        user.forget_pin()
        user.save(update_fields=['pin_blocked', 'pin_forgotten', 'pin_failures', 'pin_locked_until'])
    raise pin_blocked_error(user)


@api_view(['POST'])
@handle_errors
def pin_reset(request):
    """Deblocage d'un PIN par un admin (lien envoye par l'utilisateur bloque) :
    {"user": badge admin, "matricule", "token"} -> fiche de l'utilisateur ; avec "confirm": true -> PIN reinitialise,
    l'utilisateur en choisira un nouveau a sa prochaine connexion."""
    badge_user(request, (Role.ADMIN,))
    target = Secouristes.objects.select_related('pin_contact').filter(
        matricule=str(request.data.get('matricule', ''))).first()
    if target is None or not pin_reset_token_valid(target, request.data.get('token')):
        raise ApiError("Lien de déblocage invalide ou déjà utilisé (le PIN n'est plus bloqué)", status.HTTP_404_NOT_FOUND)
    info = {
        'matricule': target.matricule,
        'name': short_name(target.matricule),  # front web : jamais de nom de famille
        'role_label': target.get_role_display(),
        'fonction': target.fonction,
        'active': target.active,
        'blocked_since': ser.user_dict(target, local=True)['pin_blocked_since'],
        'failures': target.pin_failures_total,
        'contact': pin_contact_name(target),
    }
    if request.data.get('confirm'):
        target.reset_pin()
        target.save()
        return Response({**info, 'reset': True})
    return Response({**info, 'reset': False})


@api_view(['GET'])
@handle_errors
def item_detail(request, iid):
    item = get_object_or_404(Items.objects.select_related('pack__item_type', 'location__lot_type'), iid=iid)
    if item.pack.item_type.lost_object:
        # objet qu'on peut perdre (ordinateur...) : fiche ouverte par la personne qui l'a trouve, avec le message
        # « objet perdu » ; ni lot ni verif (pas de dernier emplacement, la fiche ne propose pas de vérif)
        return Response({**ser.item_dict(item), 'last_location': None,
                         'lost_message': ServerSettings.get().lost_object_message,
                         'revisions': [ser.revision_dict(revision) for revision in item.revisions.all()[:5]]})
    # fiche de l'item (front web) : dernier emplacement connu, pour ouvrir son lot ou lancer sa verif
    return Response({**ser.item_dict(item), 'last_location': ser.item_last_location(item),
                     'revisions': [ser.revision_dict(revision) for revision in item.revisions.all()[:5]]})


@api_view(['POST'])
@handle_errors
def item_revision(request, iid):
    """Revision ou desinfection d'un item (materiel non consommable), roles gestion et admin seulement : {"user",
    "kind": "revision" (par defaut) ou "disinfection", "tasks": taches cochees (toutes), "next_due": date de la
    prochaine (par defaut aujourd'hui + la periodicite du type), "note"}."""
    user = connected_user(request, GESTION)
    item = get_object_or_404(Items.objects.select_related('pack__item_type', 'location__lot_type'), iid=iid)
    data = request.data
    tasks = data.get('tasks') or []
    if not isinstance(tasks, list):
        raise ApiError('tasks : liste des tâches cochées attendue')
    next_due = parse_date(data['next_due'], 'next_due') if data.get('next_due') else None
    try:
        services.maintain_item(item, f'{SOURCE_VERIFIED}:{user.matricule}', str(data.get('kind') or 'revision'),
                               [str(task) for task in tasks], next_due, data.get('note', ''))
    except ValueError as exc:
        raise ApiError(str(exc))
    return Response({**ser.item_dict(item), 'last_location': ser.item_last_location(item),
                     'revisions': [ser.revision_dict(revision) for revision in item.revisions.all()[:5]]})


@api_view(['POST'])
@handle_errors
def item_maintenance_date(request, iid):
    """Date de revision ou de desinfection saisie a la main (roles gestion et admin, web ou poste), sans entretien
    fait : {"user", "kind", "last_done": derniere faite (facultatif), "next_due": prochaine (par defaut last_done + la
    periodicite), "note"}. Sert a recaler le cycle (materiel revise ailleurs, date connue)."""
    connected_user(request, GESTION)
    identity = operation_identity(request)
    item = get_object_or_404(Items.objects.select_related('pack__item_type', 'location__lot_type'), iid=iid)
    data = request.data
    next_due = parse_date(data['next_due'], 'next_due') if data.get('next_due') else None
    last_done = parse_date(data['last_done'], 'last_done') if data.get('last_done') else None
    try:
        services.schedule_maintenance(item, identity, str(data.get('kind') or 'revision'), next_due, last_done,
                                      data.get('note', ''))
    except ValueError as exc:
        raise ApiError(str(exc))
    return Response({**ser.item_dict(item), 'last_location': ser.item_last_location(item),
                     'revisions': [ser.revision_dict(revision) for revision in item.revisions.all()[:5]]})


@api_view(['GET'])
@handle_errors
def revisions_due(request):
    """Materiel a reviser (en retard ou dans les `days` jours, 30 par defaut), pour les postes."""
    days = parse_int(request.query_params.get('days', 30), 'days', 0, 366)
    return Response([ser.item_dict(item) for item in services.revisions_due(days=days)])


@api_view(['GET'])
@handle_errors
def lot_detail(request, lot_id):
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    if not is_front(request) and not lot.active:
        raise ApiError("Lot inconnu", status.HTTP_404_NOT_FOUND)
    data = ser.lot_dict(lot, local=is_privileged(request), with_items=True)
    from .. import checklists
    # bouton « Partir avec ce lot » de la fiche, ou retour du lot sorti avec une liste
    data['departure'] = checklists.lot_departure(lot)
    # QR code de scelle : ?seal=CODE -> 'valid' (scelle intact), 'unsealed' (brise), 'wrong' (ancien scelle)
    seal = request.query_params.get('seal')
    if seal is not None:
        data['seal_check'] = 'valid' if lot.check_seal(seal) else ('wrong' if lot.is_sealed else 'unsealed')
    # etiquette privee scannee : ?key=CLE -> 'valid', 'expired' ou 'wrong'. Sans connexion la cle n'est pas
    # renvoyee : le front ne peut pas la comparer lui-meme.
    key = request.query_params.get('key')
    if key is not None:
        data['key_check'] = lot.key_status(key)
        if data['key_check'] != 'valid':
            data['key_error'] = key_error(lot, key)
    return Response(data)


def lot_sheet_pdf(request, lot_id):
    """Fiche d'inventaire papier du lot et de ses sous-lots (PDF A4, liste de controle, une page par sous-lot). Meme
    acces que le detail public du lot : la fiche ne contient que les items attendus, jamais les cles."""
    if request.method != 'GET':
        return HttpResponseNotAllowed(['GET'])
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    if not is_front(request) and not lot.active:
        raise Http404()
    response = HttpResponse(lot_sheet.lot_sheet_pdf(lot), content_type='application/pdf')
    filename = re.sub(r'[^A-Za-z0-9_-]+', '-', name_key(lot.name)).strip('-') or lot.id
    response['Content-Disposition'] = f'inline; filename="fiche-{filename}.pdf"'
    response['Cache-Control'] = 'no-store'
    return response


@api_view(['POST'])
@handle_errors
def lot_state_report(request, lot_id):
    """Rapport d'etat du lot et de ses sous-lots a une date donnee (PDF A4), admins seulement, depuis le front web :
    {"user": badge admin, "at": "AAAA-MM-JJTHH:MM" (heure locale, passee), "antenna": nom de l'antenne}. Derniere
    verif, items avec iid et peremption, qui a edite le rapport et quand, case « Certifié par le TECH <antenne> ».
    Le nom de l'antenne est retenu pour les rapports suivants (reglage du serveur)."""
    user = badge_user(request, ADMIN)
    lot = get_object_or_404(Lots.objects.select_related('lot_type', 'parent'), id=lot_id)
    raw = str(request.data.get('at') or '').strip()
    try:
        at = datetime.fromisoformat(raw) if raw else timezone.now()
    except ValueError:
        raise ApiError("Date et heure du rapport invalides (attendu : AAAA-MM-JJTHH:MM)")
    if timezone.is_naive(at):
        at = timezone.make_aware(at)
    if at > timezone.now() + timedelta(minutes=1):
        raise ApiError("Date à venir : le rapport décrit l'état d'un lot à une date passée (ou maintenant)")
    settings_row = ServerSettings.get()
    if 'antenna' in request.data:
        antenna = ' '.join(str(request.data.get('antenna') or '').split())[:64]
        if antenna != settings_row.antenna_name:
            settings_row.antenna_name = antenna
            settings_row.save(update_fields=['antenna_name'])
    identity = format_identity_verified(user.matricule)
    now = timezone.now()
    pdf = lot_report.lot_report_pdf(lot, at, lot_report.who(identity), settings_row.antenna_name, edited=now)
    journal.record(OperationKind.LOT_REPORT, identity,
                   f"rapport d'état au {timezone.localtime(at).strftime('%d/%m/%Y %H:%M')}", lot=lot, at=now,
                   state_at=ser._date(at))
    response = HttpResponse(pdf, content_type='application/pdf')
    filename = re.sub(r'[^A-Za-z0-9_-]+', '-', name_key(lot.name)).strip('-') or lot.id
    response['Content-Disposition'] = (f'inline; filename="rapport-{filename}-'
                                       f'{timezone.localtime(at).strftime("%Y%m%d-%H%M")}.pdf"')
    response['Cache-Control'] = 'no-store'
    return response


@api_view(['POST'])
@handle_errors
def lot_unseal(request, lot_id):
    """Scelle brise (ouverture du lot). Sur l'API publique, exige la cle du lot (etiquette privee)."""
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    require_lot_key(request, lot)
    identity = operation_identity(request)
    with transaction.atomic():
        services.break_seal(lot, identity, str(request.data.get('reason', '')).strip()[:64] or 'ouverture')
    return Response(ser.lot_dict(lot, local=is_privileged(request), with_items=True))


@api_view(['POST'])
@handle_errors
def lot_return(request, lot_id):
    """Bouton « Retour de ce lot » de la fiche (tout utilisateur connecte) : le lot parti avec une liste sortie est
    scanne rentre sur cette liste (checklists.lot_return) ; renvoie l'identifiant de la liste pour l'ouvrir."""
    from .. import checklists
    user = connected_user(request)
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id, active=True)
    message, checklist = _checklist_call(lambda: checklists.lot_return(lot, user))
    return Response({'message': message, 'list': checklist.id})


@api_view(['POST'])
@handle_errors
def lot_used(request, lot_id):
    """Bouton « Marquer ce lot comme utilisé (demander une vérif) » : tout secouriste identifie (badge, ou nom declare
    si le serveur l'autorise), sans cle du lot. Le lot et ses sous-lots non scelles passent en verif recommandee."""
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    if not is_front(request) and not lot.active:
        raise ApiError("Lot inconnu", status.HTTP_404_NOT_FOUND)
    identity = operation_identity(request)
    try:
        with transaction.atomic():
            marked = services.mark_used(lot, identity)
    except ValueError as exc:
        raise ApiError(str(exc))
    lot.refresh_from_db()
    data = ser.lot_dict(lot, local=is_privileged(request), with_items=True)
    data['marked'] = [target.name for target in marked]
    return Response(data)


@api_view(['POST'])
@handle_errors
def lot_seal_open(request, lot_id):
    """Etiquette d'ouverture du scelle (rangee dans le lot) : son scan ouvre le scelle, sans cle ni connexion.
    L'ouverture est attribuee a l'utilisateur s'il est identifie ; sinon elle est anonyme et peut etre signee
    par un nouvel appel juste apres la connexion."""
    lot = get_object_or_404(Lots.objects.select_related('lot_type', 'parent'), id=lot_id)
    if not is_front(request) and not lot.active:
        raise ApiError("Lot inconnu", status.HTTP_404_NOT_FOUND)
    if not lot.check_seal_open(str(request.data.get('code', ''))):
        raise ApiError("Étiquette d'ouverture d'un ancien scellé : elle n'ouvre plus ce lot",
                       status.HTTP_404_NOT_FOUND)
    allowed = ServerSettings.get().declared_identity
    identity = identity_from_request(request.data, front_user(request), allowed)
    with transaction.atomic():
        result = services.open_seal_with_label(lot, identity)
    return Response({
        'result': result,
        'identified': identity is not None,
        'declared_identity': allowed,
        'lot': ser.lot_dict(lot, local=is_privileged(request), with_items=True),
        # ecran d'ouverture : le scelle etait-il valide, et jusqu'a quand
        'seal': ser.seal_opening_dict(lot),
    })


def verif_targets(request, entries, identity):
    """Lots d'une verif ([{"id", "key"}, ...]) : tous du meme lot global. Sur l'API publique, sans badge (nom declare,
    option du serveur), une cle valide d'un seul de ces lots suffit pour toute la verif : celle du lot global, d'un
    lot ajoute ou d'un de leurs sous-lots (envoye dans la liste avec sa cle, il fait deja partie de la verif). Avec un
    badge, pas de cle : l'etiquette d'un des lots suffit (require_label)."""
    return verif_lots(entries, is_local(request), verified=is_verified(identity))




def verif_lots(entries, local, holders=(), verified=False):
    """verif_targets hors requete (vérif à plusieurs enregistree par le serveur, live.py). holders : [(lot, cle)]
    autres lots du meme lot global dont l'etiquette cle a ete scannee pendant la vérif (key_holders). verified :
    secouriste reconnu, l'etiquette cle n'est plus exigee (seulement pour une identite declaree)."""
    if not isinstance(entries, list) or not entries:
        raise ApiError("lots : liste de lots attendue")
    lots = []
    keys = {}
    for entry in entries:
        if isinstance(entry, str):
            entry = {'id': entry}
        if not isinstance(entry, dict):
            raise ApiError("lots : liste de {id, key} attendue")
        lot = get_object_or_404(Lots.objects.select_related('lot_type', 'parent'), id=str(entry.get('id', '')))
        if not lot.active:
            raise ApiError(f"Lot {lot.name} archivé", status.HTTP_404_NOT_FOUND if not local else status.HTTP_400_BAD_REQUEST)
        if lot.id not in keys:
            lots.append(lot)
        if entry.get('key'):
            keys[lot.id] = entry['key']
        else:
            keys.setdefault(lot.id, '')
    roots = {lot.root().id for lot in lots}
    if len(roots) > 1:
        raise ApiError("Les lots d'une même vérif doivent appartenir au même lot global")
    if not local and not verified and not any(lot.check_key(keys[lot.id]) for lot in lots) \
            and not any(lot.root().id in roots and lot.check_key(key) for lot, key in holders):
        # une seule etiquette cle d'un des lots (un sous-lot suffit, ex : un rangement pour le stock) valide toute
        # la verif : les lots sont tous du meme lot global. Vérif en cascade : celle d'un autre lot du meme lot global
        # scannee plus tot dans la vérif (holders) aussi.
        lot = next((lot for lot in lots if keys[lot.id]), lots[0])
        raise ApiError(key_error(lot, keys[lot.id]), status.HTTP_403_FORBIDDEN)
    for lot in lots:
        lot.verif_key_sent = keys[lot.id]  # une cle valide compte aussi comme etiquette scannee (require_label)
    return lots


def require_label(lots, data, holders=(), sealed=(), extra=()):
    """API publique : l'etiquette (publique ou cle) d'au moins un des lots verifies, d'un de leurs sous-lots ou d'un
    lot qui les contient a ete scannee pendant la verif, pour ne pas valider la verif d'un autre lot. "labels" : [id]
    des lots dont l'etiquette a ete scannee ; une cle valide envoyee (lots, keys) ou le scelle intact d'un sous-lot
    (sealed) comptent aussi. extra : etiquettes scannees par les autres participants de la vérif à plusieurs."""
    labels = data.get('labels') or []
    if not isinstance(labels, list):
        raise ApiError("labels : liste d'ids de lots attendue")
    scanned = {str(value) for value in labels[:200] if value} | set(extra) | {lot.id for lot in sealed}
    scanned |= {lot.id for lot in lots if getattr(lot, 'verif_key_sent', '') and lot.check_key(lot.verif_key_sent)}
    scanned |= {lot.id for lot, key in holders if lot.check_key(key)}
    # vérif en cascade : l'etiquette d'un lot qui le contient compte (celle du VPS, puis on entre dans une armoire)
    covered = {sub_lot.id for sub_lot in services.verif_lots(lots)} | {up.id for lot in lots for up in lot.ancestors()}
    if scanned & covered:
        return
    raise ApiError(label_error(lots), status.HTTP_403_FORBIDDEN, label_required=True)


def de_name(name):
    """« de » devant un nom de lot : « de Sac de soin », « d'Armoire 1 », « du Sac O2 » (pour « Le Sac O2 »), « des … »,
    « de la … », « de l'… ». Meme regle que deName (web/app/utils.js) et de_name (french.hpp)."""
    lower = name.lower()
    for article, replacement in (('les ', 'des '), ('le ', 'du '), ('la ', 'de la '), ("l'", "de l'")):
        if lower.startswith(article):
            return replacement + name[len(article):]
    return ("d'" if name[:1].lower() in 'aeiouyàâéèêëîïôöùûü' and name else 'de ') + name


def de_names(names):
    """« de A », « de A ou de B », « de A, de B ou de C »."""
    names = [de_name(name) for name in names]
    if not names:
        return ''
    return names[0] if len(names) == 1 else ', '.join(names[:-1]) + ' ou ' + names[-1]


def label_error(lots):
    """Le nom des lots a scanner, pas « lot ou sous-lot » : « de B+ (ou celle de Sac de soin ou de Sac O2) »."""
    if len(lots) > 3:
        target = de_name(lots[0].name) + " ou d'un des autres lots de la vérif"
    elif len(lots) > 1:
        target = de_names([lot.name for lot in lots])
    else:
        subs = sorted((lot.name for lot in lots[0].children.filter(active=True)), key=str.casefold)
        subs = '' if not subs else de_names(subs) if len(subs) <= 3 else "d'un des lots qu'il contient"
        # lot de referentiel (B+) sans etiquette : seulement celles de ses sacs (comme labelTarget du web et le poste)
        target = subs if subs and services.virtual_lot(lots[0]) else (
            de_name(lots[0].name) + (f' (ou celle {subs})' if subs else ''))
    return f"Scannez l'étiquette {target} pour valider : c'est elle qui montre que la vérif est bien celle de ce lot."


def key_holders(data, extra=None):
    """"keys" d'une vérif en cascade : [{"id", "key"}], etiquettes cles d'autres lots du meme lot global scannees
    plus tot dans la vérif (ex : celle du VPS, puis la vérif d'une de ses armoires). Elles valident la vérif sans que
    ces lots en fassent partie. extra : {lot id: cle} gardees par la vérif à plusieurs (live.py)."""
    entries = data.get('keys') or []
    if not isinstance(entries, list):
        raise ApiError("keys : liste [{id, key}] attendue")
    wanted = {}
    for entry in entries[:50]:
        if isinstance(entry, dict) and entry.get('id') and entry.get('key'):
            wanted[str(entry['id'])] = str(entry['key'])
    for lot_id, key in (extra or {}).items():
        if key:
            wanted.setdefault(lot_id, key)
    lots = Lots.objects.select_related('parent').filter(id__in=list(wanted), active=True)
    return [(lot, wanted[lot.id]) for lot in lots]


def excluded_lots(data, lots):
    """"exclude" d'une vérif en cascade : [id], sous-lots des lots verifies laisses de cote avec leurs sous-lots
    (services.perform_verif). Un lot verifie lui-meme ne peut pas etre exclu."""
    ids = data.get('exclude') or []
    if not isinstance(ids, list) or not all(isinstance(value, str) for value in ids):
        raise ApiError("exclude : liste d'ids de lots attendue")
    inside = {sub_lot.id: sub_lot for sub_lot in services.verif_lots(lots)}
    result = []
    for lot_id in dict.fromkeys(ids):
        if lot_id in {lot.id for lot in lots}:
            raise ApiError("exclude : un lot vérifié ne peut pas être laissé de côté")
        if lot_id not in inside:
            raise ApiError(f"exclude : {lot_id} n'est pas un sous-lot des lots vérifiés")
        result.append(inside[lot_id])
    return result


def sealed_targets(data, lots):
    """"sealed" d'une verif : [{"id", "code"}], lots scelles dont l'etiquette du scelle a ete scannee pendant la verif
    (services.perform_verif). Le code du scelle prouve le scan (il remplace la cle, rangee dans le lot scelle). Un lot
    d'un autre lot global, au scelle brise ou a l'etiquette d'un ancien scelle est refuse."""
    entries = data.get('sealed') or []
    if not isinstance(entries, list):
        raise ApiError("sealed : liste [{id, code}] attendue")
    roots = {lot.root().id for lot in lots}
    result = []
    for entry in entries:
        if not isinstance(entry, dict):
            raise ApiError("sealed : liste [{id, code}] attendue")
        lot = Lots.objects.select_related('lot_type', 'parent').filter(id=str(entry.get('id', '')), active=True).first()
        if lot is None:
            raise ApiError(f"Lot scellé inconnu : {entry.get('id', '')}", status.HTTP_404_NOT_FOUND)
        if lot.root().id not in roots:
            raise ApiError(f"Le lot scellé {lot.name} n'appartient pas au lot global vérifié")
        if not lot.check_seal(str(entry.get('code', ''))):
            raise ApiError(f"Scellé de {lot.name} brisé ou étiquette d'un ancien scellé : vérifiez son contenu")
        if lot.id not in {done.id for done in result}:
            result.append(lot)
    return result


@api_view(['POST'])
@handle_errors
def lot_verif(request, lot_id):
    """Verif d'un lot et de ses sous-lots (cle du lot). 'partial' : voir verifs()."""
    identity = operation_identity(request)
    lots = verif_targets(request, [{'id': lot_id, 'key': request.data.get('key')}], identity)
    sealed = sealed_targets(request.data, lots)
    if not is_local(request):
        require_label(lots, request.data, sealed=sealed)
    report = services.perform_verif(lots, iid_list(request.data), identity, bool(request.data.get('partial')),
                                    places=place_map(request.data), sealed=sealed,
                                    strict=bool(request.data.get('full')), checked=checked_map(request.data))
    return Response(report)


@api_view(['POST'])
@handle_errors
def verifs(request):
    """Verif de plusieurs lots d'un meme lot global : {"lots": [{"id", "key"}], "items": [...], "partial": bool}.

    Chaque lot est verifie avec ses sous-lots. En verif partielle, seuls les lots que les items scannes rendent
    complets sont verifies ; les items destines aux autres lots y sont ajoutes (reassort).
    "places" : {iid: lot}, lot dont l'etiquette a ete scannee en dernier avant l'item (services.assign_items).
    "sealed" : [{"id", "code"}], lots scelles dont l'etiquette du scelle a ete scannee : scelle intact, comptes
    comme verifies sans etre ouverts (sealed_targets).
    Vérif à plusieurs (inventory/live.py) : "live": {"id", "pid", "token"} du participant qui envoie. Les cles des
    lots manquantes sont prises dans la verif commune, ses participants sont enregistres sur la verif et le compte
    rendu leur est envoye.
    Vérif en cascade (lot global, ex : VPS > armoires > sacs) : chaque lot est valide a part, au fil de la vérif.
    "exclude" : [id], sous-lots laisses de cote (deja valides, pas faits, ou faits par un autre participant) ;
    "keys" : [{"id", "key"}], etiquettes cles d'autres lots du meme lot global scannees plus tot (key_holders) ;
    "step" : avec "live", la vérif commune continue (les autres participants voient le lot valide) au lieu de se
    terminer.
    "full" : « vérif complète » (bouton gestion et admins du front web) : les items et sous-lots facultatifs sont
    obligatoires pour cette verif (services.perform_verif strict).
    "done_at", "sent_at" : heures de l'appareil a la validation et a l'envoi (verif faite hors connexion, envoyee
    plus tard) : la verif est datee a l'heure des scans (verif_time), l'heure de reception est gardee a cote."""
    entries = request.data.get('lots')
    iids = iid_list(request.data)
    step = bool(request.data.get('step'))
    sealed_ids = [str(entry.get('id', '')) for entry in request.data.get('sealed') or [] if isinstance(entry, dict)]
    team = live.hub.verif_team(request.data.get('live'), set(iids) | set(sealed_ids) if step else None)
    if team and isinstance(entries, list):
        entries = [dict(entry, key=entry.get('key') or team['keys'].get(str(entry.get('id', '')), ''))
                   if isinstance(entry, dict) else entry for entry in entries]
    identity = team_identity(request, team) if team and team['shared'] else operation_identity(request)
    holders = key_holders(request.data, team['keys'] if team else None)
    lots = verif_lots(entries, is_local(request), holders, is_verified(identity))
    sealed = sealed_targets(request.data, lots)
    if not is_local(request):
        require_label(lots, request.data, holders, sealed, team['labels'] if team else ())
    participants = [value for value in team['identities'] if value != identity] if team else []
    report = services.perform_verif(lots, iids, identity, bool(request.data.get('partial')),
                                    participants, place_map(request.data), sealed, excluded_lots(request.data, lots),
                                    strict=bool(request.data.get('full')) or bool(team and team['full']),
                                    checked=checked_map(request.data), now=verif_time(request.data))
    report['participants'] = [ser._who(value) for value in [identity, *participants]]
    if team and step:
        live.hub.step(team, report, iids, final=bool(request.data.get('finish')))
    elif team:
        live.hub.finish(team, report)
    return Response(report)


# Verif faite hors connexion : envoyee moins de VERIF_SENT_GAP apres sa validation, elle est datee de sa reception
# comme avant ; au-dela, de l'heure des scans (Verifs.received garde l'heure d'envoi, ser.verif_sent l'affiche)
VERIF_SENT_GAP = ser.VERIF_SENT_GAP


def _client_time(value):
    if not isinstance(value, str) or not value:
        return None
    try:
        parsed = parse_datetime(value)
    except ValueError:
        return None
    if parsed is None:
        return None
    return parsed if timezone.is_aware(parsed) else timezone.make_aware(parsed)


def verif_time(data):
    """Heure de la verif d'apres l'appareil : "done_at" (validation, juste apres les scans) recale sur l'horloge du
    serveur par "sent_at" (heure de l'appareil a l'envoi) : seul compte le temps ecoule sur l'appareil entre les deux,
    pas son heure, souvent fausse. Jamais dans le futur ; None (heure de reception) sans "done_at" ou si la verif est
    envoyee tout de suite."""
    done = _client_time(data.get('done_at'))
    if done is None:
        return None
    received = timezone.now()
    sent = _client_time(data.get('sent_at'))
    if sent is not None:
        done = received - (sent - done)
    if received - done < VERIF_SENT_GAP:
        return None
    return done


RECENT_VERIFS = 30
# etapes d'une meme verif en cascade (VPS > armoire > sac, chaque sous-lot valide a part) : meme personne, meme lot du
# haut, chacune a moins de CASCADE_GAP de la precedente et sans revoir un lot deja valide
CASCADE_GAP = timedelta(minutes=30)


@api_view(['POST'])
@handle_errors
def verifs_cache(request):
    """Cache de vérif des fronts (verif_cache) : {"buckets": {seau: empreinte deja connue}} -> seaux changes seulement
    (tout au premier appel). Public comme le detail d'un lot ou d'un item ; un badge envoye doit etre valide. Ni
    cles de lots ni codes de scelles."""
    if request.data.get('user') is not None:
        badge_user(request)
    return Response(verif_cache.diff(request.data.get('buckets')))


@api_view(['POST'])
@handle_errors
def verifs_recent(request):
    """Dernieres verifs de lots (page « Suivi des vérifs ») : tout badge valide. Une verif groupee (meme instant,
    meme personne) et les etapes d'une verif en cascade forment une seule ligne avec la liste de ses lots ; "ids" :
    une verif par etape, que le compte rendu reunit."""
    badge_user(request)
    limit = parse_int(request.data.get('limit', RECENT_VERIFS), 'limit', 1, 100)
    parents = dict(Lots.objects.values_list('id', 'parent_id'))

    def root(lot_id):
        seen = set()
        while parents.get(lot_id) and lot_id not in seen:
            seen.add(lot_id)
            lot_id = parents[lot_id]
        return lot_id

    # etapes : verifs au meme instant par la meme personne (verif groupee d'un lot et de ses sous-lots)
    steps = []
    for verif in Verifs.objects.select_related('lot').filter(lot__isnull=False).order_by('-datetime', '-id')[:limit * 20]:
        if steps and (steps[-1][0].datetime, steps[-1][0].by) == (verif.datetime, verif.by):
            steps[-1].append(verif)
        else:
            steps.append([verif])
    rows = []
    for step in steps:
        head = step[0]
        roots = {root(verif.lot_id) for verif in step}
        last = rows[-1] if rows else None
        same_cascade = (last and last['by_raw'] == head.by and roots == {last['_root']}
                        and last['_oldest'] - head.datetime <= CASCADE_GAP
                        and not {verif.lot_id for verif in step} & {lot['id'] for lot in last['lots']})
        if not same_cascade:
            if len(rows) >= limit:
                break
            last = {'_root': roots.pop() if len(roots) == 1 else None, 'by_raw': head.by, 'id': head.id, 'ids': [],
                    'at': ser._date(head.datetime), 'by': ser._who(head.by), 'participants': [], 'lots': [],
                    'complete': True, 'present': 0, 'missing': 0, 'expired': 0,
                    'replaced': 0,  # replaced : perimes retires du lot, a jeter
                    'sent': None}  # faite hors connexion : heure d'envoi (la plus recente des etapes)
            rows.append(last)
        last['_oldest'] = head.datetime
        last['sent'] = last['sent'] or ser.verif_sent(head)
        last['ids'].append(head.id)
        last['participants'] = list(dict.fromkeys(last['participants'] + [ser._who(value) for value in head.participants]))
        for verif in step:
            last['lots'].append(services.VerifTitles.row(verif))
            last['complete'] = last['complete'] and verif.complete
            last['present'] += verif.present_count
            last['missing'] += verif.missing_count
            last['expired'] += verif.expired_count
            last['replaced'] += verif.replaced_count
    titles = services.VerifTitles(parents)
    for row in rows:
        for key in ('_root', '_oldest', 'by_raw'):
            del row[key]
        # les lots qui nomment la ligne, leurs sous-lots sont dans le compte rendu
        row['top'], _ = titles.names(row['lots'])
        for lot in row['lots']:
            del lot['_own']
        row['participants'] = [who for who in row['participants'] if who != row['by']]
    return Response(rows)


# Operations du journal enregistrees au meme instant qu'une verif, citees dans son compte rendu
REPORT_OPERATION_KINDS = (OperationKind.RESTOCK, OperationKind.REMOVE, OperationKind.UNSEAL)


@api_view(['POST'])
@handle_errors
def verif_report(request, verif_id):
    """Compte rendu d'une verif passee (clic sur une ligne de « Vérifs récentes ») : tout badge valide, comme la
    liste. Une verif groupee (meme instant, meme personne) est rendue en entier, un lot par sous-lot verifie."""
    badge_user(request)
    first = get_object_or_404(Verifs.objects.select_related('lot'), id=verif_id, lot__isnull=False)
    verifs = list(Verifs.objects.select_related('lot').filter(datetime=first.datetime, by=first.by, lot__isnull=False)
                  .order_by('id'))
    by_lot = {verif.lot_id: verif for verif in verifs}
    operations = list(Operation.objects.filter(at=first.datetime, by=first.by).order_by('id'))
    details = {op.details.get('verif'): op.details for op in operations if op.kind == OperationKind.VERIF}
    entries = defaultdict(lambda: defaultdict(Counter))
    for entry in VerifItem.objects.filter(verif__in=verifs).select_related('item__pack__item_type'):
        pack = entry.item.pack
        section = 'expired' if entry.result == VerifResult.PRESENT and entry.expired else entry.result
        entries[entry.verif_id][section][(pack.item_type.name, pack.peremption)] += 1

    def depth(lot):
        level, parent = 0, lot.parent_id
        while parent in by_lot and level < len(by_lot):
            level += 1
            parent = by_lot[parent].lot.parent_id
        return level

    # ordre de l'arbre : chaque lot avant ses sous-lots
    children = defaultdict(list)
    for verif in verifs:
        children[verif.lot.parent_id if verif.lot.parent_id in by_lot else None].append(verif)
    ordered = []
    pending = list(reversed(children[None]))
    while pending:
        verif = pending.pop()
        ordered.append(verif)
        pending.extend(reversed(children[verif.lot_id]))

    titles = services.VerifTitles()
    lots = []
    for verif in ordered:
        info = details.get(verif.id, {})
        lots.append({
            # sous-lots attendus (actifs, non facultatifs) : le front ecarte un lot cadre (le Stock dont on n'a fait
            # qu'un rangement), voir services.VerifTitles
            'children': titles.children.get(verif.lot_id, []),
            # parent : les etapes d'une verif en cascade sont reunies en un seul arbre par le front
            'id': verif.lot_id, 'name': verif.lot.name, 'parent': verif.lot.parent_id, 'depth': depth(verif.lot),
            'complete': verif.complete,
            'partial': bool(info.get('partial')), 'present_count': verif.present_count,
            'missing_count': verif.missing_count, 'expired_count': verif.expired_count,
            'replaced_count': verif.replaced_count, 'lacking': info.get('lacking', []),
            **{section: [{'type_name': name, 'peremption': ser._date(peremption), 'count': count}
                         for (name, peremption), count in sorted(entries[verif.id][section].items(),
                                                                 key=lambda row: (row[0][0], str(row[0][1])))]
               for section in ('present', 'expired', 'replaced', 'missing')},
        })
    return Response({
        'id': first.id,
        'at': ser._date(first.datetime),
        'sent': ser.verif_sent(first),  # faite hors connexion : heure d'envoi, a cote de celle des scans
        'by': ser._who(first.by),
        'participants': [ser._who(value) for value in first.participants],
        'complete': all(verif.complete for verif in verifs),
        'lots': lots,
        # scelles intacts constates (comptes comme verifies, sans ligne Verifs) et reassorts, retraits, ouvertures
        'sealed': [{'id': op.lot_id, 'name': op.lot_name} for op in operations
                   if op.kind == OperationKind.VERIF and op.details.get('sealed_intact')],
        'operations': [{'kind': op.kind, 'kind_label': op.get_kind_display(), 'lot': op.lot_id,
                        'lot_name': op.lot_name, 'summary': op.summary}
                       for op in operations if op.kind in REPORT_OPERATION_KINDS],
    })


@api_view(['POST'])
@handle_errors
def items_out(request):
    """Items sortis du stock sans lot : {"items": [...]}. Ils comptent comme utilises s'ils ne sont pas revus a une
    verif dans QRPROTEC['OUT_DAYS'] jours (ou a leur peremption). Meme identite qu'une verif."""
    identity = operation_identity(request)
    return Response(services.mark_out(iid_list(request.data), identity))


@api_view(['POST'])
@handle_errors
def lot_add_items(request, lot_id):
    """Reassort d'un lot. Sur l'API publique : sa cle sous un nom declare, l'etiquette du lot avec un badge (verif)."""
    lot = get_object_or_404(Lots, id=lot_id)
    identity = operation_identity(request)
    if not is_local(request) and is_verified(identity):
        require_label([lot], request.data, extra=[lot.id] if lot.check_key(request.data.get('key')) else ())
    else:
        require_lot_key(request, lot)
    return Response(services.move_items(iid_list(request.data), lot, identity, SeenWhile.ADD,
                                        declared=not is_verified(identity), debug=debug_options(request)))


@api_view(['POST'])
@handle_errors
def lots_summary(request):
    """Liste des lots actifs et de leur etat (lecture seule) : page « État des lots » et choix d'un lot a verifier.
    Publique comme le detail d'un lot (etiquettes publiques) ; un badge envoye doit etre valide. Les cles des lots
    ne sont jamais renvoyees."""
    if request.data.get('user') is not None:
        badge_user(request)
    today = timezone.localdate()
    queryset = Lots.objects.select_related('lot_type').filter(active=True).order_by('lot_type__name', 'name')
    # lots a traiter en haut ; garde tant que la base n'a pas change (voir datacache)
    return Response(datacache.cached(('lots_summary', today), lambda: _with_out(
        ser.lot_list(queryset, today=today, attention=True))))


@api_view(['POST'])
@handle_errors
def lots_order(request):
    """Ordre choisi des lots d'un meme niveau (onglet « Ordre des lots » de la Gestion des lots, au poste seulement :
    c'est de la gestion des lots, Hippolyte 2026-11-11 ; gestion et admin) :
    {"parent": id du lot (vide : les lots globaux), "lots": [ids dans l'ordre voulu]} ; les freres absents de la liste
    passent apres, dans leur ordre actuel. {"parent", "reset": true} : ordre d'origine (type puis nom). Il departage
    les lots de meme couleur, apres le tri rouge > orange > vert. Repond l'etat des lots, dans le nouvel ordre."""
    user = require_front(request, GESTION)
    data = request.data
    parent_id = str(data.get('parent') or '')
    parent = get_object_or_404(Lots, id=parent_id, active=True) if parent_id else None
    siblings = list(Lots.objects.select_related('lot_type').filter(active=True, parent=parent)
                    .order_by('lot_type__name', 'name'))
    if parent is None:  # lot global : son parent est vide ou n'est plus actif
        siblings = [lot for lot in Lots.objects.select_related('lot_type', 'parent').filter(active=True)
                    .order_by('lot_type__name', 'name') if lot.parent is None or not lot.parent.active]
    by_id = {lot.id: lot for lot in siblings}
    if data.get('reset'):
        ordered, sort_order = siblings, None
    else:
        wanted = data.get('lots')
        if not isinstance(wanted, list):
            raise ApiError("Liste des lots manquante")
        unknown = [str(lot_id) for lot_id in wanted if str(lot_id) not in by_id]
        if unknown:
            raise ApiError("Ces lots ne sont pas au même niveau : " + ', '.join(unknown[:5]))
        chosen = list(dict.fromkeys(str(lot_id) for lot_id in wanted))
        rest = sorted((lot for lot in siblings if lot.id not in chosen), key=custom_order_key)
        ordered, sort_order = [by_id[lot_id] for lot_id in chosen] + rest, True
    with transaction.atomic():
        for index, lot in enumerate(ordered):
            value = index + 1 if sort_order else None
            if lot.sort_order != value:
                lot.sort_order = value
                lot.save(update_fields=['sort_order'])
    where = f'dans {parent.name}' if parent else 'globaux'
    journal.record(OperationKind.LOT, f'{SOURCE_VERIFIED}:{user.matricule}',
                   f"ordre des lots {where} " + ("remis par défaut" if not sort_order
                                                 else ': ' + ', '.join(lot.name for lot in ordered))[:200],
                   lot=parent)
    queryset = Lots.objects.select_related('lot_type').filter(active=True).order_by('lot_type__name', 'name')
    return Response(_with_out(ser.lot_list(queryset, local=True)))  # meme liste que GET /api/lots/


def _with_out(rows):
    """État des lots (web et poste) : `out_with` = nom de la liste avec laquelle le lot est en poste (parti avec une
    liste sortie, pas encore rentre ; sous-lots compris, avec leur lot), '' sinon."""
    from .. import checklists
    out = checklists.lots_out()
    parents = {row['id']: row['parent'] for row in rows}
    for row in rows:
        lot_id, hops = row['id'], 0
        while lot_id is not None and lot_id not in out and hops < 20:
            lot_id, hops = parents.get(lot_id), hops + 1
        row['out_with'] = out.get(lot_id, '') if lot_id is not None else ''
    return rows


@api_view(['POST'])
@handle_errors
def stock_summary(request):
    """Etat des stocks en lecture seule, reserve aux roles gestion et admin (badge)."""
    badge_user(request, (Role.GESTION, Role.ADMIN))
    soon = parse_int(request.data.get('soon_days', 30), 'soon_days', 0, 3650)
    return Response(services.stock_status(soon))


def forecast_params(params):
    return {
        'months': parse_int(params.get('months', 6), 'months', 1, 24),
        'lead_days': parse_int(params['lead_days'], 'lead_days', 0, 365) if 'lead_days' in params else None,
        'history_months': (parse_int(params['history_months'], 'history_months', 1, 36)
                           if 'history_months' in params else None),
    }


@api_view(['POST'])
@handle_errors
def stock_forecast_summary(request):
    """Previsions de stock en lecture seule pour le front web, reservees aux roles gestion et admin (badge)."""
    badge_user(request, (Role.GESTION, Role.ADMIN))
    return Response(forecast.forecast(**forecast_params(request.data)))


@api_view(['POST'])
@handle_errors
def tasks_summary(request):
    """Liste de taches de la gestion (« Gestion de la TECH » du front web), roles gestion et admin (badge)."""
    badge_user(request, GESTION)
    return Response(tasks.todo())


@api_view(['POST'])
@handle_errors
def orders_create(request):
    """Case « Commandé » d'une commande suggeree (taches du TECH, web ou poste) : {"user", "type", "quantity"}. La
    suggestion ne revient plus tant que la commande n'est pas livree (receptions de ce type) ou decochee."""
    connected_user(request, GESTION)
    identity = operation_identity(request)
    item_type = get_object_or_404(ItemType, type=str(request.data.get('type', '')))
    quantity = parse_int(request.data.get('quantity'), 'quantity', 1, 100000)
    order = StockOrder.objects.create(item_type=item_type, quantity=quantity, placed_by=identity)
    journal.record(OperationKind.ORDER, identity, f'Commande passée : {quantity} × {item_type.name}',
                   type=item_type.type, quantity=quantity, order=order.id)
    return Response(tasks.todo(), status=status.HTTP_201_CREATED)


@api_view(['POST'])
@handle_errors
def orders_cancel(request, order_id):
    """Decoche une commande passee (erreur, ou elle ne viendra pas) : la suggestion revient si elle tient toujours."""
    connected_user(request, GESTION)
    identity = operation_identity(request)
    order = get_object_or_404(StockOrder.objects.select_related('item_type'), pk=order_id)
    if order.cancelled is None and order.delivered is None:
        order.cancelled = timezone.now()
        order.cancelled_by = identity
        order.save(update_fields=['cancelled', 'cancelled_by'])
        journal.record(OperationKind.ORDER, identity,
                       f'Commande décochée : {order.quantity} × {order.item_type.name}', type=order.item_type_id,
                       quantity=order.quantity, order=order.id, cancelled=True)
    return Response(tasks.todo())


@api_view(['POST'])
@handle_errors
def debug_codes_list(request):
    """Outil de debug des codes du front web (cache derriere 5 appuis sur « État des stocks ») : contenu de toutes
    les etiquettes imprimables, sauf les badges. Administrateurs seulement, verifie ici et pas seulement par le front."""
    connected_user(request, ADMIN)
    return Response(debug_codes.catalog())


@api_view(['POST'])
@handle_errors
def fun_stats_view(request):
    """Stats fun de l'annee (front web, cache derriere 5 appuis sur « Prévisions ») : {"user", "period"?: "last12" ou
    une annee}. Roles gestion et admin, verifie ici et pas seulement par le front."""
    connected_user(request, GESTION)
    value = request.data.get('period') or 'last12'
    if value != 'last12':
        value = parse_int(value, 'period', 2000, 2100)
    return Response(fun_stats.compute(value))


@api_view(['POST'])
@handle_errors
def debug_temp_login(request):
    """QR code de connexion de depannage d'un secouriste (outil de debug, admins) : {"user", "matricule"} -> lien
    badge?m=...&t=... valable 1 h, qui ouvre une session de 24 h sur le telephone qui le scanne. Jamais pour un
    admin (son PIN est obligatoire). Chaque QR code cree est note au journal."""
    admin = connected_user(request, ADMIN)
    user = Secouristes.objects.filter(matricule=str(request.data.get('matricule', ''))[:16]).first()
    if user is None or user.role == Role.ADMIN or not user.badge_valid():
        raise ApiError("Pas de connexion de dépannage pour ce compte (administrateur, désactivé ou badge expiré)")
    if user.pin_blocked is not None:
        raise ApiError("Le code PIN de ce secouriste est bloqué : réinitialisez-le d'abord")
    journal.record(OperationKind.USER, f'{SOURCE_VERIFIED}:{admin.matricule}',
                   f'QR code de connexion temporaire de {user} ({user.matricule}) créé par {admin}',
                   matricule=user.matricule)
    return Response({
        'matricule': user.matricule,
        'name': short_name(user.matricule),
        'url': ser.public_url('badge', m=user.matricule, t=temp_login_token(user)),
        'expires': (timezone.now() + timedelta(seconds=TEMP_LOGIN_MAX_AGE)).isoformat(),
    })


@api_view(['POST'])
@handle_errors
def debug_guest_cards(request):
    """Cartes d'invites depuis l'outil de debug du web (admins) : {"user", "action"?}.
    - sans action (ou "list") : toutes les cartes, avec l'admin qui a cree chacune ;
    - "create" : nouvelle carte montree a l'ecran (lien badge?m=...&key=... en QR code), a activer dans les 24 h,
      puis valable la duree des cartes d'invites (48 h par defaut) ;
    - "disable" / "enable" {"matricule"} : desactive une carte (activee ou non, sessions coupees) ou la reactive.
    Chaque carte creee ou modifiee est notee au journal."""
    admin = connected_user(request, ADMIN)
    action = str(request.data.get('action') or 'list')
    by = f'{SOURCE_VERIFIED}:{admin.matricule}'
    if action == 'create':
        card = guests.create_guest_card(admin, activate_within_hours=guests.WEB_ACTIVATION_HOURS)
        journal.record(OperationKind.USER, by,
                       f"carte d'invité {card.matricule} créée par {admin} depuis le web : à activer dans les "
                       f"{guests.WEB_ACTIVATION_HOURS} h, valable {card.guest_hours} h ensuite",
                       matricule=card.matricule)
        row = guests.card_row(card, public=True)
        row['url'] = ser.public_url('badge', m=card.matricule, key=card.new_key)
        return Response({'card': row, 'cards': guests.cards(public=True)})
    if action in ('disable', 'enable', 'reuse', 'reactivate'):
        card = Secouristes.objects.filter(matricule=str(request.data.get('matricule', ''))[:16], guest=True).first()
        if card is None:
            raise ApiError("Carte d'invité inconnue", status.HTTP_404_NOT_FOUND)
        if action in ('disable', 'enable'):
            card.active = action == 'enable'
            card.save(update_fields=['active'])
            journal.record(OperationKind.USER, by, f"carte d'invité {card.matricule} ({card}) "
                           + ('remise en service' if card.active else 'désactivée') + f' par {admin}',
                           matricule=card.matricule)
        else:  # nouveau QR code a l'ecran : numero repris (24 h pour l'activer) ou invite qui revient
            guest_card_action(card, action, request.data, admin, by, web=True)
            row = guests.card_row(card, public=True)
            row['url'] = ser.public_url('badge', m=card.matricule, key=card.new_key)
            return Response({'card': row, 'cards': guests.cards(public=True)})
    elif action != 'list':
        raise ApiError('action : list, create, disable, enable, reuse ou reactivate')
    return Response({'cards': guests.cards(public=True)})


@api_view(['GET'])
@handle_errors
def tasks_list(request):
    """Liste de taches de la gestion pour le poste (fenetre Gestion de la TECH), roles gestion et admin."""
    require_front(request, GESTION)
    return Response(tasks.todo(consumables_tasks=True))


@api_view(['POST'])
@handle_errors
def lot_disinfection(request, lot_id):
    """Desinfection d'un lot lui-meme (cellule du VPS...), roles gestion et admin, web ou poste : {"user", "months"
    (periodicite, 0 : aucune), "done": true (faite maintenant, "next_due" facultatif), ou "last_done" / "next_due"
    (dates saisies a la main), "note"}. Repond la desinfection du lot (serializers.lot_disinfection)."""
    connected_user(request, GESTION)
    identity = operation_identity(request)
    lot = get_object_or_404(Lots, id=lot_id)
    data = request.data
    months = parse_int(data['months'], 'months', 0, 120) if data.get('months') not in (None, '') else None
    next_due = parse_date(data['next_due'], 'next_due') if data.get('next_due') else None
    last_done = parse_date(data['last_done'], 'last_done') if data.get('last_done') else None
    try:
        services.disinfect_lot(lot, identity, months, bool(data.get('done')), next_due, last_done, data.get('note', ''))
    except ValueError as exc:
        raise ApiError(str(exc))
    return Response({'id': lot.id, 'name': lot.name, 'disinfection': ser.lot_disinfection(lot),
                     'disinfection_months': lot.disinfection_months})


@api_view(['GET'])
@handle_errors
def maintenance_overview(request):
    """Onglet Materiel de l'Inventaire du poste (gestion, admin) : materiel non consommable suivi (revision,
    desinfection) et lots a desinfecter."""
    require_front(request, GESTION)
    today = timezone.localdate()
    items, lots = services.maintenance_overview(today)
    return Response({'items': [ser.item_dict(item, today) for item in items],
                     'lots': [{'id': lot.id, 'name': lot.name, 'lot_type_name': lot.lot_type.name,
                               'disinfection': ser.lot_disinfection(lot, today)}
                              for lot in Lots.objects.select_related('lot_type').filter(id__in=[l.id for l in lots])
                              .order_by('disinfection_due', 'name')]})


def _events_days(value):
    return parse_int(value if value not in (None, '') else tasks.EVENTS_DAYS, 'days', 1, 400)


@api_view(['POST'])
@handle_errors
def events_summary(request):
    """Evenements a venir de la gestion (« Gestion de la TECH » du front web, onglet À venir), roles gestion et admin
    (badge) : {"user", "days": horizon en jours}."""
    badge_user(request, GESTION)
    return Response(tasks.events(days=_events_days(request.data.get('days'))))


@api_view(['GET'])
@handle_errors
def events_list(request):
    """Evenements a venir pour le poste (fenetre Gestion de la TECH, onglet À venir), roles gestion et admin."""
    require_front(request, GESTION)
    return Response(tasks.events(days=_events_days(request.query_params.get('days'))))


# Notifications web (roles gestion et admin, depuis le front web)

def push_available():
    if not webpush.available():
        raise ApiError("Notifications web indisponibles sur ce serveur (module python3-cryptography absent)",
                       status.HTTP_503_SERVICE_UNAVAILABLE)


def push_types_list(user):
    """Types de notifications web que l'utilisateur peut choisir, dans l'ordre d'affichage."""
    allowed = user.push_types()
    return [{'type': name, 'label': label} for name, (label, _, _) in PUSH_TYPES.items() if name in allowed]


def push_options(subscription):
    return {name: getattr(subscription, name) for name in PUSH_TYPES}


def push_subscription_dict(subscription, user):
    data = {'subscribed': subscription is not None, 'types': push_types_list(user)}
    if subscription is None:
        data.update({name: default for name, (_, _, default) in PUSH_TYPES.items()})
        return data
    return {
        **data,
        **push_options(subscription),
        'last_sent': subscription.last_sent,
        'last_status': subscription.last_status,
    }


def push_device_dict(subscription, endpoint=''):
    return {
        'id': subscription.id,
        'device': subscription.device or 'Appareil',
        'current': bool(endpoint) and subscription.endpoint == endpoint,
        'created': ser._date(subscription.created),
        'last_sent': ser._date(subscription.last_sent),
        'last_status': subscription.last_status,
        **push_options(subscription),
    }


def push_devices_dict(user, endpoint=''):
    devices = user.push_subscriptions.order_by('-created')
    return {'types': push_types_list(user), 'devices': [push_device_dict(device, endpoint) for device in devices]}


def apply_push_options(subscription, data):
    for name in PUSH_TYPES:
        if name in data:
            setattr(subscription, name, bool(data[name]))


@api_view(['GET'])
@handle_errors
def push_key(request):
    """Cle publique VAPID (applicationServerKey) a donner au navigateur pour s'abonner."""
    push_available()
    return Response({'public_key': webpush.public_key()})


@api_view(['POST'])
@handle_errors
def push_subscription(request):
    """Etat de l'abonnement de ce navigateur (endpoint), ou creation / mise a jour si `subscription` est donne.

    {"user": badge, "endpoint": ...} -> etat ; {"user": badge, "subscription": {endpoint, keys: {p256dh, auth}},
    types de PUSH_TYPES: bool} -> abonnement enregistre. Roles gestion et admin ; `types` liste ce que
    l'utilisateur peut recevoir (son role, moins ce qu'un admin a coupe).
    """
    user = badge_user(request, GESTION)
    push_available()
    data = request.data
    subscription_data = data.get('subscription')
    if subscription_data is None:
        endpoint = str(data.get('endpoint', ''))
        return Response(push_subscription_dict(
            PushSubscription.objects.filter(endpoint=endpoint, user=user).first(), user))
    if not isinstance(subscription_data, dict) or not isinstance(subscription_data.get('keys'), dict):
        raise ApiError("Abonnement invalide")
    endpoint = str(subscription_data.get('endpoint', ''))
    keys = subscription_data['keys']
    if not endpoint.startswith('https://') or len(endpoint) > 1024:
        raise ApiError("Adresse de notification invalide")
    if not webpush.valid_subscription_keys(keys.get('p256dh', ''), keys.get('auth', '')):
        raise ApiError("Clés d'abonnement invalides")
    subscription, _ = PushSubscription.objects.update_or_create(endpoint=endpoint, defaults={
        'user': user,
        'p256dh': str(keys['p256dh']),
        'auth': str(keys['auth']),
        'device': webpush.device_name(request.META.get('HTTP_USER_AGENT')),
        **{name: bool(data.get(name, default)) for name, (_, _, default) in PUSH_TYPES.items()},
    })
    return Response(push_subscription_dict(subscription, user))


@api_view(['POST'])
@handle_errors
def push_devices(request):
    """Gestionnaire des notifications : les appareils abonnes de l'utilisateur connecte.

    {"user": badge, "endpoint": celui de ce navigateur (facultatif, pour le reperer)} -> {types, devices}.
    """
    user = badge_user(request, GESTION)
    return Response(push_devices_dict(user, str(request.data.get('endpoint', ''))))


@api_view(['POST'])
@handle_errors
def push_device(request, device_id):
    """Un appareil de l'utilisateur connecte : {"user": badge, types: bool} change ses alertes,
    {"user": badge, "delete": true} le desabonne. Repond la liste a jour, comme push/devices/."""
    user = badge_user(request, GESTION)
    subscription = get_object_or_404(PushSubscription, id=device_id, user=user)
    if request.data.get('delete'):
        subscription.delete()
    else:
        apply_push_options(subscription, request.data)
        subscription.save()
    return Response(push_devices_dict(user, str(request.data.get('endpoint', ''))))


@api_view(['POST'])
@handle_errors
def push_unsubscribe(request):
    """Desabonnement de ce navigateur : l'endpoint suffit (il n'est connu que du navigateur et du serveur)."""
    deleted, _ = PushSubscription.objects.filter(endpoint=str(request.data.get('endpoint', ''))).delete()
    return Response({'subscribed': False, 'deleted': bool(deleted)})


@api_view(['POST'])
@handle_errors
def push_test(request):
    """Notification de test vers ce navigateur."""
    user = badge_user(request, GESTION)
    push_available()
    subscription = PushSubscription.objects.filter(endpoint=str(request.data.get('endpoint', '')), user=user).first()
    if subscription is None:
        raise ApiError("Notifications non activées sur ce navigateur", status.HTTP_404_NOT_FOUND)
    webpush.queue([(subscription.id, {
        'title': 'QRProtec : test',
        'body': 'Les notifications fonctionnent sur cet appareil.',
        'tag': 'test',
        'url': '../',
    })])
    return Response({'queued': True})


# Signalements de problemes (voir reports.py) : utilisateur connecte, sur le front web (badge ou session) comme
# sur un poste (session du poste)

def connected_user(request, roles=None):
    if is_front(request):
        return require_front(request, roles or ALL_ROLES)
    return badge_user(request, roles)


@api_view(['POST'])
@handle_errors
def reports_create(request):
    """Nouveau signalement : {"user", "lot"?, "item"?, "type"?, "pack"? (paquet ferme), "comment", "usable"}. Envoye aux admins, independant
    de toute verif. Repond le signalement et l'item a jour (signale inutilisable : a remplacer)."""
    user = connected_user(request)
    data = request.data
    if 'usable' not in data:
        raise ApiError("Indiquez si l'objet est toujours utilisable")
    report, item = reports.create(
        f'{SOURCE_VERIFIED}:{user.matricule}', lot_id=str(data.get('lot') or ''), iid=str(data.get('item') or ''),
        type_code=str(data.get('type') or ''), comment=data.get('comment', ''), usable=bool(data['usable']),
        pack_id=str(data.get('pack') or ''),
    )
    response = {'report': reports.report_dict(report)}
    if item is not None:
        item.refresh_from_db()
        response['item'] = ser.item_dict(item)
    return Response(response, status=status.HTTP_201_CREATED)


@api_view(['POST'])
@handle_errors
def reports_list(request):
    """Signalements en lecture seule pour tout utilisateur connecte : {"user", "show": "open" | "all"}.
    `can_manage` : l'utilisateur est admin (prise en charge, statut, reponse)."""
    user = connected_user(request)
    show = 'all' if request.data.get('show') == 'all' else 'open'
    return Response({**reports.listing(show), 'can_manage': user.role == Role.ADMIN})


@api_view(['POST'])
@handle_errors
def report_update(request, report_id):
    """Prise en charge par un admin : {"user", "status": new|progress|resolved, "usable": bool, "note": reponse en
    cours (remplacee), "resolution": ok|disabled|removed (issue pour l'item, en resolvant), "resolution_note": reponse
    de la resolution}."""
    user = connected_user(request, ADMIN)
    report = get_object_or_404(ProblemReport.objects.select_related('item_type', 'item', 'lot'), id=report_id)
    data = request.data
    reports.update(report, f'{SOURCE_VERIFIED}:{user.matricule}', status=data.get('status'),
                   usable=data.get('usable'), note=data.get('note'), resolution=data.get('resolution'),
                   resolution_note=data.get('resolution_note'))
    return Response(reports.report_dict(report))


@api_view(['GET'])
@handle_errors
def sealed_pack_detail(request, pack_id):
    sealed_pack = get_object_or_404(SealedPacks.objects.select_related('item_type'), id=pack_id)
    return Response(ser.sealed_pack_dict(sealed_pack))
