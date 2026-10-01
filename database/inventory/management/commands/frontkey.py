# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          frontkey.py
#       -\-    _|__
#        |\___/  . \        Created on 1 Oct. 2026 at 23:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Cles API des fronts distants (autres machines).

    python manage.py frontkey add accueil       # cree une cle et l'affiche UNE seule fois
    python manage.py frontkey list
    python manage.py frontkey revoke accueil     # le front n'a plus acces (immediat)
    python manage.py frontkey delete accueil

Le front distant se configure avec l'URL HTTPS du serveur (ex. https://inventaire.example.org) et
cette cle, envoyee dans l'en-tete X-QRProtec-Key. Le front local de la borne n'en a pas besoin : il
utilise l'API locale (127.0.0.1:8001), jamais exposee.
"""

import re

from django.conf import settings
from django.core.management.base import BaseCommand, CommandError
from django.utils import timezone

from inventory.models import FrontKey

NAME_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')


class Command(BaseCommand):
    help = "Gere les cles API des fronts distants (add, list, revoke, delete)."

    def add_arguments(self, parser):
        sub = parser.add_subparsers(dest='action', required=True)
        add = sub.add_parser('add', help="cree une cle (affichee une seule fois)")
        add.add_argument('name')
        add.add_argument('--replace', action='store_true', help="remplace la cle d'un front existant")
        sub.add_parser('list', help="liste les fronts")
        revoke = sub.add_parser('revoke', help="retire l'acces d'un front")
        revoke.add_argument('name')
        delete = sub.add_parser('delete', help="supprime un front")
        delete.add_argument('name')

    def handle(self, *args, action, **options):
        if action == 'list':
            return self.list()
        name = options['name']
        if action == 'add':
            return self.add(name, options['replace'])
        front = FrontKey.objects.filter(name=name).first()
        if front is None:
            raise CommandError(f"Front inconnu : {name}")
        if action == 'revoke':
            front.revoked = True
            front.save(update_fields=['revoked'])
            self.stdout.write(f"Cle du front {name} revoquee.")
        else:
            front.delete()
            self.stdout.write(f"Front {name} supprime.")

    def add(self, name, replace):
        if not NAME_RE.match(name):
            raise CommandError("Nom invalide (lettres, chiffres, . _ -, 64 caracteres au plus)")
        existing = FrontKey.objects.filter(name=name).first()
        if existing is not None:
            if not replace:
                raise CommandError(f"Le front {name} existe deja (--replace pour lui donner une nouvelle cle)")
            existing.delete()
        _, key = FrontKey.create(name)
        base_url = settings.QRPROTEC['PUBLIC_BASE_URL'].rstrip('/')
        self.stdout.write(f"Cle du front {name} (notez-la, elle ne sera plus affichee) :\n\n    {key}\n")
        self.stdout.write("Sur la machine du front (paquet qrprotec-kiosk ou qrprotec-front) :")
        self.stdout.write(f"    sudo qrprotec-setup --api-url {base_url} --api-key {key}")
        self.stdout.write(f"ou dans Reglages > Serveur du front : URL {base_url}, cle ci-dessus.")

    def list(self):
        fronts = FrontKey.objects.order_by('name')
        if not fronts:
            self.stdout.write("Aucun front distant (python manage.py frontkey add NOM).")
            return
        for front in fronts:
            used = timezone.localtime(front.last_used).strftime('%Y-%m-%d %H:%M') if front.last_used else 'jamais'
            state = 'revoquee' if front.revoked else 'active'
            where = f" depuis {front.last_address}" if front.last_address else ''
            self.stdout.write(f"{front.name:24} {state:9} cree le {timezone.localtime(front.created):%Y-%m-%d}, "
                              f"derniere utilisation : {used}{where}")
