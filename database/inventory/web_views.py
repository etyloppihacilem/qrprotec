# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          web_views.py
#       -\-    _|__
#        |\___/  . \        Created on 30 Sep. 2026 at 10:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Front web mobile (page unique) servi par l'API publique.

La page repond aux URLs encodees dans les QR codes (verif?lot=..., badge?m=..., item?id=..., pack?id=...) : scanner
une etiquette avec l'appareil photo du telephone ouvre directement la bonne vue.
"""

from pathlib import Path

from django.http import FileResponse, Http404

WEB_DIR = Path(__file__).resolve().parent / 'web'

# Fichiers servis (liste blanche) et leur type
ASSETS = {
    'app.js': 'text/javascript; charset=utf-8',
    'app.css': 'text/css; charset=utf-8',
    'vendor/jsQR.js': 'text/javascript; charset=utf-8',
    'vendor/qrcode.js': 'text/javascript; charset=utf-8',  # QR code du lien de deblocage du PIN
    'scanner.js': 'text/javascript; charset=utf-8',
    'douchette.js': 'text/javascript; charset=utf-8',  # easter egg (app.js et scanner.js)
    'manifest.webmanifest': 'application/manifest+json',
    'sw.js': 'text/javascript; charset=utf-8',  # service worker des notifications web (portee web/)
    'favicon.png': 'image/png',
    'icon-192.png': 'image/png',
    'icon-512.png': 'image/png',
}


def _file(path, content_type, cache):
    response = FileResponse(open(path, 'rb'), content_type=content_type)
    response['Cache-Control'] = cache
    response['X-Content-Type-Options'] = 'nosniff'
    return response


def page(request):
    response = _file(WEB_DIR / 'index.html', 'text/html; charset=utf-8', 'no-cache')
    response['Permissions-Policy'] = 'camera=(self)'
    response['Referrer-Policy'] = 'no-referrer'  # l'URL peut contenir la cle du lot ou du badge
    return response


def scanner_page(request):
    """Page du telephone-douchette (QR code affiche par le front : scanner?s=SESSION&k=CLE)."""
    response = _file(WEB_DIR / 'scanner.html', 'text/html; charset=utf-8', 'no-cache')
    response['Permissions-Policy'] = 'camera=(self)'
    response['Referrer-Policy'] = 'no-referrer'
    return response


def favicon(request):
    # demande d'office par les navigateurs a la racine du site
    return _file(WEB_DIR / 'favicon.png', 'image/png', 'max-age=86400')


def asset(request, name):
    if name not in ASSETS:
        raise Http404()
    return _file(WEB_DIR / name, ASSETS[name], 'no-cache')
