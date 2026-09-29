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


class ApiRoleMiddleware:
    """Choisit le jeu d'URLs (public ou local) et protege l'API locale.

    Une requete n'est locale que si elle a ete recue par le serveur local (voir la commande
    `manage.py serve` ou `qrprotecDB.wsgi.local_application`). L'API locale n'accepte en plus que
    les adresses de QRPROTEC['LOCAL_API_ALLOWED_ADDRESSES'] et, si configure, le jeton
    QRPROTEC['LOCAL_API_TOKEN'] dans l'en-tete X-QRProtec-Token.
    """

    def __init__(self, get_response):
        self.get_response = get_response

    def __call__(self, request):
        config = settings.QRPROTEC
        role = request.META.get(ROLE_ENVIRON_KEY, config['DEFAULT_API_ROLE'])
        request.qrprotec_local = False
        if role == 'local':
            allowed = config['LOCAL_API_ALLOWED_ADDRESSES']
            if '*' not in allowed and request.META.get('REMOTE_ADDR') not in allowed:
                return JsonResponse({'error': "Adresse non autorisee sur l'API locale"}, status=403)
            token = config['LOCAL_API_TOKEN']
            if token and not hmac.compare_digest(request.META.get(LOCAL_TOKEN_HEADER, '').encode(), token.encode()):
                return JsonResponse({'error': "Jeton de l'API locale invalide"}, status=403)
            request.qrprotec_local = True
            request.urlconf = 'qrprotecDB.urls_local'
        return self.get_response(request)
