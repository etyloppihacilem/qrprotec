# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          services.py
#       -\-    _|__
#        |\___/  . \        Created on 29 Sep. 2026 at 14:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Logique metier de l'inventaire : verifs, mouvements d'items, etat des stocks."""

from collections import defaultdict
from datetime import timedelta

from django.db import transaction
from django.db.models import Count, Q
from django.utils import timezone

from .models import (
    ItemStatus, Items, ItemType, LotRequirements, Lots, SeenWhile, VerifItem, VerifResult, Verifs, qrprotec_setting,
)


def _items_queryset():
    return Items.objects.select_related('pack__item_type', 'location')


def _location_filter(lot):
    return Q(location=lot) if lot is not None else Q(location__isnull=True)


def requirements_status(lot, today=None):
    """Compare le contenu d'un lot aux exigences de son type."""
    today = today or timezone.localdate()
    fresh = defaultdict(int)
    expired = defaultdict(int)
    for item in _items_queryset().filter(location=lot, status=ItemStatus.ACTIVE):
        if item.is_expired(today):
            expired[item.pack.item_type_id] += 1
        else:
            fresh[item.pack.item_type_id] += 1
    rows = []
    for requirement in LotRequirements.objects.select_related('item_type').filter(lot_type=lot.lot_type):
        type_id = requirement.item_type_id
        rows.append({
            'type': type_id,
            'type_name': requirement.item_type.name,
            'required': requirement.quantity,
            'present': fresh.get(type_id, 0),
            'expired': expired.get(type_id, 0),
        })
    return rows


def perform_verif(lot, iids, identity):
    """Verifie un lot (ou le stock si lot est None) a partir des iids scannes.

    - les items scannes sont places dans le lot et marques presents ;
    - un item perime scanne alors que des items frais du meme type sont arrives dans le lot est
      considere comme remplace (il sort du lot) ;
    - les items attendus mais non scannes voient leur compteur de verifs manquees augmenter, et
      passent en 'disparu' s'ils sont perimes ou manques trop souvent.
    """
    now = timezone.now()
    today = timezone.localdate()
    requested = list(dict.fromkeys(iids))
    threshold = qrprotec_setting('MISSING_AFTER_VERIFS')
    lot_id = lot.id if lot is not None else None

    with transaction.atomic():
        scanned = list(_items_queryset().select_for_update(of=('self',)).filter(iid__in=requested))
        scanned_ids = {item.iid for item in scanned}
        unknown = [iid for iid in requested if iid not in scanned_ids]
        not_seen = list(
            _items_queryset().select_for_update(of=('self',))
            .filter(_location_filter(lot), status__in=[ItemStatus.ACTIVE, ItemStatus.MISSING])
            .exclude(iid__in=scanned_ids)
        )

        verif = Verifs.objects.create(lot=lot, datetime=now, by=identity)

        # Detection des remplacements : par type, nombre d'items frais nouvellement arrives
        new_fresh = defaultdict(int)
        expired_by_type = defaultdict(list)
        for item in scanned:
            type_id = item.pack.item_type_id
            if item.is_expired(today):
                expired_by_type[type_id].append(item)
            elif item.location_id != lot_id or item.status != ItemStatus.ACTIVE:
                new_fresh[type_id] += 1
        replaced = set()
        for type_id, expired_items in expired_by_type.items():
            for item in expired_items[:new_fresh.get(type_id, 0)]:
                replaced.add(item.iid)

        report = {'present': [], 'expired': [], 'replaced': [], 'missing': [], 'unknown': unknown, 'reactivated': []}
        entries = []
        for item in scanned:
            expired = item.is_expired(today)
            item.touch(identity, SeenWhile.VERIF, now)
            item.missed_verifs = 0
            if item.status != ItemStatus.ACTIVE:
                report['reactivated'].append(item.iid)
            if item.iid in replaced:
                item.status = ItemStatus.REPLACED
                item.location = None
                report['replaced'].append(item.iid)
                entries.append(VerifItem(verif=verif, item=item, result=VerifResult.REPLACED, expired=expired))
            else:
                item.status = ItemStatus.ACTIVE
                item.location = lot
                report['present'].append(item.iid)
                if expired:
                    report['expired'].append(item.iid)
                entries.append(VerifItem(verif=verif, item=item, result=VerifResult.PRESENT, expired=expired))
        for item in not_seen:
            expired = item.is_expired(today)
            item.missed_verifs += 1
            if expired or item.missed_verifs >= threshold:
                item.status = ItemStatus.MISSING
            report['missing'].append(item.iid)
            entries.append(VerifItem(verif=verif, item=item, result=VerifResult.MISSING, expired=expired))

        Items.objects.bulk_update(
            scanned, ['status', 'location', 'missed_verifs', 'last_seen', 'last_seen_by', 'last_seen_while']
        )
        Items.objects.bulk_update(not_seen, ['status', 'missed_verifs'])
        VerifItem.objects.bulk_create(entries)

        requirements = []
        complete = True
        if lot is not None:
            lot.last_verif = now
            lot.last_verif_by = identity
            lot.last_verif_while = SeenWhile.VERIF
            lot.last_used = now
            lot.last_used_by = identity
            lot.save(update_fields=['last_verif', 'last_verif_by', 'last_verif_while', 'last_used', 'last_used_by'])
            requirements = requirements_status(lot, today)
            complete = all(row['present'] >= row['required'] for row in requirements) and not report['expired']

        verif.complete = complete
        verif.present_count = len(report['present'])
        verif.missing_count = len(report['missing'])
        verif.expired_count = len(report['expired'])
        verif.replaced_count = len(report['replaced'])
        verif.save()

    report['verif_id'] = verif.id
    report['complete'] = complete
    report['requirements'] = requirements
    return report


def move_items(iids, lot, identity, context):
    """Deplace des items dans un lot (ou en stock si lot est None) sans toucher aux autres."""
    now = timezone.now()
    requested = list(dict.fromkeys(iids))
    with transaction.atomic():
        items = list(Items.objects.select_for_update().filter(iid__in=requested))
        found = {item.iid for item in items}
        for item in items:
            item.location = lot
            item.status = ItemStatus.ACTIVE
            item.missed_verifs = 0
            item.touch(identity, context, now)
        Items.objects.bulk_update(items, ['location', 'status', 'missed_verifs', 'last_seen', 'last_seen_by', 'last_seen_while'])
        if lot is not None:
            lot.last_used = now
            lot.last_used_by = identity
            lot.save(update_fields=['last_used', 'last_used_by'])
    return {'moved': [iid for iid in requested if iid in found], 'unknown': [iid for iid in requested if iid not in found]}


def mark_deleted(item, identity, reason):
    now = timezone.now()
    item.status = ItemStatus.DELETED
    item.deleted = now
    item.deleted_by = identity
    item.deleted_reason = reason[:128]
    item.location = None
    item.touch(identity, SeenWhile.DELETE, now)
    item.save()


def restore(item, identity):
    item.status = ItemStatus.ACTIVE
    item.deleted = None
    item.deleted_by = ''
    item.deleted_reason = ''
    item.missed_verifs = 0
    item.touch(identity, SeenWhile.RESTORE)
    item.save()


def stock_status(soon_days=30):
    """Etat des stocks par type d'item (une ligne par ItemType)."""
    today = timezone.localdate()
    soon = today + timedelta(days=soon_days)
    items = 'itemspacks__items'
    active = Q(**{f'{items}__status': ItemStatus.ACTIVE})
    fresh = Q(itemspacks__peremption__isnull=True) | Q(itemspacks__peremption__gte=today)
    expired = Q(itemspacks__peremption__lt=today)
    in_stock = Q(**{f'{items}__location__isnull': True})
    in_lot = Q(**{f'{items}__location__isnull': False})
    soon_filter = Q(itemspacks__peremption__gte=today) & Q(itemspacks__peremption__lte=soon)
    queryset = ItemType.objects.order_by('name').annotate(
        stock_fresh=Count(items, filter=active & fresh & in_stock),
        stock_expired=Count(items, filter=active & expired & in_stock),
        lots_fresh=Count(items, filter=active & fresh & in_lot),
        lots_expired=Count(items, filter=active & expired & in_lot),
        expiring_soon=Count(items, filter=active & soon_filter),
        missing=Count(items, filter=Q(**{f'{items}__status': ItemStatus.MISSING})),
    )
    return [
        {
            'type': row.type,
            'name': row.name,
            'min_quantity': row.min_quantity,
            'perissable': row.perissable,
            'stock_fresh': row.stock_fresh,
            'stock_expired': row.stock_expired,
            'lots_fresh': row.lots_fresh,
            'lots_expired': row.lots_expired,
            'expiring_soon': row.expiring_soon,
            'missing': row.missing,
        }
        for row in queryset
    ]
