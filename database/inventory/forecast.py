# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          forecast.py
#       -\-    _|__
#        |\___/  . \        Created on 02 Oct. 2026 at 09:30
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Previsions de stock : peremptions a venir, consommation, commandes et echanges suggeres.

Modele :
- la consommation d'un type dans un lot est la moyenne des utilisations (premieres absences non annulees du journal
  des mouvements) sur la periode d'historique. Le stock (hors lot et rangements) consomme ce que ses lots consomment
  (il les reassortit) plus ce qui en sort directement ;
- chaque lieu est simule en « premier perime, premier utilise » : a chaque utilisation prevue, l'item valide le plus
  ancien est pris. Un item qui atteint sa date sans avoir ete pris est perdu. Un item perdu dans un lot est remplace
  depuis le stock ;
- les items non confirmes (manques a la derniere verif) ne comptent pas : ils sont deja comptes comme utilises, et
  ils reviennent dans les calculs s'ils sont retrouves.

Les temps sont en jours depuis aujourd'hui (flottants). Un item est utilisable tant que t < limite, la limite etant le
lendemain de sa date de peremption.
"""

import calendar
import math
from collections import defaultdict
from datetime import date, timedelta

from django.db.models import Min
from django.utils import timezone

from .models import (
    ItemMovement, ItemStatus, Items, ItemType, Lots, MovementKind, Verifs, qrprotec_setting,
)

DAYS_PER_MONTH = 30.44
INFINITY = math.inf
MAX_DEMANDS = 20000  # garde-fou : nombre d'utilisations simulees par lieu
TRANSFER_MARGIN_DAYS = 14  # un echange doit permettre d'utiliser l'item au moins 2 semaines avant sa date


def add_months(day, months):
    """Meme jour, `months` mois plus tard (dernier jour du mois si ce jour n'existe pas)."""
    month = day.month - 1 + months
    year = day.year + month // 12
    month = month % 12 + 1
    return date(year, month, min(day.day, calendar.monthrange(year, month)[1]))


def first_of_month(day, months=0):
    month = day.month - 1 + months
    return date(day.year + month // 12, month % 12 + 1, 1)


def demand_times(rate, horizon):
    """Instants des utilisations pour un debit constant (items par jour) jusqu'a l'horizon."""
    if rate <= 0:
        return []
    count = min(int(horizon * rate), MAX_DEMANDS)
    return [(k + 1) / rate for k in range(count)]


def simulate(limits, demands, horizon):
    """Premier perime, premier utilise. `limits` et `demands` sont tries.

    Retourne (sorts, ruptures) : pour chaque item, ('used', t), ('lost', limite) ou ('kept', None), et la liste des
    instants ou une utilisation n'a trouve aucun item valide.
    """
    fates = [None] * len(limits)
    shortages = []
    position = 0
    for when in demands:
        while position < len(limits) and limits[position] <= when:
            fates[position] = ('lost', limits[position])
            position += 1
        if position < len(limits):
            fates[position] = ('used', when)
            position += 1
        else:
            shortages.append(when)
    for index in range(position, len(limits)):
        fates[index] = ('lost', limits[index]) if limits[index] <= horizon else ('kept', None)
    return fates, shortages


def used_if_arrives(limits, demands, limit, arrival):
    """Un item de limite `limit` arrive a l'instant `arrival` dans un lieu (items `limits` tries, deja presents) :
    est-il utilise avant sa peremption ?"""
    position = 0
    for when in demands:
        if when >= limit:
            return False
        while position < len(limits) and limits[position] <= when:
            position += 1
        if arrival <= when and (position >= len(limits) or limit <= limits[position]):
            return True
        position += 1
    return False


def latest_arrival(limits, demands, limit):
    """Dernier jour (depuis aujourd'hui) ou l'item peut encore arriver et etre utilise a temps, ou None."""
    if not used_if_arrives(limits, demands, limit, 0):
        return None
    low, high = 0, max(0, int(limit))
    while low < high:
        middle = (low + high + 1) // 2
        if used_if_arrives(limits, demands, limit, middle):
            low = middle
        else:
            high = middle - 1
    return low


def lost_count(limits, demands, horizon):
    fates, _ = simulate(sorted(limits), demands, horizon)
    return sum(1 for fate, when in fates if fate == 'lost' and when > 0)


def present_at(fate, limit, when):
    """L'item est-il encore la (non utilise, non perime) a l'instant `when` ?"""
    kind, at = fate
    if limit <= when:
        return False
    return kind == 'kept' or at > when


class Places:
    """Lieux de la simulation : 'stock' (hors lot et rangements) ou l'id d'un lot actif qui n'est pas un rangement."""

    def __init__(self):
        self.lots = {lot.id: lot for lot in Lots.objects.select_related('lot_type')}
        self._sealed = {}

    def of(self, lot_id):
        """Lieu d'un item range dans `lot_id`, ou None si ce lot n'est plus actif."""
        if lot_id is None:
            return 'stock'
        lot = self.lots.get(lot_id)
        if lot is None or not lot.active:
            return None
        return 'stock' if lot.lot_type.storage else lot_id

    def chain(self, lot_id):
        chain = []
        seen = set()
        lot = self.lots.get(lot_id)
        while lot is not None and lot.id not in seen:
            chain.append(lot)
            seen.add(lot.id)
            lot = self.lots.get(lot.parent_id)
        return chain

    def sealed(self, place):
        """Le lieu est-il (ou est-il range dans) un lot scelle ?"""
        if place == 'stock':
            return False
        if place not in self._sealed:
            self._sealed[place] = any(lot.is_sealed for lot in self.chain(place))
        return self._sealed[place]

    def describe(self, place):
        if place == 'stock':
            return {'id': None, 'name': 'Stock', 'path': 'Stock', 'sealed': False}
        chain = self.chain(place)
        return {'id': place, 'name': chain[0].name, 'path': ' › '.join(lot.name for lot in reversed(chain)),
                'sealed': self.sealed(place)}


def consumption(places, since, now):
    """({(lieu, type): items par jour}, jours d'historique) d'apres les utilisations non annulees du journal."""
    first = Verifs.objects.aggregate(first=Min('datetime'))['first']
    start = max(since, first) if first else now
    days = max(30.0, (now - start).total_seconds() / 86400)
    counts = defaultdict(int)
    used = ItemMovement.objects.filter(kind=MovementKind.USED, cancelled__isnull=True, at__gte=since)
    for row in used.values('from_lot_id', 'item_type_id'):
        place = places.of(row['from_lot_id'])
        if place is not None:
            counts[(place, row['item_type_id'])] += 1
    return {key: count / days for key, count in counts.items()}, days


def forecast(months=6, lead_days=None, history_months=None):
    """Previsions par type d'item et echanges suggeres, sur `months` mois."""
    if lead_days is None:
        lead_days = qrprotec_setting('ORDER_LEAD_DAYS')
    if history_months is None:
        history_months = qrprotec_setting('FORECAST_HISTORY_MONTHS')
    now = timezone.now()
    today = timezone.localdate()
    end = add_months(today, months)
    horizon = (end - today).days
    places = Places()
    rates, history_days = consumption(places, now - timedelta(days=history_months * DAYS_PER_MONTH), now)

    # items presents, par type et par lieu
    units = defaultdict(lambda: defaultdict(list))  # type -> lieu -> [(limite, item)]
    unconfirmed = defaultdict(int)
    for item in Items.objects.select_related('pack').filter(status=ItemStatus.ACTIVE):
        place = places.of(item.location_id)
        if place is None:
            continue
        type_id = item.pack.item_type_id
        if item.missed_verifs > 0:
            unconfirmed[type_id] += 1
            continue
        peremption = item.pack.peremption
        limit = INFINITY if peremption is None else float((peremption - today).days + 1)
        units[type_id][place].append((limit, item))

    month_starts = [today] + [first_of_month(today, k) for k in range(1, months + 1)]
    points = [add_months(today, k) for k in range(months + 1)]
    types = []
    transfers = []
    for item_type in ItemType.objects.filter(archived=False).order_by('name'):
        by_place = units.get(item_type.type, {})
        for held in by_place.values():
            held.sort(key=lambda unit: (unit[0], unit[1].iid))
        types.append(_type_forecast(item_type, by_place, rates, places, today, horizon, month_starts, points,
                                    lead_days, unconfirmed[item_type.type]))
        if item_type.perissable:
            transfers += _transfers(item_type, by_place, rates, places, today, horizon)

    return {
        'today': today,
        'months': months,
        'lead_days': lead_days,
        'history_months': history_months,
        'history_days': round(history_days),
        'types': types,
        'transfers': _group_transfers(transfers),
    }


def _lot_rates(rates, type_id, places):
    return {place: rate for (place, rate_type), rate in rates.items() if rate_type == type_id and place != 'stock'}


def _type_forecast(item_type, by_place, rates, places, today, horizon, month_starts, points, lead_days, unconfirmed):
    type_id = item_type.type
    lot_rates = _lot_rates(rates, type_id, places)
    stock_rate = rates.get(('stock', type_id), 0.0) + sum(lot_rates.values())

    # lots : chaque lot consomme ses propres items, ceux qu'il perd sont remplaces depuis le stock
    lot_units = []  # (limite, sort, lieu)
    replacements = []
    for place, held in by_place.items():
        if place == 'stock':
            continue
        limits = [limit for limit, _ in held]
        fates, _ = simulate(limits, demand_times(lot_rates.get(place, 0.0), horizon), horizon)
        for limit, fate in zip(limits, fates):
            lot_units.append((limit, fate, place))
            if fate[0] == 'lost':
                replacements.append(max(fate[1], 0.0))

    stock_limits = [limit for limit, _ in by_place.get('stock', [])]
    demands = sorted(demand_times(stock_rate, horizon) + [when for when in replacements if when <= horizon])
    stock_fates, shortages = simulate(stock_limits, demands, horizon)

    stock_now = sum(1 for limit in stock_limits if limit > 0)
    lots_now = sum(1 for limit, _, _ in lot_units if limit > 0)
    expired_now = sum(1 for limit in stock_limits if limit <= 0) + sum(1 for limit, _, _ in lot_units if limit <= 0)

    def in_window(value, start, stop):
        return start < value <= stop

    projection = []
    for day in points:
        when = (day - today).days
        stock = sum(1 for limit, fate in zip(stock_limits, stock_fates) if present_at(fate, limit, when))
        missing = sum(1 for at in shortages if at <= when)
        projection.append({
            'date': day,
            'stock': stock,
            'lots': max(0, lots_now - missing),
            'stock_static': sum(1 for limit in stock_limits if limit > when),
            'lots_static': sum(1 for limit, _, _ in lot_units if limit > when),
            'expiring': sum(1 for limit in stock_limits if in_window(limit, 0, when))
                        + sum(1 for limit, _, _ in lot_units if in_window(limit, 0, when)),
            'lost': sum(1 for fate in stock_fates if fate[0] == 'lost' and in_window(fate[1], 0, when))
                    + sum(1 for _, fate, _ in lot_units if fate[0] == 'lost' and in_window(fate[1], 0, when)),
            'shortage': missing,
        })

    calendar_months = []
    for index in range(len(month_starts) - 1):
        start = (month_starts[index] - today).days
        stop = (month_starts[index + 1] - today).days
        expiring = {'stock': 0, 'lots': 0, 'sealed': 0}
        lost = {'stock': 0, 'lots': 0, 'sealed': 0}
        for limit, fate in zip(stock_limits, stock_fates):
            if start < limit <= stop:
                expiring['stock'] += 1
                if fate[0] == 'lost':
                    lost['stock'] += 1
        for limit, fate, place in lot_units:
            if start < limit <= stop:
                key = 'sealed' if places.sealed(place) else 'lots'
                expiring[key] += 1
                if fate[0] == 'lost':
                    lost[key] += 1
        calendar_months.append({'start': month_starts[index], 'expiring': expiring, 'lost': lost})

    # passage sous le minimum et commande suggeree
    below = None
    minimum = item_type.min_quantity
    if minimum > 0:
        departures = sorted(
            max(fate[1], 0.0) for limit, fate in zip(stock_limits, stock_fates) if limit > 0 and fate[0] != 'kept'
        )
        count = stock_now
        if count < minimum:
            below = 0.0
        else:
            for when in departures:
                count -= 1
                if count < minimum:
                    below = when
                    break
    order = None
    if below is not None and below <= horizon:
        below_day = today + timedelta(days=int(below))
        rate_month = stock_rate * DAYS_PER_MONTH
        left = sum(1 for limit, fate in zip(stock_limits, stock_fates) if present_at(fate, limit, below))
        quantity = max(1, minimum + math.ceil(2 * rate_month) - left)
        pack = max(1, item_type.default_pack_size)
        quantity = math.ceil(quantity / pack) * pack
        deadline = max(today, below_day - timedelta(days=lead_days))
        order = {'quantity': quantity, 'before': deadline, 'urgent': deadline <= today, 'below_min': below_day}

    lots = [
        {**places.describe(place), 'per_month': round(rate * DAYS_PER_MONTH, 2)}
        for place, rate in sorted(lot_rates.items(), key=lambda pair: -pair[1])
    ]
    batches = defaultdict(lambda: {'count': 0, 'used': 0, 'lost': 0})
    for place, held in by_place.items():
        fates = stock_fates if place == 'stock' else None
        if fates is None:
            fates = [fate for _, fate, unit_place in lot_units if unit_place == place]
        for (limit, item), fate in zip(held, fates):
            batch = batches[(item.pack.peremption, place)]
            batch['count'] += 1
            if fate[0] in ('used', 'lost') and limit > 0:
                batch[fate[0]] += 1
    detail = [
        {'peremption': peremption, **places.describe(place), **values}
        for (peremption, place), values in sorted(
            batches.items(), key=lambda pair: (pair[0][0] or date.max, places.describe(pair[0][1])['path'])
        )
    ]

    return {
        'type': type_id,
        'name': item_type.name,
        'min_quantity': minimum,
        'perissable': item_type.perissable,
        'pack_size': item_type.default_pack_size,
        'per_month': round(stock_rate * DAYS_PER_MONTH, 2),
        'stock_per_month': round(rates.get(('stock', type_id), 0.0) * DAYS_PER_MONTH, 2),
        'lots_consumption': lots,
        'stock_now': stock_now,
        'lots_now': lots_now,
        'expired_now': expired_now,
        'unconfirmed': unconfirmed,
        'points': projection,
        'calendar': calendar_months,
        'below_min': today + timedelta(days=int(below)) if below is not None and below <= horizon else None,
        'order': order,
        'batches': detail,
    }


def _transfers(item_type, by_place, rates, places, today, horizon):
    """Echanges qui evitent des pertes : un item qui perimera dans son lieu part la ou il sera utilise a temps, et
    l'item le plus frais de ce lieu prend sa place (les deux lieux gardent le meme contenu)."""
    type_id = item_type.type
    lot_rates = _lot_rates(rates, type_id, places)
    demand = {'stock': demand_times(rates.get(('stock', type_id), 0.0) + sum(lot_rates.values()), horizon)}
    for place in set(by_place) | set(lot_rates):
        if place != 'stock':
            demand[place] = demand_times(lot_rates.get(place, 0.0), horizon)
    held = {place: list(units) for place, units in by_place.items()}
    destinations = [place for place in demand if demand[place] and not places.sealed(place)]

    def losses(place, units):
        return lost_count([limit for limit, _ in units], demand.get(place, []), horizon)

    def at_risk():
        risky = []
        for place, units in held.items():
            limits = [limit for limit, _ in units]
            fates, _ = simulate(limits, demand.get(place, []), horizon)
            risky += [(limit, place, item) for (limit, item), (fate, when) in zip(units, fates)
                      if fate == 'lost' and when > 0]
        return sorted(risky, key=lambda unit: (unit[0], unit[2].iid))

    suggestions = []
    done = set()
    while True:
        best = None
        for limit, source, item in at_risk():
            if item.iid in done:
                continue
            for target in destinations:
                if target == source or not held.get(target):
                    continue
                back_limit, back = held[target][-1]  # le plus frais du lieu d'arrivee
                if back_limit <= limit:
                    continue
                source_after = sorted([unit for unit in held[source] if unit[1] is not item] + [(back_limit, back)],
                                      key=lambda unit: (unit[0], unit[1].iid))
                target_after = sorted([unit for unit in held[target] if unit[1] is not back] + [(limit, item)],
                                      key=lambda unit: (unit[0], unit[1].iid))
                gain = (losses(source, held[source]) + losses(target, held[target])
                        - losses(source, source_after) - losses(target, target_after))
                if gain <= 0:
                    continue
                # date limite : dernier jour ou l'item peut arriver et etre utilise avant sa peremption
                # (avec une marge : la consommation reelle n'est pas reguliere)
                remaining = [unit[0] for unit in target_after if unit[1] is not item]
                deadline = latest_arrival(remaining, demand[target], limit - TRANSFER_MARGIN_DAYS)
                if deadline is None:
                    deadline = latest_arrival(remaining, demand[target], limit)
                if deadline is None:
                    continue
                score = (gain, len(demand[target]))
                if best is None or score > best[0]:
                    best = (score, source, target, item, back, source_after, target_after, deadline)
            if best is not None:
                break
            done.add(item.iid)
        if best is None:
            break
        _, source, target, item, back, source_after, target_after, deadline = best
        held[source] = source_after
        held[target] = target_after
        done.update({item.iid, back.iid})
        suggestions.append({
            'source': source, 'target': target, 'type': type_id, 'type_name': item_type.name,
            'take': item.iid, 'take_peremption': item.pack.peremption,
            'back': back.iid, 'back_peremption': back.pack.peremption,
            'before': today + timedelta(days=deadline),
            'places': places,
        })
    return suggestions


def _group_transfers(suggestions):
    """Echanges regroupes par lieu a ouvrir, puis par (type, lieu d'arrivee, dates)."""
    groups = {}
    for suggestion in suggestions:
        places = suggestion['places']
        group = groups.setdefault(suggestion['source'], {'source': places.describe(suggestion['source']),
                                                         'before': suggestion['before'], 'moves': {}})
        group['before'] = min(group['before'], suggestion['before'])
        key = (suggestion['type'], suggestion['target'], suggestion['take_peremption'], suggestion['back_peremption'])
        move = group['moves'].setdefault(key, {
            'type': suggestion['type'], 'type_name': suggestion['type_name'],
            'target': places.describe(suggestion['target']),
            'take': [], 'take_peremption': suggestion['take_peremption'],
            'back': [], 'back_peremption': suggestion['back_peremption'],
            'before': suggestion['before'],
        })
        move['take'].append(suggestion['take'])
        move['back'].append(suggestion['back'])
        move['before'] = min(move['before'], suggestion['before'])
    result = []
    for group in sorted(groups.values(), key=lambda group: (group['before'], group['source']['path'])):
        group['moves'] = sorted(group['moves'].values(), key=lambda move: (move['before'], move['type_name']))
        group['count'] = sum(len(move['take']) for move in group['moves'])
        result.append(group)
    return result
