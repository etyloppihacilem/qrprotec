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
    python manage.py frontkey add accueil --config accueil.qrprotec [--ca-file root.crt]
                                                # cle + fichier chiffre (mot de passe demande) a importer
                                                # dans le front : URL, cle, certificat de l'autorite
    python manage.py frontkey add borne --poste # poste principal : kiosk sur une autre machine que le back
    python manage.py frontkey poste borne        # donne les droits de poste principal (--off : les retire)
    python manage.py frontkey list
    python manage.py frontkey revoke accueil     # le front n'a plus acces (immediat)
    python manage.py frontkey delete accueil

Le front distant se configure avec l'URL HTTPS du serveur (ex. https://inventaire.example.org) et
cette cle, envoyee dans l'en-tete X-QRProtec-Key. Le front local de la borne n'en a pas besoin : il
utilise l'API locale (127.0.0.1:8001), jamais exposee.

Poste principal (--poste) : le kiosk avec l'imprimante, quand le back tourne sur une autre machine (serveur sans
kiosk). Il a les droits du front de l'API locale : il imprime les « Impressions en attente » (etiquettes demandees
depuis le web ou un autre ordinateur) et gere les ordinateurs connectes (fenetre Utilisateurs).
"""

import getpass
import os
import re
from pathlib import Path

from django.conf import settings
from django.core.management.base import BaseCommand, CommandError
from django.db import transaction
from django.utils import timezone

from inventory import front_config
from inventory.models import FrontKey

NAME_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')


class Command(BaseCommand):
    help = "Gere les cles API des fronts distants (add, poste, list, revoke, delete)."

    def add_arguments(self, parser):
        sub = parser.add_subparsers(dest='action', required=True)
        add = sub.add_parser('add', help="cree une cle (affichee une seule fois)")
        add.add_argument('name')
        add.add_argument('--replace', action='store_true', help="remplace la cle d'un front existant")
        add.add_argument('--poste', action='store_true',
                         help="poste principal (kiosk sur une autre machine que le back) : impressions en attente "
                              "et gestion des ordinateurs connectes")
        add.add_argument('--config', metavar='FICHIER',
                         help="ecrit aussi un fichier de configuration chiffre (AES) a importer dans le front "
                              "(- = sortie standard) ; mot de passe demande, ou variable "
                              "QRPROTEC_FRONT_CONFIG_PASSWORD")
        add.add_argument('--url', help="URL du serveur pour le front (defaut : QRPROTEC_PUBLIC_BASE_URL)")
        add.add_argument('--ca-file', metavar='PEM',
                         help="certificat de l'autorite du serveur a mettre dans le fichier "
                              "(autorite interne de Caddy, QRPROTEC_TLS=internal)")
        poste = sub.add_parser('poste', help="donne (ou retire, --off) les droits de poste principal")
        poste.add_argument('name')
        poste.add_argument('--off', action='store_true', help="retire les droits de poste principal")
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
            return self.add(name, options['replace'], options.get('config'), options.get('url'),
                            options.get('ca_file'), options.get('poste', False))
        front = FrontKey.objects.filter(name=name).first()
        if front is None:
            raise CommandError(f"Front inconnu : {name}")
        if action == 'poste':
            front.poste = not options['off']
            front.save(update_fields=['poste'])
            self.stdout.write(f"Front {name} : " + ("poste principal." if front.poste else "n'est plus poste principal."))
        elif action == 'revoke':
            front.revoked = True
            front.save(update_fields=['revoked'])
            self.stdout.write(f"Cle du front {name} revoquee.")
        else:
            front.delete()
            self.stdout.write(f"Front {name} supprime.")

    def add(self, name, replace, config=None, url=None, ca_file=None, poste=False):
        if not NAME_RE.match(name):
            raise CommandError("Nom invalide (lettres, chiffres, . _ -, 64 caracteres au plus)")
        if (url or ca_file) and not config:
            raise CommandError("--url et --ca-file vont avec --config")
        existing = FrontKey.objects.filter(name=name).first()
        if existing is not None and not replace:
            raise CommandError(f"Le front {name} existe deja (--replace pour lui donner une nouvelle cle)")
        base_url = (url or settings.QRPROTEC['PUBLIC_BASE_URL']).rstrip('/')
        # tout ce qui peut echouer avant de creer la cle : certificat, mot de passe, fichier de sortie
        if config:
            if not base_url.startswith('https://'):
                raise CommandError(f"URL du serveur en HTTPS attendue pour un front distant : {base_url}")
            ca_pem = self.read_ca(ca_file) if ca_file else ''
            password = self.ask_password()
            if config != '-' and Path(config).exists() and not replace:
                raise CommandError(f"{config} existe deja (--replace pour l'ecraser)")
        # fichier impossible a ecrire : ni nouvelle cle, ni ancienne cle supprimee
        with transaction.atomic():
            if existing is not None:
                existing.delete()
            front, key = FrontKey.create(name, poste=poste)
            if config:
                payload = {'name': name, 'api_url': base_url, 'api_key': key,
                           'created': timezone.localtime(front.created).isoformat(timespec='seconds')}
                if ca_pem:
                    payload['api_ca_pem'] = ca_pem
                self.write_config(config, front_config.encrypt(payload, password))
        if config:
            # fichier sur la sortie standard : les explications passent sur l'erreur standard
            out = self.stderr if config == '-' else self.stdout
            if config != '-':
                out.write(f"Fichier de configuration du front {name} : {config}")
            out.write("Dans le front de l'autre machine : Reglages > Serveur (ou « API hors ligne » dans la barre "
                      "de menu tant qu'il n'est pas connecte) > Importer un fichier de configuration, puis le "
                      "mot de passe choisi.")
            out.write(f"URL : {base_url}" + (" ; certificat de l'autorite inclus" if ca_pem else ''))
            return
        self.stdout.write(f"Cle du front {name} (notez-la, elle ne sera plus affichee) :\n\n    {key}\n")
        self.stdout.write("Sur la machine du front (paquet qrprotec-kiosk ou qrprotec-front) :")
        self.stdout.write(f"    sudo qrprotec-setup --api-url {base_url} --api-key {key}")
        self.stdout.write(f"ou dans Reglages > Serveur du front : URL {base_url}, cle ci-dessus.")
        self.stdout.write(f"(ou frontkey add {name} --replace --config FICHIER : un fichier chiffre a importer)")

    @staticmethod
    def read_ca(path):
        try:
            pem = Path(path).read_text(encoding='ascii')
        except (OSError, UnicodeDecodeError) as error:
            raise CommandError(f"Certificat illisible : {path} ({error})") from error
        if '-----BEGIN CERTIFICATE-----' not in pem:
            raise CommandError(f"{path} n'est pas un certificat PEM")
        return pem

    def ask_password(self):
        password = os.environ.get('QRPROTEC_FRONT_CONFIG_PASSWORD', '')
        if not password:
            try:
                password = getpass.getpass("Mot de passe du fichier (demande a l'import dans le front) : ")
                if password != getpass.getpass("Confirmez le mot de passe : "):
                    raise CommandError("Les mots de passe ne correspondent pas")
            except (EOFError, KeyboardInterrupt) as error:
                raise CommandError("Mot de passe non saisi") from error
        if len(password) < front_config.MIN_PASSWORD_LENGTH:
            raise CommandError(f"Mot de passe trop court ({front_config.MIN_PASSWORD_LENGTH} caracteres au moins)")
        return password

    def write_config(self, config, text):
        if config == '-':
            self.stdout.write(text, ending='')
            self.stdout.flush()
            return
        path = Path(config)
        try:
            # chiffre, mais il donne acces au serveur avec le bon mot de passe : lisible par son seul proprietaire
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, 'w', encoding='utf-8') as output:
                output.write(text)
        except OSError as error:
            raise CommandError(f"Impossible d'ecrire {config} : {error}") from error

    def list(self):
        fronts = FrontKey.objects.order_by('name')
        if not fronts:
            self.stdout.write("Aucun front distant (python manage.py frontkey add NOM).")
            return
        for front in fronts:
            used = timezone.localtime(front.last_used).strftime('%Y-%m-%d %H:%M') if front.last_used else 'jamais'
            state = 'revoquee' if front.revoked else 'active'
            if front.poste:
                state += ', poste principal'
            where = f" depuis {front.last_address}" if front.last_address else ''
            self.stdout.write(f"{front.name:24} {state:9}, cree le {timezone.localtime(front.created):%Y-%m-%d}, "
                              f"derniere utilisation : {used}{where}")
