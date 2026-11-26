# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          sessions.py
#       -\-    _|__
#        |\___/  . \        Created on 11 Oct. 2026 at 13:30
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Jetons de session des secouristes (reponse de /api/auth/ et de /api/auth/name/).

Le front web et le poste renvoient ce jeton au lieu de redemander le PIN. Apres une connexion par nom et PIN, le
telephone n'a pas la cle du badge : le jeton la remplace dans les identifiants {"matricule", "session"}.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from django.core import signing

from .models import Secouristes

if TYPE_CHECKING:
    from rest_framework.request import Request

SESSION_SALT = 'qrprotec.session'
SESSION_MAX_AGE = 12 * 3600  # jeton de session apres verification du PIN
# Connexion de depannage (QR code temporaire montre par un admin a un secouriste qui a oublie sa carte) : le QR code
# vaut 1h apres sa creation, la session qu'il ouvre sur le telephone 24h
TEMP_LOGIN_SALT = 'qrprotec.temp-login'
TEMP_LOGIN_MAX_AGE = 3600
TEMP_SESSION_SALT = 'qrprotec.temp-session'
TEMP_SESSION_MAX_AGE = 24 * 3600
# QR code de connexion au poste montre par un secouriste connecte au web avec sa passkey : 5 min seulement (il est a
# l'ecran du telephone, on le scanne tout de suite), il ouvre une session normale sur le poste
PASSKEY_POSTE_LOGIN_MAX_AGE = 300


# Moyens de connexion, gardes dans le jeton de session (`a`) et notes au journal des operations (details.auth)
AUTH_METHODS = {
    'card': 'carte',
    'card_pin': 'carte + PIN',
    'name_pin': 'nom + PIN',
    'matricule_pin': 'matricule + PIN',
    'temp_qr': 'QR code de dépannage du TECH',
    'passkey': 'passkey',
    'passkey_qr': 'QR code du téléphone (passkey)',
    'setup': 'création du premier administrateur',
}


def session_token(user: Secouristes, temporary: bool = False, passkey: bool = False, method: str = '') -> str:
    # lie au badge courant : renouveler le badge invalide les sessions. `pk` : session ouverte avec une passkey (le
    # web propose alors le QR code de connexion au poste). `a` : moyen de connexion (AUTH_METHODS), pour le journal
    data = {'m': user.matricule, 'k': user.key_hash[-8:]}
    if passkey:
        data['pk'] = 1
    if method:
        data['a'] = method
    return signing.dumps(data, salt=TEMP_SESSION_SALT if temporary else SESSION_SALT)


def session_method(token: str) -> str | None:
    """Moyen de connexion d'un jeton de session valide ('' : jeton d'avant ce suivi), None si le jeton est invalide."""
    data = _session_data(token)
    return None if data is None else str(data.get('a') or '')


def auth_label(request: Request | None) -> str:
    """Moyen d'authentification d'une requete, pour le journal des operations : « Web : passkey », « Poste principal :
    carte + PIN », « Poste distant (clé API « Borne B ») : nom + PIN ». '' sans requete ni personne connectee."""
    if request is None:
        return ''
    method = getattr(request, 'qrprotec_auth', None)  # connexion en cours (journal ecrit pendant /api/auth/...)
    front = getattr(request, 'qrprotec_local', False)
    if method is None:
        if front:
            method = session_method(request.META.get('HTTP_X_QRPROTEC_SESSION', ''))
        else:
            try:
                credentials = request.data.get('user') if hasattr(request, 'data') else None
            except Exception:  # corps illisible : pas de moyen de connexion  # noqa: BLE001
                credentials = None
            if isinstance(credentials, dict):
                token = credentials.get('session')
                method = session_method(token) if token else None
                if not method and credentials.get('key'):
                    method = 'card'  # cle du badge envoyee avec la requete
    if method is None:
        return ''
    label = AUTH_METHODS.get(method, 'session ouverte avant le suivi des connexions')
    if not front:
        return f'Web : {label}'
    name = getattr(request, 'qrprotec_front', '')
    return f'Poste distant (clé API « {name} ») : {label}' if name else f'Poste principal : {label}'


def passkey_session(user: Secouristes, token: str) -> bool:
    """Jeton de session valide de cet utilisateur, ouvert avec une passkey ?"""
    data = _session_data(token)
    return data is not None and data.get('pk') == 1 and session_valid(user, token)


def _session_data(token: str | None) -> dict[str, Any] | None:
    """Contenu d'un jeton de session (normale 12h, ou de depannage 24h), None s'il est invalide ou expire."""
    for salt, max_age in ((SESSION_SALT, SESSION_MAX_AGE), (TEMP_SESSION_SALT, TEMP_SESSION_MAX_AGE)):
        try:
            return signing.loads(str(token or ''), salt=salt, max_age=max_age)
        except signing.BadSignature:
            continue
    return None


def temp_login_token(user: Secouristes, passkey: bool = False) -> str:
    """Jeton du QR code de connexion de depannage (1h), lie au badge courant comme les sessions. `passkey` : QR code
    de connexion au poste montre par le secouriste lui-meme, connecte avec sa passkey (5 min)."""
    data = {'m': user.matricule, 'k': user.key_hash[-8:]}
    if passkey:
        data['pk'] = 1
    return signing.dumps(data, salt=TEMP_LOGIN_SALT)


def temp_login_data(token: str) -> dict[str, Any] | None:
    """Contenu d'un jeton de QR code de connexion valide (1h, 5 min s'il vient d'une passkey), None sinon."""
    try:
        data = signing.loads(str(token or ''), salt=TEMP_LOGIN_SALT, max_age=TEMP_LOGIN_MAX_AGE)
        if data.get('pk') == 1:
            signing.loads(str(token or ''), salt=TEMP_LOGIN_SALT, max_age=PASSKEY_POSTE_LOGIN_MAX_AGE)
    except signing.BadSignature:
        return None
    return data


def temp_login_user(matricule: str, token: str) -> Secouristes | None:
    """Secouriste d'un QR code de depannage valide (moins d'1h, meme badge, PIN non bloque), None sinon."""
    data = temp_login_data(token)
    if data is None:
        return None
    user = Secouristes.objects.filter(matricule=str(data.get('m', ''))[:16]).first()
    if (user is None or user.matricule != str(matricule or '') or data.get('k') != user.key_hash[-8:]
            or not user.badge_valid() or user.pin_blocked is not None):
        return None
    return user


def session_user(token: str) -> Secouristes | None:
    """Utilisateur d'un jeton de session valide (badge valide, PIN non bloque), None sinon."""
    data = _session_data(token)
    if data is None:
        return None
    user = Secouristes.objects.filter(matricule=str(data.get('m', ''))).first()
    if user is None or not user.badge_valid() or not session_valid(user, token):
        return None
    return user


def session_valid(user: Secouristes, token: str | None) -> bool:
    data = _session_data(token)
    if data is None:
        return False
    # un PIN bloque coupe aussi les sessions deja ouvertes
    return data.get('m') == user.matricule and data.get('k') == user.key_hash[-8:] and user.pin_blocked is None


def credentials_user(credentials: str | dict[str, str] | None) -> Secouristes | None:
    """Secouriste d'identifiants {"matricule", "key"} (badge) ou {"matricule", "session"} (jeton de connexion, seul
    identifiant du telephone apres une connexion par nom et PIN). None s'ils ne sont pas valides."""
    if not isinstance(credentials, dict):
        return None
    user = Secouristes.objects.filter(matricule=str(credentials.get('matricule', ''))[:16]).first()
    if user is None:
        return None
    if user.check_key(credentials.get('key')):
        return user
    token = credentials.get('session')
    if token and user.badge_valid() and session_valid(user, token):
        return user
    return None
