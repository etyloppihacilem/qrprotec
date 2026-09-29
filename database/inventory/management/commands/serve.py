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
"""

import ipaddress
import shutil
import ssl
import subprocess
import sys
import threading
from pathlib import Path

from django.conf import settings
from django.core.management.base import BaseCommand, CommandError
from django.core.servers.basehttp import WSGIServer, get_internal_wsgi_application, run

from inventory.middleware import RoleWSGIHandler

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
        for role, (address, port), server_cls, url_scheme in servers:
            self.stdout.write(f"API {role} : {url_scheme}://{address}:{port}/api/")
            handler = RoleWSGIHandler(application, role, 'https' if url_scheme == 'https' else None)
            thread = threading.Thread(
                target=run,
                args=(address, port, handler),
                kwargs={'threading': True, 'ipv6': ':' in address, 'server_cls': server_cls},
                daemon=True,
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
