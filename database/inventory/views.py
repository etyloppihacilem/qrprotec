# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          views.py
#       -\-    _|__
#        |\___/  . \        Created on 29 Sep. 2026 at 14:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""API JSON.

Deux familles de vues :
- publiques (servies sur les deux ports) : toute ecriture exige la cle de l'objet modifie
  (ex: cle du lot pour une verif), sauf si la requete arrive par l'API locale ;
- locales (servies uniquement sur le port local) : gestion complete sans cle.
"""

import functools
from collections import Counter, defaultdict
import hmac
import re
from datetime import date, datetime, timedelta

from django.contrib.auth.hashers import check_password, make_password
from django.core import signing
from django.core.cache import cache
from django.db import IntegrityError, transaction
from django.db.models import Count, Max, Min, Q
from django.http import Http404, HttpResponse, HttpResponseNotAllowed
from django.shortcuts import get_object_or_404
from django.utils import timezone
from rest_framework import status
from rest_framework.decorators import api_view
from rest_framework.response import Response

from . import debug_codes
from . import forecast
from . import front_config
from . import journal
from . import label_prints
from . import lot_report
from . import lot_sheet
from . import live
from . import notifications
from . import reports
from . import serializers as ser
from . import services
from . import tasks
from . import webpush
from .idendity import SOURCE_VERIFIED, format_identity_verified, guest_identity, identity_from_request, identity_name, short_name
from .remote_scanner import hub as scanner_hub
from .sessions import (  # noqa: F401 (tests)
    TEMP_LOGIN_MAX_AGE, TEMP_SESSION_MAX_AGE, credentials_user, session_token, session_user, session_valid,
    temp_login_token, temp_login_user,
)
from .models import (
    DEFAULT_FONCTION, PIN_OPTIONAL_ROLES, PUSH_TYPES, WEB_PREFS, STOCK_LOT_TYPE, ProblemReport, TYPE_LENGTH, Items, ItemsPacks, ItemStatus, ItemType, KeyExpiry, LabelStyle, LotContent, LotRequirements, Lots, LotType,
    NotificationSettings, Operation, OperationKind, PIN_RE, PinMode, PushSubscription, SealedPacks, Role, Secouristes, SeenWhile,
    SCANNER_VOLUMES, ServerSettings, FrontConfigDownload, FrontKey, VerifItem, VerifResult, Verifs, add_months, name_key, qrprotec_setting,
)

CODE_RE = re.compile(r'^[A-Za-z0-9]{%d}$' % TYPE_LENGTH)
MATRICULE_RE = re.compile(r'^[A-Za-z0-9_-]{1,16}$')


class ApiError(Exception):
    def __init__(self, message, code=status.HTTP_400_BAD_REQUEST, **extra):
        super().__init__(message)
        self.message = message
        self.code = code
        self.extra = extra  # champs ajoutes a la reponse d'erreur (ex: pin_required)


GESTION = (Role.GESTION, Role.ADMIN)
ADMIN = (Role.ADMIN,)
ALL_ROLES = tuple(Role.values)
SESSION_HEADER = 'HTTP_X_QRPROTEC_SESSION'


def is_front(request) -> bool:
    """Requete d'un poste (API locale, ou API distante avec une cle de front valide)."""
    return getattr(request, 'qrprotec_local', False)


def front_user(request):
    """Utilisateur connecte sur le poste : le front envoie le jeton de session recu de /api/auth/ (badge, puis PIN
    si besoin) dans l'en-tete X-QRProtec-Session. Le serveur ne croit plus le matricule envoye par le poste."""
    if not is_front(request):
        return None
    if not hasattr(request, 'qrprotec_user'):
        request.qrprotec_user = session_user(request.META.get(SESSION_HEADER, ''))
    return request.qrprotec_user


def is_local(request) -> bool:
    """Poste avec un utilisateur connecte : pas de cle de lot exigee, identite verifiee par la session."""
    return front_user(request) is not None


def is_privileged(request) -> bool:
    user = front_user(request)
    return user is not None and user.role in GESTION


def require_front(request, roles):
    """Routes du poste (ex-privilege du localhost) : utilisateur connecte avec l'un des roles."""
    user = front_user(request)
    if user is None:
        raise ApiError("Scannez votre badge pour continuer", status.HTTP_403_FORBIDDEN, login_required=True)
    if user.role not in roles:
        raise ApiError("Réservé aux administrateurs" if tuple(roles) == ADMIN else "Réservé aux rôles gestion et admin",
                       status.HTTP_403_FORBIDDEN)
    return user


def debug_options(request) -> bool:
    """Options de debug du poste (reglage « Activer les options de débug ») : {"debug": true} n'est pris en compte
    que pour un admin connecte sur un poste. Elles levent les refus des deplacements d'items (items perimes,
    supprimes, remplaces... ranges dans un lot), pour construire des lots inacceptables en vrai et tester l'affichage.
    Demandees par quelqu'un d'autre qu'un admin au poste : refusees, pas ignorees."""
    if not request.data.get('debug'):
        return False
    user = front_user(request)
    if user is None or user.role != Role.ADMIN:
        raise ApiError("Options de débug réservées aux administrateurs, au poste", status.HTTP_403_FORBIDDEN)
    return True


def operation_identity(request, data=None):
    """Identite d'une operation : utilisateur verifie, ou nom declare si le reglage du serveur l'autorise."""
    allowed = ServerSettings.get().declared_identity
    identity = identity_from_request(request.data if data is None else data, front_user(request), allowed)
    if identity is None:
        raise ApiError("Scannez votre badge ou indiquez votre nom" if allowed else "Scannez votre badge pour continuer",
                       status.HTTP_403_FORBIDDEN, login_required=True, declared_identity=allowed)
    return identity


def team_identity(request, team):
    """Vérif à plusieurs : il faut au moins un secouriste connecte et authentifie parmi les participants. Un invite
    (sans compte, {"guest": true, "name": prenom facultatif}) peut l'envoyer, meme sans le reglage du nom declare :
    elle est enregistree au nom d'un secouriste connecte, l'invite parmi les participants."""
    own = identity_from_request(request.data, front_user(request), ServerSettings.get().declared_identity)
    if own is None and request.data.get('guest'):
        own = guest_identity(request.data.get('name'))
    verified = [value for value in ([own] if own else []) + team['identities'] if is_verified(value)]
    if not verified:
        raise ApiError("Il faut au moins un secouriste connecté pour valider une vérif à plusieurs : scannez votre "
                       "badge, ou attendez qu'un secouriste connecté vous rejoigne.", status.HTTP_403_FORBIDDEN,
                       login_required=True, declared_identity=ServerSettings.get().declared_identity)
    if own is None:
        raise ApiError("Scannez votre badge ou indiquez votre prénom", status.HTTP_403_FORBIDDEN, login_required=True,
                       declared_identity=ServerSettings.get().declared_identity)
    if not is_verified(own):  # enregistree au nom d'un secouriste connecte, l'invite parmi les participants
        team['identities'] = [own, *team['identities']]
        return verified[0]
    return own


def front_identity(request):
    """Identite stockee de l'utilisateur connecte sur le poste (operations de gestion du journal)."""
    user = front_user(request)
    return f'{SOURCE_VERIFIED}:{user.matricule}' if user else ''


def changed_fields(data, labels):
    """Libelles des champs modifies par une requete PATCH (journal), dans l'ordre de `labels`."""
    return ', '.join(label for field, label in labels.items() if field in data)


def error(message, code=status.HTTP_400_BAD_REQUEST, **extra):
    return Response({'error': message, **extra}, status=code)


def handle_errors(view):
    def wrapper(request, *args, **kwargs):
        try:
            return view(request, *args, **kwargs)
        except ApiError as exc:
            return error(exc.message, exc.code, **exc.extra)
        except ValueError as exc:
            return error(str(exc))
    wrapper.__name__ = view.__name__
    wrapper.__doc__ = view.__doc__
    return wrapper


def parse_date(value, field):
    if value in (None, ''):
        return None
    try:
        return date.fromisoformat(str(value))
    except ValueError:
        raise ApiError(f"{field} : date invalide (format AAAA-MM-JJ attendu)")


def parse_int(value, field, minimum=0, maximum=None):
    try:
        number = int(value)
    except (TypeError, ValueError):
        raise ApiError(f"{field} : entier attendu")
    if number < minimum or (maximum is not None and number > maximum):
        raise ApiError(f"{field} : valeur hors limites")
    return number


MAX_TASKS = 20
MAX_REVISION_MONTHS = 120
TASK_LENGTH = 120


def parse_label_style(value):
    if value not in LabelStyle.values:
        raise ApiError("label_style : " + ', '.join(LabelStyle.values) + " attendu")
    return value


def check_tear_off_tasks(tear_off, tasks):
    """Une etiquette a dechirer disparait a l'ouverture : elle ne peut pas porter de taches a cocher."""
    if tear_off and tasks:
        raise ApiError("Un type à étiquette à déchirer ne peut pas avoir de tâches à la vérif")


def check_physical(physical, perissable, tear_off, revision_months):
    """Un item a cocher (sans etiquette) n'a ni date de peremption, ni etiquette a dechirer, ni revision."""
    if not physical and (perissable or tear_off or revision_months):
        raise ApiError("Un item à cocher n'a pas d'étiquette : ni péremption, ni étiquette à déchirer, ni révision")


def parse_tasks(value):
    """Taches d'un type d'item (etiquette de taches) : liste de libelles, sans vides ni doublons."""
    if value is None:
        return []
    if not isinstance(value, list):
        raise ApiError("tasks : liste de tâches attendue")
    tasks = []
    for task in value:
        text = ' '.join(str(task).split())[:TASK_LENGTH]
        if text and text not in tasks:
            tasks.append(text)
    if len(tasks) > MAX_TASKS:
        raise ApiError(f"Au plus {MAX_TASKS} tâches par type d'item")
    return tasks


def ensure_name_free(queryset, name, what):
    """Refuse un nom deja pris dans `queryset` (sans accents ni casse, voir name_key). Les fronts font la meme
    verification pendant la saisie."""
    key = name_key(name)
    for other in queryset:
        if name_key(other.name) == key:
            archived = getattr(other, 'archived', False) or not getattr(other, 'active', True)
            raise ApiError(f"{what} « {other.name} » existe déjà" + (' (archivé)' if archived else ''),
                           status.HTTP_409_CONFLICT)


def iid_list(data):
    items = data.get('items', [])
    if not isinstance(items, list) or not all(isinstance(iid, str) for iid in items):
        raise ApiError("items : liste d'iids attendue")
    return [iid.strip() for iid in items if iid.strip()]


def checked_map(data):
    """"checked" d'une verif : {lot id: [types d'items a cocher coches]}, None si absent (ancien front)."""
    value = data.get('checked')
    if value is None:
        return None
    if not isinstance(value, dict):
        raise ApiError("checked : {lot: [types]} attendu")
    return {str(lot_id)[:16]: [str(code)[:16] for code in types] for lot_id, types in value.items()
            if isinstance(types, list)}


def place_map(data):
    """"places" d'une verif : {iid: id du lot dont l'etiquette a ete scannee en dernier avant l'item}."""
    places = data.get('places') or {}
    if not isinstance(places, dict):
        raise ApiError("places : objet {iid: lot} attendu")
    return {str(iid): str(lot) for iid, lot in places.items() if isinstance(lot, str)}


def parse_role(data):
    """Role demande ('role', ou l'ancien booleen 'privileged' = admin), None si absent."""
    if 'role' in data:
        role = str(data['role'])
        if role not in Role.values:
            raise ApiError(f"Role inconnu : {role} (normal, chef, gestion ou admin)")
        return role
    if 'privileged' in data:
        return Role.ADMIN if data['privileged'] else Role.NORMAL
    return None


def badge_user(request, roles=None):
    """Secouriste authentifie par son badge ({"user": {"matricule", "key"}}) ou par son jeton de session
    ({"user": {"matricule", "session"}}), avec l'un des roles donnes."""
    credentials = request.data.get('user')
    if not isinstance(credentials, dict):
        raise ApiError("Scannez votre badge", status.HTTP_403_FORBIDDEN)
    user = credentials_user(credentials)  # badge, ou jeton de session (connexion par nom et PIN)
    if user is None:
        raise ApiError("Badge invalide ou expire", status.HTTP_403_FORBIDDEN)
    if roles and user.role not in roles:
        message = "Réservé aux administrateurs" if tuple(roles) == (Role.ADMIN,) else "Réservé aux rôles gestion et admin"
        raise ApiError(message, status.HTTP_403_FORBIDDEN)
    if roles and user.pin_required and not session_valid(user, credentials.get('session')):
        raise ApiError("Session expirée : scannez à nouveau votre badge", status.HTTP_403_FORBIDDEN, pin_required=True)
    return user


def require_lot_key(request, lot):
    """Sur l'API publique, toute ecriture sur un lot exige sa cle."""
    if is_local(request):
        return
    if not lot.check_key(request.data.get('key')):
        raise ApiError(key_error(lot, request.data.get('key')), status.HTTP_403_FORBIDDEN)


def key_error(lot, key):
    """Message quand l'etiquette privee d'un lot manque ou ne vaut plus : dit ce qu'elle est et ou la trouver."""
    state = lot.key_status(key) if key else 'missing'
    if state == 'expired':
        return (f"L'étiquette clé de {lot.name} a expiré le {lot.verif_key_expires.strftime('%d/%m/%Y')} : "
                "demandez à un responsable de la réimprimer.")
    if state == 'wrong':
        return (f"L'étiquette clé scannée n'est plus celle de {lot.name} (clé régénérée) : "
                "demandez à un responsable de la réimprimer.")
    return f"Scannez l'étiquette clé de {lot.name} pour valider. " + lot.key_hint()


# ----------------------------------------------------------------------------------------------------------------------
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
        # operations sur un lot possibles sans badge, sous un nom declare (reglage du serveur)
        'declared_identity': ServerSettings.get().declared_identity,
        # volume de la douchette quand personne n'est connecte (et des utilisateurs sans volume choisi)
        'scanner_volume_default': ServerSettings.get().scanner_volume_default,
        # lecture des codes-barres par le front web : le poste l'active a la premiere impression d'un code-barres
        'web_barcodes': ServerSettings.get().web_barcodes,
        # nom de l'antenne (case « Certifié par le TECH ... » des rapports d'etat de lot)
        'antenna_name': ServerSettings.get().antenna_name,
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
        raise ApiError("Badge invalide ou expire", status.HTTP_403_FORBIDDEN)
    verify_pin(user, request.data)
    if user.name_login_failures or user.name_login_locked_until:
        # le badge prouve que c'est bien lui : la connexion par nom et PIN redevient possible
        user.name_login_failures = 0
        user.name_login_locked_until = None
        user.save(update_fields=['name_login_failures', 'name_login_locked_until'])
    return Response(login_response(request, user))


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
    queryset = Secouristes.objects.filter(active=True, key_expires__gte=timezone.localdate()).exclude(pin_hash='')
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
    # fiche de l'item (front web) : dernier emplacement connu, pour ouvrir son lot ou lancer sa verif
    return Response({**ser.item_dict(item), 'last_location': ser.item_last_location(item),
                     'revisions': [ser.revision_dict(revision) for revision in item.revisions.all()[:5]]})


@api_view(['POST'])
@handle_errors
def item_revision(request, iid):
    """Revision d'un item (materiel a reviser) par un utilisateur connecte : {"user", "tasks": taches cochees (toutes),
    "next_due": date de la prochaine revision (par defaut aujourd'hui + la periodicite du type), "note"}."""
    user = connected_user(request)
    item = get_object_or_404(Items.objects.select_related('pack__item_type', 'location__lot_type'), iid=iid)
    data = request.data
    tasks = data.get('tasks') or []
    if not isinstance(tasks, list):
        raise ApiError('tasks : liste des tâches cochées attendue')
    next_due = parse_date(data['next_due'], 'next_due') if data.get('next_due') else None
    try:
        services.revise_item(item, f'{SOURCE_VERIFIED}:{user.matricule}', [str(task) for task in tasks], next_due,
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


def is_verified(identity):
    """Identite d'un secouriste reconnu (badge, ou nom et PIN), pas un nom declare."""
    return identity.startswith(f'{SOURCE_VERIFIED}:')


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
    « de la … », « de l'… ». Meme regle que deName (app.js) et de_name (french.hpp)."""
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
        target = de_name(lots[0].name) + (f' (ou celle {subs})' if subs else '')
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
    "full" : « vérif complète » (bouton des admins du front web) : les items et sous-lots facultatifs sont
    obligatoires pour cette verif (services.perform_verif strict)."""
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
                                    checked=checked_map(request.data))
    report['participants'] = [ser._who(value) for value in [identity, *participants]]
    if team and step:
        live.hub.step(team, report, iids, final=bool(request.data.get('finish')))
    elif team:
        live.hub.finish(team, report)
    return Response(report)


RECENT_VERIFS = 30


@api_view(['POST'])
@handle_errors
def verifs_recent(request):
    """Dernieres verifs de lots (page « Suivi des vérifs ») : tout badge valide. Une verif groupee (meme instant,
    meme personne) forme une seule ligne avec la liste de ses lots."""
    badge_user(request)
    limit = parse_int(request.data.get('limit', RECENT_VERIFS), 'limit', 1, 100)
    rows = []
    for verif in Verifs.objects.select_related('lot').filter(lot__isnull=False).order_by('-datetime', '-id')[:limit * 5]:
        last = rows[-1] if rows else None
        if last and last['_key'] == (verif.datetime, verif.by):
            last['lots'].append({'id': verif.lot_id, 'name': verif.lot.name, 'complete': verif.complete})
            last['complete'] = last['complete'] and verif.complete
            last['present'] += verif.present_count
            last['missing'] += verif.missing_count
            last['expired'] += verif.expired_count
            last['replaced'] += verif.replaced_count
            continue
        if len(rows) >= limit:
            break
        rows.append({
            '_key': (verif.datetime, verif.by),
            'id': verif.id,
            'at': ser._date(verif.datetime),
            'by': ser._who(verif.by),
            'participants': [ser._who(value) for value in verif.participants],
            'lots': [{'id': verif.lot_id, 'name': verif.lot.name, 'complete': verif.complete}],
            'complete': verif.complete,
            'present': verif.present_count,
            'missing': verif.missing_count,
            'expired': verif.expired_count,
            'replaced': verif.replaced_count,  # perimes retires du lot, a jeter
        })
    for row in rows:
        del row['_key']
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

    lots = []
    for verif in ordered:
        info = details.get(verif.id, {})
        lots.append({
            'id': verif.lot_id, 'name': verif.lot.name, 'depth': depth(verif.lot), 'complete': verif.complete,
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
    return Response(ser.lot_list(queryset, today=today, attention=True))  # lots a traiter en haut


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
def debug_codes_list(request):
    """Outil de debug des codes du front web (cache derriere 5 appuis sur « État des stocks ») : contenu de toutes
    les etiquettes imprimables, sauf les badges. Administrateurs seulement, verifie ici et pas seulement par le front."""
    connected_user(request, ADMIN)
    return Response(debug_codes.catalog())


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


@api_view(['GET'])
@handle_errors
def tasks_list(request):
    """Liste de taches de la gestion pour le poste (fenetre Gestion de la TECH), roles gestion et admin."""
    require_front(request, GESTION)
    return Response(tasks.todo())


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


# ----------------------------------------------------------------------------------------------------------------------
# Vues locales (gestion, sans cle)
# ----------------------------------------------------------------------------------------------------------------------

@api_view(['GET', 'POST'])
@handle_errors
def item_types(request):
    """Types d'items, archives compris (champ archived) : les fronts ne proposent que les types actifs."""
    if request.method != 'GET':
        require_front(request, GESTION)
    if request.method == 'GET':
        return Response([ser.item_type_dict(item_type) for item_type in ItemType.objects.order_by('name')])
    data = request.data
    code = str(data.get('type', '')).strip()
    if not CODE_RE.match(code):
        raise ApiError(f"Le code du type doit faire exactement {TYPE_LENGTH} caracteres alphanumeriques")
    if ItemType.objects.filter(type=code).exists():
        raise ApiError(f"Le type {code} existe deja", status.HTTP_409_CONFLICT)
    name = str(data.get('name', '')).strip()[:64]
    if not name:
        raise ApiError("Nom obligatoire")
    ensure_name_free(ItemType.objects.all(), name, "Le type d'item")
    check_tear_off_tasks(bool(data.get('tear_off', False)), parse_tasks(data.get('tasks')))
    check_physical(bool(data.get('physical', True)), bool(data.get('perissable', False)),
                   bool(data.get('tear_off', False)), parse_int(data.get('revision_months', 0), 'revision_months'))
    item_type = ItemType.objects.create(
        type=code,
        name=name,
        description=str(data.get('description', '')),
        min_quantity=parse_int(data.get('min_quantity', 0), 'min_quantity'),
        perissable=bool(data.get('perissable', False)),
        default_pack_size=parse_int(data.get('default_pack_size', 1), 'default_pack_size', 1),
        tear_off=bool(data.get('tear_off', False)),
        label_style=parse_label_style(data.get('label_style', LabelStyle.QR)),
        consumable=bool(data.get('consumable', True)),
        physical=bool(data.get('physical', True)),
        tasks=parse_tasks(data.get('tasks')),
        revision_months=parse_int(data.get('revision_months', 0), 'revision_months', 0, MAX_REVISION_MONTHS),
        revision_tasks=parse_tasks(data.get('revision_tasks')),
        revision_usable=bool(data.get('revision_usable', True)),
    )
    journal.record(OperationKind.CATALOG, front_identity(request), f"type d'item créé : {item_type}", type=code)
    return Response(ser.item_type_dict(item_type), status=status.HTTP_201_CREATED)


@api_view(['GET', 'PATCH'])
@handle_errors
def item_type_detail(request, type_code):
    if request.method != 'GET':
        require_front(request, GESTION)
    item_type = get_object_or_404(ItemType, type=type_code)
    if request.method == 'PATCH':
        data = request.data
        if 'name' in data:
            name = str(data['name']).strip()[:64]
            if not name:
                raise ApiError("Nom obligatoire")
            ensure_name_free(ItemType.objects.exclude(type=item_type.type), name, "Le type d'item")
            item_type.name = name
        if 'description' in data:
            item_type.description = str(data['description'])
        if 'min_quantity' in data:
            item_type.min_quantity = parse_int(data['min_quantity'], 'min_quantity')
        if 'default_pack_size' in data:
            item_type.default_pack_size = parse_int(data['default_pack_size'], 'default_pack_size', 1)
        if 'perissable' in data:
            perissable = bool(data['perissable'])
            if not perissable and ItemsPacks.objects.filter(item_type=item_type, peremption__isnull=False).exists():
                raise ApiError("Des items dates existent deja pour ce type")
            item_type.perissable = perissable
        if 'tear_off' in data:
            item_type.tear_off = bool(data['tear_off'])
        if 'label_style' in data:
            item_type.label_style = parse_label_style(data['label_style'])
        if 'consumable' in data:
            item_type.consumable = bool(data['consumable'])
        if 'tasks' in data:
            item_type.tasks = parse_tasks(data['tasks'])
        check_tear_off_tasks(item_type.tear_off, item_type.tasks)
        if 'physical' in data and bool(data['physical']) != item_type.physical:
            if not data['physical'] and ItemsPacks.objects.filter(item_type=item_type).exists():
                raise ApiError("Des items de ce type ont déjà des étiquettes : il ne peut pas devenir un item à cocher")
            item_type.physical = bool(data['physical'])
        had_revision = item_type.has_revision
        if 'revision_months' in data:
            item_type.revision_months = parse_int(data['revision_months'], 'revision_months', 0, MAX_REVISION_MONTHS)
        if 'revision_tasks' in data:
            item_type.revision_tasks = parse_tasks(data['revision_tasks'])
        if 'revision_usable' in data:
            item_type.revision_usable = bool(data['revision_usable'])
        check_physical(item_type.physical, item_type.perissable, item_type.tear_off, item_type.revision_months)
        if 'archived' in data:
            archived = bool(data['archived'])
            if archived:
                # un type encore attendu dans un lot rendrait ces lots incomplets pour toujours
                users = list(LotType.objects.filter(archived=False, requirements__item_type=item_type)
                             .order_by('name').values_list('name', flat=True).distinct())
                if users:
                    raise ApiError("Ce type est encore dans le contenu attendu de : " + ', '.join(users)
                                   + ". Retirez-le de ces types de lots d'abord.")
                lots = list(Lots.objects.filter(active=True, content__item_type=item_type, content__extra__gt=0)
                            .order_by('name').values_list('name', flat=True).distinct())
                if lots:
                    raise ApiError("Ce type est encore en plus dans le contenu des lots : " + ', '.join(lots)
                                   + ". Retirez-le de ces lots d'abord.")
            item_type.archived = archived
        item_type.save()
        if item_type.has_revision and not had_revision:
            # revision activee : premiere revision des items deja la dans la periodicite, sauf date deja prevue
            Items.objects.filter(pack__item_type=item_type, revision_due__isnull=True).update(
                revision_due=add_months(timezone.localdate(), item_type.revision_months))
        journal.record(OperationKind.CATALOG, front_identity(request),
                       f"type d'item modifié : {item_type} ({changed_fields(data, ITEM_TYPE_FIELDS)})",
                       type=item_type.type, fields=sorted(data.keys()))
    return Response(ser.item_type_dict(item_type))


ITEM_TYPE_FIELDS = {
    'name': 'nom', 'description': 'description', 'min_quantity': 'stock minimum',
    'default_pack_size': 'taille de paquet', 'perissable': 'périssable', 'tear_off': 'étiquette à déchirer',
    'label_style': "style d'étiquette",
    'consumable': 'consommable', 'physical': 'à cocher', 'tasks': 'tâches', 'revision_months': 'périodicité de révision',
    'revision_tasks': 'tâches de révision', 'revision_usable': 'utilisable en retard de révision',
    'archived': 'archivage',
}
LOT_TYPE_FIELDS = {'name': 'nom', 'description': 'description', 'storage': 'rangement', 'unique': 'lot unique',
                   'seal_required': 'scellé obligatoire', 'archived': 'archivage', 'referentiel': 'référentiel',
                   'part_of': 'rangement de'}


def apply_referentiel(lot_type, data):
    """Champs referentiel (bool) et part_of (code du referentiel dont ce type est un rangement, '' : aucun) d'un
    type de lot. Un referentiel n'a pas d'emplacements (ils sont effaces) et n'est le rangement de personne ; un
    rangement du stock n'est ni l'un ni l'autre. A enregistrer ensuite (save)."""
    referentiel = bool(data.get('referentiel', lot_type.referentiel))
    part_of = lot_type.part_of
    if 'part_of' in data:
        code = str(data.get('part_of') or '').strip()
        part_of = get_object_or_404(LotType, type=code) if code else None
    if lot_type.storage and (referentiel or part_of is not None):
        raise ApiError("Un rangement du stock n'est ni un référentiel ni le rangement d'un référentiel")
    if referentiel and part_of is not None:
        raise ApiError("Un référentiel n'est le rangement d'aucun autre : retirez « Rangement de » d'abord")
    if part_of is not None and (part_of.type == lot_type.type or not part_of.referentiel):
        raise ApiError(f"{part_of.name} n'est pas un référentiel : cochez « Référentiel » sur ce type d'abord")
    if not referentiel and lot_type.referentiel and lot_type.type and lot_type.rangements.exists():
        names = ', '.join(lot_type.rangements.order_by('name').values_list('name', flat=True))
        raise ApiError(f"Des rangements répartissent ce référentiel ({names}) : détachez-les d'abord")
    if referentiel and not lot_type.referentiel and lot_type.type:
        # la liste obligatoire n'a pas d'emplacements : ils sont portes par les rangements
        if lot_type.requirements.exclude(location='').exists() or lot_type.locations:
            lot_type.requirements.update(location='')
            lot_type.locations = []
            lot_type.version += 1
            lot_type.valid_version = lot_type.version
    lot_type.referentiel = referentiel
    lot_type.part_of = part_of


@api_view(['GET'])
@handle_errors
def items(request):
    require_front(request, GESTION)
    queryset = Items.objects.select_related('pack__item_type', 'location__lot_type').order_by('-added', 'iid')
    params = request.query_params
    if params.get('type'):
        queryset = queryset.filter(pack__item_type_id=params['type'])
    if params.get('location') == 'stock':
        queryset = queryset.filter(services.in_stock_q())  # rangements du stock compris
    elif params.get('location'):
        queryset = queryset.filter(location_id=params['location'])
    if params.get('status'):
        queryset = queryset.filter(status=params['status'])
    if params.get('q'):
        queryset = queryset.filter(iid__icontains=params['q'])
    limit = parse_int(params.get('limit', 200), 'limit', 1, 5000)
    today = timezone.localdate()
    return Response([ser.item_dict(item, today) for item in queryset[:limit]])


def storage_target(lot_id):
    """Rangement du stock ou ranger des items recus (None : le stock, sans rangement). Le lot du stock lui-meme
    vaut le stock ; un lot qui n'est pas un rangement du stock est refuse (un lot se remplit par reassort)."""
    if not lot_id:
        return None
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=str(lot_id))
    if lot.is_stock:
        return None
    if not lot.lot_type.storage:
        raise ApiError(f"{lot.name} n'est pas un rangement du stock")
    if not lot.active:
        raise ApiError(f"{lot.name} est archivé")
    return lot


@api_view(['POST'])
@handle_errors
def items_batch(request):
    """Reception : cree `count` items (et optionnellement un paquet scelle qui les contient)."""
    require_front(request, GESTION)
    data = request.data
    item_type = get_object_or_404(ItemType, type=str(data.get('type', '')))
    peremption = parse_date(data.get('peremption'), 'peremption')
    count = parse_int(data.get('count', 1), 'count', 1, 10000)
    identity = operation_identity(request, data)
    location = storage_target(data.get('location'))
    with transaction.atomic():
        sealed_pack = None
        if data.get('sealed_pack'):
            sealed_pack = SealedPacks.objects.create(
                item_type=item_type,
                peremption=peremption if item_type.perissable else None,
                count=count,
                created_by=identity[:32],
            )
        created = ItemsPacks.objects.add_items(
            item_type, peremption, count, identity[:32], location=location, sealed_pack=sealed_pack
        )
        summary = f'{count} × {item_type.name}'
        if item_type.perissable and peremption:
            summary += f", péremption {peremption.strftime('%d/%m/%Y')}"
        if sealed_pack is not None:
            summary += f' (paquet fermé {sealed_pack.id})'
        if location is not None:
            summary += f', rangés dans {location.name}'
        journal.record(OperationKind.RECEPTION, identity, summary, lot=location, type=item_type.type, count=count,
                       peremption=peremption.isoformat() if peremption else None,
                       sealed_pack=sealed_pack.id if sealed_pack else None, items=journal.item_ids(created))
        notifications.check_stock_levels([item_type.type])
    today = timezone.localdate()
    response = {'items': [ser.item_dict(item, today) for item in created]}
    if sealed_pack is not None:
        response['sealed_pack'] = ser.sealed_pack_dict(sealed_pack)
    return Response(response, status=status.HTTP_201_CREATED)


@api_view(['POST'])
@handle_errors
def item_delete(request, iid):
    """Marque un item comme supprime (ce n'est pas la voie normale : les verifs detectent les disparitions)."""
    require_front(request, GESTION)
    item = get_object_or_404(Items.objects.select_related('pack__item_type'), iid=iid)
    reason = str(request.data.get('reason', '')).strip()
    if not reason:
        raise ApiError("Une raison est obligatoire pour supprimer un item")
    services.mark_deleted(item, operation_identity(request)[:32], reason)
    return Response(ser.item_dict(item))


@api_view(['POST'])
@handle_errors
def items_disable(request):
    """Desactive ou reactive des items (rappel produit, non-conformite), au poste, par un admin (fenetre Inventaire) :
    {"user", "disabled": bool, "reason": texte (obligatoire pour desactiver), et les items vises : "items" (iids),
    "reception" (un iid : tous les items recus avec lui, meme paquet d'etiquettes et meme instant) et/ou "pack"
    (paquet ferme : tous ses items)}. Desactive, un item est invalide comme un perime."""
    require_front(request, ADMIN)
    data = request.data
    where = Q(iid__in=iid_list(data)) if data.get('items') else Q(pk__in=[])
    if data.get('reception'):
        first = get_object_or_404(Items, iid=str(data['reception']))
        where |= Q(pack_id=first.pack_id, added=first.added)
    if data.get('pack'):
        where |= Q(sealed_pack_id=get_object_or_404(SealedPacks, id=str(data['pack'])).id)
    targets = list(Items.objects.select_related('pack__item_type').filter(where))
    if not targets:
        raise ApiError('Aucun item à désactiver')
    changed = services.set_disabled(targets, operation_identity(request), bool(data.get('disabled', True)),
                                    data.get('reason', ''))
    return Response({'count': len(changed), 'total': len(targets)})


FRONT_NAME_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')


@api_view(['GET', 'POST'])
@handle_errors
def front_configs(request):
    """Connecter un autre ordinateur, depuis le poste (Utilisateurs > Ordinateurs connectes), par un admin : cree la cle d'un front
    distant et son fichier de configuration chiffre, telechargeable pendant 5 minutes par un lien (QR code).
    POST {"name", "password" (vide = genere et renvoye), "replace": bool}. GET : fronts existants.
    Seulement sur l'API locale du poste : jamais depuis un front distant (une cle ne cree pas d'autres cles)."""
    require_front(request, ADMIN)
    if getattr(request, 'qrprotec_front', ''):
        raise ApiError("Seulement depuis le poste du serveur (API locale)", status.HTTP_403_FORBIDDEN)
    now = timezone.now()
    FrontConfigDownload.objects.filter(expires__lte=now).delete()
    if request.method == 'GET':
        pending = set(FrontConfigDownload.objects.values_list('front_id', flat=True))
        return Response({'fronts': [
            {'name': front.name, 'revoked': front.revoked, 'created': front.created.isoformat(),
             'last_used': front.last_used.isoformat() if front.last_used else None,
             'last_address': front.last_address, 'download_pending': front.pk in pending}
            for front in FrontKey.objects.order_by('revoked', 'name')]})
    data = request.data
    name = str(data.get('name', '')).strip()
    if not FRONT_NAME_RE.match(name):
        raise ApiError("Nom de l'ordinateur : lettres sans accents, chiffres, . _ - (64 caractères au plus, sans espace)")
    password = str(data.get('password', ''))
    generated = not password
    if generated:
        password = front_config.generate_password()
    if len(password) < front_config.MIN_PASSWORD_LENGTH:
        raise ApiError(f"Mot de passe trop court ({front_config.MIN_PASSWORD_LENGTH} caractères au moins)")
    existing = FrontKey.objects.filter(name=name).first()
    if existing is not None and not data.get('replace'):
        raise ApiError(f"Un ordinateur s'appelle déjà « {name} » : choisissez un autre nom, ou remplacez sa clé "
                       "(l'ancien fichier ne marchera plus)", status.HTTP_409_CONFLICT, exists=True)
    base_url = qrprotec_setting('PUBLIC_BASE_URL').rstrip('/')
    ca_pem = front_config.server_ca_pem(base_url, qrprotec_setting('TLS'), qrprotec_setting('FRONT_CA_FILE'))
    with transaction.atomic():
        if existing is not None:
            existing.delete()
        front, key = FrontKey.create(name)
        payload = {'name': name, 'api_url': base_url, 'api_key': key,
                   'created': timezone.localtime(now).isoformat(timespec='seconds')}
        if ca_pem:
            payload['api_ca_pem'] = ca_pem
        download = FrontConfigDownload.objects.create(
            token=front_config.new_download_token(), front=front, content=front_config.encrypt(payload, password),
            created=now, expires=front_config.download_expiry(now))
    journal.record(OperationKind.SETTINGS, front_identity(request),
                   f"ordinateur « {name} » connecté au serveur" + (" (clé remplacée)" if existing else ''),
                   front=name)
    return Response({
        'name': name,
        'url': f'{base_url}/front-config/{download.token}',
        'expires': download.expires.isoformat(),
        'seconds': int((download.expires - now).total_seconds()),
        'password': password if generated else '',
        'ca': bool(ca_pem),
        'replaced': existing is not None,
    }, status=status.HTTP_201_CREATED)


@api_view(['POST'])
@handle_errors
def front_config_detail(request, name):
    """Ordinateurs connectes (fenetre Utilisateurs du poste), admin : {"action": "revoke" | "restore" | "delete"}.
    Revoquer coupe l'acces tout de suite (la cle reste, reactivable) ; supprimer efface la cle et ses liens."""
    require_front(request, ADMIN)
    if getattr(request, 'qrprotec_front', ''):
        raise ApiError("Seulement depuis le poste du serveur (API locale)", status.HTTP_403_FORBIDDEN)
    front = get_object_or_404(FrontKey, name=name)
    action = request.data.get('action')
    if action in ('revoke', 'restore'):
        front.revoked = action == 'revoke'
        front.save(update_fields=['revoked'])
        if front.revoked:
            FrontConfigDownload.objects.filter(front=front).delete()  # lien encore valable : coupe aussi
        summary = f"ordinateur « {name} » " + ("révoqué" if front.revoked else "réactivé")
    elif action == 'delete':
        front.delete()
        summary = f"ordinateur « {name} » supprimé"
    else:
        raise ApiError("Action inconnue (revoke, restore ou delete)")
    journal.record(OperationKind.SETTINGS, front_identity(request), summary, front=name)
    return Response({'name': name, 'action': action})


def front_config_download(request, token):
    """Lien du QR code affiche au poste : le fichier chiffre, en piece jointe, tant que le lien est valable."""
    if request.method != 'GET':
        return HttpResponseNotAllowed(['GET'])
    FrontConfigDownload.objects.filter(expires__lte=timezone.now()).delete()
    download = FrontConfigDownload.objects.select_related('front').filter(
        token=token, expires__gt=timezone.now(), front__revoked=False).first()
    if download is None:
        response = HttpResponse("Lien expiré ou inconnu : demandez un nouveau QR code au poste "
                                "(Utilisateurs > Ordinateurs connectés).\n",
                                content_type='text/plain; charset=utf-8', status=404)
        response['Cache-Control'] = 'no-store'
        return response
    FrontConfigDownload.objects.filter(pk=download.pk).update(downloads=download.downloads + 1)
    response = HttpResponse(download.content, content_type='application/octet-stream')
    response['Content-Disposition'] = f'attachment; filename="{download.front.name}{front_config.SUFFIX}"'
    response['Cache-Control'] = 'no-store'
    response['X-Content-Type-Options'] = 'nosniff'
    return response


@api_view(['GET'])
@handle_errors
def receptions(request):
    """Receptions passees (items crees ensemble : meme paquet d'etiquettes, meme instant), les plus recentes
    d'abord : ?type=CODE, ?item=IID (la reception de cet item), ?limit=N. Sert a desactiver d'un coup tout ce qui a
    ete recu ensemble (paquet ouvert a la reception, rappel produit)."""
    require_front(request, GESTION)
    params = request.query_params
    queryset = Items.objects.all()
    if params.get('type'):
        queryset = queryset.filter(pack__item_type_id=params['type'])
    if params.get('item'):
        item = get_object_or_404(Items, iid=params['item'])
        queryset = queryset.filter(pack_id=item.pack_id, added=item.added)
    limit = parse_int(params.get('limit', 100), 'limit', 1, 1000)
    rows = (queryset.values('pack_id', 'pack__item_type_id', 'pack__item_type__name', 'pack__peremption', 'added',
                            'added_by')
            .annotate(count=Count('iid'), first=Min('iid'), last=Max('iid'), sealed_pack=Max('sealed_pack_id'),
                      active=Count('iid', filter=Q(status=ItemStatus.ACTIVE)),
                      in_stock=Count('iid', filter=services.in_stock_q() & Q(status=ItemStatus.ACTIVE)),
                      disabled=Count('iid', filter=Q(disabled=True)))
            .order_by('-added')[:limit])
    return Response([{
        'item': row['first'],
        'last': row['last'],
        'type': row['pack__item_type_id'],
        'type_name': row['pack__item_type__name'],
        'peremption': row['pack__peremption'].isoformat() if row['pack__peremption'] else None,
        'added': timezone.localtime(row['added']).isoformat(),
        'added_by': identity_name(row['added_by']),
        'count': row['count'],
        'active': row['active'],
        'in_stock': row['in_stock'],
        'disabled': row['disabled'],
        'sealed_pack': row['sealed_pack'],
    } for row in rows])


@api_view(['GET'])
@handle_errors
def reception_storage(request):
    """Rangement du stock propose a la reception d'un type (?type=CODE) : le dernier ou ce type a ete range."""
    require_front(request, GESTION)
    item_type = get_object_or_404(ItemType, type=str(request.query_params.get('type', '')))
    return Response({'type': item_type.type, 'location': services.last_storage(item_type)})


@api_view(['POST'])
@handle_errors
def reception_store(request):
    """Range d'un coup toute une reception dans un rangement du stock (ou en vrac dans le stock si "location" est
    vide) : {"user", "reception": un iid de la reception, "location"}. Seuls les items encore en stock bougent : ceux
    deja mis dans un lot, utilises ou supprimes restent ou ils sont."""
    require_front(request, GESTION)
    data = request.data
    first = get_object_or_404(Items, iid=str(data.get('reception', '')))
    location = storage_target(data.get('location'))
    identity = operation_identity(request, data)
    received = Items.objects.filter(pack_id=first.pack_id, added=first.added)
    iids = list(received.filter(services.in_stock_q(), status=ItemStatus.ACTIVE).order_by('iid')
                .values_list('iid', flat=True))
    if not iids:
        raise ApiError('Aucun item de cette réception en stock')
    if location is None:
        result = services.move_items(iids, None, identity, SeenWhile.REMOVE)
    else:
        result = services.move_items(iids, location, identity, SeenWhile.ADD)
    return Response({'moved': result['moved'], 'count': len(result['moved']), 'total': received.count(),
                     'location': location.id if location else None})


@api_view(['POST'])
@handle_errors
def item_restore(request, iid):
    require_front(request, GESTION)
    item = get_object_or_404(Items.objects.select_related('pack__item_type'), iid=iid)
    services.restore(item, operation_identity(request))
    return Response(ser.item_dict(item))


@api_view(['POST'])
@handle_errors
def items_to_stock(request):
    require_front(request, GESTION)
    identity = operation_identity(request)
    return Response(services.move_items(iid_list(request.data), None, identity, SeenWhile.REMOVE,
                                        debug=debug_options(request)))


@api_view(['GET'])
@handle_errors
def stock(request):
    require_front(request, GESTION)
    soon = parse_int(request.query_params.get('soon_days', 30), 'soon_days', 0, 3650)
    # les peremptions font baisser le stock sans evenement : on verifie les seuils a chaque consultation
    notifications.check_stock_levels()
    return Response(services.stock_status(soon))


@api_view(['GET'])
@handle_errors
def stock_forecast(request):
    return Response(forecast.forecast(**forecast_params(request.query_params)))


@api_view(['POST'])
@handle_errors
def stock_verif(request):
    require_front(request, GESTION)
    identity = operation_identity(request)
    return Response(services.perform_verif(None, iid_list(request.data), identity))


@api_view(['GET'])
@handle_errors
def sealed_packs(request):
    require_front(request, GESTION)
    queryset = SealedPacks.objects.select_related('item_type').order_by('-created')
    if request.query_params.get('opened') == '0':
        queryset = queryset.filter(opened__isnull=True)
    return Response([ser.sealed_pack_dict(pack) for pack in queryset[:500]])


@api_view(['POST'])
@handle_errors
def sealed_pack_open(request, pack_id):
    require_front(request, GESTION)
    sealed_pack = get_object_or_404(SealedPacks.objects.select_related('item_type'), id=pack_id)
    identity = operation_identity(request)
    now = timezone.now()
    # un paquet deja ouvert garde sa date d'ouverture : l'appel sert alors a reimprimer les etiquettes
    if sealed_pack.opened is None:
        with transaction.atomic():
            sealed_pack.opened = now
            sealed_pack.opened_by = identity[:32]
            sealed_pack.save(update_fields=['opened', 'opened_by'])
            Items.objects.filter(sealed_pack=sealed_pack).update(
                last_seen=now, last_seen_by=identity, last_seen_while=SeenWhile.OPEN
            )
            journal.record(OperationKind.PACK_OPEN, identity,
                           f'{sealed_pack.count} × {sealed_pack.item_type.name} (paquet {sealed_pack.id})', at=now,
                           pack=sealed_pack.id, type=sealed_pack.item_type_id, count=sealed_pack.count)
    today = timezone.localdate()
    data = ser.sealed_pack_dict(sealed_pack, with_items=False)
    data['items'] = [
        ser.item_dict(item, today)
        for item in sealed_pack.items.select_related('pack__item_type', 'location').order_by('iid')
    ]
    return Response(data)


@api_view(['POST'])
@handle_errors
def sealed_pack_close(request, pack_id):
    """Annule une ouverture faite par erreur : le paquet redevient ferme (les etiquettes deja imprimees
    restent valables, les items sont les memes)."""
    require_front(request, GESTION)
    sealed_pack = get_object_or_404(SealedPacks.objects.select_related('item_type'), id=pack_id)
    was_opened = sealed_pack.opened is not None
    sealed_pack.opened = None
    sealed_pack.opened_by = ''
    sealed_pack.save(update_fields=['opened', 'opened_by'])
    if was_opened:
        journal.record(OperationKind.PACK_CLOSE, front_identity(request),
                       f'{sealed_pack.count} × {sealed_pack.item_type.name} (paquet {sealed_pack.id}, ouverture annulée)',
                       pack=sealed_pack.id, type=sealed_pack.item_type_id)
    return Response(ser.sealed_pack_dict(sealed_pack))


@api_view(['GET', 'POST'])
@handle_errors
def lot_types(request):
    """Types de lots, archives compris (champ archived).

    Un type « lot unique » (ex : un VPS, une armoire du VPS) est cree avec son lot, du meme nom (nom court compris,
    tronque a 16 caracteres) : `parent` (lot global) s'applique alors a ce lot, renvoye dans `created_lot`. Un rangement du stock est
    toujours un lot unique.
    """
    if request.method != 'GET':
        require_front(request, GESTION)
    if request.method == 'GET':
        return Response([ser.lot_type_dict(lot_type) for lot_type in LotType.objects.order_by('name')])
    data = request.data
    code = str(data.get('type', '')).strip()
    if not CODE_RE.match(code):
        raise ApiError(f"Le code du type de lot doit faire exactement {TYPE_LENGTH} caracteres alphanumeriques")
    if LotType.objects.filter(type=code).exists():
        raise ApiError(f"Le type de lot {code} existe deja", status.HTTP_409_CONFLICT)
    name = str(data.get('name', '')).strip()[:64]
    if not name:
        raise ApiError("Nom obligatoire")
    ensure_name_free(LotType.objects.all(), name, 'Le type de lot')
    storage = bool(data.get('storage', False))
    unique = storage or bool(data.get('unique', False))
    identity = operation_identity(request, data)[:32]
    with transaction.atomic():
        lot_type = LotType(
            type='', name=name, description=str(data.get('description', '')), created_by=identity,
            storage=storage, unique=unique, seal_required=bool(data.get('seal_required', False)),
        )
        apply_referentiel(lot_type, data)
        lot_type.type = code
        lot_type.save(force_insert=True)
        lot = None
        if unique:
            ensure_name_free(Lots.objects.filter(active=True), name, 'Le lot')
            lot = Lots(lot_type=lot_type, name=name, name_short=name[:16], created_by=identity)
            lot.parent = parse_parent(lot, data.get('parent'))
            lot.save()
        journal.record(OperationKind.CATALOG, identity, f'type de lot créé : {lot_type}' + (' (lot unique)' if unique else ''),
                       lot=lot, type=code)
    response = ser.lot_type_dict(lot_type)
    if lot is not None:
        response['created_lot'] = ser.lot_dict(lot, local=True, with_items=True)
    return Response(response, status=status.HTTP_201_CREATED)


@api_view(['GET', 'PATCH'])
@handle_errors
def lot_type_detail(request, type_code):
    if request.method != 'GET':
        require_front(request, GESTION)
    lot_type = get_object_or_404(LotType, type=type_code)
    if request.method == 'PATCH':
        data = request.data
        lots = list(lot_type.lots_set.select_related('parent__lot_type'))  # lot.lot_type : ce type (modifie ici)
        if lot_type.type == STOCK_LOT_TYPE and (
                ('storage' in data and not data['storage']) or ('unique' in data and not data['unique'])
                or data.get('archived')):
            raise ApiError("Le stock reste un rangement unique, toujours actif : seuls son nom et sa description "
                           "se modifient")
        if 'name' in data:
            name = str(data['name']).strip()[:64]
            if not name:
                raise ApiError("Nom obligatoire")
            ensure_name_free(LotType.objects.exclude(type=lot_type.type), name, 'Le type de lot')
            if lot_type.unique:
                # le lot d'un type unique porte le nom du type, nom court compris
                for lot in lots:
                    if lot.name == lot_type.name:
                        ensure_name_free(Lots.objects.filter(active=True).exclude(id=lot.id), name, 'Le lot')
                        lot.name = name
                        if lot.name_short == lot_type.name[:16]:
                            lot.name_short = name[:16]
                        lot.save(update_fields=['name', 'name_short'])
            lot_type.name = name
        if 'description' in data:
            lot_type.description = str(data['description'])
        if 'seal_required' in data:
            lot_type.seal_required = bool(data['seal_required'])
        if 'referentiel' in data or 'part_of' in data:
            apply_referentiel(lot_type, data)
        storage = bool(data.get('storage', lot_type.storage))
        # un rangement du stock est un lot unique (sauf un ancien type de rangement qui a deja plusieurs lots)
        unique = bool(data.get('unique', lot_type.unique)) or (storage and len(lots) <= 1)
        if storage and not lot_type.storage and (lot_type.referentiel or lot_type.part_of_id):
            raise ApiError("Un référentiel ou le rangement d'un référentiel ne peut pas devenir un rangement du stock")
        if storage and not lot_type.storage and len(lots) > 1:
            raise ApiError(f"{len(lots)} lots de ce type existent : un rangement du stock n'en a qu'un")
        if unique and not lot_type.unique and len(lots) > 1:
            raise ApiError(f"{len(lots)} lots de ce type existent : un lot unique n'en a qu'un")
        lot_type.unique = unique
        if storage != lot_type.storage:
            # un rangement fait partie du stock, un lot ordinaire non : ni l'un ni l'autre ne contient l'autre
            for lot in lots:
                if lot.children.filter(active=True).exists():
                    raise ApiError(f"Le lot {lot.name} contient des sous-lots : retirez-les d'abord")
            lot_type.storage = storage
            # rangement : jamais de contenu attendu, etiquette privee sans expiration
            if storage:
                LotContent.objects.filter(lot__lot_type=lot_type).delete()
                Lots.objects.filter(lot_type=lot_type).update(locations=[])
            if storage and lot_type.requirements.exists():
                lot_type.requirements.all().delete()
                lot_type.locations = []
                lot_type.version += 1
                lot_type.valid_version = lot_type.version
            for lot in lots:
                lot.verif_key_expires = lot.key_expiration()
                lot.key_expiry_stage = KeyExpiry.VALID
                if storage and (lot.parent is None or not lot.parent.lot_type.storage):
                    lot.parent = Lots.stock()  # un rangement est range dans le stock
                elif not storage and lot.parent is not None and lot.parent.lot_type.storage:
                    lot.parent = None  # n'est plus un rangement : sort du stock
                lot.save(update_fields=['verif_key_expires', 'key_expiry_stage', 'parent'])
        if 'archived' in data:
            archived = bool(data['archived'])
            active = [lot for lot in lots if lot.active]
            if archived and active and not lot_type.unique:
                raise ApiError("Des lots actifs sont de ce type (" + ', '.join(lot.name for lot in active)
                               + ") : archivez-les d'abord")
            with transaction.atomic():
                if lot_type.unique:
                    # le lot d'un type unique est archive et desarchive avec son type
                    for lot in lots:
                        if archived and lot.active and lot.children.filter(active=True).exists():
                            raise ApiError(f"Le lot {lot.name} contient des sous-lots actifs : archivez-les ou "
                                           "retirez-les du lot d'abord")
                        if not archived and not lot.active:
                            ensure_name_free(Lots.objects.filter(active=True).exclude(id=lot.id), lot.name, 'Le lot')
                        if not archived and lot.parent is not None and not lot.parent.active:
                            lot.parent = default_parent(lot)
                        lot.active = not archived
                        lot.save(update_fields=['active', 'parent'])
                lot_type.archived = archived
                lot_type.save()
        else:
            lot_type.save()
        journal.record(OperationKind.CATALOG, front_identity(request),
                       f'type de lot modifié : {lot_type} ({changed_fields(data, LOT_TYPE_FIELDS)})',
                       type=lot_type.type, fields=sorted(data.keys()))
    return Response(ser.lot_type_dict(lot_type))


@api_view(['PUT'])
@handle_errors
def lot_type_requirements(request, type_code):
    """Remplace la liste des exigences : [{"type": "serphy", "quantity": 4, "location": "pochette bleue",
    "optional": false}, ...], et l'ordre des emplacements (locations, facultatif : ordre actuel, les nouveaux a la
    suite). optional : facultatif a la verif, seulement pour du materiel non consommable sans taches."""
    require_front(request, GESTION)
    lot_type = get_object_or_404(LotType, type=type_code)
    rows = request.data.get('requirements', [])
    if not isinstance(rows, list):
        raise ApiError("requirements : liste attendue")
    if rows and lot_type.storage:
        raise ApiError(f"{lot_type.name} est un rangement du stock : il n'a pas de contenu attendu")
    order = request.data.get('locations', lot_type.locations)
    if not isinstance(order, list):
        raise ApiError("locations : liste attendue")
    locations = services.location_order([row for row in rows if isinstance(row, dict)], [str(name) for name in order],
                                        lot_type.locations or [])
    with transaction.atomic():
        LotRequirements.objects.filter(lot_type=lot_type).delete()
        for row in rows:
            item_type = get_object_or_404(ItemType, type=str(row.get('type', '')))
            quantity = parse_int(row.get('quantity', 1), 'quantity', 1)
            location = locations.get(name_key(services.clean_location(row.get('location'))), '')
            if lot_type.referentiel:
                location = ''  # liste obligatoire : les emplacements sont ceux de ses rangements
            optional = bool(row.get('optional', False))
            if optional and not item_type.can_be_optional:
                raise ApiError(f"{item_type.name} ne peut pas être facultatif : seul le matériel non consommable sans "
                               "tâches peut l'être")
            try:
                LotRequirements.objects.create(
                    lot_type=lot_type, item_type=item_type, quantity=quantity, location=location, optional=optional
                )
            except IntegrityError:
                raise ApiError(f"Le type {item_type.type} apparait plusieurs fois")
        lot_type.locations = [] if lot_type.referentiel else list(locations.values())
        lot_type.version += 1
        lot_type.valid_version = lot_type.version
        lot_type.save(update_fields=['version', 'valid_version', 'locations'])
        journal.record(OperationKind.CATALOG, front_identity(request), f'contenu attendu modifié : {lot_type}',
                       type=lot_type.type, requirements=[
                           {'type': str(row.get('type', '')), 'quantity': row.get('quantity', 1),
                            'optional': bool(row.get('optional', False))} for row in rows
                       ])
    return Response(ser.lot_type_dict(lot_type))


@api_view(['GET'])
@handle_errors
def lot_type_coverage(request, type_code):
    """Couverture d'un referentiel : pour chaque lot qui doit le couvrir (ses lots, ou un lot d'un de ses rangements
    hors d'un lot du referentiel), la liste obligatoire face au contenu attendu de ses sacs (voir
    services.lot_coverage)."""
    require_front(request, GESTION)
    lot_type = get_object_or_404(LotType, type=type_code)
    if not lot_type.referentiel:
        raise ApiError(f"{lot_type.name} n'est pas un référentiel")
    return Response({'type': lot_type.type, 'name': lot_type.name,
                     'lots': [services.lot_coverage(lot, lot_type) for lot in services.coverage_lots(lot_type)]})


@api_view(['PUT'])
@handle_errors
def lot_content(request, lot_id):
    """Remplace le contenu propre d'un lot, par-dessus celui de son type : [{"type": "serphy", "extra": 2,
    "location": "poche avant"}, ...] (extra : en plus de la quantite du type ; location absent ou null : l'emplacement
    du type), et l'ordre des emplacements du lot (locations, facultatif ; celui du type s'il revient au meme)."""
    require_front(request, GESTION)
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    lot_type = lot.lot_type
    rows = request.data.get('content', [])
    if not isinstance(rows, list) or not all(isinstance(row, dict) for row in rows):
        raise ApiError("content : liste attendue")
    if rows and lot_type.storage:
        raise ApiError(f"{lot.name} est un rangement du stock : il n'a pas de contenu attendu")
    order = request.data.get('locations', lot.locations)
    if not isinstance(order, list):
        raise ApiError("locations : liste attendue")
    type_rows = {row.item_type_id: row for row in LotRequirements.objects.filter(lot_type=lot_type)}
    wanted = {}
    for row in rows:
        item_type = get_object_or_404(ItemType, type=str(row.get('type', '')))
        if item_type.type in wanted:
            raise ApiError(f"Le type {item_type.type} apparait plusieurs fois")
        extra = parse_int(row.get('extra', 0), 'extra', 0)
        location = row.get('location')
        location = None if location is None else services.clean_location(location)
        requirement = type_rows.get(item_type.type)
        if requirement is not None and location is not None and name_key(location) == name_key(requirement.location):
            location = None  # meme emplacement que le type : le suit s'il change
        if requirement is None and not extra:
            continue  # ni attendu par le type ni en plus : rien a retenir
        if requirement is not None and not extra and location is None:
            continue
        wanted[item_type.type] = (item_type, extra, location)
    # emplacements du lot (ceux du type que le lot garde et les siens), une seule orthographe par emplacement
    effective = [{'location': type_rows[code].location} for code in type_rows if code not in wanted]
    effective += [{'location': (type_rows[code].location if location is None and code in type_rows else location or '')}
                  for code, (_, _, location) in wanted.items()]
    known = [*(lot.locations or []), *(lot_type.locations or [])]
    locations = services.location_order(effective, [str(name) for name in order], known)
    inherited = services.location_order(effective, lot_type.locations or [], known)
    with transaction.atomic():
        LotContent.objects.filter(lot=lot).delete()
        for item_type, extra, location in wanted.values():
            if location is not None:
                location = locations.get(name_key(location), '')
            LotContent.objects.create(lot=lot, item_type=item_type, extra=extra, location=location)
        # meme ordre que le type : le lot suit celui du type (nouveaux emplacements, ordre change par la TECH)
        lot.locations = [] if list(locations) == list(inherited) else list(locations.values())
        lot.save(update_fields=['locations'])
        journal.record(OperationKind.LOT, front_identity(request), f'contenu propre du lot modifié : {lot.name}',
                       lot=lot, content=[
                           {'type': code, 'extra': extra, 'location': location}
                           for code, (_, extra, location) in wanted.items()
                       ])
    return Response(ser.lot_dict(lot, local=True, with_items=True))


@api_view(['GET', 'POST'])
@handle_errors
def lots(request):
    if request.method != 'GET':
        require_front(request, GESTION)
    if request.method == 'GET':
        queryset = Lots.objects.select_related('lot_type').order_by('lot_type__name', 'name')
        if request.query_params.get('all') != '1':
            queryset = queryset.filter(active=True)
        # cles des lots (etiquettes privees) : roles gestion et admin seulement
        return Response(ser.lot_list(queryset, local=is_privileged(request)))
    data = request.data
    lot_type = get_object_or_404(LotType, type=str(data.get('lot_type', '')))
    if lot_type.archived:
        raise ApiError(f"Le type de lot {lot_type.name} est archivé")
    if (lot_type.unique or lot_type.storage) and lot_type.lots_set.exists():
        raise ApiError(f"{lot_type.name} est un " + ("rangement du stock" if lot_type.storage else "lot unique")
                       + " : son lot existe déjà", status.HTTP_409_CONFLICT)
    name = str(data.get('name', '')).strip()[:64]
    if not name:
        raise ApiError("Nom obligatoire")
    ensure_name_free(Lots.objects.filter(active=True), name, 'Le lot')
    lot = Lots(
        lot_type=lot_type,
        name=name,
        name_short=str(data.get('name_short', '') or name)[:16],
        key_location=str(data.get('key_location', '')).strip()[:128],
        created_by=operation_identity(request, data)[:32],
    )
    lot.parent = parse_parent(lot, data.get('parent'))
    lot.save()
    journal.record(OperationKind.LOT, lot.created_by, f'lot créé ({lot_type.name})'
                   + (f', rangé dans {lot.parent.name}' if lot.parent else ''), lot=lot)
    return Response(ser.lot_dict(lot, local=True, with_items=True), status=status.HTTP_201_CREATED)


BATCH_MAX = 30  # lots crees d'un coup (sous-lots non compris)


def batch_sub_types(lot_type):
    """Sous-lots proposes a la creation de lots de `lot_type`, avec leur nombre par defaut.

    Les types de sous-lots deja ranges dans les lots de ce type, au nombre le plus courant, et les rangements du
    referentiel : 1 par defaut, 0 si tous leurs lots sont ranges dans des lots d'un autre type (le sac de soin du
    VPS est un rangement du B+, mais pas un sac des lots B+)."""
    lots = list(lot_type.lots_set.filter(active=True).prefetch_related('children__lot_type'))
    counts = defaultdict(Counter)  # type de sous-lot -> {nombre dans un lot: nombre de lots}
    names = {}
    for lot in lots:
        per_lot = Counter(child.lot_type_id for child in lot.children.all() if child.active)
        for child in lot.children.all():
            if child.active:
                names[child.lot_type_id] = child.lot_type
        for code in names:
            counts[code][per_lot.get(code, 0)] += 1
    for code in names:  # types apparus dans un lot apres les premiers : 0 pour les lots qui ne l'ont pas
        counts[code][0] += len(lots) - sum(counts[code].values())
    rows = {}
    for code, counter in counts.items():
        rows[code] = (names[code], max(counter.items(), key=lambda pair: (pair[1], pair[0]))[0])
    for rangement in lot_type.rangements.filter(archived=False).order_by('name'):
        if rangement.type in rows:
            continue
        parents = set(rangement.lots_set.filter(active=True).values_list('parent__lot_type_id', flat=True))
        rows[rangement.type] = (rangement, 0 if parents and lot_type.type not in parents else 1)
    return [
        {'lot_type': sub_type.type, 'name': sub_type.name, 'count': count,
         'rangement': sub_type.part_of_id == lot_type.type}
        for code, (sub_type, count) in sorted(rows.items(), key=lambda pair: pair[1][0].name.lower())
        if not (sub_type.archived or sub_type.storage or sub_type.unique)
    ]


def usual_key_location(lot_type):
    """Emplacement de l'etiquette cle le plus courant parmi les lots du type (vide s'il n'y en a pas)."""
    places = Counter(lot_type.lots_set.filter(active=True).exclude(key_location='')
                     .values_list('key_location', flat=True))
    return places.most_common(1)[0][0] if places else ''


def batch_plan(lot_type, count, base_name, base_short, sub_counts):
    """Noms proposes pour `count` lots de `lot_type` et leurs sous-lots : « B+ 3 », « Sac de soin B+ 3 »…

    La numerotation reprend apres le plus grand numero deja pris par un lot actif nomme « <base> <n> »."""
    pattern = re.compile(re.escape(name_key(base_name)) + r' (\d+)$')
    taken = [int(match.group(1)) for lot in Lots.objects.filter(active=True).only('name')
             if (match := pattern.match(name_key(lot.name)))]
    start = max(taken, default=0) + 1
    sub_types = {sub.type: sub for sub in LotType.objects.filter(type__in=[code for code, n in sub_counts if n > 0])}
    key_locations = {code: usual_key_location(sub) for code, sub in sub_types.items()}
    plan = []
    for index in range(start, start + count):
        name = f'{base_name} {index}'
        short = f'{base_short} {index}'[:16]
        sub_lots = []
        for code, number in sub_counts:
            sub = sub_types.get(code)
            for rank in range(1, number + 1 if sub else 1):
                label = sub.name if number == 1 else f'{sub.name} {rank}'
                sub_lots.append({'lot_type': code, 'lot_type_name': sub.name, 'name': f'{label} {name}',
                                 'name_short': f'{label} {short}'[:16], 'key_location': key_locations[code]})
        plan.append({'name': name, 'name_short': short, 'key_location': usual_key_location(lot_type),
                     'sub_lots': sub_lots})
    return plan


def batch_lot_type(code):
    """Type de lot d'une creation groupee : ni archive, ni lot unique, ni rangement du stock."""
    lot_type = get_object_or_404(LotType, type=str(code or ''))
    if lot_type.archived:
        raise ApiError(f"Le type de lot {lot_type.name} est archivé")
    if lot_type.unique or lot_type.storage:
        raise ApiError(f"{lot_type.name} est un " + ("rangement du stock" if lot_type.storage else "lot unique")
                       + " : il n'existe qu'en un exemplaire")
    return lot_type


@api_view(['POST'])
@handle_errors
def lots_batch(request):
    """Creation de plusieurs lots d'un type, avec leurs sous-lots, en une fois.

    `preview` : renvoie les sous-lots proposes (`sub_types`) et les noms prevus (`lots`) sans rien creer, a partir
    de `count`, `name` (base des noms, le nom du type par defaut), `name_short` et `sub_lots` ([{lot_type, count}],
    les sous-lots proposes par defaut si absent). Sans `preview` : cree les `lots` donnes (noms eventuellement
    corriges a la main), tous ou aucun."""
    require_front(request, GESTION)
    data = request.data
    lot_type = batch_lot_type(data.get('lot_type'))
    if data.get('preview'):
        sub_types = batch_sub_types(lot_type)
        try:
            count = int(data.get('count', 1))
        except (TypeError, ValueError):
            raise ApiError("Nombre de lots invalide")
        if not 1 <= count <= BATCH_MAX:
            raise ApiError(f"Entre 1 et {BATCH_MAX} lots à la fois")
        base_name = str(data.get('name', '') or lot_type.name).strip()[:56] or lot_type.name
        base_short = str(data.get('name_short', '') or base_name).strip()[:12]
        wanted = data.get('sub_lots')
        if isinstance(wanted, list):
            sub_counts = []
            for row in wanted:
                if not isinstance(row, dict):
                    raise ApiError("Sous-lots invalides")
                try:
                    number = max(0, min(int(row.get('count', 0)), 9))
                except (TypeError, ValueError):
                    raise ApiError("Nombre de sous-lots invalide")
                sub_counts.append((str(row.get('lot_type', '')), number))
        else:
            sub_counts = [(row['lot_type'], row['count']) for row in sub_types]
        for code, number in sub_counts:
            if number:
                batch_lot_type(code)
        return Response({'sub_types': sub_types, 'lots': batch_plan(lot_type, count, base_name, base_short,
                                                                    sub_counts)})
    wanted = data.get('lots')
    if not isinstance(wanted, list) or not wanted:
        raise ApiError("Aucun lot à créer")
    if len(wanted) > BATCH_MAX:
        raise ApiError(f"Entre 1 et {BATCH_MAX} lots à la fois")
    who = operation_identity(request, data)[:32]
    sub_type_cache = {}
    seen = set()
    existing = list(Lots.objects.filter(active=True).only('name', 'active'))

    def clean(row, row_type):
        if not isinstance(row, dict):
            raise ApiError("Lots invalides")
        name = str(row.get('name', '')).strip()[:64]
        if not name:
            raise ApiError("Nom obligatoire")
        if name_key(name) in seen:
            raise ApiError(f"Le nom « {name} » est donné deux fois", status.HTTP_409_CONFLICT)
        seen.add(name_key(name))
        ensure_name_free(existing, name, 'Le lot')
        return Lots(lot_type=row_type, name=name, name_short=str(row.get('name_short', '') or name).strip()[:16],
                    key_location=str(row.get('key_location', '')).strip()[:128], created_by=who)

    planned = []
    for row in wanted:
        lot = clean(row, lot_type)
        children = []
        for sub_row in (row.get('sub_lots') or []) if isinstance(row, dict) else []:
            code = str(sub_row.get('lot_type', '')) if isinstance(sub_row, dict) else ''
            if code not in sub_type_cache:
                sub_type_cache[code] = batch_lot_type(code)
            children.append(clean(sub_row, sub_type_cache[code]))
        planned.append((lot, children))
    parent = parse_parent(planned[0][0], data.get('parent'))
    created = []
    with transaction.atomic():
        for lot, children in planned:
            lot.parent = parent
            lot.save()
            journal.record(OperationKind.LOT, who, f'lot créé ({lot_type.name})'
                           + (f', rangé dans {parent.name}' if parent else ''), lot=lot)
            created.append(lot)
            for child in children:
                child.parent = lot
                child.save()
                journal.record(OperationKind.LOT, who, f'lot créé ({child.lot_type.name}), rangé dans {lot.name}',
                               lot=child)
                created.append(child)
    return Response({'lots': [ser.lot_dict(lot, local=True, with_items=True) for lot in created]},
                    status=status.HTTP_201_CREATED)


def parse_parent(lot, value):
    """Lot parent demande (id, ou vide pour un lot independant). Refuse un lot archive et les boucles.

    Le stock est le lot global de tous les rangements du stock : un rangement est range dans le stock (par defaut)
    ou dans un autre rangement, un lot ordinaire jamais dans un rangement, et le stock lui-meme dans aucun lot."""
    if value in (None, ''):
        return default_parent(lot)
    if lot.is_stock:
        raise ApiError("Le stock ne peut pas être rangé dans un autre lot")
    parent = get_object_or_404(Lots.objects.select_related('lot_type'), id=str(value))
    if not parent.active:
        raise ApiError(f"Le lot {parent.name} est archivé")
    if lot.id and (parent.id == lot.id or any(sub.id == parent.id for sub in lot.descendants())):
        raise ApiError("Un lot ne peut pas être rangé dans lui-même ou dans un de ses sous-lots")
    if lot.lot_type.storage and not parent.lot_type.storage:
        raise ApiError("Un rangement du stock est rangé dans le stock ou dans un autre rangement")
    if not lot.lot_type.storage and parent.lot_type.storage:
        raise ApiError(f"{parent.name} est un rangement du stock : seuls des rangements peuvent y être rangés")
    return parent


def check_optional_lot(lot):
    """Un sous-lot facultatif (tout son contenu l'est) : range dans un lot, pas un rangement du stock."""
    if lot.lot_type.storage:
        raise ApiError("Un rangement du stock ne peut pas être facultatif")
    if lot.parent is None:
        raise ApiError("Seul un sous-lot peut être facultatif : rangez ce lot dans un autre lot d'abord")


def default_parent(lot):
    """Lot parent d'un lot independant : le stock pour un rangement du stock, aucun sinon."""
    return Lots.stock() if lot.lot_type.storage and not lot.is_stock else None


@api_view(['PATCH'])
@handle_errors
def lot_update(request, lot_id):
    require_front(request, GESTION)
    lot = get_object_or_404(Lots.objects.select_related('lot_type', 'parent'), id=lot_id)
    data = request.data
    if 'name' in data:
        name = str(data['name']).strip()[:64]
        if not name:
            raise ApiError("Nom obligatoire")
        if name_key(name) != name_key(lot.name):
            ensure_name_free(Lots.objects.filter(active=True).exclude(id=lot.id), name, 'Le lot')
        lot.name = name
    if 'name_short' in data:
        lot.name_short = str(data['name_short'])[:16]
    if 'parent' in data:
        lot.parent = parse_parent(lot, data['parent'])
    if 'key_location' in data:
        lot.key_location = str(data['key_location']).strip()[:128]
    if 'optional' in data:
        lot.optional = bool(data['optional'])
    if lot.optional and 'optional' in data or lot.optional and 'parent' in data:
        check_optional_lot(lot)
    if 'active' in data:
        active = bool(data['active'])
        if not active and lot.children.filter(active=True).exists():
            raise ApiError("Ce lot contient des sous-lots actifs : archivez-les ou retirez-les du lot d'abord")
        if active and not lot.active:
            if lot.lot_type.archived:
                raise ApiError(f"Le type de lot {lot.lot_type.name} est archivé : désarchivez-le d'abord")
            ensure_name_free(Lots.objects.filter(active=True).exclude(id=lot.id), lot.name, 'Le lot')
        if not active and lot.is_stock:
            raise ApiError("Le stock ne peut pas être archivé")
        if active and lot.parent is not None and not lot.parent.active:
            lot.parent = default_parent(lot)  # le lot parent a ete archive entre-temps : le lot redevient independant
        lot.active = active
    old_name = Lots.objects.filter(id=lot.id).values_list('name', flat=True).first()
    lot.save()
    changes = []
    if 'name' in data and old_name != lot.name:
        changes.append(f'renommé (ancien nom : {old_name})')
    if 'name_short' in data:
        changes.append('nom court')
    if 'parent' in data:
        changes.append(f'rangé dans {lot.parent.name}' if lot.parent else 'lot indépendant')
    if 'key_location' in data:
        changes.append("emplacement de l'étiquette clé")
    if 'optional' in data:
        changes.append('facultatif à la vérif' if lot.optional else 'obligatoire à la vérif')
    if 'active' in data:
        changes.append('actif' if lot.active else 'archivé')
    journal.record(OperationKind.LOT, front_identity(request), 'lot modifié : ' + (', '.join(changes) or 'aucun changement'),
                   lot=lot, fields=sorted(data.keys()))
    return Response(ser.lot_dict(lot, local=True, with_items=True))


@api_view(['POST'])
@handle_errors
def lot_seal(request, lot_id):
    require_front(request, GESTION)
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    identity = operation_identity(request)
    services.seal_lot(lot, identity, str(request.data.get('seal_number', '')).strip(), bool(request.data.get('force')))
    return Response(ser.lot_dict(lot, local=True, with_items=True))


@api_view(['POST'])
@handle_errors
def lot_rotate_key(request, lot_id):
    require_front(request, GESTION)
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    lot.rotate_key()
    lot.save(update_fields=['verif_key', 'verif_key_expires', 'key_expiry_stage'])
    journal.record(OperationKind.LOT_KEY, front_identity(request), 'étiquette clé renouvelée (ancienne invalidée)',
                   lot=lot)
    notifications.notify_admins('lot_key_renewed', 'étiquette de lot renouvelée',
                                f'étiquette clé du lot {lot.name} renouvelée par {short_name(front_user(request).matricule)}')
    return Response(ser.lot_dict(lot, local=True, with_items=True))


@api_view(['GET'])
@handle_errors
def lot_verifs(request, lot_id):
    require_front(request, GESTION)
    lot = get_object_or_404(Lots, id=lot_id)
    return Response([ser.verif_dict(verif) for verif in Verifs.objects.filter(lot=lot).order_by('-datetime')[:100]])


@api_view(['GET'])
@handle_errors
def operations(request):
    """Journal des operations, du plus recent au plus ancien (fenetre Journal du poste, roles gestion et admin).

    Filtres : kind (types separes par des virgules), by (identite stockee, ex M:1234), lot (avec ses sous-lots, archives
    compris, sauf sub=0), since / until (dates AAAA-MM-JJ incluses), q (texte cherche dans le resume, le nom du lot,
    le nom ou le matricule de la personne). Pages : before (id de la derniere ligne recue), limit.
    facets=1 ajoute les personnes presentes dans le journal et les types d'operations (listes des filtres)."""
    require_front(request, GESTION)
    params = request.query_params
    queryset = Operation.objects.order_by('-at', '-id')
    if params.get('kind'):
        kinds = [kind for kind in params['kind'].split(',') if kind]
        unknown = sorted(set(kinds) - set(OperationKind.values))
        if unknown:
            raise ApiError("Type d'opération inconnu : " + ', '.join(unknown))
        queryset = queryset.filter(kind__in=kinds)
    if params.get('by'):
        queryset = queryset.filter(by=params['by'])
    if params.get('lot'):
        lot = get_object_or_404(Lots, id=params['lot'])
        queryset = queryset.filter(lot_id__in=[lot.id] if params.get('sub') == '0' else lot_and_sub_lot_ids(lot))
    since = parse_date(params.get('since'), 'since')
    until = parse_date(params.get('until'), 'until')
    if since:
        queryset = queryset.filter(at__date__gte=since)
    if until:
        queryset = queryset.filter(at__date__lte=until)
    text = str(params.get('q', '')).strip()
    if text:
        people = Secouristes.objects.all()
        for word in text.split():
            people = people.filter(Q(nom__icontains=word) | Q(prenom__icontains=word) | Q(matricule__icontains=word))
        identities = [f'{SOURCE_VERIFIED}:{matricule}' for matricule in people.values_list('matricule', flat=True)]
        queryset = queryset.filter(Q(summary__icontains=text) | Q(lot_name__icontains=text) | Q(by__icontains=text)
                                   | Q(by__in=identities))
    if params.get('before'):
        last = Operation.objects.filter(id=parse_int(params['before'], 'before')).first()
        if last is not None:
            queryset = queryset.filter(Q(at__lt=last.at) | Q(at=last.at, id__lt=last.id))
    limit = parse_int(params.get('limit', 200), 'limit', 1, 1000)
    rows = list(queryset[:limit + 1])
    names = journal.names(row.by for row in rows)
    response = {'operations': [ser.operation_dict(row, names) for row in rows[:limit]], 'more': len(rows) > limit}
    if params.get('facets') == '1':
        identities = set(Operation.objects.exclude(by='').values_list('by', flat=True).distinct())
        everyone = journal.names(identities)
        response['people'] = sorted(
            ({'by': identity, 'name': everyone[identity], 'verified': identity.startswith(SOURCE_VERIFIED + ':')}
             for identity in identities),
            key=lambda person: (not person['verified'], person['name'].lower()),
        )
        response['kinds'] = [{'kind': value, 'label': label} for value, label in OperationKind.choices]
    return Response(response)


def lot_and_sub_lot_ids(lot):
    """Le lot et tous ses sous-lots, archives compris (l'historique d'un sous-lot archive reste celui du lot)."""
    by_parent = {}
    for lot_id, parent_id in Lots.objects.filter(parent__isnull=False).values_list('id', 'parent_id'):
        by_parent.setdefault(parent_id, []).append(lot_id)
    result, pending = [], [lot.id]
    while pending:
        current = pending.pop()
        if current not in result:
            result.append(current)
            pending.extend(by_parent.get(current, []))
    return result


def admin_count():
    return Secouristes.objects.filter(role=Role.ADMIN, active=True, key_expires__gte=timezone.localdate()).count()


def admin_exists():
    return admin_count() > 0


@api_view(['GET'])
def setup(request):
    """Etat de premiere configuration : le front propose de creer un responsable s'il n'y en a aucun
    avec un badge valide (premiere installation, ou tous les badges responsables expires)."""
    admins = admin_count()
    return Response({
        'users': Secouristes.objects.count(),
        'admins': admins,
        'needs_admin': admins == 0,
    })


@api_view(['GET', 'POST'])
@handle_errors
def users(request):
    # premier administrateur (aucun admin avec un badge valide) : le poste le cree sans etre connecte
    first_admin = request.method == 'POST' and not admin_exists()
    if not first_admin:
        require_front(request, ADMIN)
    if request.method == 'GET':
        users_list = (Secouristes.objects.select_related('pin_contact').annotate(push_device_count=Count('push_subscriptions'))
                      .order_by('nom', 'prenom'))
        pin_roles = ServerSettings.get().pin_required_roles  # lu une fois pour toute la liste
        for user in users_list:
            user.server_pin_required_roles = pin_roles
        return Response([ser.user_dict(user, local=True) for user in users_list])
    data = request.data
    matricule = str(data.get('matricule', '')).strip()
    if not MATRICULE_RE.match(matricule):
        raise ApiError("Matricule invalide (1 a 16 caracteres alphanumeriques)")
    if Secouristes.objects.filter(matricule=matricule).exists():
        raise ApiError(f"Le matricule {matricule} existe deja", status.HTTP_409_CONFLICT)
    nom = str(data.get('nom', '')).strip()
    prenom = str(data.get('prenom', '')).strip()
    if not nom or not prenom:
        raise ApiError("Nom et prenom obligatoires")
    role = Role.ADMIN if first_admin else parse_role(data) or Role.NORMAL
    user = Secouristes(matricule=matricule, nom=nom[:32], prenom=prenom[:32], role=role)
    if 'fonction' in data:
        user.fonction = parse_fonction(data['fonction'])
    if data.get('pin_mode'):
        user.set_pin_mode(str(data['pin_mode']))
    if data.get('pin'):
        user.set_pin(data['pin'])  # sinon, un admin choisira son PIN a sa premiere connexion
    if 'pin_at_login' in data:
        user.set_pin_at_login(data['pin_at_login'])
    if data.get('pin_contact'):
        user.pin_contact = parse_pin_contact(user, data['pin_contact'])
    user.renew_key()
    with transaction.atomic():
        apply_default_contact(user, data)
        user.save()
        by = f'{SOURCE_VERIFIED}:{user.matricule}' if first_admin else front_identity(request)
        journal.record(OperationKind.USER, by, f'compte créé : {user} ({user.matricule}), rôle {user.get_role_display()}',
                       matricule=user.matricule, role=user.role)
    data = ser.user_dict(user, local=True)
    if first_admin:
        data['session'] = session_token(user)  # le poste connecte directement le nouvel administrateur
    return Response(data, status=status.HTTP_201_CREATED)


@api_view(['GET', 'PATCH'])
@handle_errors
def user_detail(request, matricule):
    require_front(request, ADMIN)
    user = get_object_or_404(Secouristes, matricule=matricule)
    if request.method == 'PATCH':
        data = request.data
        for field in ('nom', 'prenom'):
            if field in data:
                setattr(user, field, str(data[field]).strip()[:32])
        if 'fonction' in data:
            user.fonction = parse_fonction(data['fonction'])
        role = parse_role(data)
        if role:
            user.role = role
            if role == Role.ADMIN and user.pin_mode == PinMode.DISABLED:
                user.pin_mode = PinMode.DEFAULT  # PIN toujours obligatoire pour un admin
        if 'pin_mode' in data:
            user.set_pin_mode(str(data['pin_mode'] or ''))  # 'disabled' supprime le PIN (refuse pour un admin)
        if 'active' in data:
            user.active = bool(data['active'])
        if 'pin' in data:
            user.set_pin(data['pin'])  # '' supprime le PIN (refuse pour un admin)
        if 'pin_at_login' in data:
            user.set_pin_at_login(data['pin_at_login'])  # « Demander a chaque connexion » (impose pour un admin)
        if data.get('pin_reset'):
            user.reset_pin()  # deblocage : nouveau PIN choisi a la prochaine connexion
        if 'pin_contact' in data:
            user.pin_contact = parse_pin_contact(user, data['pin_contact'])
        if 'push_disabled' in data:
            user.push_disabled = parse_push_disabled(data['push_disabled'])
        if 'web_prefs' in data:
            user.web_prefs = {**(user.web_prefs or {}), **parse_web_prefs(data['web_prefs'])}
        if 'scanner_volume' in data:  # vide : volume par defaut du serveur
            value = data['scanner_volume']
            user.scanner_volume = None if value in (None, '') else parse_scanner_volume(value)
        with transaction.atomic():
            apply_default_contact(user, data)
            user.save()
            journal.record(OperationKind.USER, front_identity(request),
                           f'compte modifié : {user} ({user.matricule}) : {user_changes(data, user)}',
                           matricule=user.matricule, fields=sorted(data.keys()))
    return Response(ser.user_dict(user, local=True))


def parse_fonction(value):
    """Role dans l'equipe (texte libre, 32 caracteres) : vide, c'est le role par defaut (Secouriste)."""
    return ' '.join(str(value or '').split())[:32] or DEFAULT_FONCTION


USER_FIELDS = {'nom': 'nom', 'prenom': 'prénom', 'fonction': 'rôle', 'pin': 'PIN', 'pin_reset': 'PIN réinitialisé', 'pin_mode': 'mode du PIN', 'pin_at_login': 'PIN demandé à chaque connexion',
               'pin_contact': 'admin à contacter', 'push_disabled': 'notifications', 'default_contact': 'contact par défaut',
               'web_prefs': 'réglages du web',
               'scanner_volume': 'volume de la douchette'}


def user_changes(data, user):
    """Changements d'un compte pour le journal (jamais la valeur du PIN)."""
    changes = []
    if parse_role(data):
        changes.append(f'rôle {user.get_role_display()}')
    if 'active' in data:
        changes.append('actif' if user.active else 'désactivé')
    fields = changed_fields(data, USER_FIELDS)
    if fields:
        changes.append(fields)
    return ', '.join(changes) or 'aucun changement'


def parse_push_disabled(value):
    """Types de notifications web coupes pour un utilisateur (liste de noms de PUSH_TYPES)."""
    if not isinstance(value, list) or not all(isinstance(name, str) and name in PUSH_TYPES for name in value):
        raise ApiError("push_disabled : liste de types de notifications attendue (" + ', '.join(PUSH_TYPES) + ")")
    return sorted(set(value))


def parse_pin_contact(user, value):
    """Admin a contacter si le PIN est bloque (matricule d'un administrateur actif, vide = aucun)."""
    if value in (None, ''):
        return None
    contact = Secouristes.objects.filter(matricule=str(value)).first()
    if contact is None or contact.role != Role.ADMIN or not contact.active:
        raise ApiError("L'admin à contacter doit être un administrateur actif")
    return contact


@api_view(['POST'])
@handle_errors
def user_renew_key(request, matricule):
    require_front(request, ADMIN)
    user = get_object_or_404(Secouristes, matricule=matricule)
    user.renew_key()
    user.save(update_fields=['key_hash', 'key_expires', 'key_expiry_stage'])
    journal.record(OperationKind.USER, front_identity(request), f'badge renouvelé : {user} ({user.matricule})',
                   matricule=user.matricule)
    notifications.notify_admins('badge_renewed', 'badge renouvelé',
                                f'badge de {short_name(user.matricule)} renouvelé par {short_name(front_user(request).matricule)}')
    return Response(ser.user_dict(user, local=True))


# ----------------------------------------------------------------------------------------------------------------------
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
        if 'scanner_volume_default' in request.data:
            settings_row.scanner_volume_default = parse_scanner_volume(request.data['scanner_volume_default'])
            changes.append(f'volume de la douchette par défaut : {settings_row.scanner_volume_default}')
        if 'web_barcodes' in request.data:
            settings_row.web_barcodes = bool(request.data['web_barcodes'])
            changes.append('lecture des codes-barres sur le web ' + ('activée' if settings_row.web_barcodes else 'désactivée'))
        if 'antenna_name' in request.data:
            settings_row.antenna_name = ' '.join(str(request.data['antenna_name'] or '').split())[:64]
            changes.append(f"nom de l'antenne : {settings_row.antenna_name or '(vide)'}")
        settings_row.save()
        journal.record(OperationKind.SETTINGS, front_identity(request), ', '.join(changes) or 'aucun changement',
                       declared_identity=settings_row.declared_identity,
                       pin_required_roles=settings_row.pin_required_roles,
                       scanner_volume_default=settings_row.scanner_volume_default,
                       web_barcodes=settings_row.web_barcodes)
    return Response(ser.server_settings_dict(settings_row))


def parse_pin_roles(value):
    """Roles du reglage « code obligatoire » (les admins le sont toujours), dans l'ordre des roles."""
    if not isinstance(value, list) or any(role not in PIN_OPTIONAL_ROLES + (Role.ADMIN,) for role in value):
        raise ApiError('Code obligatoire : liste de rôles parmi ' + ', '.join(PIN_OPTIONAL_ROLES))
    return [role for role in PIN_OPTIONAL_ROLES if role in value]


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


def parse_scanner_volume(value):
    try:
        volume = int(value)
    except (TypeError, ValueError):
        volume = -1
    if volume not in SCANNER_VOLUMES:
        raise ApiError("Volume de la douchette : 0, 2, 4 ou 8")
    return volume


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


def parse_web_prefs(value):
    """Reglages du front web a changer ({nom: booleen}, noms de models.WEB_PREFS) : les autres restent tels quels."""
    if not isinstance(value, dict) or not all(name in WEB_PREFS and isinstance(flag, bool) for name, flag in value.items()):
        raise ApiError("web_prefs : réglages attendus (" + ', '.join(WEB_PREFS) + ") avec true ou false")
    return value


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
    if 'pin_at_login' in data and not data.get('pin') and not data.get('disable'):
        if not user.has_pin:
            raise ApiError("Vous n'avez pas de code PIN")
        if user.pin_at_login_locked and not data['pin_at_login']:
            raise ApiError("Le PIN est demandé à chaque connexion pour un administrateur", status.HTTP_403_FORBIDDEN)
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
        if not PIN_RE.match(pin):
            raise ApiError('Le PIN doit comporter 4 à 8 chiffres')
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


# ----------------------------------------------------------------------------------------------------------------------
# Telephone utilise comme douchette (voir remote_scanner.py)
# ----------------------------------------------------------------------------------------------------------------------

def _scanner_session_dict(session):
    data = session.status()
    data['url'] = ser.public_url('scanner', s=session.id, k=session.key)
    return data


@api_view(['POST'])
@handle_errors
def remote_scanner_sessions(request):
    """Cree une session : le front affiche le QR code de `url`, puis se connecte a /ws/scanner/front?s=ID."""
    if not scanner_hub.enabled:
        raise ApiError("Le téléphone-douchette nécessite le serveur `manage.py serve` (WebSockets)",
                       status.HTTP_503_SERVICE_UNAVAILABLE)
    minutes = parse_int(request.data.get('timeout_minutes', 5), 'timeout_minutes', 1, 24 * 60)
    session = scanner_hub.create(minutes * 60)
    return Response(_scanner_session_dict(session), status=status.HTTP_201_CREATED)


@api_view(['GET', 'DELETE'])
@handle_errors
def remote_scanner_session(request, session_id):
    session = scanner_hub.get(session_id)
    if session is None:
        raise ApiError('Session fermée', status.HTTP_404_NOT_FOUND)
    if request.method == 'DELETE':
        scanner_hub.close(session_id, 'fermée depuis le poste')
        return Response({'closed': True})
    return Response(_scanner_session_dict(session))


@api_view(['GET'])
def remote_scanner_check(request):
    """Public (telephone) : la session existe-t-elle encore ? Exige la cle du QR code."""
    session = scanner_hub.check_key(request.query_params.get('s', ''), request.query_params.get('k', ''))
    if session is None:
        return error('Session fermée : scannez un nouveau QR code sur le poste', status.HTTP_404_NOT_FOUND)
    return Response({'open': True, 'front_connected': session.front is not None})


# ----------------------------------------------------------------------------------------------------------------------
# Listes de materiel (checklists.py) : utilisateur connecte (badge ou session), droits de modification par liste
# ----------------------------------------------------------------------------------------------------------------------

def _checklist(checklist_id):
    from .models import Checklist
    return get_object_or_404(Checklist.objects.select_related('owner').prefetch_related(
        'members', 'lines__lot__lot_type', 'lines__item_type', 'scans__item__pack__item_type', 'scans__lot'),
        id=checklist_id)


def _checklist_call(action):
    try:
        return action()
    except PermissionError as exc:
        raise ApiError(str(exc), status.HTTP_403_FORBIDDEN)


@api_view(['POST'])
@handle_errors
def checklists_list(request):
    """Listes : {"user", "show": "mine" (les siennes et celles qui lui sont attribuees) | "templates" (les modeles) |
    "all" (sans les modeles)} ; modeles et toutes les listes pour les chefs, la gestion et les admins."""
    from . import checklists
    user = connected_user(request)
    show = str(request.data.get('show') or 'mine')
    rows = [checklists.checklist_dict(row, user, full=False) for row in checklists.visible(user, show)[:300]]
    # a traiter en haut : listes sorties (retour a faire), puis a preparer, et les listes rentrees
    rank = {'validated': 0, 'draft': 1}
    rows.sort(key=lambda row: rank.get(row['status'], 2))
    can_make = checklists.can_make_lists(user)
    return Response({'lists': rows, 'can_see_all': can_make, 'can_create': can_make})


@api_view(['POST'])
@handle_errors
def checklists_users(request):
    """Recherche de secouristes par prenom pour les attribuer a une liste : {"user", "q"} (chefs, gestion, admins)."""
    from . import checklists
    user = connected_user(request)
    return Response({'users': _checklist_call(lambda: checklists.search_users(user, request.data.get('q')))})


@api_view(['POST'])
@handle_errors
def checklists_options(request):
    """Choix de l'editeur de liste : lots actifs (hors rangements du stock) et types d'items non archives."""
    from . import checklists
    from .models import ItemType
    user = connected_user(request)
    if not checklists.can_make_lists(user):
        raise ApiError('Réservé aux chefs, à la gestion et aux admins', status.HTTP_403_FORBIDDEN)
    lots = (Lots.objects.select_related('lot_type').filter(active=True, lot_type__storage=False)
            .order_by('name'))
    types = ItemType.objects.filter(archived=False).order_by('name')
    return Response({
        'lots': [{'id': lot.id, 'name': lot.name, 'lot_type_name': lot.lot_type.name} for lot in lots],
        'types': [{'type': row.type, 'name': row.name, 'consumable': row.consumable} for row in types],
    })


@api_view(['POST'])
@handle_errors
def checklists_create(request):
    """Nouvelle liste : {"user", "name", "note", "template", "lines": [{"lot"} | {"type", "quantity"} | {"task"}]}."""
    from . import checklists
    user = connected_user(request)
    data = request.data
    checklist = _checklist_call(lambda: checklists.create(user, data.get('name'), data.get('note', ''),
                                                          data.get('template', False), data.get('lines') or []))
    return Response(checklists.checklist_dict(_checklist(checklist.id), user), status=status.HTTP_201_CREATED)


@api_view(['POST'])
@handle_errors
def checklist_action(request, checklist_id, action=''):
    """Une liste : {"user"} lit ; update {name, note, template, lines} ; duplicate {name, template} ; delete ;
    scan / unscan {item | lot} ; check {line, done} (tache libre) ; validate {partial} ; return-scan {item | lot} ;
    close-return ; members {add: [matricules], remove: [matricules]}. A la lecture, {"open": true} (liste ouverte par
    son lien ou son QR code) l'ajoute aux listes d'un secouriste."""
    from . import checklists
    user = connected_user(request)
    checklist = _checklist(checklist_id)
    data = request.data
    _checklist_call(lambda: checklists.access(checklist, user, join=not action and bool(data.get('open'))))
    iid, lot = str(data.get('item') or ''), str(data.get('lot') or '')
    message = ''
    extra = {}
    if action == 'update':
        _checklist_call(lambda: checklists.update(checklist, user, data))
    elif action == 'members':
        add, remove = data.get('add') or [], data.get('remove') or []
        if not isinstance(add, list) or not isinstance(remove, list):
            raise ApiError('add, remove : listes de matricules attendues')
        _checklist_call(lambda: checklists.set_members(checklist, user, add, remove))
    elif action == 'duplicate':
        copy = _checklist_call(lambda: checklists.duplicate(checklist, user, data.get('name'),
                                                            data.get('template', False)))
        return Response(checklists.checklist_dict(_checklist(copy.id), user), status=status.HTTP_201_CREATED)
    elif action == 'delete':
        _checklist_call(lambda: checklists.delete(checklist, user))
        return Response({'deleted': checklist_id})
    elif action in ('scan', 'unscan', 'return-scan'):
        if not iid and not lot:
            raise ApiError('Item ou lot attendu')
        if action == 'scan':
            message = checklists.scan(checklist, user, iid, lot)
        elif action == 'unscan':
            checklists.unscan(checklist, user, iid, lot)
        else:
            message = checklists.return_scan(checklist, user, iid, lot)
    elif action == 'check':
        try:
            line = int(data.get('line'))
        except (TypeError, ValueError):
            raise ApiError('line : identifiant de la tâche attendu')
        message = checklists.check_task(checklist, user, line, data.get('done', True) is not False)
    elif action == 'validate':
        checklists.validate(checklist, user, bool(data.get('partial')))
    elif action == 'close-return':
        extra = checklists.close_return(checklist, user)
    elif action:
        raise ApiError('Action inconnue', status.HTTP_404_NOT_FOUND)
    return Response({**checklists.checklist_dict(_checklist(checklist_id), user), 'message': message, **extra})


# ----------------------------------------------------------------------------------------------------------------------
# Etiquettes d'items demandees depuis le web (admins) et imprimees au poste (voir label_prints.py)
# ----------------------------------------------------------------------------------------------------------------------

def _label_print_call(function, *args):
    try:
        return function(*args)
    except label_prints.PrintError as exc:
        raise ApiError(exc.message, exc.code)


@api_view(['POST'])
@handle_errors
def label_prints_create(request):
    """Web, admins : {"user", "type", "count", "peremption"} → demande envoyee aux postes (fenetre « Impressions en
    attente »). Les items ne sont crees qu'a l'impression."""
    user = connected_user(request, ADMIN)
    data = request.data
    count = parse_int(data.get('count', 1), 'count', 1, label_prints.MAX_COUNT)
    row = _label_print_call(label_prints.create, f'{SOURCE_VERIFIED}:{user.matricule}', str(data.get('type', '')),
                            count, parse_date(data.get('peremption'), 'peremption'))
    return Response(label_prints.print_dict(row), status=status.HTTP_201_CREATED)


@api_view(['POST'])
@handle_errors
def label_prints_list(request):
    """Web, admins : types d'items proposes et demandes d'impression (en attente, puis des 7 derniers jours)."""
    connected_user(request, ADMIN)
    return Response(label_prints.web_list())


@api_view(['POST'])
@handle_errors
def label_print_cancel(request, print_id):
    """Web, admins : annule une demande pas encore imprimee."""
    user = connected_user(request, ADMIN)
    row = _label_print_call(label_prints.cancel, print_id, f'{SOURCE_VERIFIED}:{user.matricule}')
    return Response(label_prints.print_dict(row))


def require_poste(request):
    """Routes des postes qui ne demandent personne de connecte : la machine suffit (API locale ou cle de front)."""
    if not is_front(request):
        raise ApiError("Réservé aux postes", status.HTTP_403_FORBIDDEN)


@api_view(['GET'])
@handle_errors
def label_prints_poste(request):
    """Poste, meme sans personne de connecte : impressions en attente et imprimees dans les dernieres 24 h."""
    require_poste(request)
    return Response(label_prints.poste_list())


@api_view(['POST'])
@handle_errors
def label_prints_send(request):
    """Front d'un autre ordinateur (API distante, cle de front) : « Imprimer » envoie au poste les etiquettes deja
    rendues {"title", "labels": [{description, width, height, ppm, bits}]}. Le poste, seul front branche sur l'API
    locale, les imprime depuis sa fenetre « Impressions en attente »."""
    require_poste(request)
    row = _label_print_call(label_prints.create_labels, front_identity(request),
                            getattr(request, 'qrprotec_front', ''), request.data.get('title', ''),
                            request.data.get('labels'))
    return Response(label_prints.print_dict(row), status=status.HTTP_201_CREATED)


@api_view(['GET'])
@handle_errors
def label_print_detail(request, print_id):
    """Poste : demande complete (items crees ou images), pour « Réimprimer »."""
    require_poste(request)
    return Response(_label_print_call(label_prints.detail, print_id))


@api_view(['POST'])
@handle_errors
def label_print_print(request, print_id):
    """Poste, meme sans personne de connecte : « Lancer l'impression ». Cree les items (reception au nom de l'admin
    qui les a demandes) et repond la demande avec ses items, dont le poste imprime les etiquettes."""
    require_poste(request)
    return Response(_label_print_call(label_prints.print_now, print_id, getattr(request, 'qrprotec_front', '')))
