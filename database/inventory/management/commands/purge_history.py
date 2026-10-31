"""Purge de tout ce qui est plus ancien que la retention (par defaut 5 ans, duree de vie d'un materiel perime :
QRPROTEC_HISTORY_YEARS), puis VACUUM : la base ne grossit plus indefiniment.

    python manage.py purge_history              # purge ce qui depasse la retention
    python manage.py purge_history --dry-run    # compte seulement
    python manage.py purge_history --weekly     # ne fait rien sauf le lundi (lance chaque jour par qrprotec-alerts)

Supprime, au-dela de la retention :
- les verifs (et leurs lignes d'items), les mouvements d'items et les operations du journal ;
- les signalements resolus et les listes de materiel rentrees (ou brouillons abandonnes, hors modeles) ;
- les items sortis de la circulation (disparus, remplaces, supprimes), plus vus depuis, que plus rien ne
  reference (verif gardee, signalement, liste) ; puis les paquets fermes ouverts dont il ne reste aucun item.
Garde toujours ce dont l'etat actuel depend, quel que soit son age :
- la derniere verif de chaque lot et la derniere verif du stock ;
- le dernier mouvement de chaque item restant (sortie du stock en cours, dernier rangement...) ;
- l'operation d'ouverture du scelle actuel de chaque lot ;
- les ItemsPacks (type + date) : leur compteur garantit qu'un iid deja imprime n'est jamais redonne.
"""

from datetime import timedelta

from django.db import connection, transaction
from django.db.models import Exists, Max, OuterRef, Q
from django.utils import timezone

from django.core.management.base import BaseCommand

from inventory.models import (
    Checklist, ChecklistScan, ChecklistStatus, ItemMovement, Items, ItemStatus, Lots, Operation, OperationKind,
    ProblemReport, ReportStatus, SealedPacks, VerifItem, Verifs, qrprotec_setting,
)

# sortis de la circulation pour de bon (un item sorti ou hors lot peut encore revenir)
GONE = (ItemStatus.MISSING, ItemStatus.REPLACED, ItemStatus.DELETED)


def purge(years, now=None, dry_run=False):
    """{table: lignes supprimees (ou a supprimer)} pour une retention de `years` ans."""
    cutoff = (now or timezone.now()) - timedelta(days=365 * years)
    keep_verifs = list(Verifs.objects.values('lot').annotate(last=Max('id')).values_list('last', flat=True))
    keep_moves = list(ItemMovement.objects.values('item').annotate(last=Max('id')).values_list('last', flat=True))
    seals = list(Lots.objects.exclude(unsealed=None).values_list('unsealed', flat=True))
    verifs = Verifs.objects.filter(datetime__lt=cutoff).exclude(id__in=keep_verifs)
    moves = ItemMovement.objects.filter(at__lt=cutoff).exclude(id__in=keep_moves)
    operations = Operation.objects.filter(at__lt=cutoff).exclude(kind=OperationKind.UNSEAL, at__in=seals)
    reports = ProblemReport.objects.filter(status=ReportStatus.RESOLVED).filter(
        Q(resolved__lt=cutoff) | Q(resolved__isnull=True, created__lt=cutoff))
    checklists = Checklist.objects.filter(is_template=False).filter(
        Q(status=ChecklistStatus.RETURNED, updated__lt=cutoff) | Q(status=ChecklistStatus.DRAFT, updated__lt=cutoff))

    def old_items():
        """Items hors circulation depuis la retention, sans reference restante (a evaluer apres les purges)."""
        return (Items.objects.filter(status__in=GONE, last_seen__lt=cutoff)
                .filter(Q(deleted__isnull=True) | Q(deleted__lt=cutoff))
                .exclude(Exists(VerifItem.objects.filter(item=OuterRef('pk'))))
                .exclude(Exists(ProblemReport.objects.filter(item=OuterRef('pk'))))
                .exclude(Exists(ChecklistScan.objects.filter(item=OuterRef('pk')))))

    def old_packs():
        return (SealedPacks.objects.filter(opened__lt=cutoff)
                .exclude(Exists(Items.objects.filter(sealed_pack=OuterRef('pk'))))
                .exclude(Exists(ProblemReport.objects.filter(sealed_pack=OuterRef('pk')))))

    if dry_run:
        # approximation : les items references seulement par l'historique purge ne sont pas comptes
        return {'verifs': verifs.count(), 'mouvements': moves.count(), 'operations': operations.count(),
                'signalements': reports.count(), 'listes': checklists.count(), 'items': old_items().count(),
                'paquets': old_packs().count()}
    with transaction.atomic():
        counts = {'verifs': verifs.delete()[1].get('inventory.Verifs', 0),
                  'mouvements': moves.delete()[0], 'operations': operations.delete()[0],
                  'signalements': reports.delete()[0],
                  'listes': checklists.delete()[1].get('inventory.Checklist', 0)}
        counts['items'] = old_items().delete()[1].get('inventory.Items', 0)
        counts['paquets'] = old_packs().delete()[0]
    return counts


class Command(BaseCommand):
    help = "Supprime l'historique et les items hors circulation plus anciens que la retention (QRPROTEC_HISTORY_YEARS, 5 ans) puis compacte la base."

    def add_arguments(self, parser):
        parser.add_argument('--years', type=int, help='retention en annees (defaut : QRPROTEC_HISTORY_YEARS)')
        parser.add_argument('--dry-run', action='store_true', help='compte sans rien supprimer')
        parser.add_argument('--weekly', action='store_true', help='ne fait rien sauf le lundi')

    def handle(self, years=None, dry_run=False, weekly=False, **options):
        years = qrprotec_setting('HISTORY_YEARS') if years is None else years
        if years <= 0:
            self.stdout.write('Retention desactivee (QRPROTEC_HISTORY_YEARS=0) : rien a purger.')
            return
        if weekly and timezone.localdate().weekday() != 0:
            return
        counts = purge(years, dry_run=dry_run)
        detail = ', '.join(f'{count} {name}' for name, count in counts.items())
        self.stdout.write(f"{'A supprimer' if dry_run else 'Supprime'} (plus de {years} ans) : {detail}")
        if not dry_run and any(counts.values()):
            with connection.cursor() as cursor:
                cursor.execute('VACUUM')  # rend la place au disque (hors transaction)
            self.stdout.write('Base compactee (VACUUM).')
