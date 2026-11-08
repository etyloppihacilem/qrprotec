# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          server_problem.py
#       -\-    _|__
#        |\___/  . \        Created on 08 Nov. 2026 at 15:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Notification « Problème de serveur » des admins, lancee par qrprotec-backup (sauvegarde ou envoi sur la machine
distante en echec) :

    qrprotec-manage server_problem backup "base introuvable : /var/lib/qrprotec/db.sqlite3"
    qrprotec-manage server_problem sync --resolved     # l'envoi refonctionne : prochain echec annonce tout de suite
"""

from django.core.management.base import BaseCommand

from inventory import notifications, webpush


class Command(BaseCommand):
    help = 'Prévient les admins (notification « Problème de serveur ») : sauvegarde ou envoi des sauvegardes en échec.'

    def add_arguments(self, parser):
        parser.add_argument('source', choices=sorted(notifications.SERVER_PROBLEM_SOURCES))
        parser.add_argument('message', nargs='?', default='')
        parser.add_argument('--resolved', action='store_true', help='la source refonctionne (rien n\'est envoyé)')

    def handle(self, *args, **options):
        if options['resolved']:
            notifications.resolve_server_problem(options['source'])
            return
        sent = notifications.server_problem(options['source'], options['message'])
        webpush.flush()  # le processus s'arrete ensuite : attendre la fin des envois
        self.stdout.write('Admins prévenus' if sent else 'Déjà signalé récemment : pas de nouvelle notification')
