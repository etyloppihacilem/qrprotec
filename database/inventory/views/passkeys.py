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
admin, QR code de connexion au poste montre par un secouriste connecte avec sa passkey."""

from __future__ import annotations

import logging
from typing import TYPE_CHECKING

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
    check_login_address(request)
    credential = request.data.get('credential')
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
    return Response(login_response(request, user, passkey=True, method='passkey'))


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
def my_poste_login(request: Request) -> Response:
    """QR code de connexion au poste, montre sur le telephone d'un secouriste connecte avec sa passkey :
    {"user": {"matricule", "session"}} -> lien badge?m=...&t=... valable 5 min, une seule fois, a scanner a la douchette
    d'un poste (qui ouvre une session normale). Seulement pour une session ouverte avec une passkey ; jamais pour un
    admin (son PIN est obligatoire au poste)."""
    user = badge_user(request, ALL_ROLES)
    credentials = request.data.get('user') or {}
    if not passkey_session(user, credentials.get('session')):
        raise ApiError("Réservé aux connexions avec une passkey", status.HTTP_403_FORBIDDEN)
    if user.role == Role.ADMIN:
        raise ApiError("Un administrateur se connecte au poste avec sa carte et son code PIN", status.HTTP_403_FORBIDDEN)
    return Response({
        'url': ser.public_url('badge', m=user.matricule, t=temp_login_token(user, passkey=True)),
        'expires': (timezone.now() + timedelta(seconds=PASSKEY_POSTE_LOGIN_MAX_AGE)).isoformat(),
        'seconds': PASSKEY_POSTE_LOGIN_MAX_AGE,
    })


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
