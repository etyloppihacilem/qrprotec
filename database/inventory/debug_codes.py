# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          debug_codes.py
#       -\-    _|__
#        |\___/  . \        Created on 30 Oct. 2026 at 15:30
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Outil de debug des codes (front web, admins seulement) : contenu de toutes les etiquettes que le poste a pu
imprimer, pour les reafficher a l'ecran et les scanner a la douchette.

Items (QR de l'URL, QR reduit ou code-barres de l'iid court, version taches), lots (etiquette publique, etiquette
cle, scelle et etiquette d'ouverture du scelle en cours), paquets scelles et listes de materiel. Jamais les badges
ni les liens de deblocage de PIN : ce sont des identifiants de personnes.
"""

from . import serializers as ser
from .models import Checklist, Items, LabelStyle, Lots, SealedPacks, compact_iid


def item_code(item):
    """Contenu du code imprime sur l'etiquette de l'item, d'apres le style de son type (voir LabelStyle)."""
    item_type = item.pack.item_type
    short = compact_iid(item.iid)
    if item_type.label_style != LabelStyle.QR:
        return short  # QR reduit et code-barres : l'iid court seul
    return ser.public_url('task' if item_type.tasks else 'item', id=short)


def item_row(item):
    item_type = item.pack.item_type
    return {
        'iid': item.iid,
        'short': compact_iid(item.iid),
        'type': item_type.type,
        'type_name': item_type.name,
        'peremption': ser._date(item.pack.peremption),
        'status': item.status,
        'disabled': item.disabled,
        'lot': item.location_id or '',
        'pack': item.sealed_pack_id or '',
        'style': item_type.label_style,
        'tasks': bool(item_type.tasks),
        'code': item_code(item),
    }


def lot_codes(lot):
    """Etiquettes du lot : publique, cle (seule etiquette d'un rangement), scelle et ouverture du scelle en cours."""
    codes = []
    if not lot.storage:
        codes.append({'kind': 'public', 'label': 'Étiquette publique', 'code': ser.public_url('verif', lot=lot.id)})
    codes.append({'kind': 'key', 'label': 'Étiquette de rangement' if lot.storage else 'Étiquette clé',
                  'code': ser.public_url('verif', lot=lot.id, key=lot.verif_key)})
    if lot.is_sealed and lot.seal_code:
        codes.append({'kind': 'seal', 'label': 'Scellé', 'code': ser.public_url('seal', lot=lot.id, s=lot.seal_code)})
    if lot.is_sealed and lot.seal_open_code:
        codes.append({'kind': 'sealopen', 'label': 'Ouverture du scellé',
                      'code': ser.public_url('unseal', lot=lot.id, c=lot.seal_open_code)})
    return codes


def lot_tree():
    """Tous les lots, actifs d'abord, en arbre (profondeur d'abord, freres par nom) : chaque lot suit son parent."""
    lots = list(Lots.objects.select_related('lot_type').all())
    by_parent = {}
    known = {lot.id for lot in lots}
    for lot in lots:
        by_parent.setdefault(lot.parent_id if lot.parent_id in known else None, []).append(lot)
    for children in by_parent.values():
        children.sort(key=lambda lot: (not lot.active, lot.is_stock, lot.name.lower(), lot.id))
    rows, seen = [], set()

    def walk(lot, depth):
        if lot.id in seen:
            return
        seen.add(lot.id)
        rows.append((lot, depth))
        for child in by_parent.get(lot.id, []):
            walk(child, depth + 1)

    for lot in by_parent.get(None, []):
        walk(lot, 0)
    for lot in lots:  # cycle de parents (ne devrait pas arriver) : rien n'est oublie
        walk(lot, 0)
    return rows


def catalog():
    items = list(Items.objects.select_related('pack__item_type')
                 .order_by('pack__item_type__name', 'pack__peremption', 'iid'))
    return {
        'lots': [{
            'id': lot.id,
            'name': lot.name,
            'type_name': lot.lot_type.name,
            'parent': lot.parent_id or '',
            'depth': depth,
            'storage': lot.storage,
            'stock': lot.is_stock,
            'active': lot.active,
            'sealed': lot.is_sealed,
            'seal_number': lot.seal_number,
            'codes': lot_codes(lot),
        } for lot, depth in lot_tree()],
        'items': [item_row(item) for item in items],
        'packs': [{
            'id': pack.id,
            'type_name': pack.item_type.name,
            'peremption': ser._date(pack.peremption),
            'count': pack.count,
            'opened': pack.opened is not None,
            'code': ser.public_url('pack', id=pack.id),
        } for pack in SealedPacks.objects.select_related('item_type').order_by('opened', '-created')],
        'lists': [{
            'id': checklist.id,
            'name': checklist.name,
            'status': checklist.status,
            'template': checklist.is_template,
            'code': ser.public_url('liste', id=checklist.id),
        } for checklist in Checklist.objects.order_by('is_template', '-updated')],
    }
