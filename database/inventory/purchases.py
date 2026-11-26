# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          purchases.py
#       -\-    _|__
#        |\___/  . \        Created on 26 Nov. 2026 at 18:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Commandes fournisseurs (poste seulement, pas le web) : saisie au moment ou la commande est passee, reception,
statistiques et suggestions de commande groupees par fournisseur.

- Une commande (SupplierOrder) est identifiee par sa date, son fournisseur et son numero. Ses lignes sont des
  StockOrder (type d'item, quantite en items, prix unitaire) : tant qu'une ligne attend sa livraison, la commande
  recommandee de ce type ne revient pas dans les taches du TECH (comme la case « Commandé »).
- Reception : la page de reception du poste propose les commandes pas encore recues ; les items crees pour une ligne
  y sont rattaches (Items.order_line), ce qui donne le prix d'achat de chaque item et la date de livraison.
- Delais : jours ouvres (sans week-ends ni jours feries francais, holidays_fr.py) entre la date de commande et la
  premiere reception. La livraison prevue d'une commande en cours vient du delai median du fournisseur (ses
  dernieres commandes), a defaut de tous les fournisseurs, a defaut DEFAULT_DELAY_DAYS.
- Statistiques : depenses par mois et par fournisseur, prix par type (dernier, meilleur, moyen, historique), valeur
  du stock et du materiel jete perime, cout annuel estime de la consommation, economies possibles.
- Suggestions : les commandes recommandees par les previsions (forecast.py), chacune chez le fournisseur le moins
  cher connu, groupees par fournisseur ; les commandes du meme fournisseur qui viennent plus tard y sont ajoutees pour
  payer une seule livraison.

Les phrases sont faites ici, le poste les affiche telles quelles.
"""

from __future__ import annotations

import math
import statistics
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import date, timedelta
from decimal import Decimal, InvalidOperation
from typing import TYPE_CHECKING, Any

from django.db import transaction
from django.db.models import Count, Max, Min
from django.utils import timezone

from . import forecast as stock_forecast
from . import journal
from .holidays_fr import add_business_days, business_days_between, holidays
from .idendity import identity_name
from .journal import plural
from .models import (
    ItemMovement, Items, ItemStatus, ItemType, MovementKind, OperationKind, StockOrder, Supplier, SupplierOrder,
    name_key,
)
from .print_counts import parse_decimal

if TYPE_CHECKING:
    from collections.abc import Iterable

DEFAULT_DELAY_DAYS = 5  # jours ouvres, tant qu'aucune commande n'a ete recue
DELAY_SAMPLES = 10  # dernieres commandes recues d'un fournisseur pour son delai median
SUGGEST_WINDOW_DAYS = 30  # commandes recommandees a passer dans les 30 jours (comme les taches du TECH)
GROUP_HORIZON_DAYS = 90  # commandes plus lointaines du meme fournisseur ajoutees pour grouper la livraison
STATS_MONTHS = 12
BEST_PRICE_DAYS = 365  # meilleur prix : parmi les achats de l'annee ecoulee
MAX_LINES = 200
MAX_QUANTITY = 100000
MAX_PRICE = Decimal('100000')

STATES = {
    'late': 'En retard',
    'partial': 'Reçue en partie',
    'waiting': 'En attente',
    'received': 'Reçue',
    'closed': 'Close (reçue en partie)',
    'cancelled': 'Annulée',
}
# ce qui merite l'attention d'abord : en retard, en partie, en attente, puis l'historique
STATE_ORDER = {'late': 0, 'partial': 1, 'waiting': 2, 'received': 3, 'closed': 3, 'cancelled': 4}
OPEN_STATES = ('late', 'partial', 'waiting')


class PurchaseError(Exception):
    """Saisie refusee (message pour le poste)."""


def _money(value: Decimal | float | None) -> float | None:
    return None if value is None else round(float(value), 2)


def _price(value: Decimal | float | None) -> float | None:
    return None if value is None else round(float(value), 4)


def _day(value: date) -> str:
    return value.strftime('%d/%m/%Y')


def euros(value: Decimal | float) -> str:
    """« 1 234,50 € » (texte des phrases)."""
    text = f'{float(value):,.2f}'.replace(',', ' ').replace('.', ',')
    return f'{text} €'


def unit_euros(value: Decimal | float) -> str:
    """Prix unitaire : 2 a 4 decimales (« 0,045 € »)."""
    text = f'{float(value):.4f}'.rstrip('0')
    if len(text.partition('.')[2]) < 2:
        text = f'{float(value):.2f}'
    return text.replace('.', ',') + ' €'


def normalize_name(name: str) -> str:
    return ' '.join(str(name or '').split())[:80]


def order_label(order: SupplierOrder) -> str:
    """Identifiant lisible d'une commande : « 12/11/2026 · Medisafe · n° 4512 » (sans numero : date et fournisseur)."""
    label = f'{_day(order.ordered)} · {order.supplier.name}'
    return f'{label} · n° {order.number}' if order.number else label


# ---- Lecture des commandes : lignes, quantites recues, dates de reception ----------------------------------------

@dataclass
class Line:
    row: StockOrder
    received: int
    first: date | None  # premiere et derniere reception (jour local)
    last: date | None

    @property
    def paid_quantity(self) -> int:
        """Quantite payee : commandee, ou seulement recue si la commande est close ou la ligne decochee."""
        order = self.row.supplier_order
        if (order is not None and order.closed is not None) or self.row.cancelled is not None:
            return self.received
        return self.row.quantity

    @property
    def paid(self) -> Decimal:
        return (self.row.unit_price or Decimal(0)) * self.paid_quantity


@dataclass
class Order:
    row: SupplierOrder
    lines: list[Line] = field(default_factory=list)

    @property
    def first_received(self) -> date | None:
        days = [line.first for line in self.lines if line.first is not None]
        return min(days) if days else None

    @property
    def last_received(self) -> date | None:
        days = [line.last for line in self.lines if line.last is not None]
        return max(days) if days else None

    @property
    def complete(self) -> bool:
        """Tout est arrive."""
        return bool(self.lines) and all(line.received >= line.row.quantity for line in self.lines)

    @property
    def waiting(self) -> bool:
        """Attend encore une livraison : une ligne pas entierement reçue ni decochee, commande ni close ni annulee."""
        return (self.row.cancelled is None and self.row.closed is None
                and any(line.received < line.row.quantity and line.row.cancelled is None for line in self.lines))

    @property
    def delay(self) -> int | None:
        """Delai de livraison en jours ouvres (jusqu'a la premiere reception)."""
        first = self.first_received
        return None if first is None else business_days_between(self.row.ordered, first)

    @property
    def items_total(self) -> Decimal:
        return sum((line.paid for line in self.lines), Decimal(0))

    @property
    def total(self) -> Decimal:
        return self.items_total + self.row.shipping


def load_orders(queryset: Iterable[SupplierOrder] | None = None) -> list[Order]:
    """Commandes avec leurs lignes et ce qui a ete recu pour chacune (une requete pour les lignes)."""
    rows = list(SupplierOrder.objects.select_related('supplier') if queryset is None else queryset)
    by_id = {row.id: Order(row) for row in rows}
    lines = (StockOrder.objects.select_related('item_type').filter(supplier_order_id__in=list(by_id))
             .annotate(got=Count('items'), first_at=Min('items__added'), last_at=Max('items__added')).order_by('id'))
    for line in lines:
        line.supplier_order = by_id[line.supplier_order_id].row
        by_id[line.supplier_order_id].lines.append(Line(
            line, line.got,
            timezone.localtime(line.first_at).date() if line.first_at else None,
            timezone.localtime(line.last_at).date() if line.last_at else None))
    return [by_id[row.id] for row in rows]


@dataclass
class Delay:
    days: int
    basis: str  # supplier, all, default
    samples: int

    def text(self, supplier: str) -> str:
        days = plural(self.days, 'jour ouvré', 'jours ouvrés')
        if self.basis == 'supplier':
            return (f"{days} : délai médian de {supplier} sur ses "
                    f"{plural(self.samples, 'dernière commande reçue', 'dernières commandes reçues')}")
        if self.basis == 'all':
            return (f"{days} : délai médian de tous les fournisseurs ({supplier} n'a pas encore livré de commande "
                    f"enregistrée)")
        return f"{days} : délai par défaut, aucune commande reçue pour l'instant"


def delays(orders: list[Order]) -> dict[int, list[int]]:
    """Delais (jours ouvres) des commandes recues, par fournisseur, les plus recentes d'abord."""
    by_supplier: defaultdict[int, list[tuple[date, int]]] = defaultdict(list)
    for order in orders:
        if order.row.cancelled is None and order.delay is not None:
            by_supplier[order.row.supplier_id].append((order.row.ordered, order.delay))
    return {supplier: [days for _, days in sorted(rows, reverse=True)] for supplier, rows in by_supplier.items()}


def _median(values: list[int]) -> int:
    return math.ceil(statistics.median(values))


def estimate_delay(supplier_id: int, measured: dict[int, list[int]]) -> Delay:
    own = measured.get(supplier_id, [])[:DELAY_SAMPLES]
    if own:
        return Delay(_median(own), 'supplier', len(own))
    every = [days for rows in measured.values() for days in rows[:DELAY_SAMPLES]]
    if every:
        return Delay(_median(every), 'all', len(every))
    return Delay(DEFAULT_DELAY_DAYS, 'default', 0)


def order_state(order: Order, expected: date, today: date) -> str:
    if order.row.cancelled is not None:
        return 'cancelled'
    if order.complete:
        return 'received'
    if not order.waiting:
        return 'closed'  # close a la main, ou toutes les lignes pas reçues decochees
    if today > expected:
        return 'late'
    return 'partial' if any(line.received for line in order.lines) else 'waiting'


def expected_dates() -> dict[int, date]:
    """Livraison prevue de chaque commande fournisseur (taches du TECH : ligne pas livree en retard)."""
    orders = load_orders()
    measured = delays(orders)
    return {order.row.id: add_business_days(order.row.ordered, estimate_delay(order.row.supplier_id, measured).days)
            for order in orders}


def line_dict(line: Line) -> dict[str, Any]:
    row = line.row
    item_type = row.item_type
    return {
        'id': row.id, 'type': item_type.type, 'name': item_type.name, 'perishable': item_type.perissable,
        'pack_size': item_type.default_pack_size, 'quantity': row.quantity, 'received': line.received,
        'remaining': max(0, row.quantity - line.received) if row.cancelled is None else 0,
        'cancelled': row.cancelled is not None, 'unit_price': _price(row.unit_price),
        'total': _money((row.unit_price or Decimal(0)) * row.quantity) if row.unit_price is not None else None,
    }


def order_dict(order: Order, measured: dict[int, list[int]], today: date) -> dict[str, Any]:
    row = order.row
    delay = estimate_delay(row.supplier_id, measured)
    expected = add_business_days(row.ordered, delay.days)
    state = order_state(order, expected, today)
    first = order.first_received
    short = f"prévue le {expected.strftime('%d/%m')}"
    if state == 'received':
        when = order.last_received
        detail = (f"reçue le {_day(when)}" if when == first else f"reçue du {_day(first)} au {_day(when)}") \
            if when and first else 'reçue'
        short = f"reçue le {when.strftime('%d/%m/%y')}" if when else 'reçue'
        if order.delay is not None:
            detail += f" · livrée en {plural(order.delay, 'jour ouvré', 'jours ouvrés')}"
            short += f" ({plural(order.delay, 'jour ouvré', 'jours ouvrés')})"
    elif state == 'cancelled':
        detail = f"annulée le {_day(timezone.localtime(row.cancelled).date())}"
        short = 'annulée'
    elif state == 'closed':
        detail = f"close le {_day(timezone.localtime(row.closed).date())} : le reste ne viendra pas"
        short = 'le reste ne viendra pas'
    elif state == 'late':
        late = business_days_between(expected, today)
        detail = (f"livraison prévue le {_day(expected)}, en retard de "
                  f"{plural(late, 'jour ouvré', 'jours ouvrés')} : relancer {row.supplier.name}")
        short += f", {plural(late, 'jour ouvré', 'jours ouvrés')} de retard"
    else:
        detail = f"livraison prévue le {_day(expected)}"
    got = sum(min(line.received, line.row.quantity) for line in order.lines)
    if state in ('late', 'partial') and got:
        wanted = sum(line.row.quantity for line in order.lines if line.row.cancelled is None or line.received)
        detail = f"reçu {got}/{wanted} · {detail}"
        short = f"reste {wanted - got} · {short}"
    quantity = sum(line.row.quantity for line in order.lines)
    received = sum(min(line.received, line.row.quantity) for line in order.lines)
    return {
        'id': row.id, 'label': order_label(row), 'supplier': row.supplier.name, 'supplier_id': row.supplier_id,
        'number': row.number, 'ordered': row.ordered.isoformat(), 'shipping': _money(row.shipping), 'note': row.note,
        'created_by': identity_name(row.created_by), 'state': state, 'state_text': STATES[state], 'detail': detail,
        'short': short,
        'open': state in OPEN_STATES, 'expected': expected.isoformat(), 'delay': delay.days,
        'delay_text': delay.text(row.supplier.name), 'measured_delay': order.delay,
        'first_received': first.isoformat() if first else None,
        'last_received': order.last_received.isoformat() if order.last_received else None,
        'quantity': quantity, 'received': received, 'items_total': _money(order.items_total),
        'total': _money(order.total), 'priced': all(line.row.unit_price is not None for line in order.lines),
        'lines': [line_dict(line) for line in order.lines],
    }


def orders_list(only_open: bool = False, limit: int = 300, today: date | None = None) -> list[dict[str, Any]]:
    """Commandes, ce qui merite l'attention d'abord (en retard, reçues en partie, en attente), puis les plus
    recentes. only_open : celles qui attendent encore une livraison (page de reception)."""
    today = today or timezone.localdate()
    orders = load_orders()
    measured = delays(orders)
    rows = [order_dict(order, measured, today) for order in orders]
    if only_open:
        rows = [row for row in rows if row['open']]
    rows.sort(key=lambda row: (STATE_ORDER[row['state']], row['expected'] if row['open'] else '',
                               '' if row['open'] else _reverse(row['ordered']), -row['id']))
    return rows[:limit]


def _reverse(iso: str) -> str:
    """Tri decroissant d'une date ISO dans un tri croissant."""
    return ''.join(chr(ord('9') - ord(c) + ord('0')) if c.isdigit() else c for c in iso)


def order_detail(order_id: int, today: date | None = None) -> dict[str, Any]:
    today = today or timezone.localdate()
    orders = load_orders()
    measured = delays(orders)
    for order in orders:
        if order.row.id == order_id:
            return order_dict(order, measured, today)
    raise SupplierOrder.DoesNotExist


def suppliers_list() -> list[dict[str, Any]]:
    """Fournisseurs deja enregistres (suggestions de la saisie), les plus utilises d'abord."""
    rows = Supplier.objects.annotate(count=Count('orders'), last=Max('orders__ordered')).order_by('name')
    return [{'id': row.id, 'name': row.name, 'orders': row.count, 'last_order': row.last.isoformat() if row.last else None}
            for row in rows]


# ---- Saisie -------------------------------------------------------------------------------------------------------

def _parse_price(value: Any, field_name: str) -> Decimal | None:
    if value in (None, ''):
        return None
    try:
        number = parse_decimal(value)
    except (InvalidOperation, ValueError, TypeError):
        raise PurchaseError(f'{field_name} : nombre attendu')
    if number < 0 or number > MAX_PRICE:
        raise PurchaseError(f'{field_name} : entre 0 et {MAX_PRICE} €')
    return number.quantize(Decimal('0.0001'))


def _parse_quantity(value: Any, name: str) -> int:
    try:
        quantity = int(value)
    except (TypeError, ValueError):
        raise PurchaseError(f'Quantité de {name} : nombre entier attendu')
    if not 1 <= quantity <= MAX_QUANTITY:
        raise PurchaseError(f'Quantité de {name} : entre 1 et {MAX_QUANTITY}')
    return quantity


def _parse_date(value: Any) -> date:
    if value in (None, ''):
        return timezone.localdate()
    try:
        day = date.fromisoformat(str(value))
    except ValueError:
        raise PurchaseError('Date de commande invalide')
    if day > timezone.localdate() + timedelta(days=1):
        raise PurchaseError('La date de commande est dans le futur')
    return day


def _orderable(code: Any) -> ItemType:
    item_type = ItemType.objects.filter(type=str(code or '')).first()
    if item_type is None:
        raise PurchaseError(f"Type d'item inconnu : {code}")
    if not item_type.physical:
        raise PurchaseError(f"{item_type.name} est un item à cocher : rien à commander ni à réceptionner")
    if item_type.is_unit_group:
        raise PurchaseError(f"{item_type.name} est rempli par ses sous-types : commandez un de ses sous-types")
    return item_type


def supplier_for(name: Any) -> Supplier:
    """Fournisseur de ce nom (sans accents ni casse, espaces reduits), cree s'il n'existe pas."""
    clean = normalize_name(name)
    if not clean:
        raise PurchaseError('Nom du fournisseur obligatoire')
    key = name_key(clean)
    for supplier in Supplier.objects.all():
        if name_key(supplier.name) == key:
            return supplier
    return Supplier.objects.create(name=clean)


def _check_unique(supplier: Supplier, number: str, ordered: date, skip: int | None = None) -> None:
    if not number:
        return
    clash = SupplierOrder.objects.filter(supplier=supplier, number=number, ordered=ordered).exclude(pk=skip)
    if clash.exists():
        raise PurchaseError(f"La commande n° {number} de {supplier.name} du {_day(ordered)} est déjà enregistrée")


def _header(data: dict[str, Any]) -> tuple[Supplier, str, date, Decimal, str]:
    supplier = supplier_for(data.get('supplier'))
    number = ' '.join(str(data.get('number') or '').split())[:64]
    ordered = _parse_date(data.get('ordered'))
    shipping = _parse_price(data.get('shipping'), 'Frais de livraison') or Decimal(0)
    note = ' '.join(str(data.get('note') or '').split())[:200]
    return supplier, number, ordered, shipping.quantize(Decimal('0.01')), note


def _lines(data: dict[str, Any]) -> list[dict[str, Any]]:
    lines = data.get('lines')
    if not isinstance(lines, list) or not lines:
        raise PurchaseError('Ajoutez au moins une ligne (type et quantité)')
    if len(lines) > MAX_LINES:
        raise PurchaseError(f'{MAX_LINES} lignes au plus par commande')
    parsed = []
    for line in lines:
        if not isinstance(line, dict):
            raise PurchaseError('Ligne de commande invalide')
        item_type = _orderable(line.get('type'))
        parsed.append({'id': line.get('id'), 'type': item_type,
                       'quantity': _parse_quantity(line.get('quantity'), item_type.name),
                       'unit_price': _parse_price(line.get('unit_price'), f'Prix de {item_type.name}')})
    return parsed


def _summary(order: SupplierOrder, lines: list[StockOrder]) -> str:
    items = ', '.join(f'{line.quantity} × {line.item_type.name}' for line in lines[:4])
    return f"{order_label(order)} : {items}{'…' if len(lines) > 4 else ''}"


def create_order(data: dict[str, Any], identity: str) -> SupplierOrder:
    with transaction.atomic():
        supplier, number, ordered, shipping, note = _header(data)
        lines = _lines(data)
        _check_unique(supplier, number, ordered)
        order = SupplierOrder.objects.create(supplier=supplier, number=number, ordered=ordered, shipping=shipping,
                                             note=note, created_by=identity)
        created = [StockOrder.objects.create(item_type=line['type'], quantity=line['quantity'],
                                             unit_price=line['unit_price'], supplier_order=order, placed_by=identity)
                   for line in lines]
        journal.record(OperationKind.ORDER, identity, 'Commande fournisseur enregistrée : ' + _summary(order, created),
                       supplier_order=order.id, supplier=supplier.name, number=number, total=_money(
                           sum(((line.unit_price or Decimal(0)) * line.quantity for line in created), shipping)))
    return order


def update_order(order: SupplierOrder, data: dict[str, Any], identity: str) -> SupplierOrder:
    """Correction d'une commande : en-tete, lignes (quantite et prix ; une ligne deja recue ne peut ni disparaitre,
    ni changer de type, ni descendre sous ce qui a ete recu), nouvelles lignes."""
    if order.cancelled is not None:
        raise PurchaseError('Commande annulée : plus modifiable')
    with transaction.atomic():
        supplier, number, ordered, shipping, note = _header(data)
        lines = _lines(data)
        _check_unique(supplier, number, ordered, skip=order.pk)
        existing = {line.id: line for line in order.lines.annotate(got=Count('items'))}
        kept = set()
        for line in lines:
            row = existing.get(line['id']) if line['id'] is not None else None
            if row is None:
                StockOrder.objects.create(item_type=line['type'], quantity=line['quantity'],
                                          unit_price=line['unit_price'], supplier_order=order, placed_by=identity,
                                          cancelled=order.closed, cancelled_by=order.closed_by)
                continue
            kept.add(row.id)
            if row.got and row.item_type_id != line['type'].type:
                raise PurchaseError(f'{row.item_type.name} a déjà été reçu : son type ne peut plus changer')
            if line['quantity'] < row.got:
                raise PurchaseError(f'{row.item_type.name} : {row.got} déjà reçus, la quantité ne peut pas être plus '
                                    'petite')
            row.item_type = line['type']
            row.quantity = line['quantity']
            row.unit_price = line['unit_price']
            # quantite augmentee : la ligne attend de nouveau le reste
            row.delivered = None if row.got < row.quantity else (row.delivered or timezone.now())
            row.save(update_fields=['item_type', 'quantity', 'unit_price', 'delivered'])
        for line_id, row in existing.items():
            if line_id in kept:
                continue
            if row.got:
                raise PurchaseError(f'{row.item_type.name} a déjà été reçu : la ligne ne peut pas être retirée')
            row.delete()
        order.supplier, order.number, order.ordered, order.shipping, order.note = (supplier, number, ordered,
                                                                                   shipping, note)
        order.save(update_fields=['supplier', 'number', 'ordered', 'shipping', 'note'])
        journal.record(OperationKind.ORDER, identity, 'Commande fournisseur modifiée : ' + order_label(order),
                       supplier_order=order.id)
    return order


def cancel_order(order: SupplierOrder, identity: str) -> None:
    """Annuler une commande saisie par erreur ou jamais passee : seulement si rien n'a ete recu."""
    if order.cancelled is not None:
        return
    if Items.objects.filter(order_line__supplier_order=order).exists():
        raise PurchaseError('Des items de cette commande ont déjà été reçus : clore la commande à la place')
    now = timezone.now()
    with transaction.atomic():
        order.cancelled, order.cancelled_by = now, identity
        order.save(update_fields=['cancelled', 'cancelled_by'])
        order.lines.filter(delivered__isnull=True, cancelled__isnull=True).update(cancelled=now, cancelled_by=identity)
        journal.record(OperationKind.ORDER, identity, 'Commande fournisseur annulée : ' + order_label(order),
                       supplier_order=order.id, cancelled=True)


def close_order(order: SupplierOrder, identity: str, reopen: bool = False) -> None:
    """Clore une commande reçue en partie (le reste ne viendra pas), ou la rouvrir."""
    if order.cancelled is not None:
        raise PurchaseError('Commande annulée')
    now = timezone.now()
    with transaction.atomic():
        if reopen:
            if order.closed is None:
                return
            order.lines.filter(cancelled=order.closed).update(cancelled=None, cancelled_by='')
            order.closed, order.closed_by = None, ''
            summary = 'Commande fournisseur rouverte : '
        else:
            if order.closed is not None:
                return
            order.closed, order.closed_by = now, identity
            order.lines.filter(delivered__isnull=True, cancelled__isnull=True).update(cancelled=now,
                                                                                     cancelled_by=identity)
            summary = 'Commande fournisseur close (reste pas livré) : '
        order.save(update_fields=['closed', 'closed_by'])
        journal.record(OperationKind.ORDER, identity, summary + order_label(order), supplier_order=order.id,
                       closed=not reopen)


def mark_delivered(line: StockOrder) -> None:
    """Ligne entierement recue : elle n'attend plus rien (la suggestion de ce type peut revenir)."""
    if line.delivered is None and line.items.count() >= line.quantity:
        line.delivered = timezone.now()
        line.save(update_fields=['delivered'])


# ---- Prix ---------------------------------------------------------------------------------------------------------

@dataclass
class TypePrices:
    """Achats d'un type d'item avec un prix, du plus ancien au plus recent : (jour, prix unitaire, quantite,
    id fournisseur, nom fournisseur)."""
    purchases: list[tuple[date, Decimal, int, int, str]] = field(default_factory=list)

    @property
    def last(self) -> tuple[date, Decimal, int, int, str]:
        return self.purchases[-1]

    def best(self, today: date) -> tuple[date, Decimal, int, int, str]:
        """Moins cher parmi les achats de l'annee ecoulee (a prix egal, le plus recent), sinon le dernier."""
        recent = [row for row in self.purchases if row[0] >= today - timedelta(days=BEST_PRICE_DAYS)] or [self.last]
        return min(reversed(recent), key=lambda row: row[1])

    def supplier_price(self, supplier_id: int) -> Decimal | None:
        """Dernier prix de ce fournisseur."""
        for row in reversed(self.purchases):
            if row[3] == supplier_id:
                return row[1]
        return None


def type_prices(orders: list[Order]) -> dict[str, TypePrices]:
    prices: defaultdict[str, TypePrices] = defaultdict(TypePrices)
    for order in sorted(orders, key=lambda order: (order.row.ordered, order.row.id)):
        if order.row.cancelled is not None:
            continue
        for line in order.lines:
            if line.row.unit_price is not None:
                prices[line.row.item_type_id].purchases.append(
                    (order.row.ordered, line.row.unit_price, line.row.quantity, order.row.supplier_id,
                     order.row.supplier.name))
    return dict(prices)


def _shipping_by_supplier(orders: list[Order]) -> dict[int, Decimal]:
    """Frais de livraison habituels de chaque fournisseur : mediane de ses commandes."""
    rows: defaultdict[int, list[Decimal]] = defaultdict(list)
    for order in orders:
        if order.row.cancelled is None:
            rows[order.row.supplier_id].append(order.row.shipping)
    return {supplier: Decimal(statistics.median(values)).quantize(Decimal('0.01')) for supplier, values in rows.items()}


# ---- Suggestions de commande, groupees par fournisseur -----------------------------------------------------------

def _open_types() -> set[str]:
    from .tasks import open_orders  # tasks lit aussi les commandes fournisseurs (livraison prevue)
    return set(open_orders())


def suggestions(today: date | None = None, data: dict[str, Any] | None = None,
                orders: list[Order] | None = None) -> dict[str, Any]:
    """Commandes recommandees a passer dans SUGGEST_WINDOW_DAYS jours, chez le fournisseur le moins cher connu,
    groupees par fournisseur ; les commandes du meme fournisseur a passer d'ici GROUP_HORIZON_DAYS jours y sont
    ajoutees (facultatives) pour ne payer qu'une livraison. Les types deja commandes (en attente de livraison) sont
    laisses de cote. Les groupes les plus urgents d'abord, les types sans prix connu a la fin."""
    today = today or timezone.localdate()
    data = data or stock_forecast.forecast(months=6)
    orders = load_orders() if orders is None else orders
    prices = type_prices(orders)
    shipping = _shipping_by_supplier(orders)
    names = {supplier.id: supplier.name for supplier in Supplier.objects.all()}
    waiting = _open_types()
    window, horizon = today + timedelta(days=SUGGEST_WINDOW_DAYS), today + timedelta(days=GROUP_HORIZON_DAYS)
    lines = []
    for row in data['types']:
        order = row['order']
        if not order or order['before'] > horizon:
            continue
        code, name = order.get('subtype') or row['type'], order.get('subtype_name') or row['name']
        if code in waiting:
            continue
        known = prices.get(code)
        supplier_id, unit_price, why = None, None, 'aucun prix connu : fournisseur à choisir'
        if known:
            best, last = known.best(today), known.last
            supplier_id, unit_price = best[3], best[1]
            if best[3] == last[3]:
                why = f"dernier fournisseur ({unit_euros(best[1])} le {_day(best[0])})"
            else:
                why = (f"le moins cher ({unit_euros(best[1])} le {_day(best[0])}, contre {unit_euros(last[1])} chez "
                       f"{last[4]} la dernière fois)")
        quantity = order['quantity']
        lines.append({
            'type': code, 'name': name, 'quantity': quantity, 'before': order['before'].isoformat(),
            'urgent': bool(order['urgent']), 'due': order['before'] <= window, 'supplier_id': supplier_id,
            'unit_price': _price(unit_price), 'total': _money(unit_price * quantity) if unit_price is not None else None,
            'why': why,
            'when': 'dès que possible' if order['urgent'] else f"avant le {_day(order['before'])}",
        })
    groups = []
    by_supplier: defaultdict[int | None, list[dict[str, Any]]] = defaultdict(list)
    for line in lines:
        by_supplier[line['supplier_id']].append(line)
    for supplier_id, rows in by_supplier.items():
        due = [row for row in rows if row['due']]
        if not due:
            continue  # rien a commander tout de suite chez lui
        later = [row for row in rows if not row['due']] if supplier_id is not None else []
        rows = sorted(due, key=lambda row: row['before']) + sorted(later, key=lambda row: row['before'])
        items_total = sum(Decimal(str(row['total'])) for row in rows if row['total'] is not None)
        fee = shipping.get(supplier_id) if supplier_id is not None else None
        # sans grouper : une commande par semaine d'echeance (celles de la meme semaine partent ensemble)
        weeks = len({date.fromisoformat(row['before']).isocalendar()[:2] for row in rows})
        saved = fee * (weeks - 1) if fee and weeks > 1 else Decimal(0)
        if supplier_id is None:
            advice = 'Pas encore acheté avec un prix : choisissez le fournisseur en créant la commande.'
        elif weeks > 1 and fee:
            advice = (f"{plural(len(rows), 'ligne')} à échéance sur {plural(weeks, 'semaine')} différentes : "
                      f"commandées ensemble, une seule livraison au lieu de {weeks} (≈ {euros(saved)} économisés).")
        elif len(rows) > 1:
            advice = f"{plural(len(rows), 'ligne')} chez le même fournisseur : une seule livraison."
        else:
            advice = ''
        groups.append({
            'supplier_id': supplier_id, 'supplier': names.get(supplier_id, '') if supplier_id else '',
            'before': min(row['before'] for row in rows), 'urgent': any(row['urgent'] for row in rows),
            'lines': rows, 'due': len(due), 'later': len(later), 'items_total': _money(items_total),
            'shipping': _money(fee), 'total': _money(items_total + (fee or 0)), 'saved': _money(saved),
            'priced': all(row['total'] is not None for row in rows), 'advice': advice,
        })
    groups.sort(key=lambda group: (group['supplier_id'] is None, not group['urgent'], group['before'],
                                   name_key(group['supplier'])))
    return {'groups': groups, 'window_days': SUGGEST_WINDOW_DAYS, 'horizon_days': GROUP_HORIZON_DAYS,
            'saved': _money(sum((Decimal(str(group['saved'])) for group in groups), Decimal(0))),
            'total': _money(sum((Decimal(str(group['total'])) for group in groups), Decimal(0)))}


# ---- Statistiques -------------------------------------------------------------------------------------------------

def _month_key(day: date) -> str:
    return f'{day.year:04d}-{day.month:02d}'


def _months(today: date, count: int) -> list[str]:
    keys = []
    year, month = today.year, today.month
    for _ in range(count):
        keys.append(f'{year:04d}-{month:02d}')
        month -= 1
        if month == 0:
            year, month = year - 1, 12
    return list(reversed(keys))


def stats(today: date | None = None) -> dict[str, Any]:
    """Statistiques des achats (poste, onglet Statistiques des Commandes)."""
    today = today or timezone.localdate()
    year_ago = today - timedelta(days=365)
    orders = load_orders()
    live = [order for order in orders if order.row.cancelled is None]
    measured = delays(orders)
    prices = type_prices(orders)
    data = stock_forecast.forecast(months=6)
    per_month = {row['type']: row['per_month'] for row in data['types']}

    # depenses par mois (date de commande) et totaux de l'annee ecoulee
    month_keys = _months(today, STATS_MONTHS)
    months = {key: {'month': key, 'items': Decimal(0), 'shipping': Decimal(0), 'orders': 0} for key in month_keys}
    for order in live:
        month = months.get(_month_key(order.row.ordered))
        if month is not None:
            month['items'] += order.items_total
            month['shipping'] += order.row.shipping
            month['orders'] += 1
    recent = [order for order in live if order.row.ordered > year_ago]
    spent_items = sum((order.items_total for order in recent), Decimal(0))
    spent_shipping = sum((order.row.shipping for order in recent), Decimal(0))
    spent = spent_items + spent_shipping

    # fournisseurs
    suppliers = []
    by_supplier: defaultdict[int, list[Order]] = defaultdict(list)
    for order in live:
        by_supplier[order.row.supplier_id].append(order)
    for supplier_id, rows in by_supplier.items():
        own = measured.get(supplier_id, [])
        year_rows = [order for order in rows if order.row.ordered > year_ago]
        late = 0
        for order in rows:  # livrees apres la date prevue avec le delai habituel de ce fournisseur
            if order.delay is not None and own and order.delay > _median(own[:DELAY_SAMPLES]):
                late += 1
        suppliers.append({
            'id': supplier_id, 'name': rows[0].row.supplier.name, 'orders': len(rows),
            'orders_year': len(year_rows),
            'spent_year': _money(sum((order.total for order in year_rows), Decimal(0))),
            'spent_total': _money(sum((order.total for order in rows), Decimal(0))),
            'shipping_avg': _money(sum((order.row.shipping for order in rows), Decimal(0)) / len(rows)),
            'free_shipping': sum(1 for order in rows if not order.row.shipping),
            'delay_median': _median(own[:DELAY_SAMPLES]) if own else None,
            'delay_min': min(own) if own else None, 'delay_max': max(own) if own else None,
            'delays': len(own), 'slower': late,
            'types': len({line.row.item_type_id for order in rows for line in order.lines}),
            'last_order': max(order.row.ordered for order in rows).isoformat(),
            'open': sum(1 for order in rows if order.waiting),
        })
    suppliers.sort(key=lambda row: (-(row['spent_year'] or 0), name_key(row['name'])))

    # prix par type
    type_names = dict(ItemType.objects.values_list('type', 'name'))
    types = []
    yearly_cost = Decimal(0)
    savings = Decimal(0)
    for code, known in prices.items():
        last, best = known.last, known.best(today)
        quantity = sum(row[2] for row in known.purchases)
        average = sum((row[1] * row[2] for row in known.purchases), Decimal(0)) / quantity if quantity else last[1]
        year_rows = [row for row in known.purchases if row[0] > year_ago]
        consumption = Decimal(str(per_month.get(code, 0) or 0)) * 12
        cost = consumption * last[1]
        saving = (last[1] - best[1]) * consumption if best[3] != last[3] and best[1] < last[1] else Decimal(0)
        yearly_cost += cost
        savings += saving
        first = known.purchases[0][1]
        types.append({
            'type': code, 'name': type_names.get(code, code),
            'last_price': _price(last[1]), 'last_supplier': last[4], 'last_date': last[0].isoformat(),
            'best_price': _price(best[1]), 'best_supplier': best[4], 'best_date': best[0].isoformat(),
            'average_price': _price(average), 'min_price': _price(min(row[1] for row in known.purchases)),
            'max_price': _price(max(row[1] for row in known.purchases)),
            'change': round(float((last[1] - first) / first * 100), 1) if first else None,
            'bought_year': sum(row[2] for row in year_rows),
            'spent_year': _money(sum((row[1] * row[2] for row in year_rows), Decimal(0))),
            'per_year': round(float(consumption), 1), 'yearly_cost': _money(cost), 'saving': _money(saving),
            'suppliers': len({row[3] for row in known.purchases}),
            'history': [{'date': row[0].isoformat(), 'price': _price(row[1]), 'quantity': row[2], 'supplier': row[4]}
                        for row in known.purchases],
        })
    # economies possibles d'abord (ce qui merite l'attention), puis les plus grosses depenses
    types.sort(key=lambda row: (-(row['saving'] or 0), -(row['spent_year'] or 0), name_key(row['name'])))

    stock = _stock_value(prices, today, year_ago)
    upcoming = suggestions(today, data, orders)
    measured_all = [days for rows in measured.values() for days in rows]
    return {
        'today': today.isoformat(),
        'months': [{'month': row['month'], 'items': _money(row['items']), 'shipping': _money(row['shipping']),
                    'orders': row['orders']} for row in months.values()],
        'year': {
            'spent': _money(spent), 'items': _money(spent_items), 'shipping': _money(spent_shipping),
            'shipping_share': round(float(spent_shipping / spent * 100), 1) if spent else None,
            'orders': len(recent),
            'average_order': _money(spent / len(recent)) if recent else None,
            'items_bought': sum(min(line.paid_quantity, line.row.quantity) for order in recent for line in order.lines),
            'monthly_average': _money(spent / STATS_MONTHS),
        },
        'delay': {'median': _median(measured_all) if measured_all else None, 'samples': len(measured_all),
                  'default': DEFAULT_DELAY_DAYS},
        'suppliers': suppliers,
        'types': types,
        'stock': stock,
        'estimates': {
            'yearly_cost': _money(yearly_cost),
            'yearly_text': (f"Au rythme de consommation actuel, le matériel dont le prix est connu coûte environ "
                            f"{euros(yearly_cost)} par an (hors livraison).") if yearly_cost else '',
            'savings': _money(savings),
            'savings_text': (f"En achetant chaque type au meilleur prix de l'année au lieu du dernier, environ "
                             f"{euros(savings)} économisés par an.") if savings else '',
            'grouping_saved': upcoming['saved'],
            'next_orders': upcoming['total'],
            'next_text': (f"Commandes suggérées (à passer dans les {SUGGEST_WINDOW_DAYS} jours, groupées par "
                          f"fournisseur) : environ {euros(upcoming['total'])} livraison comprise.")
            if upcoming['groups'] else '',
        },
        'holidays': [{'date': day.isoformat(), 'name': name}
                     for year in (today.year, today.year + 1) for day, name in sorted(holidays(year).items())
                     if day >= today][:8],
    }


def _stock_value(prices: dict[str, TypePrices], today: date, year_ago: date) -> dict[str, Any]:
    """Valeur du materiel present (prix d'achat de l'item s'il vient d'une commande, sinon dernier prix de son
    type), du materiel utilise et jete perime sur l'annee ecoulee."""
    last = {code: known.last[1] for code, known in prices.items()}
    value_stock = value_lots = Decimal(0)
    priced = unpriced = 0
    by_type: defaultdict[str, Decimal] = defaultdict(Decimal)
    items = (Items.objects.filter(status=ItemStatus.ACTIVE, deleted__isnull=True)
             .values_list('pack__item_type_id', 'order_line__unit_price', 'location_id', 'location__lot_type__storage'))
    for code, own, location, storage in items:
        price = own if own is not None else last.get(code)
        if price is None:
            unpriced += 1
            continue
        priced += 1
        by_type[code] += price
        if location is None or storage:
            value_stock += price
        else:
            value_lots += price
    used = discarded = Decimal(0)
    used_count = discarded_count = 0
    moves = (ItemMovement.objects.filter(kind__in=(MovementKind.USED, MovementKind.DISCARDED), cancelled__isnull=True,
                                         at__date__gt=year_ago)
             .values_list('kind', 'item_type_id', 'item__order_line__unit_price'))
    for kind, code, own in moves:
        price = own if own is not None else last.get(code)
        if price is None:
            continue
        if kind == MovementKind.DISCARDED:
            discarded += price
            discarded_count += 1
        else:
            used += price
            used_count += 1
    total = value_stock + value_lots
    names = dict(ItemType.objects.values_list('type', 'name'))
    top = sorted(by_type.items(), key=lambda pair: -pair[1])[:8]
    return {
        'value': _money(total), 'value_stock': _money(value_stock), 'value_lots': _money(value_lots),
        'priced': priced, 'unpriced': unpriced,
        'coverage': round(priced / (priced + unpriced) * 100) if priced + unpriced else None,
        'used_year': _money(used), 'used_count': used_count,
        'discarded_year': _money(discarded), 'discarded_count': discarded_count,
        'waste_share': round(float(discarded / (used + discarded) * 100), 1) if used + discarded else None,
        'top': [{'type': code, 'name': names.get(code, code), 'value': _money(value)} for code, value in top],
        'text': (f"{plural(priced, 'item')} sur {priced + unpriced} ont un prix connu"
                 + (f" ({plural(unpriced, 'item')} sans prix : types jamais achetés avec une commande)"
                    if unpriced else '') + '.'),
        'waste_text': (f"Jeté périmé sur l'année : {euros(discarded)} ({plural(discarded_count, 'item')}), "
                       f"soit {round(float(discarded / (used + discarded) * 100))} % de ce qui est sorti (utilisé ou jeté).")
        if discarded else '',
    }
