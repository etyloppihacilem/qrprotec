# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          views/passkeys.py
#       -\-    _|__
#        |\___/  . \        Created on 20 Nov. 2026 at 11:30
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Passkeys du front web (voir inventory/passkeys.py) : connexion, gestion par le secouriste, suppression par un
admin. Le QR code de connexion au poste est calcule par le telephone (cle OTP remise a la connexion, voir
sessions.poste_otp). Connexion d'un poste non kiosk par la passkey du navigateur de l'ordinateur."""

from __future__ import annotations

import logging
import hashlib
import secrets
from typing import TYPE_CHECKING, Any

from .. import passkeys as webauthn
from .common import *  # noqa: F401,F403
from .public import *  # noqa: F401,F403

if TYPE_CHECKING:
    from inventory.models import Passkey, Secouristes
    from inventory.passkeys import PasskeyError
    from rest_framework.request import Request

log = logging.getLogger('inventory.passkeys')

PASSKEY_LOGIN_ERROR = "Passkey refusée : réessayez, ou connectez-vous avec votre carte"
MAX_PASSKEYS = 10


def passkey_row(passkey: Passkey) -> dict[str, Any]:
    return {
        'id': passkey.pk,
        'device': passkey.device,
        'created': ser._date(passkey.created),
        'last_used': ser._date(passkey.last_used),
    }


def passkey_rows(user: Secouristes) -> list[dict[str, Any]]:
    return [passkey_row(passkey) for passkey in user.passkeys.order_by('-created')]


def refuse(request: Request, what: str, exc: PasskeyError, matricule: str = '') -> None:
    """Refus d'une reponse WebAuthn : la raison technique part dans le journal du serveur (journalctl), pour le
    debogage ; le telephone ne recoit qu'un message general."""
    log.warning('passkey refusée (%s%s) depuis %s : %s', what, f' de {matricule}' if matricule else '',
                client_address(request), exc)


@api_view(['POST'])
@handle_errors
def passkey_login_options(request: Request) -> Response:
    """Options de navigator.credentials.get() pour se connecter avec une passkey (sans nom a taper)."""
    if request.data.get('debug_rp'):  # aide au debogage : domaine des passkeys vu par le serveur
        return Response({'rp_id': webauthn.rp_id(request)})
    return Response({'publicKey': webauthn.authentication_options(request)})


@api_view(['POST'])
@handle_errors
def passkey_login(request: Request) -> Response:
    """Connexion avec une passkey : {"credential"} (reponse de navigator.credentials.get()). Meme reponse que
    /api/auth/, avec `passkey_session` : le web propose alors le QR code de connexion au poste. Remplace le badge et le
    PIN (le telephone a verifie l'empreinte, le visage ou son code)."""
    user = passkey_user(request, request.data.get('credential'))
    return Response(login_response(request, user, passkey=True, method='passkey'))


def passkey_user(request: Request, credential: Any) -> Secouristes:
    """Secouriste d'une reponse de navigator.credentials.get() (defi de connexion), verifiee : signature, compteur,
    compte utilisable. Leve ApiError (403) sinon, essai compte pour l'adresse."""
    check_login_address(request)
    credential_id = credential.get('id') if isinstance(credential, dict) else None
    passkey = (Passkey.objects.select_related('user').filter(credential_id=str(credential_id)[:1400]).first()
               if isinstance(credential_id, str) and credential_id else None)
    if passkey is None:
        reserve_login_attempt(login_address_key(request))
        log.warning('passkey refusée (connexion) depuis %s : identifiant inconnu (passkey supprimée par un admin ?)',
                    client_address(request))
        raise ApiError("Passkey inconnue : elle a peut-être été supprimée. Connectez-vous avec votre carte",
                       status.HTTP_403_FORBIDDEN, passkey_unknown=True)
    user = passkey.user
    try:
        count = webauthn.verify_authentication(request, passkey, credential)
    except webauthn.PasskeyError as exc:
        reserve_login_attempt(login_address_key(request))
        refuse(request, 'connexion', exc, user.matricule)
        raise ApiError(PASSKEY_LOGIN_ERROR, status.HTTP_403_FORBIDDEN)
    if user.guest or not user.badge_valid():
        raise ApiError("Compte désactivé ou badge expiré : voyez le TECH", status.HTTP_403_FORBIDDEN)
    if user.pin_blocked is not None:
        raise pin_blocked_error(user)
    with transaction.atomic():
        updated = Passkey.objects.filter(pk=passkey.pk, sign_count=passkey.sign_count).update(
            sign_count=count, last_used=timezone.now())
    if not updated:  # deux connexions simultanees avec la meme signature
        raise ApiError(PASSKEY_LOGIN_ERROR, status.HTTP_403_FORBIDDEN)
    return user


def first_login_user(request: Request) -> Secouristes | None:
    """Premiere connexion au web (Hippolyte, 2026-11-21) : la passkey est proposee avant le choix du code PIN
    obligatoire. La connexion attend encore ce code : la carte seule ({"user": {"matricule", "key"}}, sans jeton de
    session) suffit alors pour creer la passkey, comme elle suffit pour choisir le code. None dans tout autre cas."""
    credentials = request.data.get('user')
    if not isinstance(credentials, dict) or credentials.get('session') or not credentials.get('key'):
        return None
    if str(request.data.get('action') or '') not in ('options', 'create'):
        return None
    user = credentials_user(credentials)
    return user if user is not None and pin_setup_pending(user) else None


@api_view(['POST'])
@handle_errors
def my_passkeys(request: Request) -> Response:
    """Passkeys de l'utilisateur connecte (web) : {"user", "action"} :
    - "list" (defaut) : ses passkeys ;
    - "options" : options de navigator.credentials.create() ;
    - "create" {"credential"} : enregistre la passkey creee (la connexion par nom et PIN ne marche plus) ;
    - "delete" {"id"} : supprime une passkey (sans passkey, le nom et le PIN remarchent).
    Jamais pour une carte d'invite. "options" et "create" marchent aussi avec la carte seule tant que le code PIN
    obligatoire n'est pas choisi (premiere connexion : passkey d'abord, voir first_login_user)."""
    user = first_login_user(request) or badge_user(request, ALL_ROLES)
    if user.guest:
        raise ApiError("Pas de passkey pour une carte d'invité", status.HTTP_403_FORBIDDEN)
    action = str(request.data.get('action') or 'list')
    by = f'{SOURCE_VERIFIED}:{user.matricule}'
    if action == 'options':
        existing = list(user.passkeys.values_list('credential_id', flat=True))
        if len(existing) >= MAX_PASSKEYS:
            raise ApiError(f"{MAX_PASSKEYS} passkeys au plus : supprimez-en une d'abord")
        return Response({'publicKey': webauthn.registration_options(request, user, existing)})
    if action == 'create':
        try:
            created = webauthn.verify_registration(request, user, request.data.get('credential'))
        except webauthn.PasskeyError as exc:
            refuse(request, 'création', exc, user.matricule)
            raise ApiError("Passkey refusée par le serveur : réessayez")
        if Passkey.objects.filter(credential_id=created['credential_id']).exists():
            raise ApiError("Cette passkey est déjà enregistrée")
        if user.passkeys.count() >= MAX_PASSKEYS:
            raise ApiError(f"{MAX_PASSKEYS} passkeys au plus : supprimez-en une d'abord")
        device = webpush.device_name(request.META.get('HTTP_USER_AGENT'))
        Passkey.objects.create(user=user, device=device, **created)
        journal.record(OperationKind.USER, by, f'passkey créée par {user} ({user.matricule}) sur {device} : plus de '
                       'connexion par nom et PIN sur le web', matricule=user.matricule)
    elif action == 'delete':
        passkey = user.passkeys.filter(pk=parse_int(request.data.get('id'), 'id', 1)).first()
        if passkey is None:
            raise ApiError("Passkey introuvable", status.HTTP_404_NOT_FOUND)
        passkey.delete()
        journal.record(OperationKind.USER, by, f'passkey {passkey.device} supprimée par {user} ({user.matricule})'
                       + ('' if user.passkeys.exists() else ' : connexion par nom et PIN rétablie'),
                       matricule=user.matricule)
    elif action != 'list':
        raise ApiError('action : list, options, create ou delete')
    return Response({'passkeys': passkey_rows(user), 'has_pin': user.has_pin})


@api_view(['POST'])
@handle_errors
def debug_passkeys(request: Request) -> Response:
    """Passkeys des secouristes depuis l'outil de debug du web (admins) : {"user", "matricule", "action"?} :
    "list" (defaut) ou "delete" (toutes ses passkeys : la connexion par nom et PIN redevient possible). Note au
    journal."""
    admin = connected_user(request, ADMIN)
    user = Secouristes.objects.filter(matricule=str(request.data.get('matricule', ''))[:16], guest=False).first()
    if user is None:
        raise ApiError("Secouriste introuvable", status.HTTP_404_NOT_FOUND)
    action = str(request.data.get('action') or 'list')
    if action == 'delete':
        count = user.passkeys.count()
        user.passkeys.all().delete()
        if count:
            journal.record(OperationKind.USER, f'{SOURCE_VERIFIED}:{admin.matricule}',
                           f'{journal.plural(count, "passkey")} de {user} ({user.matricule}) '
                           f'{"supprimées" if count > 1 else "supprimée"} par {admin} : connexion par nom et PIN '
                           'rétablie', matricule=user.matricule)
    elif action != 'list':
        raise ApiError('action : list ou delete')
    return Response({'matricule': user.matricule, 'passkeys': passkey_rows(user)})


# Connexion d'un poste par le navigateur (Hippolyte, 2026-11-27)
# ----------------------------------------------------------------------------------------------------------------------
# Un poste non kiosk (ordinateur perso, Flatpak, Mac, Windows) n'a pas acces aux passkeys de l'ordinateur : ni Bitwarden
# ni le trousseau d'Apple ne les offrent aux applis natives non signees. Il ouvre donc le navigateur sur
# poste?r=<demande> ; la personne y valide avec sa passkey (cle USB, gestionnaire, telephone), et le poste, qui
# interroge le serveur pendant ce temps avec un secret qu'il est seul a connaitre, recoit une session normale. Le
# navigateur n'est pas connecte pour autant. Un code a 4 chiffres affiche des deux cotes permet de verifier qu'on
# valide bien la demande de son propre poste.

POSTE_BROWSER_MAX_AGE = 300  # demande valable 5 min
POSTE_BROWSER_CACHE = 'qrprotec.poste-browser.'
POSTE_BROWSER_NOTIFY_EVERY = 20  # secondes entre deux notifications de demande de connexion a la meme personne


def poste_browser_key(request_id: Any) -> str:
    return POSTE_BROWSER_CACHE + hashlib.sha256(str(request_id or '').encode()).hexdigest()


def poste_browser_request(request_id: Any) -> dict[str, Any]:
    data = cache.get(poste_browser_key(request_id)) if isinstance(request_id, str) and request_id else None
    if not isinstance(data, dict):
        raise ApiError("Demande de connexion expirée : relancez « Se connecter avec une passkey » dans l'application",
                       status.HTTP_404_NOT_FOUND)
    return data


def poste_browser_left(data: dict[str, Any]) -> int:
    return max(0, int(data['expires'] - timezone.now().timestamp()))


@api_view(['POST'])
@handle_errors
def poste_browser_start(request: Request) -> Response:
    """Poste (API locale ou cle de front) : nouvelle demande de connexion par le navigateur -> {"request", "poll",
    "code", "url", "seconds", "notified"}. `url` est a ouvrir dans le navigateur de l'ordinateur ; `poll` reste dans le
    poste (auth/poste-browser/poll/).

    {"matricule"} facultatif (derniere personne connectee sur ce poste) : notification « Demande de connexion » sur ses
    appareils abonnes (Hippolyte, 2026-11-27), le clic ouvre la meme page de validation, sur le telephone par exemple.
    `notified` : nombre d'appareils prevenus. Une notification par personne toutes les 20 s au plus."""
    if not is_front(request):
        raise ApiError("Réservé aux postes", status.HTTP_403_FORBIDDEN)
    check_login_address(request)
    request_id, poll = secrets.token_urlsafe(24), secrets.token_urlsafe(32)
    code = f'{secrets.randbelow(10000):04d}'
    expires = timezone.now().timestamp() + POSTE_BROWSER_MAX_AGE
    cache.set(poste_browser_key(request_id), {
        'poll': hashlib.sha256(poll.encode()).hexdigest(), 'code': code, 'expires': expires,
        'poste': getattr(request, 'qrprotec_front', '') or 'poste principal', 'matricule': None,
    }, POSTE_BROWSER_MAX_AGE + 60)
    poste = getattr(request, 'qrprotec_front', '') or 'poste principal'
    notified = 0
    matricule = str(request.data.get('matricule') or '')[:16]
    if matricule and cache.add(f'{POSTE_BROWSER_CACHE}notified.{matricule}', 1, POSTE_BROWSER_NOTIFY_EVERY):
        user = Secouristes.objects.filter(matricule=matricule, guest=False).first()
        if user is not None and user.badge_valid() and user.passkeys.exists():
            notified = webpush.notify_login_request(user, request_id, code, poste)
    return Response({'request': request_id, 'poll': poll, 'code': code, 'seconds': POSTE_BROWSER_MAX_AGE,
                     'url': ser.public_url('poste', r=request_id), 'notified': notified})


@api_view(['POST'])
@handle_errors
def poste_browser_info(request: Request) -> Response:
    """Page poste?r=... du navigateur : {"request"} -> {"code", "poste", "seconds"} (secondes restantes)."""
    data = poste_browser_request(request.data.get('request'))
    if data['matricule']:
        raise ApiError("Cette demande de connexion a déjà été validée", status.HTTP_409_CONFLICT)
    return Response({'code': data['code'], 'poste': data['poste'], 'seconds': poste_browser_left(data)})


@api_view(['POST'])
@handle_errors
def poste_browser_approve(request: Request) -> Response:
    """Navigateur : {"request", "credential"} (navigator.credentials.get() avec les options de auth/passkey/options/)
    -> {"prenom"}. La passkey est verifiee maintenant, comme pour une connexion au web ; une seule validation."""
    key = poste_browser_key(request.data.get('request'))
    data = poste_browser_request(request.data.get('request'))
    if data['matricule']:
        raise ApiError("Cette demande de connexion a déjà été validée", status.HTTP_409_CONFLICT)
    user = passkey_user(request, request.data.get('credential'))
    if not cache.add(key + '.approved', user.matricule, POSTE_BROWSER_MAX_AGE + 60):
        raise ApiError("Cette demande de connexion a déjà été validée", status.HTTP_409_CONFLICT)
    left = poste_browser_left(data)
    if not left:
        raise ApiError("Demande de connexion expirée : relancez « Se connecter avec une passkey » dans l'application",
                       status.HTTP_404_NOT_FOUND)
    cache.set(key, {**data, 'matricule': user.matricule}, left + 60)
    return Response({'prenom': user.prenom})


@api_view(['POST'])
@handle_errors
def poste_browser_poll(request: Request) -> Response:
    """Poste : {"request", "poll"} -> {"pending": true} tant que personne n'a valide, puis la reponse de /api/auth/
    (session normale du poste), une seule fois. 404 quand la demande a expire."""
    if not is_front(request):
        raise ApiError("Réservé aux postes", status.HTTP_403_FORBIDDEN)
    key = poste_browser_key(request.data.get('request'))
    data = poste_browser_request(request.data.get('request'))
    if not secrets.compare_digest(data['poll'], hashlib.sha256(str(request.data.get('poll') or '').encode()).hexdigest()):
        raise ApiError("Demande de connexion inconnue", status.HTTP_403_FORBIDDEN)
    if not poste_browser_left(data):
        raise ApiError("Demande de connexion expirée : relancez « Se connecter avec une passkey »",
                       status.HTTP_404_NOT_FOUND)
    if not data['matricule']:
        return Response({'pending': True, 'seconds': poste_browser_left(data)})
    if not cache.add(key + '.used', 1, POSTE_BROWSER_MAX_AGE + 60):
        raise ApiError("Demande de connexion déjà utilisée", status.HTTP_404_NOT_FOUND)
    cache.delete(key)
    user = Secouristes.objects.filter(matricule=data['matricule']).first()
    if user is None or user.guest or not user.badge_valid():
        raise ApiError("Compte désactivé ou badge expiré : voyez le TECH", status.HTTP_403_FORBIDDEN)
    if user.pin_blocked is not None:
        raise pin_blocked_error(user)
    journal.record(OperationKind.USER, f'{SOURCE_VERIFIED}:{user.matricule}',
                   f'connexion de {user} ({user.matricule}) au poste {data["poste"]} par la passkey du navigateur',
                   matricule=user.matricule)
    return Response(login_response(request, user, passkey=True, method='passkey_browser'))
