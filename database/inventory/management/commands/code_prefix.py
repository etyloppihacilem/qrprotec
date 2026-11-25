# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          code_prefix.py
#       -\-    _|__
#        |\___/  . \        Created on 25 Nov. 2026 at 13:30
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Code de l'antenne devant les iid et les id de lots et de paquets dans les codes imprimes (voir models.code_prefix).

    qrprotec-manage code_prefix              # affiche le code enregistre
    qrprotec-manage code_prefix --sync       # avant le demarrage du serveur (qrprotec.service) : enregistre
                                             # QRPROTEC_CODE_PREFIX s'il n'y a pas encore de code, sinon le garde
    qrprotec-manage code_prefix PC --force   # change le code : les etiquettes deja imprimees avec l'ancien
                                             # deviennent celles d'une autre antenne

Le code est fige au premier demarrage : changer QRPROTEC_CODE_PREFIX ensuite ne fait qu'un avertissement.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from django.conf import settings
from django.core.management.base import BaseCommand, CommandError

from inventory.models import CODE_PREFIX_RE, ServerSettings, forget_code_prefix

if TYPE_CHECKING:
    from django.core.management.base import CommandParser


def valid_prefix(prefix: str) -> bool:
    return prefix == '' or bool(CODE_PREFIX_RE.fullmatch(prefix))


class Command(BaseCommand):
    help = "Code de l'antenne (2 lettres majuscules ou chiffres) devant les codes imprimés : affiché, enregistré ou changé."

    def add_arguments(self, parser: CommandParser) -> None:
        parser.add_argument('prefix', nargs='?', help='nouveau code (2 lettres majuscules ou chiffres)')
        parser.add_argument('--sync', action='store_true',
                            help='enregistre QRPROTEC_CODE_PREFIX si aucun code ne l\'est encore')
        parser.add_argument('--force', action='store_true', help='change un code déjà enregistré')

    def handle(self, *args: Any, **options: Any) -> None:
        row = ServerSettings.get()
        if options['sync']:
            self.sync(row)
        elif options['prefix'] is not None:
            self.change(row, options['prefix'], options['force'])
        self.stdout.write(row.code_prefix or '(aucun code)')

    def sync(self, row: ServerSettings) -> None:
        wanted = settings.QRPROTEC['CODE_PREFIX']
        if not valid_prefix(wanted):
            self.stderr.write(f'QRPROTEC_CODE_PREFIX ignoré : « {wanted} » n\'est pas 2 lettres majuscules ou chiffres.')
            return
        if not wanted or wanted == row.code_prefix:
            return
        if row.code_prefix:
            self.stderr.write(f'QRPROTEC_CODE_PREFIX={wanted} ignoré : le code {row.code_prefix} est déjà enregistré, '
                              'les étiquettes imprimées le portent. Pour en changer : '
                              f'qrprotec-manage code_prefix {wanted} --force')
            return
        self.save(row, wanted)

    def change(self, row: ServerSettings, prefix: str, force: bool) -> None:
        if not valid_prefix(prefix):
            raise CommandError('Le code doit faire 2 caractères : lettres majuscules ou chiffres (ex : PC, 12).')
        if row.code_prefix and prefix != row.code_prefix and not force:
            raise CommandError(f'Le code {row.code_prefix} est déjà enregistré : les étiquettes imprimées avec lui '
                               'deviendraient celles d\'une autre antenne. Ajoutez --force pour le changer quand même.')
        self.save(row, prefix)
        self.stderr.write('Code enregistré : redémarrez le serveur (sudo systemctl restart qrprotec.service) pour '
                          'qu\'il le prenne en compte.')

    def save(self, row: ServerSettings, prefix: str) -> None:
        row.code_prefix = prefix
        row.save(update_fields=['code_prefix'])
        forget_code_prefix()
