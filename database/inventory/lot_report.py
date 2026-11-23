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
Le rapport prouve ce qui etait dans le lot a T : un item note absent a une verif avant T et pas encore retrouve a T
n'y figure pas. Un type d'item attendu en nombre insuffisant a T a sa propre ligne « manquant » dans le tableau, un
item au-dela des attendus est marque « en plus ». La derniere verif est la plus recente avant T (debut, fin et duree
quand le front les a envoyes) ; le scelle, le dernier scellage ou ouverture du journal avant T. Chaque item a l'heure
de son dernier scan avant T et, pour un item a taches, les taches cochees a ce scan.

Mise en page : nom du lot et etat a la date en tete, puis les secouristes cites (« Prénom Nom (matricule) », une
seule fois : le reste du rapport ne donne que les noms), un court texte qui explique le scan (pour les controles du
siege), puis le lot et ses sous-lots en cascade. Couleurs du front web, toujours doublees d'un mot (lisible en gris).

Le contenu attendu et l'arborescence des sous-lots sont ceux d'aujourd'hui (le journal ne garde pas leur historique) ;
la date de leur derniere modification avant T est indiquee, avec une mise en garde s'ils ont change depuis. Le rapport
finit par une case « Certifié par le TECH <antenne> » avec un espace pour la signature.
"""

from __future__ import annotations

from collections import defaultdict
from datetime import date as date_type, timedelta
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from django.db.models import Q
from django.utils import timezone

from .idendity import ANONYMOUS_OPENING, parse_identity
from .lot_sheet import (CONTENT_WIDTH, GREY_BAND, GREY_TEXT, MARGIN, PAGE_HEIGHT, PAGE_WIDTH, Page, Shade, build_pdf,
                        fit, rgb, wrap)
from .models import (ABSENCE_KINDS, MAINTENANCE, ItemMovement, ItemRevision, Items, MovementKind, Operation,
                     OperationKind, SeenWhile, VerifItem, VerifResult, Verifs, add_months, name_key)
from .services import lot_requirements

if TYPE_CHECKING:
    from datetime import date, datetime
    from inventory.models import Lots, Secouristes
    from inventory.services import Requirement

WEEKDAYS = ['lundi', 'mardi', 'mercredi', 'jeudi', 'vendredi', 'samedi', 'dimanche']
NO_LOCATION = 'Sans emplacement'
OUT_OF_CONTENT = 'Hors contenu attendu'
# antenne ou QRProtec est en service (texte explicatif) quand le nom de l'antenne n'est pas renseigne
DEFAULT_ANTENNA = 'Paris Centre'

FOOTER_Y = PAGE_HEIGHT - 22
BODY_BOTTOM = FOOTER_Y - 18
INDENT = 12              # decalage par niveau de sous-lot
ROW = 11                 # hauteur d'une ligne du tableau des items
TABLE_SIZE = 7.5         # police du tableau
TASK_SIZE = 6.5          # ligne des taches cochees sous un item
TASK_ROW = 8.5
SIGN_HEIGHT = 120        # case de certification

# Couleurs du front web (app.css), toujours accompagnees d'un mot : le rapport reste lisible imprime en gris
GREEN = rgb('#1f8f3a')
GREEN_BG = rgb('#e2f5e6')
RED = rgb('#d11d1d')
RED_BG = rgb('#fde3e3')
ORANGE = rgb('#e8820c')
BLUE = rgb('#2463b3')
BLUE_BG = rgb('#e3edf9')
NAVY = rgb('#14284b')
NAVY_BG = rgb('#e6ecf5')
PC_ORANGE = rgb('#f39200')  # filet orange de l'identite Protection Civile


def long_datetime(value: datetime, seconds: bool = False) -> str:
    """« jeudi 11/12/2026 à 15:45 » (heure locale), « … à 15:45:12 » avec seconds."""
    value = timezone.localtime(value)
    return (f"{WEEKDAYS[value.weekday()]} {value.strftime('%d/%m/%Y')} à "
            f"{value.strftime('%H:%M:%S' if seconds else '%H:%M')}")


def short_date(value: date | None) -> str:
    return value.strftime('%d/%m/%Y') if value else ''


def scan_time(value: datetime | None) -> str:
    """Colonne « Dernier scan » : « 11/12/2026 15:45:12 »."""
    return timezone.localtime(value).strftime('%d/%m/%Y %H:%M:%S') if value else '—'


def duration(delta: timedelta) -> str:
    """« 45 s », « 12 min 05 s », « 1 h 07 min »."""
    total = max(0, int(delta.total_seconds()))
    hours, rest = divmod(total, 3600)
    minutes, seconds = divmod(rest, 60)
    if hours:
        return f'{hours} h {minutes:02d} min'
    if minutes:
        return f'{minutes} min {seconds:02d} s'
    return f'{seconds} s'


def join_names(names: list[str]) -> str:
    """« A », « A et B », « A, B et C »."""
    if len(names) <= 1:
        return ''.join(names)
    return ', '.join(names[:-1]) + ' et ' + names[-1]


def guest_card(info: dict[str, Any]) -> Secouristes | None:
    if not info['verified']:
        return None
    from .models import Secouristes
    return Secouristes.objects.filter(matricule=info['matricule'], guest=True).first()


def person(identity: str) -> tuple[str, str]:
    """(nom, precision) d'une identite stockee : (« Jeanne Dupont », « M001 »), invite (« Léa Martin », « carte
    INVK7Q2X, invité de l'antenne Lyon 3 »), nom declare (« Paul », « nom déclaré »)."""
    if not identity:
        return '?', ''
    info = parse_identity(identity)
    card = guest_card(info)
    if card is not None:
        antenna = f" de l'antenne {card.guest_antenna}" if card.guest_antenna else ''
        return f'{card.prenom} {card.nom}'.strip() or "Carte d'invité", f'carte {card.matricule}, invité{antenna}'
    if info['verified']:
        name = info['display_name']
        return (name, '') if name.endswith(')') else (name, info['matricule'])
    name = info['display_name']
    return (name, '') if name == ANONYMOUS_OPENING else (name, 'nom déclaré')


def who(identity: str) -> str:
    """Nom seul (« Jeanne Dupont ») : le matricule n'est donne qu'une fois, dans la liste des secouristes cites."""
    return person(identity)[0]


def cited(identity: str) -> str:
    """« Jeanne Dupont (M001) » : liste des secouristes cites en tete du rapport. Le rapport est un document certifie,
    imprime et signe, et fait exception a la regle des prenoms seuls du front web (choix d'Hippolyte)."""
    name, detail = person(identity)
    return f'{name} ({detail})' if detail else name


# ----------------------------------------------------------------------------------------------------------------------
# Etat a la date T

@dataclass
class Maintenance:
    """Revision ou desinfection d'un item ou d'un lot a la date du rapport."""
    kind: str                        # 'revision' ou 'disinfection' (MAINTENANCE)
    done: date | None = None         # derniere faite avant la date
    tasks: list[str] = field(default_factory=list)  # taches cochees a cette derniere (faite dans le logiciel)
    due: date | None = None          # prochaine echeance prevue a la date
    late: bool = False               # echeance passee a la date
    usable: bool = True              # en retard, l'item reste utilisable (ItemType.revision_usable...)

    @property
    def noun(self) -> str:
        return MAINTENANCE[self.kind]['noun']

    def text(self) -> str:
        """« Révision : dernière le 12/03/2026 (tâches cochées : a · b), prochaine le 12/03/2027 »."""
        parts = []
        if self.done is not None:
            parts.append(f'dernière le {short_date(self.done)}'
                         + (f" (tâches cochées : {' · '.join(self.tasks)})" if self.tasks else ''))
        else:
            parts.append('aucune enregistrée avant cette date')
        if self.due is not None:
            parts.append(f'prochaine le {short_date(self.due)}' + (' : EN RETARD à cette date' if self.late else ''))
        return f"{MAINTENANCE[self.kind]['label']} : " + ', '.join(parts)


@dataclass
class ItemState:
    item: Items
    expired: bool = False    # perime a la date du rapport
    disabled: bool = False   # desactive (rappel produit...) a la date du rapport
    extra: bool = False      # en plus des attendus (au-dela de la quantite attendue, ou type non attendu)
    last_scan: datetime | None = None             # dernier scan avant la date du rapport
    tasks: list[str] = field(default_factory=list)  # taches cochees a son dernier scan en vérif
    # revision et desinfection a la date (materiel non consommable suivi) : derniere faite, prochaine echeance
    maintenance: list[Maintenance] = field(default_factory=list)

    @property
    def blocked(self) -> Maintenance | None:
        """Revision ou desinfection en retard qui rend l'item inutilisable (type sans « utilisable en retard »)."""
        return next((row for row in self.maintenance if row.late and not row.usable), None)

    @property
    def good(self) -> bool:
        return not (self.expired or self.disabled or self.blocked)


@dataclass
class LotState:
    lot: Lots
    depth: int
    requirements: list[Requirement]
    items: list[ItemState] = field(default_factory=list)
    last_verif: Verifs | None = None
    sealed: Operation | None = None                     # Operation SEAL en vigueur a T
    lacking: list[tuple[Requirement, int]] = field(default_factory=list)  # (attendu, manquants)
    # debut de la vérif en cascade dont la derniere vérif du lot est la fin (lot parent : debut de la 1re etape, celle
    # d'un de ses sous-lots), son debut a lui pour un lot sans sous-lot ; None : pas d'heure de debut enregistree
    verif_start: datetime | None = None
    disinfection: Maintenance | None = None             # desinfection du lot lui-meme (cellule du VPS...)
    content_change: Operation | None = None             # derniere modification du contenu attendu avant T
    changed_since: bool = False                         # contenu attendu modifie apres T

    @property
    def counted(self) -> bool:
        return bool(self.requirements)

    @property
    def complete(self) -> bool:
        return not self.lacking


def item_locations(lots: list[Lots], at: datetime) -> dict[str, tuple[Items, str]]:
    """{iid: (item, lot id a la date at)} des items qui etaient dans l'un des lots a cette date."""
    ids = {lot.id for lot in lots}
    touched = set(ItemMovement.objects.filter(at__gt=at, from_lot_id__in=ids).values_list('item_id', flat=True))
    touched |= set(ItemMovement.objects.filter(at__gt=at, to_lot_id__in=ids).values_list('item_id', flat=True))
    candidates = (Items.objects.select_related('pack__item_type')
                  .filter(added__lte=at)
                  .filter(location_id__in=ids) | Items.objects.select_related('pack__item_type')
                  .filter(added__lte=at, iid__in=touched))
    by_iid = {item.iid: item for item in candidates}
    first_after: dict[str, str | None] = {}
    for move in (ItemMovement.objects.filter(item_id__in=list(by_iid), at__gt=at)
                 .order_by('at', 'id').only('item_id', 'from_lot_id')):
        first_after.setdefault(move.item_id, move.from_lot_id)
    result: dict[str, tuple[Items, str]] = {}
    for iid, item in by_iid.items():
        location = first_after[iid] if iid in first_after else item.location_id
        if location in ids:
            result[iid] = (item, location)
    return result


def open_absences(iids: dict[str, tuple[Items, str]], at: datetime) -> set[str]:
    """Items notes absents a une verif avant at et pas encore retrouves a cette date."""
    return set(ItemMovement.objects.filter(item_id__in=list(iids), kind__in=ABSENCE_KINDS, at__lte=at)
               .exclude(cancelled__lte=at).values_list('item_id', flat=True))


# passages qui ne sont pas un scan de l'item (creation de l'etiquette, suppression ou decoupage depuis le poste)
NOT_SCANS = (SeenWhile.CREATE, SeenWhile.DELETE, SeenWhile.SPLIT)
# mouvements faits en scannant l'item (ajout a un lot, retrait vers le stock, sortie, liste de materiel)
SCAN_MOVES = (MovementKind.MOVE, MovementKind.OUT, MovementKind.RESTORED)


def last_scans(items: list[Items], at: datetime) -> tuple[dict[str, datetime], dict[str, list[str]]]:
    """({iid: dernier scan avant at}, {iid: taches cochees a sa derniere vérif avant at}).

    Le scan est l'heure enregistree pour l'item dans la vérif (VerifItem.scanned_at), a defaut l'heure de la vérif ;
    aussi les mouvements faits en le scannant, et son dernier passage (Items.last_seen) s'il est avant at."""
    iids = [item.iid for item in items]
    scans: dict[str, datetime] = {}
    tasks: dict[str, list[str]] = {}

    def seen(iid: str, moment: datetime | None) -> None:
        if moment is not None and moment <= at and (iid not in scans or moment > scans[iid]):
            scans[iid] = moment

    for entry in (VerifItem.objects.filter(item_id__in=iids, verif__datetime__lte=at,
                                           result__in=(VerifResult.PRESENT, VerifResult.REPLACED))
                  .select_related('verif').order_by('verif__datetime', 'id')):
        seen(entry.item_id, entry.scanned_at or entry.verif.datetime)
        tasks[entry.item_id] = list(entry.tasks or [])
    for iid, moment in ItemMovement.objects.filter(item_id__in=iids, at__lte=at, kind__in=SCAN_MOVES) \
            .exclude(by='').values_list('item_id', 'at'):
        seen(iid, moment)
    for item in items:
        if item.last_seen_while not in NOT_SCANS:
            seen(item.iid, item.last_seen)
    return scans, tasks


MAINTENANCE_OPERATIONS = {OperationKind.REVISION: 'revision', OperationKind.DISINFECTION: 'disinfection'}


def _due(value: Any) -> date | None:
    try:
        return date_type.fromisoformat(str(value)) if value else None
    except ValueError:
        return None


def item_maintenance(items: list[Items], at: datetime) -> dict[str, list[Maintenance]]:
    """{iid: [Maintenance]} des items suivis (revision, desinfection) a la date at.

    Derniere faite : ItemRevision avant at (faite dans le logiciel, avec ses taches, ou date saisie a la main).
    Prochaine echeance : celle du dernier enregistrement du journal avant at (revision faite, date saisie), a defaut
    celle de la derniere revision, a defaut la reception + la periodicite du type (premiere echeance)."""
    day = timezone.localdate(at)
    tracked = {item.iid: item for item in items
               if any(item.pack.item_type.has_maintenance(kind) for kind in MAINTENANCE)}
    if not tracked:
        return {}
    done: dict[tuple[str, str], ItemRevision] = {}
    for revision in ItemRevision.objects.filter(item_id__in=list(tracked), at__lte=at).order_by('at', 'id'):
        done[(revision.item_id, revision.kind)] = revision
    planned: dict[tuple[str, str], date] = {}
    for operation in (Operation.objects.filter(kind__in=list(MAINTENANCE_OPERATIONS), at__lte=at,
                                               cancelled_by__isnull=True, lot__isnull=True)
                      .order_by('at', 'id').only('kind', 'details')):
        due = _due(operation.details.get('next_due'))
        if due is None:
            continue
        for iid in operation.details.get('items') or []:
            if iid in tracked:
                planned[(iid, MAINTENANCE_OPERATIONS[OperationKind(operation.kind)])] = due
    result: dict[str, list[Maintenance]] = {}
    for iid, item in tracked.items():
        item_type = item.pack.item_type
        for kind, fields in MAINTENANCE.items():
            if not item_type.has_maintenance(kind):
                continue
            last = done.get((iid, kind))
            due = planned.get((iid, kind)) or (last.next_due if last is not None else None)
            if due is None and item.added <= at:
                due = add_months(timezone.localdate(item.added), getattr(item_type, fields['months']))
            result.setdefault(iid, []).append(Maintenance(
                kind, done=timezone.localdate(last.at) if last is not None else None,
                tasks=[str(task) for task in last.tasks or []] if last is not None and not last.manual else [],
                due=due, late=due is not None and due < day, usable=bool(getattr(item_type, fields['usable']))))
    return result


def lot_disinfection(lot: Lots, at: datetime) -> Maintenance | None:
    """Desinfection du lot lui-meme a la date at (journal : desinfections faites et dates saisies), None si le lot
    n'en a pas."""
    operations = list(Operation.objects.filter(kind=OperationKind.DISINFECTION, lot_id=lot.id, at__lte=at,
                                               cancelled_by__isnull=True).order_by('at', 'id'))
    if not operations and not lot.disinfection_months:
        return None
    due = next((value for value in (_due(op.details.get('next_due')) for op in reversed(operations)) if value), None)
    done_at = [op.at for op in operations if not op.details.get('manual')]
    if lot.disinfected_at is not None and lot.disinfected_at <= at:
        done_at.append(lot.disinfected_at)
    day = timezone.localdate(at)
    return Maintenance('disinfection', done=timezone.localdate(max(done_at)) if done_at else None, due=due,
                       late=due is not None and due < day)


def last_verifs(lots: list[Lots], at: datetime) -> dict[str | None, Verifs]:
    result: dict[str | None, Verifs] = {}
    for verif in Verifs.objects.filter(lot_id__in=[lot.id for lot in lots], datetime__lte=at).order_by('datetime', 'id'):
        result[verif.lot_id] = verif
    return result


def seals(lots: list[Lots], at: datetime) -> dict[str | None, Operation]:
    """{lot id: operation de scellage} des lots scelles a la date at."""
    last: dict[str | None, Operation] = {}
    for operation in (Operation.objects.filter(lot_id__in=[lot.id for lot in lots], at__lte=at,
                                               kind__in=(OperationKind.SEAL, OperationKind.UNSEAL))
                      .order_by('at', 'id')):
        last[operation.lot_id] = operation
    return {lot_id: operation for lot_id, operation in last.items() if operation.kind == OperationKind.SEAL}


# etapes d'une meme vérif en cascade : chacune a moins de CASCADE_GAP de la suivante (comme views.public, fun_stats)
CASCADE_GAP = timedelta(minutes=30)
CASCADE_MAX = timedelta(hours=12)  # au-dela : jamais la meme vérif


def cascade_start(verif: Verifs, sub_lots: list[str]) -> datetime | None:
    """Debut de la vérif en cascade qui finit par verif (lot parent valide apres ses sous-lots, ex : VPS > armoires >
    sacs) : en remontant le temps, les vérifs de ses sous-lots faites avant elle, chacune a moins de CASCADE_GAP de
    l'etape suivante, et le debut de la plus ancienne. None si la vérif n'a pas d'heure de debut (vérif d'avant le
    chronometrage)."""
    if verif.started is None:
        return None
    start = verif.started
    if not sub_lots:
        return start
    steps = (Verifs.objects.filter(lot_id__in=sub_lots, datetime__lte=verif.datetime,
                                   datetime__gte=verif.datetime - CASCADE_MAX)
             .exclude(id=verif.id).order_by('-datetime', '-id').only('datetime', 'started'))
    for step in steps:
        if start - step.datetime > CASCADE_GAP:
            break
        start = min(start, step.started or step.datetime)
    return start


def content_changes(lot: Lots) -> Q:
    """Operations du journal qui fixent le contenu attendu du lot : creation ou modification du contenu de son type,
    modification de son contenu propre."""
    return (Q(kind=OperationKind.CATALOG, details__type=lot.lot_type_id)
            & (Q(summary__startswith='contenu attendu modifié') | Q(summary__startswith='type de lot créé'))) \
        | Q(kind=OperationKind.LOT, lot_id=lot.id, summary__startswith='contenu propre du lot modifié')


def content_change(lot: Lots, at: datetime) -> tuple[Operation | None, bool]:
    """(derniere modification du contenu attendu avant at, modifie depuis)."""
    changes = Operation.objects.filter(content_changes(lot), cancelled_by__isnull=True)
    before = changes.filter(at__lte=at).order_by('-at', '-id').first()
    return before, changes.filter(at__gt=at).exists()


def mark_extra(state: LotState) -> None:
    """Items en plus des attendus : type non attendu, ou au-dela de la quantite attendue (les derniers dans l'ordre
    du tableau, peremption la plus lointaine). Les sous-types d'un attendu a unites ne sont jamais marques."""
    expected = {requirement.item_type_id: requirement.quantity for requirement in state.requirements}
    fills = {code for requirement in state.requirements for code in requirement.fills}
    groups = {requirement.item_type_id for requirement in state.requirements if requirement.fills}
    by_type: defaultdict[str, list[ItemState]] = defaultdict(list)
    for row in state.items:
        by_type[row.item.pack.item_type_id].append(row)
    for type_id, rows in by_type.items():
        if type_id in fills or type_id in groups:
            continue
        if type_id not in expected:
            for row in rows:
                row.extra = True
            continue
        good = sorted((row for row in rows if row.good), key=lambda row: (str(row.item.pack.peremption or '9999'),
                                                                          row.item.iid))
        for row in good[expected[type_id]:]:
            row.extra = True


def state_at(root: Lots, at: datetime) -> list[LotState]:
    """[LotState] du lot et de ses sous-lots (en cascade) a la date at."""
    lots = [lot for lot in root.descendants() if lot.created <= at or lot.id == root.id]
    kept = {root.id}
    lots = [lot for lot in lots if lot.id == root.id or (lot.parent_id in kept and not kept.add(lot.id))]
    day = timezone.localdate(at)
    located = item_locations(lots, at)
    absent = open_absences(located, at)
    verifs = last_verifs(lots, at)
    sealed = seals(lots, at)
    states: dict[str, LotState] = {}
    parent_of = {lot.id: lot.parent_id for lot in lots}
    below: defaultdict[str | None, list[str]] = defaultdict(list)  # lot -> tous ses sous-lots (en profondeur)
    for lot in lots:
        parent = parent_of.get(lot.id)
        seen: set[str] = set()
        while parent is not None and parent in parent_of and parent not in seen:
            seen.add(parent)
            below[parent].append(lot.id)
            parent = parent_of[parent]
    for lot in lots:
        change, since = content_change(lot, at)
        states[lot.id] = LotState(lot, lot.depth, lot_requirements(lot), last_verif=verifs.get(lot.id),
                                  sealed=sealed.get(lot.id), content_change=change, changed_since=since)
        states[lot.id].disinfection = lot_disinfection(lot, at)
        verif = verifs.get(lot.id)
        states[lot.id].verif_start = cascade_start(verif, below[lot.id]) if verif is not None else None
    present = [item for iid, (item, _) in located.items() if iid not in absent]
    scans, tasks = last_scans(present, at)
    maintenance = item_maintenance(present, at)
    for item in present:
        # (un item note absent a une verif avant T, pas encore retrouve, n'etait pas dans le lot : ecarte plus haut)
        item_tasks = tasks.get(item.iid) or []
        if not item_tasks and item.iid in tasks:
            # vérif d'avant l'enregistrement des taches : l'item n'entre dans la vérif qu'une fois toutes ses taches
            # cochees (front), celles de son type
            item_tasks = list(item.pack.item_type.tasks or [])
        states[located[item.iid][1]].items.append(ItemState(
            item, expired=item.is_expired(day),
            disabled=item.disabled and item.disabled_at is not None and item.disabled_at <= at,
            last_scan=scans.get(item.iid), tasks=item_tasks, maintenance=maintenance.get(item.iid, [])))
    for state in states.values():
        good: defaultdict[str, int] = defaultdict(int)
        for row in state.items:
            if row.good:
                good[row.item.pack.item_type_id] += 1
        state.lacking = []
        for requirement in state.requirements:
            # item a cocher : sans etiquette ni historique, il n'est pas suivi dans le rapport
            if not requirement.item_type.physical:
                continue
            # type a unites (200 mL de serum phy) : ses sous-types comptent leurs unites
            have = good[requirement.item_type_id] + sum(good[code] * units for code, units in requirement.fills.items())
            if have < requirement.quantity:
                state.lacking.append((requirement, requirement.quantity - have))
        mark_extra(state)
    return [states[lot.id] for lot in lots]


# ----------------------------------------------------------------------------------------------------------------------
# PDF

COLUMNS = [  # (titre, largeur fixe, ou None : la place restante)
    ('Item', None), ('iid', 106), ('Péremption', 48), ('Dernier scan', 84), ('État à cette date', 86),
]
STATE_COLUMN = 4


def missing_remark(requirement: Requirement, count: int) -> str:
    """Colonne « État » d'une ligne de type manquant : « manquant », « 2 manquants », « manquant : 50 mL »."""
    if requirement.unit:
        return f'manquant : {count} {requirement.unit}'
    return 'manquant' if count == 1 else f'{count} manquants'


def item_remark(row: ItemState) -> tuple[str, Shade]:
    """(colonne « État », couleur) d'un item."""
    if row.disabled:
        return 'désactivé', RED
    if row.expired:
        return 'périmé', RED
    if row.blocked is not None:
        return f'{row.blocked.noun} en retard', RED
    if row.extra:
        return 'présent, en plus', BLUE
    return 'présent', GREEN


def explanation(antenna: str) -> str:
    return (f"QRProtec est l'outil de suivi du matériel de secours en service à l'antenne {antenna or DEFAULT_ANTENNA}."
            " Chaque item porte une étiquette à code unique, reliée "
            "dans la base à son type et à sa date de péremption. Lors d'une vérif, le secouriste identifié scanne une "
            "à une les étiquettes des items qu'il a en main ; un item n'est compté présent que si son étiquette a été "
            "lue. Un item scanné a donc été vu et manipulé par un secouriste à l'heure indiquée dans la colonne "
            "« Dernier scan ». L'état à la date demandée est reconstitué à partir de l'historique des vérifs et des "
            "mouvements d'items.")


class ReportWriter:
    def __init__(self, root: Lots, at: datetime, editor: str, antenna: str, edited: datetime | None = None) -> None:
        """editor : identite stockee de l'admin qui edite le rapport."""
        self.root = root
        self.at = at
        self.editor = editor
        self.antenna = ' '.join(str(antenna or '').split())
        self.edited = timezone.localtime(edited or timezone.now())
        self.states = state_at(root, at)
        self.pages: list[Page] = []
        self.page: Page = Page()
        self.y: float = 0

    # -- personnes citees ----------------------------------------------------------------------------------------------

    def people(self) -> list[str]:
        """Identites citees dans le rapport, dans l'ordre d'apparition (editeur du rapport en dernier)."""
        found: list[str] = []
        for state in self.states:
            verif = state.last_verif
            if verif is not None:
                found += [verif.by, *verif.participants]
            if state.sealed is not None:
                found.append(state.sealed.by)
            if state.content_change is not None:
                found.append(state.content_change.by)
        found.append(self.editor)
        return [identity for identity in dict.fromkeys(found) if identity]

    # -- mise en page --------------------------------------------------------------------------------------------------

    def new_page(self) -> None:
        self.page = Page()
        self.pages.append(self.page)
        self.page.text(MARGIN, MARGIN + 8, "RAPPORT D'ÉTAT DE LOT", size=8, bold=True, grey=NAVY)
        self.page.text(PAGE_WIDTH - MARGIN, MARGIN + 8, 'État au ' + long_datetime(self.at), size=8, grey=GREY_TEXT,
                       align='right')
        self.page.line(MARGIN, MARGIN + 13, PAGE_WIDTH - MARGIN, MARGIN + 13, line_width=1.2, grey=PC_ORANGE)
        self.y = MARGIN + 24

    def need(self, height: float) -> bool:
        if not self.pages or self.y + height > BODY_BOTTOM:
            self.new_page()
            return True
        return False

    def text(self, x: float, text: str, size: float = 10, bold: bool = False, grey: Shade = 0.0,
             width: float | None = None, step: float | None = None) -> None:
        width = width or (PAGE_WIDTH - MARGIN - x)
        self.page.text(x, self.y + size, fit(text, width, size, bold), size=size, bold=bold, grey=grey)
        self.y += step if step is not None else size + 4

    def paragraph(self, x: float, text: str, size: float, step: float, bold: bool = False, grey: Shade = 0.0,
                  max_lines: int = 12) -> None:
        """Texte sur plusieurs lignes, passe a la page suivante si besoin."""
        for line in wrap(text, PAGE_WIDTH - MARGIN - x, size, bold, max_lines=max_lines):
            self.need(step)
            self.text(x, line, size=size, bold=bold, grey=grey, step=step)

    # -- contenu ----------------------------------------------------------------------------------------------------

    def title(self) -> None:
        self.new_page()
        root = self.root
        self.text(MARGIN, root.name, size=20, bold=True, grey=NAVY, step=26)
        details = [root.lot_type.name, root.id]
        ancestors = root.ancestors()
        if ancestors:
            details.insert(0, 'dans ' + ' › '.join(parent.name for parent in reversed(ancestors)))
        self.text(MARGIN, ' · '.join(details), size=10, grey=GREY_TEXT, step=20)
        counted = [state for state in self.states if state.counted]
        incomplete = [state for state in counted if not state.complete]
        sub_lots = len(self.states) - 1
        if not counted:
            summary, color, band = 'Aucun item attendu dans ce lot.', 0.0, GREY_BAND
        elif incomplete:
            summary, color, band = ('Incomplet à cette date : ' + ', '.join(state.lot.name for state in incomplete),
                                    RED, RED_BG)
        else:
            summary, color, band = ('Complet à cette date' + (' (tous les sous-lots)' if sub_lots else ''),
                                    GREEN, GREEN_BG)
        top = self.y
        self.page.rect(MARGIN, top, CONTENT_WIDTH, 62, fill=band, stroke=False)
        self.page.rect(MARGIN, top, 4, 62, fill=color, stroke=False)
        self.y += 6
        self.text(MARGIN + 12, 'État du lot au ' + long_datetime(self.at), size=14, bold=True, step=20)
        self.text(MARGIN + 12, summary, size=12, bold=True, grey=color, step=18)
        self.text(MARGIN + 12, f'Rapport édité le {long_datetime(self.edited)} par {who(self.editor)}', size=9.5,
                  step=18)
        self.y = top + 62 + 8
        if sub_lots:
            self.text(MARGIN, f"Sous-lots en cascade : {sub_lots}, détaillés à la suite du lot.", size=9.5,
                      grey=GREY_TEXT, step=14)

    def people_section(self) -> None:
        people = self.people()
        self.need(16 + 12 * 2)
        self.y += 4
        self.text(MARGIN, 'Secouristes cités dans ce rapport', size=10.5, bold=True, grey=NAVY, step=14)
        self.paragraph(MARGIN, join_names([cited(identity) for identity in people]) + '.', 9, 12)
        self.text(MARGIN, 'Dans la suite du rapport, les secouristes ne sont désignés que par leur nom.', size=7.5,
                  grey=GREY_TEXT, step=12)

    def explanation_section(self) -> None:
        lines = wrap(explanation(self.antenna), CONTENT_WIDTH - 20, 8, max_lines=12)
        notes = wrap("Contenu attendu et sous-lots tels qu'ils sont définis aujourd'hui (date de leur dernière "
                     'modification indiquée pour chaque lot). Un item « en plus » dépasse le contenu attendu.',
                     CONTENT_WIDTH - 20, 7.5, max_lines=3)
        height = 18 + 11 * len(lines) + 4 + 10 * len(notes) + 6
        self.need(height + 6)
        self.y += 4
        top = self.y
        self.page.rect(MARGIN, top, CONTENT_WIDTH, height, fill=NAVY_BG, stroke=False)
        self.y += 5
        self.text(MARGIN + 10, 'Comment ce rapport est établi', size=9.5, bold=True, grey=NAVY, step=14)
        for line in lines:
            self.text(MARGIN + 10, line, size=8, step=11)
        self.y += 4
        for line in notes:
            self.text(MARGIN + 10, line, size=7.5, grey=GREY_TEXT, step=10)
        self.y = top + height + 4

    def verif_line(self, verif: Verifs, started: datetime | None) -> str:
        """started : debut de la vérif, celui de la 1re etape pour un lot parent valide en cascade (cascade_start)."""
        others = [who(identity) for identity in verif.participants if identity and identity != verif.by]
        names = join_names([who(verif.by), *others])
        if started is not None and started < verif.datetime:
            start, end = timezone.localtime(started), timezone.localtime(verif.datetime)
            whole = verif.started is not None and started < verif.started  # sous-lots compris
            when = (f'{long_datetime(started, seconds=True)}, terminée '
                    + (f"à {end.strftime('%H:%M:%S')}" if start.date() == end.date()
                       else f'le {long_datetime(verif.datetime, seconds=True)}')
                    + f' (durée {duration(verif.datetime - started)}'
                    + (', sous-lots compris)' if whole else ')'))
            line = f'Dernière vérif : commencée le {when}, par {names}'
        else:
            line = f'Dernière vérif : {long_datetime(verif.datetime, seconds=True)}, par {names}'
        line += (' · complète' if verif.complete else ' · incomplète') \
            + f' ({verif.present_count} présent' + ('s' if verif.present_count > 1 else '') \
            + f', {verif.missing_count} manquant' + ('s' if verif.missing_count > 1 else '') + ')'
        if verif.sent_late():  # faite hors connexion : datee aux scans, heure de reception a cote (antidatage)
            line += f' · faite hors connexion, reçue par le serveur le {long_datetime(verif.sent_late())}'
        return line

    def content_line(self, state: LotState) -> str:
        change = state.content_change
        lot = state.lot
        if change is not None:
            line = f'Contenu attendu modifié pour la dernière fois le {long_datetime(change.at)} par {who(change.by)}'
        elif lot.lot_type.created <= self.at:
            line = f'Contenu attendu défini à la création du type « {lot.lot_type.name} » le ' \
                   f'{long_datetime(lot.lot_type.created)}'
        else:
            line = 'Contenu attendu : pas de modification enregistrée avant cette date'
        if state.changed_since:
            line += " ; modifié depuis : le tableau suit le contenu attendu d'aujourd'hui"
        return line

    def lot_section(self, state: LotState) -> None:
        lot = state.lot
        x = MARGIN + state.depth * INDENT
        width = PAGE_WIDTH - MARGIN - x
        self.need(22 + 4 * 13 + 2 * ROW)
        self.y += 6
        if state.counted:
            verdict, color, band = ('Complet', GREEN, GREEN_BG) if state.complete else ('Incomplet', RED, RED_BG)
        else:
            grouping = any(other.lot.parent_id == lot.id for other in self.states)
            verdict, color, band = ('Regroupement' if grouping else "Rien d'attendu"), 0.0, GREY_BAND
        self.page.rect(x, self.y, width, 20, fill=band, stroke=False)
        self.page.rect(x, self.y, 3, 20, fill=color, stroke=False)
        name = ('Sous-lot · ' if state.depth else '') + lot.name
        self.page.text(x + 8, self.y + 14, fit(name, width - 100, 12, True), size=12, bold=True, grey=NAVY)
        self.page.text(PAGE_WIDTH - MARGIN - 6, self.y + 14, verdict, size=10.5, bold=True, grey=color, align='right')
        self.y += 26
        good = sum(1 for row in state.items if row.good)
        extra = sum(1 for row in state.items if row.extra)
        children = sum(1 for other in self.states if other.lot.parent_id == lot.id)
        if not state.counted and not state.items:
            # lot de regroupement : ses sous-lots suivent, rien a verifier en lui-meme
            what = f'regroupement de {children} sous-lot' + ('s' if children > 1 else '') if children else 'aucun item'
        else:
            what = f'{good} item' + ('s' if good > 1 else '') + ' valable' + ('s' if good > 1 else '') + ' à cette date'
            if extra:
                what += f', dont {extra} en plus des attendus'
        self.text(x, f'{lot.lot_type.name} · {lot.id} · {what}', size=9, grey=GREY_TEXT, step=13)
        verif = state.last_verif
        if verif is not None:  # noms complets : la ligne peut deborder, elle passe a la ligne au lieu d'etre coupee
            self.paragraph(x, self.verif_line(verif, state.verif_start), 9, 12, max_lines=4)
        elif state.counted or state.items:
            self.text(x, 'Dernière vérif : aucune avant cette date', size=9, step=12)
        if state.sealed is not None:
            seal = state.sealed
            number = seal.details.get('seal_number') or ''
            self.paragraph(x, f"Scellé{' n°' + number if number else ''} posé le {long_datetime(seal.at)} par "
                              f'{who(seal.by)}, intact à cette date', 9, 12, max_lines=2)
        if state.disinfection is not None:
            disinfection = state.disinfection
            self.paragraph(x, disinfection.text().replace('Désinfection', 'Désinfection du lot', 1), 9, 12,
                           grey=RED if disinfection.late else 0.0, max_lines=2)
        if state.counted:
            self.paragraph(x, self.content_line(state), 8, 11, grey=GREY_TEXT, max_lines=2)
        self.items_table(state, x, width)

    def items_table(self, state: LotState, x: float, width: float) -> None:
        if not state.items and not state.lacking:
            if state.counted:
                self.need(14)
                self.text(x, 'Aucun item dans ce lot à cette date.', size=9, grey=GREY_TEXT, step=14)
            return
        fixed = sum(column_width for _, column_width in COLUMNS if column_width)
        widths = [column_width or width - fixed for _, column_width in COLUMNS]
        places = {requirement.item_type_id: requirement.location for requirement in state.requirements}
        order = {requirement.item_type_id: index for index, requirement in enumerate(state.requirements)}
        for requirement in state.requirements:  # sous-types d'un attendu a unites : a sa place
            for code in requirement.fills:
                places.setdefault(code, requirement.location)
                order.setdefault(code, order[requirement.item_type_id])
        # (type, cle de tri, [valeurs des colonnes], couleur de l'etat, item) : items presents a cette date, puis une
        # ligne par type manquant
        rows: list[tuple[Any, tuple[Any, ...], list[str], Shade, ItemState | None]] = []
        for row in state.items:
            item_type = row.item.pack.item_type
            remark, color = item_remark(row)
            rows.append((item_type, (order.get(item_type.pk, len(order)), name_key(item_type.name), 0,
                                     str(row.item.pack.peremption or '9999'), row.item.iid),
                         [item_type.name, row.item.iid, short_date(row.item.pack.peremption) or '—',
                          scan_time(row.last_scan), remark], color, row))
        for requirement, count in state.lacking:
            rows.append((requirement.item_type,
                         (order[requirement.item_type_id], name_key(requirement.item_type.name), 1, '', ''),
                         [requirement.item_type.name, '—', '—', '—', missing_remark(requirement, count)], RED, None))
        rows.sort(key=lambda row: row[1])

        def header() -> None:
            self.y += 4
            self.page.rect(x, self.y, width, 12, fill=NAVY_BG, stroke=False)
            column_x = x
            for (title, _), column_width in zip(COLUMNS, widths):
                self.page.text(column_x + 2, self.y + 8.5, title, size=7, bold=True, grey=NAVY)
                column_x += column_width
            self.y += 12
            self.page.line(x, self.y, x + width, self.y, line_width=0.5, grey=NAVY)

        self.need(4 + 12 + ROW * 2)
        header()
        location = None
        for item_type, _, values, color, item_row in rows:
            type_id = item_type.pk
            place = (places.get(type_id) or NO_LOCATION) if type_id in places else OUT_OF_CONTENT
            band = place != location and (any(places.values()) or place == OUT_OF_CONTENT)
            task_lines = self.task_lines(item_row, widths[0] - 8 + sum(widths[1:]))
            if self.need(ROW + (ROW if band else 0) + TASK_ROW * len(task_lines)):
                header()
            if band:
                self.page.text(x + 2, self.y + 8.5, fit(place, width - 4, 7.5, True), size=7.5, bold=True,
                               grey=BLUE if place == OUT_OF_CONTENT else NAVY)
                self.y += ROW
            location = place
            column_x = x
            for index, (value, column_width) in enumerate(zip(values, widths)):
                state_column = index == STATE_COLUMN
                bold = state_column and color not in (GREEN,)
                self.page.text(column_x + 2, self.y + 8, fit(value, column_width - 5, TABLE_SIZE, bold),
                               size=TABLE_SIZE, bold=bold, grey=color if state_column else 0.0)
                column_x += column_width
            self.y += ROW
            for line, shade in task_lines:
                self.page.text(x + 10, self.y + 6, line, size=TASK_SIZE, grey=shade)
                self.y += TASK_ROW
            self.page.line(x, self.y - 2, x + width, self.y - 2, line_width=0.25, grey=0.8)

    def task_lines(self, row: ItemState | None, width: float) -> list[tuple[str, Shade]]:
        """Lignes sous un item : taches cochees a son dernier scan en vérif, puis sa revision et sa desinfection a la
        date (derniere faite, taches cochees, prochaine echeance ; en rouge si en retard)."""
        if row is None:
            return []
        lines: list[tuple[str, Shade]] = []
        if row.tasks:
            lines += [(line, GREY_TEXT) for line in wrap('Tâches cochées au dernier scan : ' + ' · '.join(row.tasks),
                                                         width, TASK_SIZE, max_lines=3)]
        for maintenance in row.maintenance:
            lines += [(line, RED if maintenance.late else GREY_TEXT)
                      for line in wrap(maintenance.text(), width, TASK_SIZE, max_lines=3)]
        return lines

    def certification(self) -> None:
        self.need(SIGN_HEIGHT + 20)
        self.y += 16
        top = self.y
        self.page.rect(MARGIN, top, CONTENT_WIDTH, SIGN_HEIGHT, line_width=1, stroke_shade=NAVY)
        label = 'Certifié par le TECH' + (f' {self.antenna}' if self.antenna else '')
        for index, line in enumerate(wrap(label, CONTENT_WIDTH - 20, 12, True, max_lines=2)):
            self.page.text(MARGIN + 10, top + 18 + index * 15, line, size=12, bold=True, grey=NAVY)
        half = MARGIN + CONTENT_WIDTH * 0.45
        self.page.text(MARGIN + 10, top + 58, 'Nom :', size=10)
        self.page.line(MARGIN + 46, top + 60, half - 14, top + 60, line_width=0.4, grey=0.5)
        self.page.text(MARGIN + 10, top + 88, 'Date :', size=10)
        self.page.line(MARGIN + 46, top + 90, half - 14, top + 90, line_width=0.4, grey=0.5)
        self.page.text(half, top + 44, 'Signature :', size=10)
        self.page.rect(half, top + 50, PAGE_WIDTH - MARGIN - 10 - half, SIGN_HEIGHT - 60, line_width=0.4, dash=(2, 2))
        self.y = top + SIGN_HEIGHT

    def write(self) -> bytes:
        self.title()
        self.people_section()
        self.explanation_section()
        for state in self.states:
            self.lot_section(state)
        self.certification()
        total = len(self.pages)
        footer = (f"QRProtec · {self.root.name} au {timezone.localtime(self.at).strftime('%d/%m/%Y %H:%M')} · "
                  f"rapport édité le {self.edited.strftime('%d/%m/%Y à %H:%M')} par {who(self.editor)}")
        for number, page in enumerate(self.pages, start=1):
            page.line(MARGIN, FOOTER_Y - 10, PAGE_WIDTH - MARGIN, FOOTER_Y - 10, line_width=0.4, grey=0.6)
            page.text(MARGIN, FOOTER_Y, fit(footer, CONTENT_WIDTH - 70, 8), size=8, grey=GREY_TEXT)
            page.text(PAGE_WIDTH - MARGIN, FOOTER_Y, f'page {number}/{total}', size=8, grey=GREY_TEXT, align='right')
        return build_pdf(self.pages, title=f"Rapport d'état - {self.root.name}")


def lot_report_pdf(root: Lots, at: datetime, editor: str, antenna: str, edited: datetime | None = None) -> bytes:
    """editor : identite stockee de l'admin qui edite le rapport (« M:P001 »)."""
    return ReportWriter(root, at, editor, antenna, edited).write()
