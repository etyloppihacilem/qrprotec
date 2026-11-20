# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          views/notif_settings.py
#       -\-    _|__
#        |\___/  . \        Created on 16 Nov. 2026 at 19:45
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Reglages des notifications et du serveur (delai d'alerte, reglages des utilisateurs)."""

from .common import *  # noqa: F401,F403
from .public import *  # noqa: F401,F403
from .local import *  # noqa: F401,F403


# Reglages des notifications (le delai d'alerte d'expiration ; l'API SMS n'est plus maintenue, voir notifications.py)
# ----------------------------------------------------------------------------------------------------------------------

def _notification_response():
    return Response(ser.notification_settings_dict(NotificationSettings.get()))


@api_view(['GET', 'PATCH'])
@handle_errors
def notification_settings(request):
    require_front(request, ADMIN)
    if request.method == 'PATCH':
        settings_row = NotificationSettings.get()
        if 'key_expiry_warning_days' in request.data:
            settings_row.key_expiry_warning_days = parse_warning_days(request.data['key_expiry_warning_days'])
        settings_row.save()
        journal.record(OperationKind.SETTINGS, front_identity(request), 'réglages des notifications modifiés',
                       fields=sorted(request.data.keys()))
    return _notification_response()


@api_view(['GET', 'PATCH'])
@handle_errors
def server_settings(request):
    """Reglages du serveur communs a tous les fronts (admin)."""
    require_front(request, ADMIN)
    settings_row = ServerSettings.get()
    if request.method == 'PATCH':
        changes = []
        if 'declared_identity' in request.data:
            settings_row.declared_identity = bool(request.data['declared_identity'])
            changes.append('identité déclarée (nom sans badge) '
                           + ('autorisée' if settings_row.declared_identity else 'refusée'))
        roles = None
        if 'pin_required_roles' in request.data:
            roles = parse_pin_roles(request.data['pin_required_roles'])
        elif 'pin_required_default' in request.data:  # ancien poste : une case pour tous les roles
            roles = list(PIN_OPTIONAL_ROLES) if request.data['pin_required_default'] else []
        if roles is not None:
            settings_row.pin_required_roles = roles
            labels = [Role(role).label.lower() for role in roles]
            changes.append('code PIN obligatoire pour les administrateurs'
                           + (' et les rôles ' + ', '.join(labels) if labels else ' seulement'))
        if 'pin_login_roles' in request.data:
            settings_row.pin_login_roles = parse_pin_roles(request.data['pin_login_roles'], 'Code à la connexion obligatoire')
            labels = [Role(role).label.lower() for role in settings_row.pin_login_roles]
            changes.append('code PIN à la connexion obligatoire pour les administrateurs'
                           + (' et les rôles ' + ', '.join(labels) if labels else ' seulement'))
        if 'scanner_volume_default' in request.data:
            settings_row.scanner_volume_default = parse_scanner_volume(request.data['scanner_volume_default'])
            changes.append(f'volume de la douchette par défaut : {settings_row.scanner_volume_default}')
        if 'web_barcodes' in request.data:
            settings_row.web_barcodes = bool(request.data['web_barcodes'])
            changes.append('lecture des codes-barres sur le web ' + ('activée' if settings_row.web_barcodes else 'désactivée'))
        if 'departure_full_verif' in request.data:
            settings_row.departure_full_verif = bool(request.data['departure_full_verif'])
            changes.append('vérif complète à chaque départ ' + ('demandée' if settings_row.departure_full_verif
                                                                else 'non demandée'))
        if 'restock_verif_recommended' in request.data:
            settings_row.restock_verif_recommended = bool(request.data['restock_verif_recommended'])
            changes.append('réassort : lot ' + ('en vérif recommandée' if settings_row.restock_verif_recommended
                                                else 'au vert sous le crédit de confiance'))
        for field, label, top in (('restock_credit_outings', 'sorties', 50), ('restock_credit_days', 'jours', 365)):
            if field in request.data:
                try:
                    value = int(request.data[field])
                except (TypeError, ValueError):
                    return Response({'error': f'Crédit de confiance : nombre de {label} invalide'}, status=400)
                if not 0 <= value <= top:
                    return Response({'error': f'Crédit de confiance : de 0 à {top} {label}'}, status=400)
                setattr(settings_row, field, value)
                changes.append(f'crédit de confiance du réassort : {value} {label}')
        if 'storage_verif_outings' in request.data:
            try:
                value = int(request.data['storage_verif_outings'])
            except (TypeError, ValueError):
                return Response({'error': 'Vérif des rangements : nombre de sorties invalide'}, status=400)
            if not 1 <= value <= 100:
                return Response({'error': 'Vérif des rangements : de 1 à 100 sorties'}, status=400)
            settings_row.storage_verif_outings = value
            changes.append(f'vérif d\'un rangement du stock proposée toutes les {value} sorties')
        if 'guest_hours' in request.data:
            try:
                settings_row.guest_hours = guests.parse_guest_hours(request.data['guest_hours'])
            except ValueError as exc:
                return Response({'error': str(exc)}, status=400)
            changes.append(f"cartes d'invités valables {settings_row.guest_hours} h après leur première connexion")
        if 'antenna_name' in request.data:
            settings_row.antenna_name = ' '.join(str(request.data['antenna_name'] or '').split())[:64]
            changes.append(f"nom de l'antenne : {settings_row.antenna_name or '(vide)'}")
        settings_row.save()
        journal.record(OperationKind.SETTINGS, front_identity(request), ', '.join(changes) or 'aucun changement',
                       declared_identity=settings_row.declared_identity,
                       pin_required_roles=settings_row.pin_required_roles,
                       pin_login_roles=settings_row.pin_login_roles,
                       scanner_volume_default=settings_row.scanner_volume_default,
                       web_barcodes=settings_row.web_barcodes,
                       departure_full_verif=settings_row.departure_full_verif,
                       restock_verif_recommended=settings_row.restock_verif_recommended,
                       restock_credit_outings=settings_row.restock_credit_outings,
                       restock_credit_days=settings_row.restock_credit_days,
                       storage_verif_outings=settings_row.storage_verif_outings)
    return Response(ser.server_settings_dict(settings_row))


@api_view(['GET', 'PATCH'])
@handle_errors
def lost_object_message(request):
    """Message « objet perdu », le meme pour tous les types d'items coches lost_object : {"message"}. Lecture par
    tous (il s'affiche sur la fiche publique de ces objets), modification par la gestion (comme les types d'items)."""
    if request.method == 'PATCH':
        require_front(request, GESTION)
        settings_row = ServerSettings.get()
        message = str(request.data.get('message') or '').strip()
        if len(message) > LOST_OBJECT_MESSAGE_MAX:
            raise ApiError(f'Message objet perdu : {LOST_OBJECT_MESSAGE_MAX} caractères au plus')
        settings_row.lost_object_message = message
        settings_row.save(update_fields=['lost_object_message'])
        journal.record(OperationKind.SETTINGS, front_identity(request), 'message objet perdu modifié')
    return Response({'message': ServerSettings.get().lost_object_message})


def parse_pin_roles(value, name='Code obligatoire'):
    """Roles du reglage « code obligatoire » ou « code a la connexion obligatoire » (les admins le sont toujours),
    dans l'ordre des roles."""
    if not isinstance(value, list) or any(role not in PIN_OPTIONAL_ROLES + (Role.ADMIN,) for role in value):
        raise ApiError(name + ' : liste de rôles parmi ' + ', '.join(PIN_OPTIONAL_ROLES))
    return [role for role in PIN_OPTIONAL_ROLES if role in value]


@api_view(['GET', 'POST'])
@handle_errors
def print_counts_view(request):
    """Compteur d'impressions (etiquettes envoyees a l'imprimante, par categorie d'etiquette).

    POST, tout front meme sans personne de connecte : {"batch": "<id unique de l'envoi>", "counts": [{"day":
    "AAAA-MM-JJ", "width_mm", "height_mm", "support": "direct" | "transfer", "count"}]}. Un envoi deja recu (reponse
    perdue, renvoye) n'est pas recompte : {"counted": 0, "duplicate": true}.
    GET ?year=AAAA, roles gestion et admin : tableau des 12 mois par categorie et total de l'annee."""
    if request.method == 'POST':
        if not is_front(request):
            raise ApiError("Réservé aux postes", status.HTTP_403_FORBIDDEN)
        try:
            counted, duplicate = print_counts.record(request.data.get('batch'), getattr(request, 'qrprotec_front', ''),
                                                     request.data.get('counts'))
        except print_counts.CountError as exc:
            raise ApiError(str(exc))
        return Response({'counted': counted, 'duplicate': duplicate},
                        status=status.HTTP_200_OK if duplicate else status.HTTP_201_CREATED)
    require_front(request, GESTION)
    year = request.query_params.get('year')
    return Response(print_counts.summary(parse_int(year, 'year', 2000, 2200) if year else None))


def _consumables_call(function, *args):
    try:
        return function(*args)
    except consumables.ConsumableError as exc:
        raise ApiError(str(exc))


@api_view(['GET', 'PATCH'])
@handle_errors
def consumables_view(request):
    """Consommables d'etiquettes (fronts, roles gestion et admin) : stock estime par categorie, references, derniers
    mouvements. PATCH {"alert_days"} : seuil de la tache « Commander »."""
    require_front(request, GESTION)
    if request.method == 'PATCH':
        _consumables_call(consumables.set_alert_days, request.data.get('alert_days'))
    return Response(consumables.summary())


@api_view(['POST'])
@handle_errors
def consumable_refs(request):
    """Nouvelle reference : {"kind": "labels" | "ribbon", "name", "supplier", "width_mm", "height_mm", "support",
    "per_roll" | "length_m", "per_box", "box_price"}."""
    require_front(request, GESTION)
    _consumables_call(consumables.save_ref, request.data)
    return Response(consumables.summary(), status=status.HTTP_201_CREATED)


@api_view(['PATCH'])
@handle_errors
def consumable_ref_detail(request, ref_id):
    """Modifier une reference (memes champs, sauf le type), ou l'archiver ({"archived": true})."""
    require_front(request, GESTION)
    ref = get_object_or_404(ConsumableRef, id=ref_id)
    _consumables_call(consumables.save_ref, request.data, ref)
    return Response(consumables.summary())


@api_view(['POST'])
@handle_errors
def consumable_moves(request):
    """Reception ou inventaire : {"ref", "kind": "reception" | "inventory", "boxes", "rolls", "loose"}."""
    require_front(request, GESTION)
    _consumables_call(consumables.add_move, request.data, front_identity(request))
    return Response(consumables.summary(), status=status.HTTP_201_CREATED)


@api_view(['POST'])
@handle_errors
def consumable_move_delete(request, move_id):
    """Annuler une reception ou un inventaire saisi par erreur."""
    require_front(request, GESTION)
    _consumables_call(consumables.delete_move, move_id)
    return Response(consumables.summary())


@api_view(['POST'])
@handle_errors
def barcodes_printed(request):
    """Le poste imprime une etiquette a code-barres : des codes-barres circulent, le front web charge desormais leur
    decodeur (reglage web_barcodes, desactivable par un admin quand il n'y en a plus). Tout poste, meme sans
    utilisateur connecte."""
    if not is_front(request):
        raise ApiError("Réservé aux postes", status.HTTP_403_FORBIDDEN)
    settings_row = ServerSettings.get()
    if not settings_row.web_barcodes:
        settings_row.web_barcodes = True
        settings_row.save(update_fields=['web_barcodes'])
        journal.record(OperationKind.SETTINGS, front_identity(request),
                       'lecture des codes-barres sur le web activée (première étiquette à code-barres imprimée)',
                       web_barcodes=True)
    return Response({'web_barcodes': True})




@api_view(['PATCH'])
@handle_errors
def my_scanner_volume(request):
    """Volume de la douchette de l'utilisateur connecte sur le poste (tous les roles), repris a sa connexion.
    `volume` vide : revient au volume par defaut du serveur."""
    user = require_front(request, ALL_ROLES)
    value = request.data.get('volume')
    user.scanner_volume = None if value in (None, '') else parse_scanner_volume(value)
    user.save(update_fields=['scanner_volume'])
    return Response({'scanner_volume': user.effective_scanner_volume, 'custom': user.scanner_volume is not None})




@api_view(['POST'])
@handle_errors
def my_prefs(request):
    """Reglages du front web de l'utilisateur connecte (tous les roles) : {"user", "web_prefs": {nom: booleen}}."""
    user = badge_user(request, ALL_ROLES)  # badge ou jeton de session
    user.web_prefs = {**(user.web_prefs or {}), **parse_web_prefs(request.data.get('web_prefs'))}
    user.save(update_fields=['web_prefs'])
    return Response(ser.user_dict(user))


PIN_SAVE_FIELDS = ['pin_hash', 'pin_mode', 'pin_at_login', 'pin_failures', 'pin_failures_total', 'pin_locked_until', 'pin_blocked',
                   'pin_reset_required', 'pin_forgotten', 'pin_reset_notified']


@api_view(['POST'])
@handle_errors
def my_pin(request):
    """Code PIN de l'utilisateur connecte, depuis ses reglages (web) : {"user", "current_pin", "pin"} le choisit ou le
    change, {"user", "current_pin", "disable": true} le supprime (refuse pour un admin, ou si un admin l'a rendu
    obligatoire). Le PIN actuel est redemande (essais faux comptes comme a la connexion). La session reste ouverte.
    `pin_at_login` (case « Demander a chaque connexion ») accompagne le PIN choisi, ou se change seul, sans PIN."""
    user = badge_user(request, ALL_ROLES)  # badge ou jeton de session, PIN deja verifie s'il est demande
    data = request.data
    if user.guest:
        raise ApiError("Pas de code PIN pour une carte d'invité", status.HTTP_403_FORBIDDEN)
    if 'pin_at_login' in data and not data.get('pin') and not data.get('disable'):
        if not user.has_pin:
            raise ApiError("Vous n'avez pas de code PIN")
        if user.pin_at_login_locked and not data['pin_at_login']:
            raise ApiError("Le PIN est demandé à chaque connexion pour un administrateur" if user.role == Role.ADMIN
                           else "Le TECH a rendu le code obligatoire à chaque connexion", status.HTTP_403_FORBIDDEN)
        user.set_pin_at_login(data['pin_at_login'])
        user.save(update_fields=['pin_at_login'])
        journal.record(OperationKind.USER, f'{SOURCE_VERIFIED}:{user.matricule}',
                       f'code PIN {"demandé" if user.pin_at_login else "plus demandé"} à chaque connexion pour '
                       f'{user} ({user.matricule}) dans ses réglages', matricule=user.matricule, fields=['pin_at_login'])
        return Response(ser.user_dict(user))
    if user.pin_blocked:
        raise pin_blocked_error(user)
    if user.pin_locked():
        raise ApiError("Trop d'essais : PIN bloqué quelques minutes", status.HTTP_403_FORBIDDEN, pin_locked=True)
    disable = bool(data.get('disable'))
    if disable and not user.pin_can_disable:
        raise ApiError("Le PIN est obligatoire pour un administrateur" if user.role == Role.ADMIN
                       else "Le TECH a rendu le code PIN obligatoire pour vous", status.HTTP_403_FORBIDDEN)
    if user.has_pin and not user.check_pin(str(data.get('current_pin', '') or '')):
        if user.pin_blocked:
            raise pin_blocked_error(user)
        raise ApiError("PIN bloqué 5 min après trop d'essais" if user.pin_locked() else "Code PIN actuel incorrect",
                       status.HTTP_403_FORBIDDEN, pin_locked=user.pin_locked())
    if disable:
        if not user.has_pin:
            raise ApiError("Vous n'avez pas de code PIN")
        user.pin_hash = ''  # mode du PIN inchange : il redevient facultatif, comme avant d'en choisir un
        user._clear_pin_failures()
        summary = 'code PIN supprimé'
    else:
        pin = str(data.get('pin', '') or '').strip()
        if not NEW_PIN_RE.match(pin):
            raise ApiError(NEW_PIN_ERROR)
        summary = 'code PIN modifié' if user.has_pin else 'code PIN choisi'
        user.set_pin(pin)
        if 'pin_at_login' in data:
            user.set_pin_at_login(data['pin_at_login'])
    user.save(update_fields=PIN_SAVE_FIELDS)
    journal.record(OperationKind.USER, f'{SOURCE_VERIFIED}:{user.matricule}',
                   f'{summary} par {user} ({user.matricule}) dans ses réglages', matricule=user.matricule, fields=['pin'])
    return Response(ser.user_dict(user))


def parse_warning_days(value):
    """Delai de l'alerte « expire bientot » (1 a 365 jours), None : valeur de qrprotec.conf."""
    if value in (None, ''):
        return None
    try:
        days = int(value)
    except (TypeError, ValueError):
        days = 0
    if not 1 <= days <= 365:
        raise ApiError("Délai d'alerte d'expiration : entre 1 et 365 jours")
    return days


# Destinataires et SMS de test : API SMS plus maintenue (voir notifications.py), garde en commentaire.
# def _recipient_fields(recipient, data, creating):
#     for field, length in (('name', 64), ('user', 32), ('password', 64)):
#         if field in data:
#             value = str(data[field] or '').strip()[:length]
#             if field == 'password' and not value and not creating:
#                 continue  # champ vide : cle inchangee
#             setattr(recipient, field, value)
#     if 'active' in data:
#         recipient.active = bool(data['active'])
#     if not recipient.name or not recipient.user or not recipient.password:
#         raise ApiError('Nom, identifiant et clé API obligatoires')
#
#
# @api_view(['POST'])
# @handle_errors
# def sms_recipients(request):
#     require_front(request, ADMIN)
#     recipient = SmsRecipient()
#     _recipient_fields(recipient, request.data, True)
#     recipient.save()
#     journal.record(OperationKind.SETTINGS, front_identity(request), f'destinataire SMS ajouté : {recipient.name}')
#     return _notification_response()
#
#
# @api_view(['PATCH', 'DELETE'])
# @handle_errors
# def sms_recipient_detail(request, recipient_id):
#     require_front(request, ADMIN)
#     recipient = get_object_or_404(SmsRecipient, id=recipient_id)
#     if request.method == 'DELETE':
#         recipient.delete()
#         journal.record(OperationKind.SETTINGS, front_identity(request), f'destinataire SMS supprimé : {recipient.name}')
#     else:
#         _recipient_fields(recipient, request.data, False)
#         recipient.save()
#         journal.record(OperationKind.SETTINGS, front_identity(request), f'destinataire SMS modifié : {recipient.name}')
#     return _notification_response()
#
#
# @api_view(['POST'])
# @handle_errors
# def sms_test(request):
#     """Envoie un SMS de test (a un destinataire, ou a tous les actifs), meme si les notifications sont coupees."""
#     require_front(request, ADMIN)
#     recipient_id = request.data.get('recipient')
#     if recipient_id:
#         recipients = [get_object_or_404(SmsRecipient, id=recipient_id)]
#     else:
#         recipients = list(SmsRecipient.objects.filter(active=True))
#     if not recipients:
#         raise ApiError('Aucun destinataire actif')
#     identity = operation_identity(request)
#     count = notifications.send(f'QRProtec : SMS de test envoyé par {services.display_name(identity)}.', recipients)
#     return Response({'sent': count})
