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

from django.core import signing

from .models import Secouristes

SESSION_SALT = 'qrprotec.session'
SESSION_MAX_AGE = 12 * 3600  # jeton de session apres verification du PIN
# Connexion de depannage (QR code temporaire montre par un admin a un secouriste qui a oublie sa carte) : le QR code
# vaut 1 h apres sa creation, la session qu'il ouvre sur le telephone 24 h
TEMP_LOGIN_SALT = 'qrprotec.temp-login'
TEMP_LOGIN_MAX_AGE = 3600
TEMP_SESSION_SALT = 'qrprotec.temp-session'
TEMP_SESSION_MAX_AGE = 24 * 3600


def session_token(user, temporary=False):
    # lie au badge courant : renouveler le badge invalide les sessions
    return signing.dumps({'m': user.matricule, 'k': user.key_hash[-8:]},
                         salt=TEMP_SESSION_SALT if temporary else SESSION_SALT)


def _session_data(token):
    """Contenu d'un jeton de session (normale 12 h, ou de depannage 24 h), None s'il est invalide ou expire."""
    for salt, max_age in ((SESSION_SALT, SESSION_MAX_AGE), (TEMP_SESSION_SALT, TEMP_SESSION_MAX_AGE)):
        try:
            return signing.loads(str(token or ''), salt=salt, max_age=max_age)
        except signing.BadSignature:
            continue
    return None


def temp_login_token(user):
    """Jeton du QR code de connexion de depannage (1 h), lie au badge courant comme les sessions."""
    return signing.dumps({'m': user.matricule, 'k': user.key_hash[-8:]}, salt=TEMP_LOGIN_SALT)


def temp_login_user(matricule, token):
    """Secouriste d'un QR code de depannage valide (moins d'1 h, meme badge, PIN non bloque), None sinon."""
    try:
        data = signing.loads(str(token or ''), salt=TEMP_LOGIN_SALT, max_age=TEMP_LOGIN_MAX_AGE)
    except signing.BadSignature:
        return None
    user = Secouristes.objects.filter(matricule=str(data.get('m', ''))[:16]).first()
    if (user is None or user.matricule != str(matricule or '') or data.get('k') != user.key_hash[-8:]
            or not user.badge_valid() or user.pin_blocked is not None):
        return None
    return user


def session_user(token):
    """Utilisateur d'un jeton de session valide (badge valide, PIN non bloque), None sinon."""
    data = _session_data(token)
    if data is None:
        return None
    user = Secouristes.objects.filter(matricule=str(data.get('m', ''))).first()
    if user is None or not user.badge_valid() or not session_valid(user, token):
        return None
    return user


def session_valid(user, token):
    data = _session_data(token)
    if data is None:
        return False
    # un PIN bloque coupe aussi les sessions deja ouvertes
    return data.get('m') == user.matricule and data.get('k') == user.key_hash[-8:] and user.pin_blocked is None


def credentials_user(credentials):
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
