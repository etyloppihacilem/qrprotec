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

La page repond aux URLs encodees dans les QR codes (verif?lot=..., badge?m=..., item?id=..., task?id=..., pack?id=...) : scanner
une etiquette avec l'appareil photo du telephone ouvre directement la bonne vue.
"""

from pathlib import Path

from django.http import FileResponse, Http404, HttpResponse, HttpResponseNotModified
from django.utils.http import http_date, parse_http_date_safe

from .models import ServerSettings

WEB_DIR = Path(__file__).resolve().parent / 'web'

# Module WebAssembly de zxing-cpp (~1 Mo, lu par decoder.js) : nom versionne, garde en cache par le navigateur
WASM = 'vendor/zxing-reader-3.1.6.wasm'

# Fichiers servis (liste blanche) et leur type
ASSETS = {
    'app.js': 'text/javascript; charset=utf-8',
    'app.css': 'text/css; charset=utf-8',
    'vendor/jsQR.js': 'text/javascript; charset=utf-8',
    'camera-scan.js': 'text/javascript; charset=utf-8',  # lecture limitee au viseur (app.js et scanner.js)
    'decoder.js': 'text/javascript; charset=utf-8',  # worker zxing-cpp quand BarcodeDetector manque (Firefox, Safari iOS)
    'vendor/zxing-reader.js': 'text/javascript; charset=utf-8',
    WASM: 'application/wasm',
    'code128.js': 'text/javascript; charset=utf-8',  # codes-barres des etiquettes d'items (app.js et scanner.js)
    'vendor/qrcode.js': 'text/javascript; charset=utf-8',  # QR code du lien de deblocage du PIN
    'scanner.js': 'text/javascript; charset=utf-8',
    'douchette.js': 'text/javascript; charset=utf-8',  # easter egg (app.js et scanner.js)
    'manifest.webmanifest': 'application/manifest+json',
    'sw.js': 'text/javascript; charset=utf-8',  # service worker des notifications web (portee web/)
    'favicon.png': 'image/png',
    'icon-192.png': 'image/png',
    'icon-512.png': 'image/png',
    'icon-maskable-192.png': 'image/png',  # plein cadre : Android le decoupe en rond (ou autre forme du lanceur)
    'icon-maskable-512.png': 'image/png',
    'apple-touch-icon.png': 'image/png',  # iPhone : sans transparence, iOS arrondit lui-meme les coins
}
# Icones des boutons (app.js) : un fichier SVG par icone, web/icons/<nom>.svg
ASSETS.update({f'icons/{path.name}': 'image/svg+xml' for path in (WEB_DIR / 'icons').glob('*.svg')})


def _file(request, path, content_type, cache):
    """Fichier statique avec ETag et Last-Modified : le navigateur garde sa copie et la revalide (304, sans corps)
    au lieu de retelecharger app.js et les autres (~600 Ko) a chaque ouverture. Comparaison a l'egalite (date et
    taille du fichier) : une mise a jour du paquet est toujours reprise, meme si la date du nouveau fichier est
    plus ancienne."""
    stat = path.stat()
    mtime = int(stat.st_mtime)
    etag = f'"{mtime:x}-{stat.st_size:x}"'
    if_none_match = request.headers.get('If-None-Match')
    if if_none_match is not None:
        # Caddy (encode gzip/zstd) peut renvoyer l'ETag avec un suffixe : "...-zstd"
        tags = [tag.strip().removeprefix('W/') for tag in if_none_match.split(',')]
        fresh = any(tag == etag or tag.startswith(etag[:-1] + '-') for tag in tags)
    else:
        fresh = parse_http_date_safe(request.headers.get('If-Modified-Since', '')) == mtime
    response = HttpResponseNotModified() if fresh else FileResponse(open(path, 'rb'), content_type=content_type)
    response['ETag'] = etag
    response['Last-Modified'] = http_date(mtime)
    response['Cache-Control'] = cache
    response['X-Content-Type-Options'] = 'nosniff'
    return response


def _csp(request):
    """Content-Security-Policy des pages : scripts, styles, images et connexions du site seulement. Les styles
    en ligne restent permis (attributs style poses par app.js, <style> de scanner.html), les images data: aussi
    (QR codes generes par qrcode.js). WebSocket du suivi en direct et de la douchette : meme hote en ws(s)://."""
    host = request.get_host()
    sockets = f'wss://{host}' if request.is_secure() else f'ws://{host} wss://{host}'
    return '; '.join((
        "default-src 'self'",
        "script-src 'self'",
        "style-src 'self' 'unsafe-inline'",
        "img-src 'self' data:",
        f"connect-src 'self' {sockets}",
        "worker-src 'self'",
        "manifest-src 'self'",
        "object-src 'none'",
        "base-uri 'self'",
        "form-action 'self'",
        "frame-ancestors 'none'",
    ))


BARCODE_SCRIPT = '  <script src="web/code128.js" defer></script>\n'


def _page(request, name):
    """Page HTML ; sans le decodeur de codes-barres (web/code128.js) tant que le reglage du serveur le coupe :
    aucune etiquette a code-barres en circulation, la page reste plus legere."""
    html = (WEB_DIR / name).read_text(encoding='utf-8')
    if not ServerSettings.get().web_barcodes:
        html = html.replace(BARCODE_SCRIPT, '')
    response = HttpResponse(html, content_type='text/html; charset=utf-8')
    response['Cache-Control'] = 'no-cache'
    response['X-Content-Type-Options'] = 'nosniff'
    response['Content-Security-Policy'] = _csp(request)
    return response


def page(request):
    response = _page(request, 'index.html')
    response['Permissions-Policy'] = 'camera=(self)'
    response['Referrer-Policy'] = 'no-referrer'  # l'URL peut contenir la cle du lot ou du badge
    return response


def scanner_page(request):
    """Page du telephone-douchette (QR code affiche par le front : scanner?s=SESSION&k=CLE)."""
    response = _page(request, 'scanner.html')
    response['Permissions-Policy'] = 'camera=(self)'
    response['Referrer-Policy'] = 'no-referrer'
    return response


def favicon(request):
    # demande d'office par les navigateurs a la racine du site
    return _file(request, WEB_DIR / 'favicon.png', 'image/png', 'max-age=86400')


def asset(request, name):
    if name not in ASSETS:
        raise Http404()
    return _file(request, WEB_DIR / name, ASSETS[name], 'max-age=31536000, immutable' if name == WASM else 'no-cache')
