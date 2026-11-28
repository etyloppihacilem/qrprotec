# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          restock.py
#       -\-    _|__
#        |\___/  . \        Created on 28 Nov. 2026 at 09:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Ecran de reassort du front web (vérif incomplete) : ou prendre dans le stock les items qui manquent au lot.

Pour chaque type demande, les rangements du stock (Lots dont le type est un rangement, stock compris) qui en ont
d'utilisables (ni perimes, ni desactives, ni signales inutilisables), et les items du stock sans rangement. Ordre :
le rangement dont le premier item perime le plus tot d'abord (on sort d'abord ce qui perime, moins de perimes a
jeter), puis le plus fourni ; un rangement sans peremption apres ceux qui en ont.
"""

from __future__ import annotations

from datetime import date
from typing import TYPE_CHECKING, Any

from django.utils import timezone

from .models import Items, ItemStatus

if TYPE_CHECKING:
    from .models import Lots

MAX_TYPES = 60   # types demandes en une fois (une liste de reassort)
MAX_PLACES = 8   # rangements rendus par type


def _place(lot: Lots) -> dict[str, Any]:
    """Rangement tel qu'affiche : nom, genre, chemin dans le stock (sans le lot du stock) et ou le trouver."""
    path = [parent for parent in reversed(lot.ancestors()) if not parent.is_stock]
    return {
        'id': lot.id,
        'name': lot.name,
        'kind': '' if lot.is_stock else lot.storage_kind,
        'path': [parent.name for parent in path],
        'location': lot.storage_location,
        # ou trouver le rangement, de la piece au rangement lui-meme (« Local TECH : sous-sol », « Armoire A : au fond »)
        'hints': [{'name': row.name, 'location': row.storage_location} for row in [*path, lot] if row.storage_location],
        'stock': lot.is_stock,
    }


def stock_places(types: list[str], today: date | None = None) -> dict[str, list[dict[str, Any]]]:
    """{type: [{id, name, kind, path, location, hints, stock, count, next_expiry}]} des rangements du stock qui ont des items
    utilisables de ces types. Items du stock sans rangement : une ligne {id: '', name: 'Stock (sans rangement)'}."""
    today = today or timezone.localdate()
    wanted = list(dict.fromkeys(str(value)[:16] for value in types if value))[:MAX_TYPES]
    items = (Items.objects.filter(status=ItemStatus.ACTIVE, pack__item_type_id__in=wanted, disabled=False, unusable=False)
             .select_related('pack__item_type', 'location__lot_type', 'location__parent'))
    rows: dict[str, dict[str, dict[str, Any]]] = {}
    lots: dict[str, Lots] = {}
    for item in items:
        lot = item.location
        if lot is not None and (not lot.active or not lot.lot_type.storage):
            continue  # dans un lot ordinaire : pas dans le stock
        if item.needs_replacement(today):
            continue
        key = lot.id if lot is not None else ''
        if lot is not None:
            lots[key] = lot
        row = rows.setdefault(item.pack.item_type_id, {}).setdefault(key, {'count': 0, 'next_expiry': None})
        row['count'] += 1
        peremption = item.pack.peremption
        if peremption is not None and (row['next_expiry'] is None or peremption < row['next_expiry']):
            row['next_expiry'] = peremption
    places = {key: _place(lot) for key, lot in lots.items()}
    loose = {'id': '', 'name': 'Stock (sans rangement)', 'kind': '', 'path': [], 'location': '', 'hints': [], 'stock': True}
    result: dict[str, list[dict[str, Any]]] = {}
    for type_id in wanted:
        found = [dict(places.get(key, loose), count=row['count'], next_expiry=row['next_expiry'])
                 for key, row in rows.get(type_id, {}).items()]
        found.sort(key=lambda row: (row['next_expiry'] is None, row['next_expiry'] or date.max, -row['count'], row['name']))
        result[type_id] = [dict(row, next_expiry=row['next_expiry'].isoformat() if row['next_expiry'] else None)
                           for row in found[:MAX_PLACES]]
    return result
