# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          serializers.py
#       -\-    _|__
#        |\___/  . \        Created on 29 Sep. 2026 at 14:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Conversion des modeles en dictionnaires JSON pour les API."""

from datetime import datetime, timedelta
from urllib.parse import urlencode

from django.utils import timezone

from .idendity import parse_identity
from .models import ItemStatus, qrprotec_setting
from .services import requirements_status


def public_url(path: str, **params) -> str:
    base = qrprotec_setting('PUBLIC_BASE_URL')
    if not base.endswith('/'):
        base += '/'
    return f"{base}{path}?{urlencode(params)}"


def _date(value):
    """Date seule (peremption, expiration) ou horodatage en heure locale (verifs, passages)."""
    if not value:
        return None
    if isinstance(value, datetime):
        return timezone.localtime(value).isoformat(timespec='seconds')
    return value.isoformat()


def _who(value):
    return parse_identity(value)['display_name'] if value else ''


def item_type_dict(item_type):
    return {
        'type': item_type.type,
        'name': item_type.name,
        'description': item_type.description,
        'min_quantity': item_type.min_quantity,
        'perissable': item_type.perissable,
        'default_pack_size': item_type.default_pack_size,
    }


def item_dict(item, today=None):
    today = today or timezone.localdate()
    return {
        'iid': item.iid,
        'type': item.pack.item_type_id,
        'type_name': item.pack.item_type.name,
        'peremption': _date(item.pack.peremption),
        'expired': item.is_expired(today),
        'status': item.status,
        'location': item.location_id,
        'location_name': item.location.name if item.location_id else '',
        # en stock : hors lot, ou range dans un rangement du stock (armoire, tiroir...)
        'in_stock': item.location_id is None or item.location.lot_type.storage,
        'sealed_pack': item.sealed_pack_id,
        'missed_verifs': item.missed_verifs,
        'last_seen': _date(item.last_seen),
        'last_seen_by': _who(item.last_seen_by),
        'last_seen_while': item.last_seen_while,
        'added': _date(item.added),
        'added_by': _who(item.added_by),
        'deleted_reason': item.deleted_reason,
    }


def sealed_pack_dict(sealed_pack, with_items=True):
    data = {
        'id': sealed_pack.id,
        'type': sealed_pack.item_type_id,
        'type_name': sealed_pack.item_type.name,
        'peremption': _date(sealed_pack.peremption),
        'count': sealed_pack.count,
        'created': _date(sealed_pack.created),
        'created_by': _who(sealed_pack.created_by),
        'opened': _date(sealed_pack.opened),
        'opened_by': _who(sealed_pack.opened_by),
        'url': public_url('pack', id=sealed_pack.id),
    }
    if with_items:
        data['items'] = [item.iid for item in sealed_pack.items.order_by('iid')]
    return data


def lot_type_dict(lot_type):
    return {
        'type': lot_type.type,
        'name': lot_type.name,
        'description': lot_type.description,
        'version': lot_type.version,
        'storage': lot_type.storage,
        'requirements': [
            {
                'type': requirement.item_type_id,
                'type_name': requirement.item_type.name,
                'quantity': requirement.quantity,
                'location': requirement.location,
            }
            for requirement in lot_type.requirements.select_related('item_type').order_by('location', 'item_type__name')
        ],
    }


STATE_LABELS = {
    'sealed': '✔ Scellé',
    'sealed_expired': '✘ Scellé, contient des périmés',
    'never': '✘ Jamais vérifié',
    'incomplete': '✘ Incomplet',
    'recommended': '⚠ Vérif recommandée, réassort',
    'verified': '✔ Vérifié, complet',
}
KIND_ORDER = {'ok': 0, 'warn': 1, 'bad': 2}


def lot_state(data):
    """Etat d'un lot seul (memes regles que les fronts) : kind 'ok' (vert), 'warn' (orange) ou 'bad' (rouge)."""
    if data['is_sealed']:
        code = 'sealed_expired' if data['expired_count'] else 'sealed'
    elif not data['last_verif']:
        code = 'never'
    elif not data['complete']:
        code = 'incomplete'
    elif data['verif_recommended']:
        code = 'recommended'
    else:
        code = 'verified'
    kind = 'ok' if code in ('sealed', 'verified') else 'warn' if code == 'recommended' else 'bad'
    return {'kind': kind, 'code': code, 'label': STATE_LABELS[code]}


def _lot_core(lot, today, items=None):
    """Champs communs a toutes les vues d'un lot (sans cles ni contenu detaille)."""
    if items is None:
        items = list(lot.items.select_related('pack__item_type').filter(status=ItemStatus.ACTIVE).order_by('iid'))
    requirements = requirements_status(lot, today)
    expired = [item for item in items if item.is_expired(today)]
    soon_limit = today + timedelta(days=30)
    soon = [item for item in items if item.pack.peremption and today <= item.pack.peremption <= soon_limit]
    data = {
        'id': lot.id,
        'lot_type': lot.lot_type_id,
        'lot_type_name': lot.lot_type.name,
        'storage': lot.lot_type.storage,
        'name': lot.name,
        'name_short': lot.name_short,
        'version': lot.version,
        'active': lot.active,
        'parent': lot.parent_id,
        'is_sealed': lot.is_sealed,
        'sealed': _date(lot.sealed) if lot.is_sealed else None,
        'sealed_by': _who(lot.sealed_by) if lot.is_sealed else '',
        'seal_number': lot.seal_number if lot.is_sealed else '',
        'unsealed': _date(lot.unsealed),
        'unsealed_by': _who(lot.unsealed_by),
        # un lot scelle reste valide jusqu'a la premiere peremption de son contenu
        'valid_until': _date(min((item.pack.peremption for item in items if item.pack.peremption), default=None)),
        'created': _date(lot.created),
        'last_verif': _date(lot.last_verif),
        'last_verif_by': _who(lot.last_verif_by),
        'verif_recommended': lot.verif_recommended,
        'restocked': _date(lot.restocked) if lot.verif_recommended else None,
        'restocked_by': _who(lot.restocked_by) if lot.verif_recommended else '',
        'restocked_count': lot.restocked_count if lot.verif_recommended else 0,
        'item_count': len(items),
        'expired_count': len(expired),
        'expiring_soon_count': len(soon),
        'requirements': requirements,
        'complete': all(row['present'] >= row['required'] for row in requirements) and not expired,
        'public_url': public_url('verif', lot=lot.id),
    }
    data['state'] = lot_state(data)
    return data


def aggregate_tree(rows):
    """Etat d'un lot global a partir de ses lots (lignes de _lot_core avec 'depth', en profondeur d'abord).

    - un lot qui ne contient rien et n'attend rien mais a des sous-lots (simple regroupement) ne compte pas ;
    - un sous-lot d'un lot scelle intact est valide tant qu'il ne contient pas de perimes ;
    - le lot global est valide si tous ses lots le sont, et sa derniere verif est la plus ancienne des
      dernieres verifs de ses lots (aucune si l'un d'eux n'a jamais ete verifie).
    Ajoute a chaque ligne 'counted' et 'effective' (etat dans le lot global).
    """
    parents = {row['id']: row['parent'] for row in rows}
    by_id = {row['id']: row for row in rows}
    has_children = {row['parent'] for row in rows if row['parent']}
    kind = 'ok'
    oldest = None
    never = False
    counted = 0
    problems = 0
    for row in rows:
        covered = False
        parent = parents.get(row['id'])
        while parent in by_id:
            if by_id[parent]['state']['code'] == 'sealed':
                covered = True
                break
            parent = parents.get(parent)
        row['counted'] = bool(row['requirements'] or row['item_count'] or row['id'] not in has_children)
        if covered and not row['expired_count']:
            row['effective'] = {'kind': 'ok', 'code': 'covered', 'label': '✔ Dans un lot scellé'}
        else:
            row['effective'] = row['state']
        if not row['counted']:
            continue
        counted += 1
        effective = row['effective']
        if KIND_ORDER[effective['kind']] > KIND_ORDER[kind]:
            kind = effective['kind']
        if effective['kind'] != 'ok':
            problems += 1
        if effective['code'] == 'covered':
            continue
        if row['is_sealed'] and not row['last_verif']:
            stamp = row['sealed']
        else:
            stamp = row['last_verif']
        if not stamp:
            never = True
        elif oldest is None or stamp < oldest:
            oldest = stamp
    if kind == 'ok':
        label = '✔ Tous les lots sont valides'
    elif kind == 'warn':
        label = f'⚠ Vérif recommandée ({problems} lot(s))'
    else:
        label = f'✘ {problems} lot(s) à traiter'
    return {
        'kind': kind,
        'label': label,
        'complete': kind == 'ok',
        'last_verif': None if never else oldest,
        'never_verified': never,
        'lot_count': counted,
    }


def _tree_row(lot, today, depth):
    row = _lot_core(lot, today)
    row['depth'] = depth
    return row


def lot_global(lot, today):
    """Lot global (racine) du lot et etat de chacun de ses lots ; None si le lot n'a ni parent ni sous-lot."""
    root = lot.root()
    tree = root.descendants()
    if len(tree) <= 1:
        return None
    rows = [_tree_row(sub_lot, today, sub_lot.depth) for sub_lot in tree]
    summary = aggregate_tree(rows)
    keep = ('id', 'name', 'name_short', 'lot_type_name', 'storage', 'parent', 'depth', 'is_sealed', 'last_verif',
            'last_verif_by', 'item_count', 'expired_count', 'complete', 'verif_recommended', 'state', 'effective',
            'counted')
    summary.update({
        'id': root.id,
        'name': root.name,
        'lots': [{key: row[key] for key in keep} for row in rows],
    })
    return summary


def lot_dict(lot, local=False, with_items=False, today=None, with_global=True):
    today = today or timezone.localdate()
    items = list(lot.items.select_related('pack__item_type').filter(status=ItemStatus.ACTIVE).order_by('iid'))
    data = _lot_core(lot, today, items)
    ancestors = lot.ancestors()
    data['parent_name'] = ancestors[0].name if ancestors else ''
    data['path'] = [{'id': parent.id, 'name': parent.name} for parent in reversed(ancestors)]
    data['children'] = [
        {'id': child.id, 'name': child.name}
        for child in sorted(lot.children.filter(active=True), key=lambda child: (child.name.lower(), child.id))
    ]
    if with_global:
        data['global'] = lot_global(lot, today)
    if with_items:
        _add_items(data, lot, items, today)
        # sous-lots (en profondeur d'abord) : une verif du lot les couvre aussi
        data['descendants'] = []
        for sub_lot in lot.descendants(include_self=False):
            sub_items = list(sub_lot.items.select_related('pack__item_type').filter(status=ItemStatus.ACTIVE)
                             .order_by('iid'))
            row = _lot_core(sub_lot, today, sub_items)
            row['depth'] = sub_lot.depth
            _add_items(row, sub_lot, sub_items, today)
            data['descendants'].append(row)
    if local:
        data['verif_key'] = lot.verif_key
        data['verif_key_expires'] = _date(lot.verif_key_expires)
        data['private_url'] = public_url('verif', lot=lot.id, key=lot.verif_key)
        data['seal_url'] = public_url('seal', lot=lot.id, s=lot.seal_code) if lot.is_sealed else ''
    return data


def _add_items(data, lot, items, today):
    data['items'] = [item_dict(item, today) for item in items]
    data['missing_items'] = [
        item_dict(item, today)
        for item in lot.items.select_related('pack__item_type').filter(status=ItemStatus.MISSING).order_by('iid')
    ]


def lot_list(lots, local=False, today=None):
    """Liste de lots dans l'ordre de l'arborescence (chaque lot global suivi de ses sous-lots), avec pour chaque
    lot sa profondeur et l'etat de son lot global."""
    today = today or timezone.localdate()
    lots = list(lots)
    ids = {lot.id for lot in lots}
    children = {}
    for lot in lots:
        if lot.parent_id in ids:
            children.setdefault(lot.parent_id, []).append(lot)
    for siblings in children.values():
        siblings.sort(key=lambda child: (child.name.lower(), child.id))  # meme ordre que Lots.descendants
    ordered = []
    seen = set()

    def walk(lot, depth, root):
        if lot.id in seen:
            return
        seen.add(lot.id)
        ordered.append((lot, depth, root))
        for child in children.get(lot.id, []):
            walk(child, depth + 1, root)

    for lot in lots:
        if lot.parent_id not in ids:
            walk(lot, 0, lot)
    for lot in lots:  # cycle eventuel (ne devrait pas exister)
        walk(lot, 0, lot)
    rows = []
    trees = {}
    for lot, depth, root in ordered:
        row = lot_dict(lot, local=local, today=today, with_global=False)
        row['depth'] = depth
        row['root'] = root.id
        rows.append(row)
        trees.setdefault(root.id, []).append(row)
    for root_id, tree in trees.items():
        if len(tree) <= 1:
            continue
        summary = aggregate_tree(tree)
        summary.update({'id': root_id, 'name': tree[0]['name']})
        for row in tree:
            row['global'] = summary
    return rows


def user_dict(user, local=False):
    data = {
        'matricule': user.matricule,
        'nom': user.nom,
        'prenom': user.prenom,
        'role': user.role,
        'role_label': user.get_role_display(),
        'privileged': user.privileged,  # gestion ou admin (mode privilegie du front)
        'has_pin': user.has_pin,
        'pin_required': user.pin_required,
        'active': user.active,
        'key_expires': _date(user.key_expires),
        'pin_blocked': user.pin_blocked is not None,
        'pin_reset_required': user.pin_reset_required,
    }
    if local:
        contact = user.pin_contact
        data['pin_blocked_since'] = _date(user.pin_blocked)
        data['pin_failures'] = user.pin_failures_total
        data['pin_contact'] = contact.matricule if contact else ''
        data['pin_contact_name'] = str(contact) if contact else ''
        # La cle du badge n'est connue qu'a sa creation ou son renouvellement (seule son empreinte est en base) :
        # elle n'apparait que dans cette reponse-la, pour imprimer le badge.
        key = getattr(user, 'new_key', None)
        if key:
            data['key'] = key
            data['badge_url'] = public_url('badge', m=user.matricule, key=key)
    return data


def verif_dict(verif):
    return {
        'id': verif.id,
        'lot': verif.lot_id,
        'datetime': _date(verif.datetime),
        'by': _who(verif.by),
        'complete': verif.complete,
        'present_count': verif.present_count,
        'missing_count': verif.missing_count,
        'expired_count': verif.expired_count,
        'replaced_count': verif.replaced_count,
    }


def recipient_dict(recipient):
    # la cle d'identification n'est jamais renvoyee
    return {
        'id': recipient.id,
        'name': recipient.name,
        'user': recipient.user,
        'has_password': bool(recipient.password),
        'active': recipient.active,
        'last_sent': _date(recipient.last_sent),
        'last_status': recipient.last_status,
    }


def notification_settings_dict(settings_row, recipients):
    return {
        'enabled': settings_row.enabled,
        'events': {
            'stock_low': settings_row.stock_low,
            'verif_problem': settings_row.verif_problem,
            'seal_broken': settings_row.seal_broken,
            'expired_daily': settings_row.expired_daily,
        },
        'recipients': [recipient_dict(recipient) for recipient in recipients],
    }
