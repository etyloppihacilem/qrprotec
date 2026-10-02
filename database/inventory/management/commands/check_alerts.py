# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          check_alerts.py
#       -\-    _|__
#        |\___/  . \        Created on 30 Sep. 2026 at 10:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Verification periodique (a lancer par cron, ex: chaque matin) :

- stock sous le minimum (un SMS par passage sous le seuil) ;
- resume des lots contenant des items perimes (si « résumé quotidien des périmés » est active) ;
- etiquettes privees de lot et badges qui expirent bientot ou ont expire (une alerte par etape).
"""

from django.core.management.base import BaseCommand
from django.db.models import Count
from django.utils import timezone

from inventory import notifications
from inventory.models import ItemStatus, Lots


class Command(BaseCommand):
    help = ('Envoie les notifications de stock bas, le résumé des lots contenant des périmés et les alertes '
            "d'expiration des étiquettes privées de lot et des badges.")

    def handle(self, *args, **options):
        low = notifications.check_stock_levels()
        today = timezone.localdate()
        lots = (
            Lots.objects.filter(active=True, items__status=ItemStatus.ACTIVE, items__pack__peremption__lt=today)
            .annotate(expired=Count('items'))
            .order_by('name')
        )
        summary = [f'{lot.name} ({lot.expired})' for lot in lots]
        if summary:
            notifications.notify('expired_daily', 'lots contenant des périmés : ' + ', '.join(summary))
        self.stdout.write(f'Stock bas : {", ".join(low) or "aucun"}')
        self.stdout.write(f'Lots avec périmés : {", ".join(summary) or "aucun"}')
        expirations = notifications.check_key_expirations(today)
        self.stdout.write(f'Étiquettes de lot à renouveler : {", ".join(expirations["lot_key_expiring"]) or "aucune"}')
        self.stdout.write(f'Badges à renouveler : {", ".join(expirations["badge_expiring"]) or "aucun"}')
