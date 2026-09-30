# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          createadmin.py
#       -\-    _|__
#        |\___/  . \        Created on 30 Sep. 2026 at 11:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Cree (ou repare) un compte administrateur et affiche l'URL de son badge.

    python manage.py createadmin M001 Dupont Jeanne
    python manage.py createadmin M001            # compte existant : repasse administrateur, nouvelle cle
    python manage.py createadmin M001 --pin 4821 # fixe aussi le PIN (sinon demande a la connexion)

Utile a la premiere installation ou si tous les badges administrateur sont perdus ou expires. Le front
propose aussi de creer le premier administrateur quand il n'y en a aucun.
"""

from django.core.management.base import BaseCommand, CommandError

from inventory.models import Role, Secouristes
from inventory.serializers import user_dict
from inventory.views import MATRICULE_RE


class Command(BaseCommand):
    help = "Cree un administrateur (mode privilegie complet) et affiche l'URL de son badge."

    def add_arguments(self, parser):
        parser.add_argument('matricule')
        parser.add_argument('nom', nargs='?')
        parser.add_argument('prenom', nargs='?')
        parser.add_argument('--pin', help="Code PIN (4 a 8 chiffres) ; sinon demande a la premiere connexion")

    def handle(self, matricule, nom=None, prenom=None, pin=None, **options):
        if not MATRICULE_RE.match(matricule):
            raise CommandError("Matricule invalide (1 a 16 caracteres alphanumeriques)")
        user = Secouristes.objects.filter(matricule=matricule).first()
        if user is None:
            if not nom or not prenom:
                raise CommandError("Nouveau compte : indiquez aussi le nom et le prenom")
            user = Secouristes(matricule=matricule, nom=nom[:32], prenom=prenom[:32])
            self.stdout.write(f"Creation de l'administrateur {prenom} {nom}.")
        else:
            if nom:
                user.nom = nom[:32]
            if prenom:
                user.prenom = prenom[:32]
            self.stdout.write(f"Compte {matricule} existant : nouvelle cle de badge, droits administrateur.")
        user.role = Role.ADMIN
        if pin:
            try:
                user.set_pin(pin)
            except ValueError as exc:
                raise CommandError(str(exc))
        user.active = True
        user.renew_key()
        user.save()
        data = user_dict(user, local=True)
        self.stdout.write(self.style.SUCCESS(f"Badge valable jusqu'au {data['key_expires']}"))
        self.stdout.write(f"URL du badge (a encoder dans un QR code) :\n{data['badge_url']}")
        self.stdout.write("Scannez cette URL (ou le badge imprime) sur le poste pour passer en mode privilegie.")
        if not user.has_pin:
            self.stdout.write("Aucun PIN : il sera demande (4 a 8 chiffres) a la premiere connexion.")
