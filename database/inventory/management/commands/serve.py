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

Pour l'HTTPS, placer un reverse proxy (nginx, caddy) devant l'API publique.
"""

import threading

from django.conf import settings
from django.core.management.base import BaseCommand, CommandError
from django.core.servers.basehttp import get_internal_wsgi_application, run

from inventory.middleware import RoleWSGIHandler


def parse_address(value, default_address):
    if ':' in value and not value.startswith('['):
        address, _, port = value.rpartition(':')
    else:
        address, port = default_address, value
    try:
        return address or default_address, int(port)
    except ValueError:
        raise CommandError(f"Adresse invalide : {value}")


class Command(BaseCommand):
    help = "Lance l'API publique et l'API locale (ports distincts)."

    def add_arguments(self, parser):
        config = settings.QRPROTEC
        parser.add_argument('--public', default=f"{config['PUBLIC_API_ADDRESS']}:{config['PUBLIC_API_PORT']}")
        parser.add_argument('--local', default=f"{config['LOCAL_API_ADDRESS']}:{config['LOCAL_API_PORT']}")
        parser.add_argument('--no-public', action='store_true', help="Ne lance que l'API locale")

    def handle(self, *args, **options):
        application = get_internal_wsgi_application()
        servers = [('local', parse_address(options['local'], '127.0.0.1'))]
        if not options['no_public']:
            servers.append(('public', parse_address(options['public'], '0.0.0.0')))
        threads = []
        for role, (address, port) in servers:
            self.stdout.write(f"API {role} : http://{address}:{port}/api/")
            thread = threading.Thread(
                target=run,
                args=(address, port, RoleWSGIHandler(application, role)),
                kwargs={'threading': True, 'ipv6': ':' in address},
                daemon=True,
            )
            thread.start()
            threads.append(thread)
        try:
            for thread in threads:
                thread.join()
        except KeyboardInterrupt:
            self.stdout.write("Arret.")
