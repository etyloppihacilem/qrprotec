# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          order_explain.py
#       -\-    _|__
#        |\___/  . \        Created on 21 Nov. 2026 at 13:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Detail d'une recommandation de commande (« Pourquoi cette commande ? », web et poste) : ce qui l'a provoquee et
pourquoi elle serait une bonne idee, avec de quoi tracer deux graphes :

- la consommation mois par mois sur l'historique des previsions (utilises, jetes perimes), et la consommation
  estimee (moyenne) qui sert aux previsions ;
- le stock prevu semaine par semaine, avec la ligne du minimum en stock, le stock si la commande est passee a temps,
  les items dans les lots et la ligne de ce que les lots doivent contenir (besoin des lots).

Les phrases sont faites ici pour que le web et le poste disent la meme chose.
"""

import math
from collections import defaultdict
from datetime import timedelta

from django.utils import timezone

from . import forecast as stock_forecast
from .journal import pl, plural
from .models import ItemMovement, ItemType, Lots, MovementKind


def _day(value):
    return value.strftime('%d/%m/%Y')


def _iso(value):
    return value.isoformat() if value else None


def lots_need():
    """{type: quantite} que les lots actifs doivent contenir (attendus non facultatifs, contenu propre compris). Un
    type a unites compte en unites, sous-types attendus compris."""
    from . import services
    lots = list(Lots.objects.select_related('lot_type').filter(active=True, lot_type__storage=False))
    preload = services.LotPreload(lots)
    fills = {row.type: (row.group_id, max(1, row.fills)) for row in ItemType.objects.filter(group__isnull=False)
             .only('type', 'group_id', 'fills')}
    need = defaultdict(int)
    for lot in lots:
        for requirement in services.lot_requirements(lot, preload):
            if requirement.optional or not requirement.item_type.physical:
                continue
            need[requirement.item_type_id] += requirement.quantity
            group = fills.get(requirement.item_type_id)
            if group:
                need[group[0]] += requirement.quantity * group[1]
    return need


def monthly_uses(type_ids, today, months, fills=None):
    """Consommation des `months` derniers mois (mois en cours compris, tronque), par mois : utilises et jetes
    perimes (absences non annulees du journal). fills : unites par item d'un sous-type (type a unites)."""
    fills = fills or {}
    start = stock_forecast.first_of_month(today, -(months - 1))
    rows = [{'start': stock_forecast.first_of_month(today, -(months - 1) + index), 'used': 0, 'discarded': 0}
            for index in range(months)]
    index_of = {(row['start'].year, row['start'].month): index for index, row in enumerate(rows)}
    moves = ItemMovement.objects.filter(item_type_id__in=type_ids, cancelled__isnull=True, at__date__gte=start,
                                        kind__in=(MovementKind.USED, MovementKind.DISCARDED))
    for kind, type_id, at in moves.values_list('kind', 'item_type_id', 'at'):
        local = timezone.localtime(at)
        index = index_of.get((local.year, local.month))
        if index is None:
            continue
        key = 'used' if kind == MovementKind.USED else 'discarded'
        rows[index][key] += fills.get(type_id, 1)
    for row in rows:
        row['start'] = _iso(row['start'])
    return rows


def explain(row, data, need=None, today=None):
    """Detail de la commande suggeree de la ligne `row` des previsions (`data` : forecast(weekly=True))."""
    order = row.get('order')
    if not order:
        return None
    today = today or data['today']
    unit = f" {row['unit']}" if row.get('unit') else ''
    minimum = row['min_quantity']
    rate = row['per_month']
    lead = data['lead_days']
    below = order['below_min']
    quantity_units = order.get('units', order['quantity'])  # type a unites : quantite en unites
    weekly = row.get('weekly') or []
    need = lots_need() if need is None else need
    lots_need_count = need.get(row['type'], 0)

    # stock au moment ou il passe sous le minimum, et ce que la commande y ajoute (arrivee au plus tard le jour du
    # passage sous le minimum : commandee avant la date limite, livree en `lead` jours)
    left = None
    arrival = order['before'] + timedelta(days=lead)
    points = []
    for point in weekly:
        day = point['date']
        # avec la commande : la commande arrivee couvre aussi les manques d'apres (utilisations sans item valide)
        missed_since = 0
        if day >= arrival:
            before_arrival = next((p['shortage'] for p in reversed(weekly) if p['date'] < arrival), 0)
            missed_since = point['shortage'] - before_arrival
        with_order = point['stock'] + max(0, quantity_units - missed_since) if day >= arrival else point['stock']
        points.append({'date': _iso(day), 'stock': point['stock'], 'with_order': with_order, 'lots': point['lots'],
                       'lost': point['lost'], 'shortage': point['shortage']})
        if left is None and day >= below:
            left = point['stock']
    if left is None:
        left = row['stock_now']
    lost_until = next((p['lost'] for p in weekly if p['date'] >= below), weekly[-1]['lost'] if weekly else 0)

    # ce qui a provoque la recommandation
    if row['stock_now'] < minimum:
        trigger = 'below'
    elif lost_until and lost_until * 2 >= max(1, row['stock_now'] - minimum):
        trigger = 'expiry'
    else:
        trigger = 'consumption'
    triggers = {
        'below': 'Le stock est déjà sous le minimum',
        'expiry': 'Des péremptions font passer le stock sous le minimum',
        'consumption': 'La consommation fait passer le stock sous le minimum',
    }

    why = []
    if trigger == 'below':
        why.append(f"Il reste {row['stock_now']}{unit} en stock pour un minimum de {minimum}{unit} : le stock est "
                   'déjà sous le minimum.')
    else:
        why.append(f"Le stock ({row['stock_now']}{unit} aujourd'hui) passera sous le minimum de {minimum}{unit} "
                   f"vers le {_day(below)}.")
    history = data['history_months']
    if rate > 0:
        why.append(f"Consommation estimée : {_rate(rate)}{unit} par mois (moyenne des {plural(history, 'dernier mois', 'derniers mois')}, "
                   f"lots réassortis depuis le stock compris).")
    else:
        why.append('Aucune consommation mesurée sur les ' + plural(history, 'dernier mois', 'derniers mois') + '.')
    if lost_until:
        why.append(f"{plural(lost_until, 'item périmera', 'items périmeront')} avant d'être "
                   f"{pl(lost_until, 'utilisé', 'utilisés')} d'ici là (premier périmé, premier utilisé) : "
                   f"{pl(lost_until, 'il ne compte', 'ils ne comptent')} plus dans le stock.")
    contributors = [lot for lot in row.get('lots_consumption', []) if lot['per_month'] > 0][:3]
    if contributors:
        why.append('Ceux qui consomment le plus : ' + ', '.join(
            f"{lot['path']} ({_rate(lot['per_month'])}{unit}/mois)" for lot in contributors) + '.')
    if lots_need_count:
        lots_now = row['lots_now']
        line = f"Les lots doivent contenir {lots_need_count}{unit} en tout, ils en ont {lots_now}{unit}"
        line += (f" : il en manque {lots_need_count - lots_now}{unit}, à prendre dans le stock."
                 if lots_now < lots_need_count else '.')
        why.append(line)
    if order['urgent']:
        why.append(f"Avec {plural(lead, 'jour')} de livraison (Réglages), il faut commander dès que possible.")
    else:
        why.append(f"Avec {plural(lead, 'jour')} de livraison (Réglages), commander avant le "
                   f"{_day(order['before'])} évite de passer sous le minimum.")

    # quantite : minimum + 2 mois de consommation - ce qui restera, arrondi au paquet
    two_months = math.ceil(2 * rate)
    raw = max(1, minimum + two_months - left)
    how = (f"Quantité : minimum {minimum}{unit} + 2 mois de consommation ({two_months}{unit}) − ce qui restera "
           f"({left}{unit}) = {raw}{unit}")
    if order.get('subtype'):
        how += f", soit {order['quantity']} × {order['subtype_name']} (arrondi au paquet)."
    elif order['quantity'] != raw:
        how += f", arrondi au paquet de {row.get('pack_size') or 1} : {order['quantity']}."
    else:
        how += '.'

    return {
        'type': row['type'],
        'name': row['name'],
        'unit': row.get('unit') or '',
        'trigger': trigger,
        'trigger_label': triggers[trigger],
        'why': why,
        'quantity_text': how,
        'quantity': order['quantity'],
        'ordered_name': order.get('subtype_name') or row['name'],
        'before': _iso(order['before']),
        'urgent': order['urgent'],
        'below_min': _iso(below),
        'arrival': _iso(arrival),
        'lead_days': lead,
        'min_quantity': minimum,
        'stock_now': row['stock_now'],
        'lots_now': row['lots_now'],
        'lots_need': lots_need_count,
        'per_month': rate,
        'history_months': history,
        'monthly': monthly_uses(_history_types(row), today, max(1, min(history, 24)), _history_fills(row)),
        'points': points,
    }


def _rate(value):
    return f'{value:.1f}'.replace('.', ',').replace(',0', '') if value < 10 else str(round(value))


def _history_types(row):
    if not row.get('is_group'):
        return [row['type']]
    return [row['type']] + list(ItemType.objects.filter(group_id=row['type']).values_list('type', flat=True))


def _history_fills(row):
    if not row.get('is_group'):
        return {}
    return {code: max(1, fills) for code, fills in
            ItemType.objects.filter(group_id=row['type']).values_list('type', 'fills')}


def for_rows(data, rows, today=None):
    """Details des commandes de `rows` (lignes de `data`), {type: detail}, besoin des lots lu une fois."""
    rows = [row for row in rows if row.get('order')]
    if not rows:
        return {}
    need = lots_need()
    return {row['type']: explain(row, data, need, today) for row in rows}

