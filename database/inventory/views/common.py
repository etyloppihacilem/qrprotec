# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          views/common.py
#       -\-    _|__
#        |\___/  . \        Created on 29 Sep. 2026 at 14:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""API JSON : aides communes a toutes les vues (erreurs, droits, identite, cles).

Les vues sont reparties dans le paquet views/ (public, local, notif_settings, douchette, lists, prints) ;
views/__init__.py les reexporte toutes : urls.py et les tests ecrivent toujours views.<nom>.

Deux familles de vues :
- publiques (servies sur les deux ports) : toute ecriture exige la cle de l'objet modifie
  (ex: cle du lot pour une verif), sauf si la requete arrive par l'API locale ;
- locales (servies uniquement sur le port local) : gestion complete sans cle.
"""

import functools
from collections import Counter, defaultdict
import hashlib
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
from django.utils.dateparse import parse_datetime
from rest_framework import status
from rest_framework.decorators import api_view
from rest_framework.response import Response

from .. import datacache
from .. import debug_codes
from .. import forecast
from .. import fun_stats
from .. import front_config
from .. import journal
from .. import label_prints
from .. import lot_report
from .. import lot_sheet
from .. import live
from .. import consumables
from .. import notifications
from .. import print_counts
from .. import reports
from .. import guests
from .. import serializers as ser
from .. import services
from .. import tasks
from .. import undo
from .. import verif_cache
from .. import webpush
from ..idendity import SOURCE_VERIFIED, format_identity_verified, guest_identity, identity_from_request, identity_name, short_name
from ..remote_scanner import hub as scanner_hub
from ..sessions import (  # noqa: F401 (tests)
    PASSKEY_POSTE_LOGIN_MAX_AGE, TEMP_LOGIN_MAX_AGE, TEMP_SESSION_MAX_AGE, credentials_user, passkey_session,
    session_token, session_user, session_valid, temp_login_data, temp_login_token, temp_login_user,
)
from ..models import (
    DEFAULT_FONCTION, PIN_OPTIONAL_ROLES, PUSH_TYPES, WEB_PREFS, STOCK_LOT_TYPE, ProblemReport, TYPE_LENGTH, Items, ItemsPacks, ItemStatus, ItemType, KeyExpiry, LabelStyle, LotContent, LotRequirements, Lots, LotType, LotTypeSlot,
    SlotMode,
    NotificationSettings, Operation, Passkey, OperationChange, OperationKind, PIN_RE, NEW_PIN_RE, NEW_PIN_ERROR, PIN_MIN_LENGTH, PinMode, PushSubscription, SealedPacks, Role, Secouristes, SeenWhile, StockOrder,
    SCANNER_VOLUMES, LOST_OBJECT_MESSAGE_MAX, ConsumableRef, ServerSettings, FrontConfigDownload, FrontKey, VerifItem, VerifResult, Verifs, add_months, custom_order_key, name_key, qrprotec_setting,
    hash_key, keys_match,
    storage_kind_value, MAINTENANCE,
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


def is_main_poste(request) -> bool:
    """Poste principal : front de l'API locale (meme machine que le back) ou front distant dont la cle est marquee
    « poste principal » (kiosk sur une autre machine, back sans kiosk). Il imprime les « Impressions en attente » et
    gere les ordinateurs connectes."""
    return getattr(request, 'qrprotec_poste', False)


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
    """Un item a cocher (sans etiquette) n'a ni date de peremption, ni etiquette a dechirer, ni revision ou
    desinfection (revision_months : somme des deux periodicites)."""
    if not physical and (perissable or tear_off or revision_months):
        raise ApiError("Un item à cocher n'a pas d'étiquette : ni péremption, ni étiquette à déchirer, ni révision "
                       "ou désinfection")


MAX_FILLS = 1_000_000


def apply_units(item_type, data):
    """Attentes en quantite (ItemType.is_group, unit, group, fills, default_subtype) d'un type d'item, depuis les
    champs presents dans data. Un type parent (is_group : a unites, ex : « Sérum phy » en mL, ou sans unite, ex :
    « Bandes ») n'a ni etiquette ni item a lui : ce sont ses sous-types qu'on receptionne et qu'on imprime ; un seul
    niveau (un type parent n'est le sous-type de personne). Une unite fait d'un type un type parent ; sous un type
    parent sans unite, chaque item d'un sous-type compte pour 1 (fills = 1)."""
    was_group = item_type.is_unit_group
    had_unit = item_type.unit
    if 'is_group' in data:
        item_type.is_group = bool(data['is_group'])
    if 'unit' in data:
        item_type.unit = ' '.join(str(data['unit'] or '').split())[:16]
        if item_type.unit:
            item_type.is_group = True
    if item_type.unit:
        item_type.is_group = True
    if item_type.is_group and not was_group and item_type.pk \
            and ItemsPacks.objects.filter(item_type=item_type).exists():
        raise ApiError("Des items de ce type ont déjà des étiquettes : créez un nouveau type parent (ex : « Sérum "
                       "phy » en mL, ou « Bandes ») et faites de celui-ci un de ses sous-types")
    if (had_unit and not item_type.unit or was_group and not item_type.is_group) and item_type.pk \
            and ItemType.objects.filter(group=item_type).exists():
        raise ApiError("Ce type a des sous-types : retirez-les d'abord")
    if 'group' in data:
        code = str(data['group'] or '')
        if not code:
            item_type.group = None
        else:
            group = ItemType.objects.filter(type=code).first()
            if group is None or not group.is_unit_group:
                raise ApiError("Le type parent doit être un type parent (ex : « Sérum phy » en mL, ou « Bandes »)")
            if group.pk == item_type.pk:
                raise ApiError("Un type ne peut pas être son propre sous-type")
            item_type.group = group
    if 'fills' in data:
        item_type.fills = parse_int(data['fills'], 'fills', 1, MAX_FILLS)
    if item_type.group_id and not item_type.group.unit:
        item_type.fills = 1  # type parent sans unite : un item compte pour 1
    if 'default_subtype' in data:
        code = str(data['default_subtype'] or '')
        if code and not (item_type.pk and ItemType.objects.filter(type=code, group=item_type).exists()):
            raise ApiError("Le sous-type par défaut doit être un sous-type de ce type")
        item_type.default_subtype_id = code or None
    if item_type.is_group:
        if item_type.group_id:
            raise ApiError("Un type parent ne peut pas être lui-même un sous-type (un seul niveau)")
        if not item_type.physical or item_type.tear_off or item_type.tasks or item_type.perissable:
            raise ApiError("Un type parent n'a pas d'étiquette : ni item à cocher, ni péremption, ni étiquette à "
                           "déchirer, ni tâches (réglez-les sur ses sous-types)")
    else:
        item_type.default_subtype_id = None
    if item_type.group_id and not item_type.physical:
        raise ApiError("Un item à cocher ne peut pas être le sous-type d'un type parent")


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


def is_verified(identity):
    """Identite d'un secouriste reconnu (badge, ou nom et PIN), pas un nom declare."""
    return identity.startswith(f'{SOURCE_VERIFIED}:')


def parse_scanner_volume(value):
    try:
        volume = int(value)
    except (TypeError, ValueError):
        volume = -1
    if volume not in SCANNER_VOLUMES:
        raise ApiError("Volume de la douchette : 0, 2, 4 ou 8")
    return volume


def parse_web_prefs(value):
    """Reglages du front web a changer ({nom: booleen}, noms de models.WEB_PREFS) : les autres restent tels quels."""
    if not isinstance(value, dict) or not all(name in WEB_PREFS and isinstance(flag, bool) for name, flag in value.items()):
        raise ApiError("web_prefs : réglages attendus (" + ', '.join(WEB_PREFS) + ") avec true ou false")
    return value


def _checklist_call(action):
    try:
        return action()
    except PermissionError as exc:
        raise ApiError(str(exc), status.HTTP_403_FORBIDDEN)
