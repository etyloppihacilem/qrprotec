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

- stock sous le minimum (une notification web par passage sous le seuil) ;
- resume des lots contenant des items perimes (notification web « résumé quotidien des périmés ») ;
- etiquettes privees de lot et badges qui expirent bientot ou ont expire (une alerte par etape) ;
- commandes a passer d'apres les previsions de stock (une alerte par type et par commande) ;
- items sortis du stock sans lot et non revus dans le delai : comptes comme utilises.
"""

from django.core.management.base import BaseCommand
from django.db.models import Count
from django.utils import timezone

from inventory import notifications, services
from inventory.models import ItemStatus, Lots


class Command(BaseCommand):
    help = ('Envoie les notifications web de stock bas, le résumé des lots contenant des périmés et les alertes '
            "d'expiration des étiquettes clés de lot et des badges, et les commandes à passer.")

    def handle(self, *args, **options):
        used = services.expire_out_items()
        self.stdout.write(f'Sorties non revenues comptées utilisées : {", ".join(used) or "aucune"}')
        low = notifications.check_stock_levels()
        today = timezone.localdate()
        lots = (
            Lots.objects.filter(active=True, items__status=ItemStatus.ACTIVE, items__pack__peremption__lt=today)
            .annotate(expired=Count('items'))
            .order_by('name')
        )
        summary = [f'{lot.name} ({lot.expired})' for lot in lots]
        if summary:
            notifications.notify_admins('expired_daily', 'lots avec périmés',
                                        'lots contenant des périmés : ' + ', '.join(summary), tag='expired-daily')
        self.stdout.write(f'Stock bas : {", ".join(low) or "aucun"}')
        self.stdout.write(f'Lots avec périmés : {", ".join(summary) or "aucun"}')
        # materiel a reviser : en retard, ou dans la semaine
        due = services.revisions_due(today, days=7)
        late = [item for item in due if item.revision_due < today]
        week = [item for item in due if item.revision_due >= today]
        where = lambda item: f'{item.pack.item_type.name} ({item.location.name if item.location_id else "stock"})'
        parts = []
        if late:
            parts.append('en retard : ' + ', '.join(where(item) for item in late))
        if week:
            parts.append('cette semaine : ' + ', '.join(where(item) for item in week))
        if parts:
            notifications.notify_admins('revision_daily', 'révisions de matériel',
                                        'révisions à faire, ' + ' ; '.join(parts), tag='revision-daily')
        self.stdout.write(f'Révisions à faire : {" ; ".join(parts) or "aucune"}')
        expirations = notifications.check_key_expirations(today)
        self.stdout.write(f'Étiquettes de lot à renouveler : {", ".join(expirations["lot_key_expiring"]) or "aucune"}')
        self.stdout.write(f'Badges à renouveler : {", ".join(expirations["badge_expiring"]) or "aucun"}')
        orders = notifications.check_orders(today)
        self.stdout.write(f'Commandes à passer : {", ".join(orders) or "aucune"}')
