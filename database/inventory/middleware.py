# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          middleware.py
#       -\-    _|__
#        |\___/  . \        Created on 29 Sep. 2026 at 14:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

import hmac

from django.conf import settings
from django.http import JsonResponse

ROLE_ENVIRON_KEY = 'qrprotec.role'
LOCAL_TOKEN_HEADER = 'HTTP_X_QRPROTEC_TOKEN'
FRONT_KEY_HEADER = 'HTTP_X_QRPROTEC_KEY'


class RoleWSGIHandler:
    """Enveloppe WSGI qui marque les requetes selon le serveur (port) qui les a recues."""

    def __init__(self, application, role, scheme=None):
        self.application = application
        self.role = role
        self.scheme = scheme  # 'https' quand le serveur de developpement chiffre lui-meme (serve --https)

    def __call__(self, environ, start_response):
        environ[ROLE_ENVIRON_KEY] = self.role
        if self.scheme:
            environ['wsgi.url_scheme'] = self.scheme
        return self.application(environ, start_response)


def local_access_error(remote_address, token_header):
    """Message d'erreur si le client n'a pas acces a l'API locale (adresse ou jeton), None sinon."""
    config = settings.QRPROTEC
    allowed = config['LOCAL_API_ALLOWED_ADDRESSES']
    if '*' not in allowed and remote_address not in allowed:
        return "Adresse non autorisee sur l'API locale"
    token = config['LOCAL_API_TOKEN']
    if token and not hmac.compare_digest((token_header or '').encode(), token.encode()):
        return "Jeton de l'API locale invalide"
    return None


def remote_client_address(remote_address, forwarded_for):
    """Adresse du front distant : l'API distante n'est joignable que via le reverse proxy (Caddy)."""
    if forwarded_for:
        return forwarded_for.split(',')[0].strip()
    return remote_address or ''


def authenticate_front(key, address):
    """(FrontKey, None) si la cle de front distant est valide, (None, message d'erreur) sinon."""
    from .models import FrontKey  # le middleware est charge avant les applications

    if not key:
        return None, "Cle API du front manquante (en-tete X-QRProtec-Key)"
    front = FrontKey.authenticate(key, address)
    if front is None:
        return None, "Cle API du front invalide ou revoquee"
    return front, None


class ApiRoleMiddleware:
    """Choisit le jeu d'URLs (public ou local) et protege l'API locale.

    Une requete n'est locale que si elle a ete recue par le serveur local (voir la commande
    `manage.py serve` ou `qrprotecDB.wsgi.local_application`). L'API locale n'accepte en plus que
    les adresses de QRPROTEC['LOCAL_API_ALLOWED_ADDRESSES'] et, si configure, le jeton
    QRPROTEC['LOCAL_API_TOKEN'] dans l'en-tete X-QRProtec-Token.

    Le serveur distant (role 'remote', derriere Caddy) donne aux fronts d'autres machines le meme
    jeu d'URLs que l'API locale (sans l'admin Django), mais chaque requete doit porter une cle de front
    valide (modele FrontKey) dans l'en-tete X-QRProtec-Key.

    Sur les deux, les routes de gestion exigent en plus l'utilisateur connecte sur le poste (jeton de
    session dans l'en-tete X-QRProtec-Session, voir views.require_front) : la cle ou le localhost
    identifient la machine, le badge et le PIN la personne.
    """

    def __init__(self, get_response):
        self.get_response = get_response

    def __call__(self, request):
        config = settings.QRPROTEC
        role = request.META.get(ROLE_ENVIRON_KEY, config['DEFAULT_API_ROLE'])
        request.qrprotec_local = False
        if role == 'local':
            problem = local_access_error(request.META.get('REMOTE_ADDR'), request.META.get(LOCAL_TOKEN_HEADER, ''))
            if problem:
                return JsonResponse({'error': problem}, status=403)
            request.qrprotec_local = True
            request.urlconf = 'qrprotecDB.urls_local'
        elif role == 'remote':
            address = remote_client_address(request.META.get('REMOTE_ADDR'), request.META.get('HTTP_X_FORWARDED_FOR'))
            front, problem = authenticate_front(request.META.get(FRONT_KEY_HEADER, ''), address)
            if problem:
                return JsonResponse({'error': problem}, status=401)
            request.qrprotec_local = True
            request.qrprotec_front = front.name
            request.urlconf = 'qrprotecDB.urls_remote'
        return self.get_response(request)
