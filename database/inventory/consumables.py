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

Etiquettes perdues (models.LabelLoss, notees au poste : calibrage, etiquettes ratees, accident) : sorties du rouleau
sans etre des impressions. Elles baissent le stock (et le ruban pour le transfert, qui avance avec le papier) des
qu'elles sont notees apres le dernier inventaire. Dans le rythme, calibrages et etiquettes ratees comptent comme des
impressions (ils reviennent a chaque rouleau) ; un accident ponctuel (bourrage, rouleau abime) ne compte pas, pour ne
pas faire commander trop tot pendant 90 jours.

Autonomie : stock / consommation par jour. Sous ServerSettings.consumable_alert_days jours, une tache « Commander »
apparait dans la Gestion de la TECH du poste.

Rythme (consommation par jour), sans se laisser tromper par la mise en place du logiciel, ou tout le materiel deja la
est etiquete d'un coup :
- mesure : impressions des 90 derniers jours, mais jamais celles des SETUP_DAYS premiers jours du compteur (mise en
  place). Tant qu'il n'y a pas MEASURE_MIN_DAYS jours apres la mise en place, pas de rythme mesure ;
- prevu (categorie des etiquettes d'items seulement, ServerSettings.item_label_pool) : items utilises ou jetes par
  jour sur les 6 derniers mois, chacun remplace par un item neuf a etiqueter. Ne compte pas les autres etiquettes
  (lots, emplacements, listes) ni les paquets fermes (une etiquette pour tout le paquet) : c'est un plancher ;
- rythme retenu : le plus grand des deux. Pendant la mise en place, seulement le prevu, et une alerte des qu'il reste
  moins de SETUP_FLOOR_ROLLS bobines (ou rouleaux) : on ne commande pas trop tot, sans risquer la rupture.
"""

from __future__ import annotations

from datetime import timedelta
from decimal import Decimal, InvalidOperation
from typing import TYPE_CHECKING, Any

from django.db.models import Min, Sum
from django.utils import timezone

from .idendity import identity_name
from .models import (
    ConsumableKind, ConsumableMove, ConsumableMoveKind, ConsumableRef, ItemMovement, LabelLoss, LabelLossReason,
    LabelSupport, MovementKind, PrintCount, ServerSettings, Verifs,
)
from .print_counts import category_label, parse_decimal, _number

if TYPE_CHECKING:
    from datetime import date, datetime

    from django.db.models import QuerySet

RIBBON_GAP_MM = Decimal(3)  # espace entre deux etiquettes sur le ruban (etiquettes pre-decoupees)
RATE_DAYS = 90              # consommation moyenne sur les 90 derniers jours
SETUP_DAYS = 56             # mise en place : les 8 premieres semaines du compteur ne comptent pas dans le rythme
MEASURE_MIN_DAYS = 28       # rythme mesure seulement avec 4 semaines d'impressions apres la mise en place
SETUP_FLOOR_ROLLS = 2       # pendant la mise en place : alerte sous 2 bobines (ou rouleaux) en stock
ITEM_RATE_DAYS = 182        # rythme prevu : items utilises ou jetes des 6 derniers mois
DAYS_PER_MONTH = Decimal('30.44')
RIBBON_KEY = 'ribbon'
RECENT_MOVES = 30
MAX_LOSS = 10000            # etiquettes perdues notees d'un coup


class ConsumableError(Exception):
    pass


def pool_key(ref: ConsumableRef) -> str:
    if ref.kind == ConsumableKind.RIBBON:
        return RIBBON_KEY
    return f'{_number(ref.width_mm)}x{_number(ref.height_mm)}-{ref.support}'


def pool_label(ref: ConsumableRef) -> str:
    if ref.kind == ConsumableKind.RIBBON:
        return 'Rubans de transfert'
    return category_label(ref.width_mm, ref.height_mm, ref.support)


def roll_units(ref: ConsumableRef) -> int:
    """Etiquettes d'une bobine, ou millimetres d'un rouleau de ruban."""
    if ref.kind == ConsumableKind.RIBBON:
        return int((ref.length_m or 0) * 1000)
    return ref.per_roll


def ref_dict(ref: ConsumableRef) -> dict[str, Any]:
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


def ref_summary(ref: ConsumableRef) -> str:
    """« boîte de 12 bobines de 2 000 étiquettes, 57,00 € » / « boîte de 25 rouleaux de 74 m »."""
    if ref.kind == ConsumableKind.RIBBON:
        text = f'boîte de {ref.per_box} rouleau{"x" if ref.per_box > 1 else ""} de {_number(ref.length_m or 0)} m'
    else:
        text = (f'boîte de {ref.per_box} bobine{"s" if ref.per_box > 1 else ""} de '
                f'{_thousands(ref.per_roll)} étiquette{"s" if ref.per_roll > 1 else ""}')
    if ref.box_price is not None:
        text += f', {_euros(ref.box_price)}'
    return text


def _thousands(value: int | Decimal) -> str:
    return f'{int(value):,}'.replace(',', ' ')


def _euros(value: Decimal) -> str:
    return f'{Decimal(value):.2f}'.replace('.', ',') + ' €'


# ---------------------------------------------------------------------------------------------------------------------
# References

def _decimal(value: str | int | bool | None, field: str, minimum: int, maximum: int, places: str = '0.1') -> Decimal:
    try:
        number = parse_decimal(value).quantize(Decimal(places))
    except (InvalidOperation, ValueError, TypeError):
        raise ConsumableError(f'{field} : nombre attendu')
    if not minimum <= number <= maximum:
        raise ConsumableError(f'{field} : entre {minimum} et {maximum}')
    return number


def _int(value: int, field: str, minimum: int, maximum: int) -> int:
    try:
        number = int(value)
    except (TypeError, ValueError):
        raise ConsumableError(f'{field} : entier attendu')
    if not minimum <= number <= maximum:
        raise ConsumableError(f'{field} : entre {minimum} et {maximum}')
    return number


def save_ref(data: dict[str, Any], ref: ConsumableRef | None = None) -> ConsumableRef | None:
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

def add_move(data: dict[str, Any], by: str) -> ConsumableMove:
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


def delete_move(move_id: int) -> None:
    deleted, _ = ConsumableMove.objects.filter(id=move_id).delete()
    if not deleted:
        raise ConsumableError('Mouvement inconnu')


def move_dict(move: ConsumableMove) -> dict[str, Any]:
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


def _units_text(kind: str, units: int) -> str:
    if kind == ConsumableKind.RIBBON:
        meters = Decimal(units) / 1000
        meters = meters.quantize(Decimal('0.1') if meters < 10 else Decimal('1'))
        return f'{_thousands(meters) if meters >= 10 else _number(meters)} m'
    return f'{_thousands(units)} étiquette{"s" if units > 1 else ""}'


# ---------------------------------------------------------------------------------------------------------------------
# Etiquettes perdues

def add_loss(data: dict[str, Any], by: str, front: str) -> LabelLoss:
    """Etiquettes perdues du rouleau : {"width_mm", "height_mm", "support", "count", "reason"}."""
    width = _decimal(data.get('width_mm'), 'Largeur', 5, 300)
    height = _decimal(data.get('height_mm'), 'Hauteur', 5, 300)
    support = data.get('support')
    if support not in LabelSupport.values:
        raise ConsumableError('support : direct ou transfer attendu')
    count = _int(data.get('count'), 'Étiquettes perdues', 1, MAX_LOSS)
    reason = data.get('reason') or LabelLossReason.CALIBRATION
    if reason not in LabelLossReason.values:
        raise ConsumableError('reason : ' + ', '.join(LabelLossReason.values) + ' attendu')
    return LabelLoss.objects.create(day=timezone.localdate(), width_mm=width, height_mm=height, support=support,
                                    count=count, reason=reason, by=by[:64], front=(front or '')[:64])


def delete_loss(loss_id: int) -> None:
    deleted, _ = LabelLoss.objects.filter(id=loss_id).delete()
    if not deleted:
        raise ConsumableError('Perte inconnue')


def loss_dict(loss: LabelLoss) -> dict[str, Any]:
    return {
        'id': loss.id, 'reason': loss.reason, 'reason_label': LabelLossReason(loss.reason).label,
        'pool': f'{_number(loss.width_mm)}x{_number(loss.height_mm)}-{loss.support}',
        'pool_label': category_label(loss.width_mm, loss.height_mm, loss.support),
        'count': loss.count, 'count_text': _units_text(ConsumableKind.LABELS, loss.count),
        'paced': loss.reason != LabelLossReason.ACCIDENT,
        'created': timezone.localtime(loss.created).isoformat(timespec='seconds'),
        'by': identity_name(loss.by) if loss.by else '', 'front': loss.front,
    }


def _category_total(key: str, rows: QuerySet[PrintCount] | QuerySet[LabelLoss]) -> int:
    """Etiquettes de la categorie `key` parmi `rows` (impressions ou pertes), ou millimetres de ruban."""
    if key == RIBBON_KEY:
        total = Decimal(0)
        for row in rows.filter(support=LabelSupport.TRANSFER).values('height_mm').annotate(count=Sum('count')):
            total += (row['height_mm'] + RIBBON_GAP_MM) * row['count']
        return int(total)
    width, rest = key.split('x', 1)
    height, support = rest.rsplit('-', 1)
    return rows.filter(width_mm=parse_decimal(width), height_mm=parse_decimal(height),
                       support=support).aggregate(total=Sum('count'))['total'] or 0


def _lost(key: str, since: date | None = None, until: date | None = None, after: datetime | None = None,
          paced: bool = False) -> int:
    """Pertes notees les jours `since` exclu a `until` inclus, ou apres l'instant `after` (stock : une perte notee
    le jour d'un inventaire compte si elle vient apres). `paced` : sans les accidents ponctuels (rythme)."""
    rows = LabelLoss.objects.all()
    if since is not None:
        rows = rows.filter(day__gt=since)
    if until is not None:
        rows = rows.filter(day__lte=until)
    if after is not None:
        rows = rows.filter(created__gt=after)
    if paced:
        rows = rows.exclude(reason=LabelLossReason.ACCIDENT)
    return _category_total(key, rows)


# ---------------------------------------------------------------------------------------------------------------------
# Stock estime, consommation, autonomie

def _consumption(key: str, since: date | None = None, until: date | None = None) -> int:
    """Etiquettes (categorie) ou millimetres de ruban imprimes, jours `since` exclu a `until` inclus."""
    rows = PrintCount.objects.all()
    if since is not None:
        rows = rows.filter(day__gt=since)
    if until is not None:
        rows = rows.filter(day__lte=until)
    return _category_total(key, rows)


def _stock(key: str, moves: list[ConsumableMove]) -> tuple[int | None, date | None]:
    """(stock, depuis) : None si aucun mouvement (stock inconnu)."""
    if not moves:
        return None, None
    inventories = [move for move in moves if move.kind == ConsumableMoveKind.INVENTORY]
    start = inventories[-1] if inventories else moves[0]
    base = start.units if inventories else 0
    received = sum(move.units for move in moves
                   if move.kind == ConsumableMoveKind.RECEPTION and move.created >= start.created)
    since = timezone.localtime(start.created).date()
    return base + received - _consumption(key, since=since) - _lost(key, after=start.created), since


def _measured_rate(key: str, today: date) -> tuple[Decimal | None, date | None]:
    """(consommation mesuree par jour, fin de la mise en place) : impressions des RATE_DAYS derniers jours, hors des
    SETUP_DAYS premiers jours du compteur, plus les pertes qui reviennent (calibrages, etiquettes ratees, pas les
    accidents). None : pas encore assez de jours apres la mise en place (ou rien imprime)."""
    first = PrintCount.objects.aggregate(first=Min('day'))['first']
    if first is None:
        return None, None
    setup_end = first + timedelta(days=SETUP_DAYS - 1)  # dernier jour de la mise en place
    start = max(today - timedelta(days=RATE_DAYS), setup_end)  # exclu
    days = (today - start).days
    if days < MEASURE_MIN_DAYS:
        return None, setup_end
    used = _consumption(key, since=start, until=today) + _lost(key, since=start, until=today, paced=True)
    return Decimal(used) / days, setup_end


def item_rate(today: date) -> Decimal:
    """Items utilises ou jetes par jour (6 derniers mois, ou depuis la premiere vérif) : autant d'items neufs a
    etiqueter pour les remplacer."""
    since = timezone.now() - timedelta(days=ITEM_RATE_DAYS)
    first = Verifs.objects.aggregate(first=Min('datetime'))['first']
    if first is None:
        return Decimal(0)
    days = max(30, (timezone.now() - max(since, first)).days)
    count = ItemMovement.objects.filter(kind__in=(MovementKind.USED, MovementKind.DISCARDED, MovementKind.REPLACED),
                                        cancelled__isnull=True, at__gte=since).count()
    return Decimal(count) / days


def item_pool_key(settings_row: ServerSettings, keys: list[str]) -> str:
    """Categorie des etiquettes d'items : celle choisie au poste, sinon la categorie d'etiquettes la plus imprimee."""
    if settings_row.item_label_pool in keys:
        return settings_row.item_label_pool
    labels = [key for key in keys if key != RIBBON_KEY]
    return max(labels, key=lambda key: (_consumption(key), key), default='')


def _rate(key: str, today: date, item_pool: bool, items_per_day: Decimal) -> dict[str, Any]:
    """Rythme d'une categorie : {rate, measured, planned, setup, setup_end, source} (voir la docstring du module)."""
    measured, setup_end = _measured_rate(key, today)
    planned = items_per_day if item_pool else None
    setup = setup_end is not None and measured is None
    rate = max(measured or Decimal(0), planned or Decimal(0))
    parts = []
    if measured is not None:
        parts.append('mesuré sur les impressions (hors mise en place)')
    if planned:
        parts.append('d\'après la consommation d\'items (une étiquette par item à remplacer)')
    source = 'Rythme ' + parts[0] if len(parts) == 1 else ''
    if len(parts) == 2:
        source = ('Rythme le plus haut entre les impressions mesurées (hors mise en place) et la consommation '
                  'd\'items (une étiquette par item à remplacer)')
    return {'rate': rate, 'measured': measured, 'planned': planned, 'setup': setup, 'setup_end': setup_end,
            'source': source}


def _unit_price(refs: list[ConsumableRef], moves: list[ConsumableMove]) -> Decimal | None:
    """Prix d'une etiquette ou d'un mm : reference de la derniere reception qui a un prix, sinon la plus recente."""
    ordered = [move.ref for move in reversed(moves) if move.kind == ConsumableMoveKind.RECEPTION]
    ordered += sorted(refs, key=lambda ref: ref.created, reverse=True)
    for ref in ordered:
        units = roll_units(ref) * ref.per_box
        if ref.box_price is not None and units:
            return ref.box_price / units
    return None


def _duration(days: int) -> str:
    """Autonomie lisible : « environ 12 jours », « environ 4 mois », « plus de 2 ans »."""
    if days < 60:
        return f'environ {days} jour{"s" if days > 1 else ""}'
    if days < 730:
        return f'environ {round(days / 30.44)} mois'
    return 'plus de 2 ans'


def _stock_text(kind: str, stock: int, ref: ConsumableRef) -> str:
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


def pools(today: date | None = None, settings_row: ServerSettings | None = None) -> list[dict[str, Any]]:
    today = today or timezone.localdate()
    settings_row = settings_row or ServerSettings.get()
    alert_days = settings_row.consumable_alert_days
    refs = list(ConsumableRef.objects.all())
    moves = list(ConsumableMove.objects.select_related('ref').order_by('created', 'id'))
    grouped: dict[Any, dict[str, Any]] = {}
    for ref in refs:
        grouped.setdefault(pool_key(ref), {'refs': [], 'moves': [], 'kind': ref.kind, 'label': pool_label(ref)})
        grouped[pool_key(ref)]['refs'].append(ref)
    for move in moves:
        grouped[pool_key(move.ref)]['moves'].append(move)
    year_start = today.replace(month=1, day=1) - timedelta(days=1)
    item_key = item_pool_key(settings_row, list(grouped))
    items_per_day = item_rate(today) if item_key else Decimal(0)
    result = []
    for key, pool in grouped.items():
        if all(ref.archived for ref in pool['refs']) and not pool['moves']:
            continue
        stock, since = _stock(key, pool['moves'])
        pace = _rate(key, today, key == item_key, items_per_day)
        rate = pace['rate']
        price = _unit_price(pool['refs'], pool['moves'])
        autonomy = int(stock / rate) if stock is not None and rate > 0 else None
        main_ref = next((move.ref for move in reversed(pool['moves'])), None) or pool['refs'][-1]
        floor = SETUP_FLOOR_ROLLS * roll_units(main_ref)
        if stock is None:
            level, state = 'unknown', 'Stock inconnu : faites un inventaire ou une réception'
        elif stock <= 0:
            level, state = 'bad', 'Épuisé d\'après les impressions : à commander (ou faites un inventaire)'
        elif autonomy is not None and autonomy < alert_days:
            level, state = 'warn', f'À commander : {_duration(autonomy)} de stock'
        elif pace['setup'] and floor and stock < floor:
            level, state = 'warn', (f'À commander : moins de {SETUP_FLOOR_ROLLS} '
                                    f'{"rouleaux" if pool["kind"] == ConsumableKind.RIBBON else "bobines"} en stock')
        else:
            level, state = 'ok', (f'{_duration(autonomy).capitalize()} de stock' if autonomy is not None
                                  else 'Pas encore de consommation')
        if pace['setup'] and level != 'bad':
            end = pace['setup_end'] + timedelta(days=MEASURE_MIN_DAYS)
            state += (f" (mise en place : les impressions comptent dans le rythme à partir du "
                      f"{end.strftime('%d/%m/%Y')} ; d'ici là, alerte sous {SETUP_FLOOR_ROLLS} "
                      f"{'rouleaux' if pool['kind'] == ConsumableKind.RIBBON else 'bobines'})")
        year_lost = _lost(key, since=year_start, until=today)
        year_used = _consumption(key, since=year_start, until=today) + year_lost
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
            # dont etiquettes perdues (ou mm de ruban) : comptees dans year_used et dans le cout
            'year_lost': year_lost, 'year_lost_text': _units_text(pool['kind'], year_lost) if year_lost else '',
            'level': level, 'state': state,
            'refs': [ref.id for ref in pool['refs']],
            'item_labels': key == item_key,  # categorie des etiquettes d'items (rythme prevu)
            'setup': pace['setup'], 'rate_source': pace['source'],
            'measured_monthly': int(pace['measured'] * DAYS_PER_MONTH) if pace['measured'] is not None else None,
            'planned_monthly': int(pace['planned'] * DAYS_PER_MONTH) if pace['planned'] is not None else None,
        })
    level_rank = {'bad': 0, 'warn': 1, 'unknown': 2, 'ok': 3}
    result.sort(key=lambda pool: (level_rank[pool['level']], pool['kind'] != ConsumableKind.LABELS, pool['label']))
    return result


def summary() -> dict[str, Any]:
    settings_row = ServerSettings.get()
    moves = ConsumableMove.objects.select_related('ref').order_by('-created', '-id')[:RECENT_MOVES]
    return {
        'pools': pools(settings_row=settings_row),
        'refs': [ref_dict(ref) for ref in ConsumableRef.objects.all()],
        'moves': [move_dict(move) for move in moves],
        'losses': [loss_dict(loss) for loss in LabelLoss.objects.order_by('-created', '-id')[:RECENT_MOVES]],
        'loss_reasons': [{'id': value, 'label': label} for value, label in LabelLossReason.choices],
        'alert_days': settings_row.consumable_alert_days,
        # categorie des etiquettes d'items choisie au poste (vide : la plus imprimee, voir item_pool_key)
        'item_label_pool': settings_row.item_label_pool,
        'ribbon_gap_mm': float(RIBBON_GAP_MM),
        'rate_days': RATE_DAYS,
    }


def set_item_label_pool(value: Any) -> None:
    """Categorie des etiquettes d'items (cle d'une categorie d'etiquettes), vide : automatique."""
    key = str(value or '')
    if key and (key == RIBBON_KEY or key not in {pool_key(ref) for ref in ConsumableRef.objects.all()}):
        raise ConsumableError('Catégorie d\'étiquettes inconnue')
    settings_row = ServerSettings.get()
    settings_row.item_label_pool = key
    settings_row.save(update_fields=['item_label_pool'])


def set_alert_days(value: int) -> None:
    settings_row = ServerSettings.get()
    settings_row.consumable_alert_days = _int(value, 'Alerte (jours)', 1, 365)
    settings_row.save(update_fields=['consumable_alert_days'])


def order_tasks(today: date | None = None) -> list[dict[str, Any]]:
    """Taches « Commander » de la Gestion de la TECH (poste seulement) : stock epuise ou sous le seuil d'alerte."""
    tasks = []
    for pool in pools(today):
        if pool['level'] not in ('bad', 'warn'):
            continue
        detail = f"stock estimé {pool['stock_text']}"
        if pool['monthly_text']:
            detail += f" · {pool['monthly_text']} par mois"
        detail += ' · menu Impression > Impressions et consommables'
        tasks.append({'level': pool['level'], 'title': f"Commander : {pool['label']}", 'detail': detail,
                      'autonomy': pool['autonomy_days'] if pool['autonomy_days'] is not None else -1})
    return tasks
