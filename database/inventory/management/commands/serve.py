# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          serve.py
#       -\-    _|__
#        |\___/  . \        Created on 29 Sep. 2026 at 14:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Lance l'API publique et l'API locale, chacune sur son port.

    python manage.py serve
    python manage.py serve --public 0.0.0.0:8000 --local 127.0.0.1:8001
    python manage.py serve --https            # API publique en HTTPS (certificat de developpement)

--https chiffre l'API publique avec un certificat auto-signe genere dans database/.dev-certs/ (couvre
localhost et les hotes de ALLOWED_HOSTS, dont les hotes de debug) : suffisant pour tester la camera d'un
telephone sur le reseau local, apres avoir accepte l'avertissement du navigateur. En production, placer
un reverse proxy HTTPS (nginx, caddy) devant l'API publique. L'API locale reste en HTTP.

Cette commande gere aussi les WebSockets du telephone-douchette (/ws/scanner/..., voir
inventory/remote_scanner.py) : le reverse proxy doit transmettre les en-tetes Upgrade/Connection.
"""

import ipaddress
import shutil
import socketserver
import ssl
import subprocess
import sys
import threading
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from django.conf import settings
from django.core.management.base import BaseCommand, CommandError
from django.core.servers.basehttp import WSGIRequestHandler, WSGIServer, get_internal_wsgi_application

from inventory.middleware import RoleWSGIHandler, local_access_error
from inventory.remote_scanner import RECEIVE_TIMEOUT, WebSocket, accept_key, hub

CERT_DIR = Path(settings.BASE_DIR) / '.dev-certs'


def parse_address(value, default_address):
    if ':' in value and not value.startswith('['):
        address, _, port = value.rpartition(':')
    else:
        address, port = default_address, value
    try:
        return address or default_address, int(port)
    except ValueError:
        raise CommandError(f"Adresse invalide : {value}")


def certificate_names():
    """Noms couverts par le certificat : localhost + hotes autorises (sauf jokers)."""
    names = ['localhost', '127.0.0.1', '::1']
    for host in settings.ALLOWED_HOSTS:
        host = host.strip().strip('[]')
        if host and '*' not in host and not host.startswith('.') and host not in names:
            names.append(host)
    entries = []
    for name in names:
        try:
            ipaddress.ip_address(name)
            entries.append(f'IP:{name}')
        except ValueError:
            entries.append(f'DNS:{name}')
    return ','.join(entries)


def development_certificate(stdout):
    """Genere (ou reutilise) un certificat auto-signe couvrant les hotes courants."""
    cert, key, names_file = CERT_DIR / 'cert.pem', CERT_DIR / 'key.pem', CERT_DIR / 'names.txt'
    names = certificate_names()
    if cert.exists() and key.exists() and names_file.exists() and names_file.read_text() == names:
        return cert, key
    if not shutil.which('openssl'):
        raise CommandError("openssl est introuvable : installez-le ou fournissez --cert et --key")
    CERT_DIR.mkdir(exist_ok=True)
    subprocess.run(
        ['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '825',
         '-keyout', str(key), '-out', str(cert), '-subj', '/CN=QRProtec developpement',
         '-addext', f'subjectAltName={names}'],
        check=True, capture_output=True,
    )
    key.chmod(0o600)
    names_file.write_text(names)
    stdout.write(f"Certificat de developpement genere pour : {names}")
    return cert, key


def tls_server_class(cert, key):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(str(cert), str(key))

    class TLSWSGIServer(WSGIServer):
        def server_bind(self):
            super().server_bind()
            # la poignee de main TLS se fait dans le thread de la requete, pas dans la boucle d'accept
            self.socket = context.wrap_socket(self.socket, server_side=True, do_handshake_on_connect=False)

        def handle_error(self, request, client_address):
            # navigateur qui refuse le certificat auto-signe : pas de trace a chaque tentative
            if isinstance(sys.exc_info()[1], (ssl.SSLError, ConnectionError)):
                return
            super().handle_error(request, client_address)

    return TLSWSGIServer


class QRProtecRequestHandler(WSGIRequestHandler):
    """Requetes HTTP ordinaires, plus les WebSockets du telephone-douchette (/ws/scanner/phone et /front)."""

    def handle_one_request(self):
        self.raw_requestline = self.rfile.readline(65537)
        if len(self.raw_requestline) > 65536:
            self.requestline = self.request_version = self.command = ''
            self.send_error(414)
            return
        if not self.parse_request():
            return
        path = urlsplit(self.path).path
        if path.startswith('/ws/') and self.headers.get('Upgrade', '').lower() == 'websocket':
            self.close_connection = True
            self.handle_websocket(path)
            return
        # meme traitement que WSGIRequestHandler.handle_one_request
        from django.core.servers.basehttp import ServerHandler
        handler = ServerHandler(self.rfile, self.wfile, self.get_stderr(), self.get_environ())
        handler.request_handler = self
        handler.run(self.server.get_app())

    def refuse(self, code, message):
        body = message.encode()
        self.send_response(code)
        self.send_header('Content-Type', 'text/plain; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def handle_websocket(self, path):
        role = getattr(self.server, 'qrprotec_role', 'public')
        params = {key: values[0] for key, values in parse_qs(urlsplit(self.path).query).items()}
        key = self.headers.get('Sec-WebSocket-Key', '')
        if not key:
            return self.refuse(400, 'Sec-WebSocket-Key manquant')
        if path == '/ws/scanner/phone':
            session = hub.check_key(params.get('s', ''), params.get('k', ''))
            if session is None:
                return self.refuse(404, 'Session inconnue ou fermee : scannez un nouveau QR code')
        elif path == '/ws/scanner/front':
            if role != 'local':
                return self.refuse(404, 'Inconnu')
            problem = local_access_error(self.client_address[0], self.headers.get('X-QRProtec-Token', ''))
            if problem:
                return self.refuse(403, problem)
            session = hub.get(params.get('s', ''))
            if session is None:
                return self.refuse(404, 'Session inconnue ou fermee')
        else:
            return self.refuse(404, 'Inconnu')
        self.send_response(101, 'Switching Protocols')
        self.send_header('Upgrade', 'websocket')
        self.send_header('Connection', 'Upgrade')
        self.send_header('Sec-WebSocket-Accept', accept_key(key))
        self.end_headers()
        self.wfile.flush()
        self.connection.settimeout(RECEIVE_TIMEOUT)
        websocket = WebSocket(self.rfile, self.wfile, self.connection)
        try:
            if path == '/ws/scanner/phone':
                hub.run_phone(session, websocket, self.headers.get('User-Agent', ''))
            else:
                hub.run_front(session, websocket)
        finally:
            websocket.close()


def run_server(address, port, handler, server_cls, role):
    httpd_cls = type('QRProtecServer', (socketserver.ThreadingMixIn, server_cls), {})
    httpd = httpd_cls((address, port), QRProtecRequestHandler, ipv6=':' in address)
    httpd.daemon_threads = True
    httpd.qrprotec_role = role
    httpd.set_app(handler)
    httpd.serve_forever()


class Command(BaseCommand):
    help = "Lance l'API publique et l'API locale (ports distincts)."

    def add_arguments(self, parser):
        config = settings.QRPROTEC
        parser.add_argument('--public', default=f"{config['PUBLIC_API_ADDRESS']}:{config['PUBLIC_API_PORT']}")
        parser.add_argument('--local', default=f"{config['LOCAL_API_ADDRESS']}:{config['LOCAL_API_PORT']}")
        parser.add_argument('--no-public', action='store_true', help="Ne lance que l'API locale")
        parser.add_argument('--https', action='store_true',
                            help="API publique en HTTPS (certificat auto-signe genere si --cert/--key absents)")
        parser.add_argument('--cert', help="Certificat PEM a utiliser avec --https")
        parser.add_argument('--key', help="Cle privee PEM a utiliser avec --https")

    def handle(self, *args, **options):
        application = get_internal_wsgi_application()
        public_server_cls = WSGIServer
        scheme = 'http'
        if options['https'] or options['cert']:
            if bool(options['cert']) != bool(options['key']):
                raise CommandError("--cert et --key vont ensemble")
            if options['cert']:
                cert, key = Path(options['cert']), Path(options['key'])
            else:
                cert, key = development_certificate(self.stdout)
            public_server_cls = tls_server_class(cert, key)
            scheme = 'https'

        servers = [('local', parse_address(options['local'], '127.0.0.1'), WSGIServer, 'http')]
        if not options['no_public']:
            servers.append(('public', parse_address(options['public'], '0.0.0.0'), public_server_cls, scheme))
        threads = []
        hub.enabled = True  # le telephone-douchette (WebSockets) fonctionne avec cette commande
        for role, (address, port), server_cls, url_scheme in servers:
            self.stdout.write(f"API {role} : {url_scheme}://{address}:{port}/api/")
            handler = RoleWSGIHandler(application, role, 'https' if url_scheme == 'https' else None)
            thread = threading.Thread(
                target=run_server, args=(address, port, handler, server_cls, role), daemon=True,
            )
            thread.start()
            threads.append(thread)
        if scheme == 'https':
            self.stdout.write("Certificat auto-signe : acceptez l'avertissement du navigateur a la premiere visite.")
        try:
            for thread in threads:
                thread.join()
        except KeyboardInterrupt:
            self.stdout.write("Arret.")
