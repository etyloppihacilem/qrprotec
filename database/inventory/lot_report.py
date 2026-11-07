# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          lot_report.py
#       -\-    _|__
#        |\___/  . \        Created on 07 Nov. 2026 at 11:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Rapport d'etat d'un lot a une date donnee, en PDF A4 (admins, depuis le front web).

L'etat est reconstitue depuis le journal des mouvements d'items (ItemMovement) : l'emplacement d'un item a la date T
est le lot d'origine (from_lot) de son premier mouvement apres T, ou son emplacement actuel s'il n'a pas bouge depuis.
Une absence notee a une verif avant T et pas encore annulee a T (item retrouve plus tard) le marque « absent ». La
derniere verif est la plus recente avant T ; le scelle, le dernier scellage ou ouverture du journal avant T.

Le contenu attendu et l'arborescence des sous-lots sont ceux d'aujourd'hui (le journal ne garde pas leur historique).
Le rapport suit les sous-lots en cascade (lot, puis ses sous-lots en profondeur d'abord) et finit par une case
« Certifié par le TECH <antenne> » avec un espace pour la signature.
"""

from collections import defaultdict
from dataclasses import dataclass, field

from django.utils import timezone

from .idendity import short_identity_name
from .lot_sheet import CONTENT_WIDTH, GREY_BAND, GREY_TEXT, MARGIN, PAGE_HEIGHT, PAGE_WIDTH, Page, build_pdf, fit, wrap
from .models import ABSENCE_KINDS, ItemMovement, Items, Operation, OperationKind, Verifs, name_key
from .services import lot_requirements

WEEKDAYS = ['lundi', 'mardi', 'mercredi', 'jeudi', 'vendredi', 'samedi', 'dimanche']
NO_LOCATION = 'Sans emplacement'

FOOTER_Y = PAGE_HEIGHT - 22
BODY_BOTTOM = FOOTER_Y - 18
INDENT = 12              # decalage par niveau de sous-lot
ROW = 13                 # hauteur d'une ligne du tableau des items
SIGN_HEIGHT = 120        # case de certification


def long_datetime(value):
    """« jeudi 11/12/2026 à 15:45 » (heure locale)."""
    value = timezone.localtime(value)
    return f"{WEEKDAYS[value.weekday()]} {value.strftime('%d/%m/%Y')} à {value.strftime('%H:%M')}"


def short_date(value):
    return value.strftime('%d/%m/%Y') if value else ''


def who(identity):
    # front web : jamais de nom de famille (prenom, initiale en cas d'homonyme)
    return short_identity_name(identity) if identity else '?'


# ----------------------------------------------------------------------------------------------------------------------
# Etat a la date T

@dataclass
class ItemState:
    item: Items
    absent: bool = False     # note absent a une verif avant T, pas encore retrouve a T
    expired: bool = False    # perime a la date du rapport
    disabled: bool = False   # desactive (rappel produit...) a la date du rapport


@dataclass
class LotState:
    lot: object
    depth: int
    requirements: list
    items: list = field(default_factory=list)          # [ItemState]
    last_verif: object = None
    sealed: object = None                               # Operation SEAL en vigueur a T
    lacking: list = field(default_factory=list)         # [(Requirement, manquants)]

    @property
    def counted(self):
        return bool(self.requirements)

    @property
    def complete(self):
        return not self.lacking


def item_locations(lots, at):
    """{iid: (item, lot id a la date at)} des items qui etaient dans l'un des lots a cette date."""
    ids = {lot.id for lot in lots}
    touched = set(ItemMovement.objects.filter(at__gt=at, from_lot_id__in=ids).values_list('item_id', flat=True))
    touched |= set(ItemMovement.objects.filter(at__gt=at, to_lot_id__in=ids).values_list('item_id', flat=True))
    candidates = (Items.objects.select_related('pack__item_type')
                  .filter(added__lte=at)
                  .filter(location_id__in=ids) | Items.objects.select_related('pack__item_type')
                  .filter(added__lte=at, iid__in=touched))
    candidates = {item.iid: item for item in candidates}
    first_after = {}
    for move in (ItemMovement.objects.filter(item_id__in=list(candidates), at__gt=at)
                 .order_by('at', 'id').only('item_id', 'from_lot_id')):
        first_after.setdefault(move.item_id, move.from_lot_id)
    result = {}
    for iid, item in candidates.items():
        location = first_after[iid] if iid in first_after else item.location_id
        if location in ids:
            result[iid] = (item, location)
    return result


def open_absences(iids, at):
    """Items notes absents a une verif avant at et pas encore retrouves a cette date."""
    return set(ItemMovement.objects.filter(item_id__in=list(iids), kind__in=ABSENCE_KINDS, at__lte=at)
               .exclude(cancelled__lte=at).values_list('item_id', flat=True))


def last_verifs(lots, at):
    result = {}
    for verif in Verifs.objects.filter(lot_id__in=[lot.id for lot in lots], datetime__lte=at).order_by('datetime', 'id'):
        result[verif.lot_id] = verif
    return result


def seals(lots, at):
    """{lot id: operation de scellage} des lots scelles a la date at."""
    last = {}
    for operation in (Operation.objects.filter(lot_id__in=[lot.id for lot in lots], at__lte=at,
                                               kind__in=(OperationKind.SEAL, OperationKind.UNSEAL))
                      .order_by('at', 'id')):
        last[operation.lot_id] = operation
    return {lot_id: operation for lot_id, operation in last.items() if operation.kind == OperationKind.SEAL}


def state_at(root, at):
    """[LotState] du lot et de ses sous-lots (en cascade) a la date at."""
    lots = [lot for lot in root.descendants() if lot.created <= at or lot.id == root.id]
    kept = {root.id}
    lots = [lot for lot in lots if lot.id == root.id or (lot.parent_id in kept and not kept.add(lot.id))]
    day = timezone.localdate(at)
    located = item_locations(lots, at)
    absent = open_absences(located, at)
    verifs = last_verifs(lots, at)
    sealed = seals(lots, at)
    states = {lot.id: LotState(lot, lot.depth, lot_requirements(lot), last_verif=verifs.get(lot.id),
                               sealed=sealed.get(lot.id)) for lot in lots}
    for iid, (item, location) in located.items():
        states[location].items.append(ItemState(
            item, absent=iid in absent, expired=item.is_expired(day),
            disabled=item.disabled and item.disabled_at is not None and item.disabled_at <= at))
    for state in states.values():
        good = defaultdict(int)
        for row in state.items:
            if not (row.absent or row.expired or row.disabled):
                good[row.item.pack.item_type_id] += 1
        state.lacking = [(requirement, requirement.quantity - good[requirement.item_type_id])
                         for requirement in state.requirements if good[requirement.item_type_id] < requirement.quantity]
    return [states[lot.id] for lot in lots]


# ----------------------------------------------------------------------------------------------------------------------
# PDF

COLUMNS = [  # (titre, largeur relative a la place restante)
    ('Item', None), ('iid', 118), ('Péremption', 62), ('État à cette date', 120),
]


def item_remark(row):
    if row.absent:
        return 'absent à la dernière vérif'
    if row.disabled:
        return 'désactivé'
    if row.expired:
        return 'périmé'
    return 'présent'


class ReportWriter:
    def __init__(self, root, at, editor, antenna, edited=None):
        self.root = root
        self.at = at
        self.editor = editor
        self.antenna = ' '.join(str(antenna or '').split())
        self.edited = timezone.localtime(edited or timezone.now())
        self.states = state_at(root, at)
        self.pages = []
        self.page = None
        self.y = 0

    # -- mise en page --------------------------------------------------------------------------------------------------

    def new_page(self):
        self.page = Page()
        self.pages.append(self.page)
        self.page.text(MARGIN, MARGIN + 8, "RAPPORT D'ÉTAT DE LOT", size=8, bold=True, grey=GREY_TEXT)
        self.page.text(PAGE_WIDTH - MARGIN, MARGIN + 8, 'État au ' + long_datetime(self.at), size=8, grey=GREY_TEXT,
                       align='right')
        self.y = MARGIN + 24

    def need(self, height):
        if self.page is None or self.y + height > BODY_BOTTOM:
            self.new_page()
            return True
        return False

    def text(self, x, text, size=10, bold=False, grey=0.0, width=None, step=None):
        width = width or (PAGE_WIDTH - MARGIN - x)
        self.page.text(x, self.y + size, fit(text, width, size, bold), size=size, bold=bold, grey=grey)
        self.y += step if step is not None else size + 4

    # -- contenu ----------------------------------------------------------------------------------------------------

    def title(self):
        self.new_page()
        root = self.root
        self.text(MARGIN, root.name, size=20, bold=True, step=26)
        details = [root.lot_type.name, root.id]
        ancestors = root.ancestors()
        if ancestors:
            details.insert(0, 'dans ' + ' › '.join(parent.name for parent in reversed(ancestors)))
        self.text(MARGIN, ' · '.join(details), size=10, grey=GREY_TEXT, step=20)
        self.page.rect(MARGIN, self.y, CONTENT_WIDTH, 44, fill=GREY_BAND, stroke=False)
        self.y += 6
        self.text(MARGIN + 10, 'État du lot au ' + long_datetime(self.at), size=14, bold=True, step=20)
        self.text(MARGIN + 10, f'Rapport édité le {long_datetime(self.edited)} par {self.editor}', size=10, step=24)
        counted = [state for state in self.states if state.counted]
        incomplete = [state for state in counted if not state.complete]
        sub_lots = len(self.states) - 1
        if not counted:
            summary = 'Aucun item attendu dans ce lot.'
        elif incomplete:
            summary = ('Incomplet à cette date : ' + ', '.join(state.lot.name for state in incomplete))
        else:
            summary = 'Complet à cette date' + (' (tous les sous-lots)' if sub_lots else '')
        self.text(MARGIN, summary, size=12, bold=True, step=18)
        if sub_lots:
            self.text(MARGIN, f"Sous-lots en cascade : {sub_lots}, détaillés à la suite du lot.", size=9.5,
                      grey=GREY_TEXT, step=14)
        self.text(MARGIN, "Contenu attendu et sous-lots tels qu'ils sont définis aujourd'hui ; items reconstitués "
                          'depuis le journal des mouvements.', size=8, grey=GREY_TEXT, step=16)

    def lot_section(self, state):
        lot = state.lot
        x = MARGIN + state.depth * INDENT
        width = PAGE_WIDTH - MARGIN - x
        self.need(22 + 3 * 14 + 2 * ROW)
        self.y += 6
        self.page.rect(x, self.y, width, 20, fill=GREY_BAND, stroke=False)
        name = ('Sous-lot · ' if state.depth else '') + lot.name
        if state.counted:
            verdict = 'Complet' if state.complete else 'Incomplet'
        else:
            verdict = 'Regroupement' if any(other.lot.parent_id == lot.id for other in self.states) else 'Rien d\'attendu'
        self.page.text(x + 6, self.y + 14, fit(name, width - 100, 12, True), size=12, bold=True)
        self.page.text(PAGE_WIDTH - MARGIN - 6, self.y + 14, verdict, size=10.5, bold=True, align='right')
        self.y += 26
        good = sum(1 for row in state.items if not (row.absent or row.expired or row.disabled))
        children = sum(1 for other in self.states if other.lot.parent_id == lot.id)
        if not state.counted and not state.items:
            # lot de regroupement : ses sous-lots suivent, rien a verifier en lui-meme
            what = f'regroupement de {children} sous-lot' + ('s' if children > 1 else '') if children else 'aucun item'
        else:
            what = f'{good} item' + ('s' if good > 1 else '') + ' valable' + ('s' if good > 1 else '') + ' à cette date'
        self.text(x, f'{lot.lot_type.name} · {lot.id} · {what}', size=9, grey=GREY_TEXT, step=13)
        verif = state.last_verif
        if verif is None and not state.counted and not state.items:
            line = None
        elif verif is None:
            line = 'Dernière vérif : aucune avant cette date'
        else:
            others = [who(person) for person in verif.participants if person and person != verif.by]
            line = (f'Dernière vérif : {long_datetime(verif.datetime)} par {who(verif.by)}'
                    + (f" (avec {', '.join(others)})" if others else '')
                    + (' · complète' if verif.complete else ' · incomplète')
                    + f' ({verif.present_count} présent' + ('s' if verif.present_count > 1 else '')
                    + f', {verif.missing_count} manquant' + ('s' if verif.missing_count > 1 else '') + ')')
        if line:
            self.text(x, line, size=9.5, step=13)
        if state.sealed is not None:
            seal = state.sealed
            number = seal.details.get('seal_number') or ''
            self.text(x, f"Scellé{' n°' + number if number else ''} posé le {long_datetime(seal.at)} par "
                         f'{who(seal.by)}, intact à cette date', size=9.5, step=13)
        for requirement, count in state.lacking:
            self.need(13)
            self.text(x, f'Manque {count} × {requirement.item_type.name}', size=9.5, bold=True, step=13)
        self.items_table(state, x, width)

    def items_table(self, state, x, width):
        if not state.items:
            if state.counted:
                self.need(14)
                self.text(x, 'Aucun item dans ce lot à cette date.', size=9.5, grey=GREY_TEXT, step=14)
            return
        fixed = sum(column_width for _, column_width in COLUMNS if column_width)
        widths = [column_width or width - fixed for _, column_width in COLUMNS]
        places = {requirement.item_type_id: requirement.location for requirement in state.requirements}
        order = {requirement.item_type_id: index for index, requirement in enumerate(state.requirements)}
        rows = sorted(state.items, key=lambda row: (
            order.get(row.item.pack.item_type_id, len(order)), name_key(row.item.pack.item_type.name),
            str(row.item.pack.peremption or '9999'), row.item.iid))

        def header():
            self.y += 4
            column_x = x
            for (title, _), column_width in zip(COLUMNS, widths):
                self.page.text(column_x + 2, self.y + 8, title, size=8, bold=True, grey=GREY_TEXT)
                column_x += column_width
            self.y += 11
            self.page.line(x, self.y, x + width, self.y, line_width=0.5)

        self.need(4 + 11 + ROW * 2)
        header()
        location = None
        for row in rows:
            item = row.item
            type_id = item.pack.item_type_id
            place = places.get(type_id, None)
            place = (place or NO_LOCATION) if type_id in places else 'Hors contenu attendu'
            band = place != location and (any(places.values()) or place == 'Hors contenu attendu')
            if self.need(ROW + (ROW if band else 0)):
                header()
            if band:
                self.page.text(x + 2, self.y + 10, fit(place, width - 4, 8.5, True), size=8.5, bold=True)
                self.y += ROW
            location = place
            values = [item.pack.item_type.name, item.iid, short_date(item.pack.peremption) or '—', item_remark(row)]
            column_x = x
            bad = row.absent or row.expired or row.disabled
            for index, (value, column_width) in enumerate(zip(values, widths)):
                self.page.text(column_x + 2, self.y + 9.5, fit(value, column_width - 6, 8.5, bad and index == 3),
                               size=8.5, bold=bad and index == 3)
                column_x += column_width
            self.y += ROW
            self.page.line(x, self.y - 2, x + width, self.y - 2, line_width=0.25, grey=0.8)

    def certification(self):
        self.need(SIGN_HEIGHT + 20)
        self.y += 16
        top = self.y
        self.page.rect(MARGIN, top, CONTENT_WIDTH, SIGN_HEIGHT, line_width=1)
        label = 'Certifié par le TECH' + (f' {self.antenna}' if self.antenna else '')
        for index, line in enumerate(wrap(label, CONTENT_WIDTH - 20, 12, True, max_lines=2)):
            self.page.text(MARGIN + 10, top + 18 + index * 15, line, size=12, bold=True)
        half = MARGIN + CONTENT_WIDTH * 0.45
        self.page.text(MARGIN + 10, top + 58, 'Nom :', size=10)
        self.page.line(MARGIN + 46, top + 60, half - 14, top + 60, line_width=0.4, grey=0.5)
        self.page.text(MARGIN + 10, top + 88, 'Date :', size=10)
        self.page.line(MARGIN + 46, top + 90, half - 14, top + 90, line_width=0.4, grey=0.5)
        self.page.text(half, top + 44, 'Signature :', size=10)
        self.page.rect(half, top + 50, PAGE_WIDTH - MARGIN - 10 - half, SIGN_HEIGHT - 60, line_width=0.4, dash=(2, 2))
        self.y = top + SIGN_HEIGHT

    def write(self):
        self.title()
        for state in self.states:
            self.lot_section(state)
        self.certification()
        total = len(self.pages)
        footer = (f"QRProtec · {self.root.name} au {timezone.localtime(self.at).strftime('%d/%m/%Y %H:%M')} · "
                  f"rapport édité le {self.edited.strftime('%d/%m/%Y à %H:%M')} par {self.editor}")
        for number, page in enumerate(self.pages, start=1):
            page.line(MARGIN, FOOTER_Y - 10, PAGE_WIDTH - MARGIN, FOOTER_Y - 10, line_width=0.4, grey=0.6)
            page.text(MARGIN, FOOTER_Y, fit(footer, CONTENT_WIDTH - 70, 8), size=8, grey=GREY_TEXT)
            page.text(PAGE_WIDTH - MARGIN, FOOTER_Y, f'page {number}/{total}', size=8, grey=GREY_TEXT, align='right')
        return build_pdf(self.pages, title=f"Rapport d'état - {self.root.name}")


def lot_report_pdf(root, at, editor, antenna, edited=None):
    return ReportWriter(root, at, editor, antenna, edited).write()
