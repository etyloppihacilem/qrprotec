# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          consumables.py
#       -\-    _|__
#        |\___/  . \        Created on 12 Nov. 2026 at 18:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Consommables d'etiquettes : bobines et rubans de transfert, stock estime, autonomie et couts (fronts seulement).

On ne suit pas chaque bobine : chaque categorie d'etiquette (largeur x hauteur, support) a un stock theorique en
etiquettes, et les rubans un stock en millimetres. Le stock part du dernier inventaire (ou de la premiere reception),
augmente a chaque reception et baisse avec les impressions comptees (print_counts) a partir du lendemain. Un ruban
s'use de (hauteur + espace) par etiquette de transfert imprimee, toutes categories de transfert confondues.

Autonomie : stock / consommation par jour des 90 derniers jours (ou depuis le debut du compteur). Sous
ServerSettings.consumable_alert_days jours, une tache « Commander » apparait dans la Gestion de la TECH du poste.
"""

from datetime import timedelta
from decimal import Decimal, InvalidOperation

from django.db.models import Min, Sum
from django.utils import timezone

from .idendity import identity_name
from .models import (
    ConsumableKind, ConsumableMove, ConsumableMoveKind, ConsumableRef, LabelSupport, PrintCount, ServerSettings,
)
from .print_counts import category_label, _number

RIBBON_GAP_MM = Decimal(3)  # espace entre deux etiquettes sur le ruban (etiquettes pre-decoupees)
RATE_DAYS = 90              # consommation moyenne sur les 90 derniers jours
DAYS_PER_MONTH = Decimal('30.44')
RIBBON_KEY = 'ribbon'
RECENT_MOVES = 30


class ConsumableError(Exception):
    pass


def pool_key(ref):
    if ref.kind == ConsumableKind.RIBBON:
        return RIBBON_KEY
    return f'{_number(ref.width_mm)}x{_number(ref.height_mm)}-{ref.support}'


def pool_label(ref):
    if ref.kind == ConsumableKind.RIBBON:
        return 'Rubans de transfert'
    return category_label(ref.width_mm, ref.height_mm, ref.support)


def roll_units(ref):
    """Etiquettes d'une bobine, ou millimetres d'un rouleau de ruban."""
    if ref.kind == ConsumableKind.RIBBON:
        return int((ref.length_m or 0) * 1000)
    return ref.per_roll


def ref_dict(ref):
    return {
        'id': ref.id, 'kind': ref.kind, 'name': ref.name, 'supplier': ref.supplier,
        'width_mm': float(ref.width_mm) if ref.width_mm is not None else None,
        'height_mm': float(ref.height_mm) if ref.height_mm is not None else None,
        'support': ref.support, 'per_roll': ref.per_roll,
        'length_m': float(ref.length_m) if ref.length_m is not None else None,
        'per_box': ref.per_box, 'box_price': float(ref.box_price) if ref.box_price is not None else None,
        'archived': ref.archived, 'pool': pool_key(ref), 'pool_label': pool_label(ref),
        'summary': ref_summary(ref),
    }


def ref_summary(ref):
    """« boîte de 12 bobines de 2 000 étiquettes, 57,00 € » / « boîte de 25 rouleaux de 74 m »."""
    if ref.kind == ConsumableKind.RIBBON:
        text = f'boîte de {ref.per_box} rouleau{"x" if ref.per_box > 1 else ""} de {_number(ref.length_m or 0)} m'
    else:
        text = (f'boîte de {ref.per_box} bobine{"s" if ref.per_box > 1 else ""} de '
                f'{_thousands(ref.per_roll)} étiquette{"s" if ref.per_roll > 1 else ""}')
    if ref.box_price is not None:
        text += f', {_euros(ref.box_price)}'
    return text


def _thousands(value):
    return f'{int(value):,}'.replace(',', ' ')


def _euros(value):
    return f'{Decimal(value):.2f}'.replace('.', ',') + ' €'


# ---------------------------------------------------------------------------------------------------------------------
# References

def _decimal(value, field, minimum, maximum, places='0.1'):
    try:
        number = Decimal(str(value)).quantize(Decimal(places))
    except (InvalidOperation, ValueError, TypeError):
        raise ConsumableError(f'{field} : nombre attendu')
    if not minimum <= number <= maximum:
        raise ConsumableError(f'{field} : entre {minimum} et {maximum}')
    return number


def _int(value, field, minimum, maximum):
    try:
        number = int(value)
    except (TypeError, ValueError):
        raise ConsumableError(f'{field} : entier attendu')
    if not minimum <= number <= maximum:
        raise ConsumableError(f'{field} : entre {minimum} et {maximum}')
    return number


def save_ref(data, ref=None):
    """Cree ou modifie une reference. Le type (bobines / rubans) ne change plus apres la creation."""
    creating = ref is None
    if creating:
        kind = data.get('kind')
        if kind not in ConsumableKind.values:
            raise ConsumableError('kind : labels ou ribbon attendu')
        ref = ConsumableRef(kind=kind)
    if creating or 'name' in data:
        name = str(data.get('name') or '').strip()
        if not name:
            raise ConsumableError('Nom de la référence obligatoire')
        ref.name = name[:80]
    if 'supplier' in data:
        ref.supplier = str(data.get('supplier') or '').strip()[:80]
    if ref.kind == ConsumableKind.LABELS:
        if creating or 'width_mm' in data:
            ref.width_mm = _decimal(data.get('width_mm'), 'Largeur', 5, 300)
        if creating or 'height_mm' in data:
            ref.height_mm = _decimal(data.get('height_mm'), 'Hauteur', 5, 300)
        if creating or 'support' in data:
            if data.get('support') not in LabelSupport.values:
                raise ConsumableError('support : direct ou transfer attendu')
            ref.support = data['support']
        if creating or 'per_roll' in data:
            ref.per_roll = _int(data.get('per_roll'), 'Étiquettes par bobine', 1, 1000000)
    elif creating or 'length_m' in data:
        ref.length_m = _decimal(data.get('length_m'), 'Longueur du rouleau (m)', 1, 100000)
    if creating or 'per_box' in data:
        ref.per_box = _int(data.get('per_box', 1), 'Par boîte', 1, 10000)
    if 'box_price' in data:
        price = data.get('box_price')
        ref.box_price = None if price in (None, '') else _decimal(price, 'Prix de la boîte', 0, 1000000, '0.01')
    if 'archived' in data:
        ref.archived = bool(data.get('archived'))
    ref.save()
    return ref


# ---------------------------------------------------------------------------------------------------------------------
# Receptions et inventaires

def add_move(data, by):
    ref = ConsumableRef.objects.filter(id=data.get('ref')).first()
    if ref is None:
        raise ConsumableError('Référence inconnue')
    kind = data.get('kind')
    if kind not in ConsumableMoveKind.values:
        raise ConsumableError('kind : reception ou inventory attendu')
    if ref.archived and kind == ConsumableMoveKind.RECEPTION:
        raise ConsumableError(f'La référence « {ref.name} » est archivée')
    boxes = _int(data.get('boxes') or 0, 'Boîtes', 0, 100000)
    rolls = _int(data.get('rolls') or 0, 'Bobines' if ref.kind == ConsumableKind.LABELS else 'Rouleaux', 0, 1000000)
    loose = _int(data.get('loose') or 0, 'Étiquettes en vrac' if ref.kind == ConsumableKind.LABELS
                 else 'Mètres en vrac', 0, 100000000)
    per_roll = roll_units(ref)
    units = (boxes * ref.per_box + rolls) * per_roll + loose * (1000 if ref.kind == ConsumableKind.RIBBON else 1)
    if kind == ConsumableMoveKind.RECEPTION and units == 0:
        raise ConsumableError('Indiquez ce qui a été reçu')
    return ConsumableMove.objects.create(ref=ref, kind=kind, boxes=boxes, rolls=rolls, loose=loose, units=units,
                                         by=by[:64])


def delete_move(move_id):
    deleted, _ = ConsumableMove.objects.filter(id=move_id).delete()
    if not deleted:
        raise ConsumableError('Mouvement inconnu')


def move_dict(move):
    ref = move.ref
    unit = 'm' if ref.kind == ConsumableKind.RIBBON else 'étiquette'
    parts = []
    if move.boxes:
        parts.append(f'{move.boxes} boîte{"s" if move.boxes > 1 else ""}')
    if move.rolls:
        word = 'rouleau' if ref.kind == ConsumableKind.RIBBON else 'bobine'
        parts.append(f'{move.rolls} {word}{("x" if word == "rouleau" else "s") if move.rolls > 1 else ""}')
    if move.loose:
        parts.append(f'{_thousands(move.loose)} {unit}{"s" if move.loose > 1 and unit != "m" else ""}')
    return {
        'id': move.id, 'kind': move.kind, 'kind_label': ConsumableMoveKind(move.kind).label,
        'ref': ref.id, 'ref_name': ref.name, 'pool': pool_key(ref), 'pool_label': pool_label(ref),
        'what': ' + '.join(parts) or 'rien', 'units': move.units, 'units_text': _units_text(ref.kind, move.units),
        'created': timezone.localtime(move.created).isoformat(timespec='seconds'),
        'by': identity_name(move.by) if move.by else '',
    }


def _units_text(kind, units):
    if kind == ConsumableKind.RIBBON:
        meters = Decimal(units) / 1000
        meters = meters.quantize(Decimal('0.1') if meters < 10 else Decimal('1'))
        return f'{_thousands(meters) if meters >= 10 else _number(meters)} m'
    return f'{_thousands(units)} étiquette{"s" if units > 1 else ""}'


# ---------------------------------------------------------------------------------------------------------------------
# Stock estime, consommation, autonomie

def _consumption(key, since=None, until=None):
    """Etiquettes (categorie) ou millimetres de ruban imprimes, jours `since` exclu a `until` inclus."""
    rows = PrintCount.objects.all()
    if since is not None:
        rows = rows.filter(day__gt=since)
    if until is not None:
        rows = rows.filter(day__lte=until)
    if key == RIBBON_KEY:
        total = Decimal(0)
        for row in rows.filter(support=LabelSupport.TRANSFER).values('height_mm').annotate(count=Sum('count')):
            total += (row['height_mm'] + RIBBON_GAP_MM) * row['count']
        return int(total)
    width, rest = key.split('x', 1)
    height, support = rest.rsplit('-', 1)
    return rows.filter(width_mm=Decimal(width.replace(',', '.')), height_mm=Decimal(height.replace(',', '.')),
                       support=support).aggregate(total=Sum('count'))['total'] or 0


def _stock(key, moves):
    """(stock, depuis) : None si aucun mouvement (stock inconnu)."""
    if not moves:
        return None, None
    inventories = [move for move in moves if move.kind == ConsumableMoveKind.INVENTORY]
    start = inventories[-1] if inventories else moves[0]
    base = start.units if inventories else 0
    received = sum(move.units for move in moves
                   if move.kind == ConsumableMoveKind.RECEPTION and move.created >= start.created)
    since = timezone.localtime(start.created).date()
    return base + received - _consumption(key, since=since), since


def _rate(key, today):
    """Consommation par jour (etiquettes ou mm) sur les 90 derniers jours, ou depuis le debut du compteur."""
    first = PrintCount.objects.aggregate(first=Min('day'))['first']
    if first is None:
        return Decimal(0)
    days = min(RATE_DAYS, (today - first).days + 1)
    used = _consumption(key, since=today - timedelta(days=days), until=today)
    return Decimal(used) / days


def _unit_price(refs, moves):
    """Prix d'une etiquette ou d'un mm : reference de la derniere reception qui a un prix, sinon la plus recente."""
    ordered = [move.ref for move in reversed(moves) if move.kind == ConsumableMoveKind.RECEPTION]
    ordered += sorted(refs, key=lambda ref: ref.created, reverse=True)
    for ref in ordered:
        units = roll_units(ref) * ref.per_box
        if ref.box_price is not None and units:
            return ref.box_price / units
    return None


def _duration(days):
    """Autonomie lisible : « environ 12 jours », « environ 4 mois », « plus de 2 ans »."""
    if days < 60:
        return f'environ {days} jour{"s" if days > 1 else ""}'
    if days < 730:
        return f'environ {round(days / 30.44)} mois'
    return 'plus de 2 ans'


def _stock_text(kind, stock, ref):
    """« 8 600 étiquettes, ≈ 4 bobines (12 par boîte) » ; rubans : « 152 m, ≈ 2 rouleaux »."""
    text = _units_text(kind, max(stock, 0))
    per_roll = roll_units(ref) if ref else 0
    if per_roll and stock > 0:
        rolls = Decimal(stock) / per_roll
        word = 'rouleau' if kind == ConsumableKind.RIBBON else 'bobine'
        count = f'{rolls:.1f}'.replace('.', ',').replace(',0', '')
        plural = ('x' if word == 'rouleau' else 's') if rolls >= 2 else ''
        text += f', ≈ {count} {word}{plural}'
    return text


def pools(today=None, settings_row=None):
    today = today or timezone.localdate()
    settings_row = settings_row or ServerSettings.get()
    alert_days = settings_row.consumable_alert_days
    refs = list(ConsumableRef.objects.all())
    moves = list(ConsumableMove.objects.select_related('ref').order_by('created', 'id'))
    grouped = {}
    for ref in refs:
        grouped.setdefault(pool_key(ref), {'refs': [], 'moves': [], 'kind': ref.kind, 'label': pool_label(ref)})
        grouped[pool_key(ref)]['refs'].append(ref)
    for move in moves:
        grouped[pool_key(move.ref)]['moves'].append(move)
    year_start = today.replace(month=1, day=1) - timedelta(days=1)
    result = []
    for key, pool in grouped.items():
        if all(ref.archived for ref in pool['refs']) and not pool['moves']:
            continue
        stock, since = _stock(key, pool['moves'])
        rate = _rate(key, today)
        price = _unit_price(pool['refs'], pool['moves'])
        autonomy = int(stock / rate) if stock is not None and rate > 0 else None
        if stock is None:
            level, state = 'unknown', 'Stock inconnu : faites un inventaire ou une réception'
        elif stock <= 0:
            level, state = 'bad', 'Épuisé d\'après les impressions : à commander (ou faites un inventaire)'
        elif autonomy is not None and autonomy < alert_days:
            level, state = 'warn', f'À commander : {_duration(autonomy)} de stock'
        else:
            level, state = 'ok', (f'{_duration(autonomy).capitalize()} de stock' if autonomy is not None
                                  else 'Pas encore de consommation')
        main_ref = next((move.ref for move in reversed(pool['moves'])), None) or pool['refs'][-1]
        year_used = _consumption(key, since=year_start, until=today)
        monthly = rate * DAYS_PER_MONTH
        result.append({
            'key': key, 'kind': pool['kind'], 'label': pool['label'],
            'stock': stock, 'stock_text': _stock_text(pool['kind'], stock, main_ref) if stock is not None else '',
            'since': since.isoformat() if since else None,
            'monthly': int(monthly), 'monthly_text': _units_text(pool['kind'], int(monthly)) if rate > 0 else '',
            'autonomy_days': autonomy,
            'monthly_cost': float(round(monthly * price, 2)) if price is not None and rate > 0 else None,
            'year_cost': float(round(year_used * price, 2)) if price is not None else None,
            'year_used': year_used, 'year_used_text': _units_text(pool['kind'], year_used),
            'level': level, 'state': state,
            'refs': [ref.id for ref in pool['refs']],
        })
    level_rank = {'bad': 0, 'warn': 1, 'unknown': 2, 'ok': 3}
    result.sort(key=lambda pool: (level_rank[pool['level']], pool['kind'] != ConsumableKind.LABELS, pool['label']))
    return result


def summary():
    settings_row = ServerSettings.get()
    moves = ConsumableMove.objects.select_related('ref').order_by('-created', '-id')[:RECENT_MOVES]
    return {
        'pools': pools(settings_row=settings_row),
        'refs': [ref_dict(ref) for ref in ConsumableRef.objects.all()],
        'moves': [move_dict(move) for move in moves],
        'alert_days': settings_row.consumable_alert_days,
        'ribbon_gap_mm': float(RIBBON_GAP_MM),
        'rate_days': RATE_DAYS,
    }


def set_alert_days(value):
    settings_row = ServerSettings.get()
    settings_row.consumable_alert_days = _int(value, 'Alerte (jours)', 1, 365)
    settings_row.save(update_fields=['consumable_alert_days'])


def order_tasks(today=None):
    """Taches « Commander » de la Gestion de la TECH (poste seulement) : stock epuise ou sous le seuil d'alerte."""
    tasks = []
    for pool in pools(today):
        if pool['level'] not in ('bad', 'warn'):
            continue
        detail = f"stock estimé {pool['stock_text']}"
        if pool['monthly_text']:
            detail += f" · {pool['monthly_text']} par mois"
        detail += ' · fenêtre Gestion > Impressions et consommables'
        tasks.append({'level': pool['level'], 'title': f"Commander : {pool['label']}", 'detail': detail,
                      'autonomy': pool['autonomy_days'] if pool['autonomy_days'] is not None else -1})
    return tasks
