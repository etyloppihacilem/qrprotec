# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          services.py
#       -\-    _|__
#        |\___/  . \        Created on 29 Sep. 2026 at 14:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Logique metier de l'inventaire : verifs, mouvements d'items, etat des stocks."""

from collections import defaultdict
from dataclasses import dataclass, field
from datetime import timedelta

from django.db import transaction
from django.db.models import Count, F, Q
from django.utils import timezone

from . import journal, movements, notifications, webpush
from .idendity import ANONYMOUS_OPENING, SOURCE_DECLARED, parse_identity, short_identity_name
from .models import (
    ItemMovement, ItemRevision, ItemStatus, Items, ItemsPacks, ItemType, LotContent, LotRequirements, Lots, LotType, LotTypeSlot, MovementKind,
    OperationKind, SlotMode,
    ProblemReport, ReportStatus, STOCK_LOT_TYPE, SeenWhile, ServerSettings, VerifItem, Operation, VerifResult,
    Verifs, add_months, generate_key, name_key,
    qrprotec_setting, revision_blocked_q,
)


def restock_credit(lot, settings_row=None):
    """Credit de confiance du reassort (reglage restock_verif_recommended decoche) : {'outings_left', 'until'} tant
    que le lot n'a pas eu plus de restock_credit_outings sorties ni plus de restock_credit_days jours depuis sa
    derniere verif complete, sinon None. None aussi quand le reglage est coche (tout reassort recommande la verif),
    pour un rangement du stock ou un lot jamais verifie."""
    settings_row = settings_row or ServerSettings.get()
    if settings_row.restock_verif_recommended or lot.lot_type.storage or not lot.last_verif:
        return None
    until = lot.last_verif + timedelta(days=settings_row.restock_credit_days)
    left = settings_row.restock_credit_outings - lot.outings
    if left < 0 or timezone.now() > until:
        return None
    return {'outings_left': left, 'until': until}


def apply_restock(lot, identity, now, count):
    """Reassort de `count` items dans `lot` sans verif complete. Reglage du serveur coche (defaut) : verif
    recommandee pour les suivants dans tous les cas. Decoche : sous le credit de confiance (restock_credit), le lot
    revient au vert (« réassorti »), meme rentre d'une liste ou marque utilise ; credit epuise : verif recommandee.
    Un scelle ouvert reste recommande. Rien pour un rangement du stock. Renvoie les champs modifies."""
    if lot.lot_type.storage:
        return []
    lot.restocked = now
    lot.restocked_by = identity
    fields = ['restocked', 'restocked_by']
    if restock_credit(lot) is None:
        # la prochaine personne devra verifier tout le lot
        lot.verif_recommended = True
        lot.restocked_count += count
        return fields + ['verif_recommended', 'restocked_count']
    if lot.verif_recommended and (lot.back_from_list or lot.marked_used or lot.restocked_count):
        lot.verif_recommended = False
        lot.back_from_list = ''
        lot.marked_used = None
        lot.marked_used_by = ''
        lot.restocked_count = 0
        fields += ['verif_recommended', 'back_from_list', 'marked_used', 'marked_used_by', 'restocked_count']
    return fields


def count_outing(lots):
    """Une sortie de plus (liste de depart validee, lot marque utilise) pour le credit de confiance du reassort."""
    ids = [lot.id for lot in lots if not lot.is_sealed and not lot.lot_type.storage]
    if ids:
        Lots.objects.filter(id__in=ids).update(outings=F('outings') + 1)


def display_name(identity):
    return parse_identity(identity)['display_name'] if identity else '?'


def _items_queryset():
    return Items.objects.select_related('pack__item_type', 'location__lot_type')


def save_items(items, fields):
    """Enregistre `fields` de chaque item, comme Items.objects.bulk_update, en un UPDATE par groupe d'items qui
    prennent les memes valeurs. bulk_update ecrit un CASE WHEN par champ et par item : pour une verif de 30 items, sa
    construction coutait plus cher que la verif elle-meme (processeur de portable, Python sur un coeur). Les items
    d'une verif partagent presque tous les memes valeurs (statut, lot, date et auteur du passage) : quelques UPDATE
    courts. Pas de signaux, comme bulk_update."""
    attnames = [Items._meta.get_field(field).attname for field in fields]
    groups = defaultdict(list)
    for item in items:
        groups[tuple(getattr(item, name) for name in attnames)].append(item.pk)
    for values, pks in groups.items():
        for start in range(0, len(pks), 500):  # limite de variables d'une requete SQLite
            Items.objects.filter(pk__in=pks[start:start + 500]).update(**dict(zip(attnames, values)))


def _location_filter(lot):
    return Q(location=lot) if lot is not None else Q(location__isnull=True)


def in_stock_q(prefix=''):
    """Items en stock : hors de tout lot, ou ranges dans un rangement du stock (armoire, tiroir...)."""
    return Q(**{f'{prefix}location__isnull': True}) | Q(**{f'{prefix}location__lot_type__storage': True})


def last_storage(item_type):
    """Dernier rangement du stock (actif) ou des items de ce type ont ete ranges : reception directement dans le
    rangement, ou items deplaces dedans ensuite. None si ce type n'a jamais ete range nulle part."""
    places = Lots.objects.filter(lot_type__storage=True, active=True).exclude(lot_type_id=STOCK_LOT_TYPE)
    received = (Items.objects.filter(pack__item_type=item_type, location__in=places)
                .order_by('-added').values_list('added', 'location_id').first())
    moved = (ItemMovement.objects.filter(item_type=item_type, kind=MovementKind.MOVE, to_lot__in=places)
             .order_by('-at').values_list('at', 'to_lot_id').first())
    latest = max((row for row in (received, moved) if row), default=None, key=lambda row: row[0])
    return latest[1] if latest else None


def storage_of_origin(iids):
    """Rangement du stock d'ou chaque item est sorti (sa derniere sortie), s'il est toujours actif : {iid: lot}, None
    pour un item sorti du stock sans rangement (ou d'un rangement supprime depuis). Au retour d'une liste, chaque item
    y est remis : on fait confiance aux secouristes pour le ranger a sa place."""
    origin = {}
    for move in (ItemMovement.objects.select_related('from_lot__lot_type').filter(item_id__in=list(iids),
                                                                                 kind=MovementKind.OUT)
                 .order_by('at', 'id')):
        lot = move.from_lot
        origin[move.item_id] = lot if lot is not None and lot.active and lot.lot_type.storage else None
    return {iid: origin.get(iid) for iid in iids}


def return_to_origin(iids, identity, context):
    """Remet des items rentres dans le rangement d'ou ils sont sortis (ou dans le stock sans rangement), voir
    storage_of_origin. Rend {iid: lot ou None} de ce qui a ete range."""
    by_place = defaultdict(list)
    places = {}
    for iid, lot in storage_of_origin(iids).items():
        by_place[lot.id if lot else None].append(iid)
        places[lot.id if lot else None] = lot
    done = {}
    for key, group in by_place.items():
        result = move_items(group, places[key], identity, context)
        done.update({iid: places[key] for iid in result['moved']})
    return done


def clean_location(location):
    return ' '.join(str(location or '').split())[:64]


@dataclass
class Requirement:
    """Exigence d'un lot : celle de son type, avec les items en plus du lot (extra) et son emplacement dans ce lot."""
    item_type: ItemType
    quantity: int
    location: str
    extra: int = 0
    # facultatif a la verif (voir LotRequirements.optional) : le lot se valide sans, ses items non scannes restent
    # ou ils etaient
    optional: bool = False
    # attendu d'un type a unites (ItemType.unit, ex : 200 mL de serum phy) : quantity est en unites, et chaque item
    # d'un sous-type en remplit fills[sous-type] (noms des sous-types dans names)
    fills: dict = field(default_factory=dict)
    names: dict = field(default_factory=dict)

    @property
    def unit(self):
        return self.item_type.unit

    @property
    def item_type_id(self):
        return self.item_type.pk

    @property
    def required(self):
        """Minimum qui retient le lot : 0 pour un attendu facultatif (quantity reste le nombre prevu)."""
        return 0 if self.optional else self.quantity


def unit_members(preload=None):
    """Sous-types des types a unites : {type a unites: {sous-type: (unites remplies, nom)}} (ItemType.group)."""
    if preload is not None:
        return preload.unit_members
    result = defaultdict(dict)
    for group_id, type_id, fills, name in (ItemType.objects.filter(group__isnull=False)
                                           .values_list('group_id', 'type', 'fills', 'name')):
        result[group_id][type_id] = (max(1, fills), name)
    return dict(result)


def with_units(rows, preload=None):
    """Complete les attendus d'un type a unites avec ses sous-types (Requirement.fills et names)."""
    if not any(row.item_type.unit for row in rows):
        return rows
    members = unit_members(preload)
    for row in rows:
        if row.item_type.unit:
            subtypes = members.get(row.item_type_id, {})
            row.fills = {code: value[0] for code, value in subtypes.items()}
            row.names = {code: value[1] for code, value in subtypes.items()}
    return rows


def lacking_text(row, times=' × '):
    """Ce qui manque sur une ligne de requirements_status : « 3 × Compresses », ou pour un type a unites
    « 50 mL de Sérum phy (2 × Sachet…) » avec le sous-type a prendre."""
    lacking = row['required'] - row['present']
    if not row.get('unit') or row.get('within'):
        return f"{lacking}{times}{row['type_name']}"
    text = f"{lacking} {row['unit']} de {row['type_name']}"
    suggest = row.get('suggest')
    return text + (f" ({suggest['count']} × {suggest['type_name']})" if suggest else '')


def row_weights(fills, required, type_id):
    """Attendus d'un lot que remplit un item de ce type et combien d'unites il y compte : [(attendu, unites)].
    `required` : {attendu: quantite} du lot, `fills` : {attendu a unites: {sous-type: unites}}. Un item compte pour
    son propre type (1) et pour le type a unites dont il est un sous-type (ses unites) : « 200 mL, dont 2 sachets »."""
    result = [(type_id, 1)] if type_id in required else []
    for row, members in fills.items():
        if row in required and type_id in members:
            result.append((row, members[type_id]))
    return result


def main_row(weights, type_id):
    """Attendu principal d'un item parmi ses row_weights : celui du type a unites s'il y en a un, sinon son type."""
    for row, units in weights:
        if row != type_id:
            return row, units
    return type_id, 1


def optional_lot(lot) -> bool:
    """Sous-lot facultatif a la verif de son lot parent (Lots.optional), un lot global ou un rangement ne l'est
    jamais."""
    return bool(lot.optional and lot.parent_id and not lot.lot_type.storage)


def hidden_in_verif(lot, core=None) -> bool:
    """Lot cache dans la hierarchie de la vérif en cascade (Lots.hidden_in_verif, ex : le B+ du VPS) : ses sous-lots
    y apparaissent comme s'ils etaient directement dans son parent. Seulement un sous-lot (pas un lot global) qui n'a
    ni contenu attendu ni item a lui : sinon on ne pourrait plus le verifier, il reste affiche. `core` : ligne deja
    calculee (serializers._lot_core) pour ne pas relire son contenu."""
    if not (lot.hidden_in_verif and lot.parent_id) or lot.lot_type.storage:
        return False
    # liste pas toute dans les sacs (unplaced_lines) : il reste affiche, rouge, pour qu'on voie le probleme
    if core is not None:
        return not core['requirements'] and not core['item_count'] and not core.get('unplaced')
    return (not lot_requirements(lot) and not lot.items.filter(status=ItemStatus.ACTIVE).exists()
            and not unplaced_lines(lot))


def virtual_lot(lot) -> bool:
    """Lot d'un referentiel (B+) : il n'existe pas physiquement, c'est l'ensemble de ses sacs. Pas d'etiquette a lui ;
    il n'est verifie qu'en verifiant tous ses sacs (verify_hidden_parents). Seulement s'il n'a ni contenu attendu
    ni item a lui."""
    return (lot.lot_type.referentiel and not lot.lot_type.storage and lot.children.filter(active=True).exists()
            and not lot_requirements(lot) and not lot.items.filter(status=ItemStatus.ACTIVE).exists())


def verify_hidden_parents(lots, identity, now):
    """Vérif en cascade : un lot cache (hidden_in_verif) ou un lot de referentiel (virtual_lot) n'a pas d'etape a lui, il est valide des que tous ses
    sous-lots l'ont ete depuis sa derniere verif (ex : les deux sacs du B+ du VPS). Complet si leurs dernieres
    verifs le sont. Remonte les lots caches imbriques."""
    done = []
    seen = set()
    pending = [lot.parent_id for lot in lots if lot.parent_id]
    while pending:
        parent_id = pending.pop()
        if parent_id in seen:
            continue
        seen.add(parent_id)
        parent = Lots.objects.select_related('lot_type').filter(id=parent_id, active=True).first()
        if parent is None or not (hidden_in_verif(parent) or virtual_lot(parent)):
            continue
        children = [child for child in parent.children.filter(active=True) if not optional_lot(child)]
        if not children or any(child.last_verif is None or (parent.last_verif and child.last_verif <= parent.last_verif)
                               for child in children):
            continue
        latest = {child.id: Verifs.objects.filter(lot=child).order_by('-datetime').first() for child in children}
        verif = Verifs.objects.create(lot=parent, datetime=now, by=identity,
                                      complete=all(row is not None and row.complete for row in latest.values()),
                                      lot_type_version=parent.lot_type.version, lot_version=parent.version)
        parent.last_verif = now
        parent.last_verif_by = identity
        parent.last_verif_while = SeenWhile.VERIF
        parent.verif_recommended = False
        parent.restocked_count = 0
        parent.outings = 0
        parent.back_from_list = ''
        parent.marked_used = None
        parent.marked_used_by = ''
        parent.save(update_fields=['last_verif', 'last_verif_by', 'last_verif_while', 'verif_recommended',
                                   'restocked_count', 'outings', 'back_from_list', 'marked_used',
                                   'marked_used_by'])
        journal.record(OperationKind.VERIF, identity,
                       ('complète' if verif.complete else 'incomplète') + ' : ses sous-lots sont vérifiés',
                       lot=parent, at=now, verif=verif.id, complete=verif.complete, hidden_in_verif=True)
        done.append((parent, verif))
        if parent.parent_id:
            pending.append(parent.parent_id)
    return done


def optional_in_lot(lot, item_type, requirement_optional=False) -> bool:
    """Un type d'item est facultatif dans un lot : marque facultatif dans le contenu du type de lot
    (requirement_optional, seulement du materiel non consommable sans taches), ou range dans un sous-lot facultatif
    (tout son contenu l'est)."""
    return (requirement_optional and item_type.can_be_optional) or optional_lot(lot)


def optional_types(lot, requirements=None, strict=False):
    """Types d'items facultatifs d'un lot (attendus marques facultatifs), et fonction item -> facultatif dans ce
    lot (attendu facultatif, ou tout item d'un sous-lot facultatif). strict : vérif complete demandee par un admin,
    rien n'est facultatif."""
    if strict:
        return set(), lambda item: False
    if requirements is None:
        requirements = lot_requirements(lot)
    types = {row.item_type_id for row in requirements if row.optional}
    whole = optional_lot(lot)

    def check(item):
        return whole or item.pack.item_type_id in types
    return types, check


def sort_requirements(requirements, locations=()):
    """Exigences regroupees par emplacement, dans l'ordre choisi par la TECH (locations), « sans emplacement » en
    dernier, et les types d'items par nom dans chaque emplacement."""
    order = {}
    for name in locations or []:
        order.setdefault(name_key(name), len(order))

    def key(requirement):
        place = name_key(requirement.location)
        return (place == '', order.get(place, len(order)), place, name_key(requirement.item_type.name),
                requirement.item_type_id)
    return sorted(requirements, key=key)


def ordered_requirements(lot_type, requirements=None):
    """Exigences d'un type de lot, dans l'ordre de ses emplacements (LotType.locations)."""
    if requirements is None:
        requirements = LotRequirements.objects.select_related('item_type').filter(lot_type=lot_type)
    return sort_requirements(requirements, lot_type.locations)


def lot_type_content_snapshot(lot_type):
    """Contenu attendu d'un type de lot, comparable avant/apres un enregistrement (exigences et ordre des
    emplacements)."""
    rows = sorted((row.item_type_id, row.quantity, row.location, row.optional)
                  for row in LotRequirements.objects.filter(lot_type=lot_type))
    return rows, list(lot_type.locations or [])


def lot_content_snapshot(lot):
    """Contenu propre d'un lot, comparable avant/apres un enregistrement."""
    rows = sorted((row.item_type_id, row.extra, row.removed, row.location) for row in LotContent.objects.filter(lot=lot))
    return rows, list(lot.locations or [])


def bump_lot_type_version(lot_type):
    """Contenu du type de lot modifie : nouvelle version seulement si une verif d'un lot de ce type a ete faite sur
    la version en cours ; sinon la version en cours est corrigee en place. Renvoie vrai si la version a change (a
    enregistrer ensuite)."""
    if not lot_type.type or not Verifs.objects.filter(lot__lot_type=lot_type,
                                                      lot_type_version=lot_type.version).exists():
        return False
    lot_type.version += 1
    lot_type.valid_version = lot_type.version
    return True


def bump_lot_version(lot):
    """Contenu propre du lot modifie : nouvelle version seulement si une verif de ce lot a ete faite sur la version
    en cours. Renvoie vrai si la version a change (a enregistrer ensuite)."""
    if not Verifs.objects.filter(lot=lot, lot_version=lot.version).exists():
        return False
    lot.version += 1
    return True


def lot_requirements(lot, preload=None, strict=False):
    """Contenu attendu d'un lot (Requirement) : celui de son type, plus son contenu propre (LotContent : items en
    plus, emplacements de ce lot), dans l'ordre des emplacements du lot (Lots.locations, sinon ceux du type).
    `preload` (LotPreload) : exigences et contenus deja lus pour une liste de lots. strict : vérif complete demandee
    par un admin, aucun attendu n'est facultatif."""
    if lot.lot_type.storage:
        return []  # un rangement du stock n'attend rien
    if carried_by_rangements(lot.lot_type, preload):
        return []  # referentiel reparti entre des rangements : le contenu est porte par ses sous-lots
    if preload is not None:
        own_rows, type_rows = preload.content[lot.id], preload.requirements[lot.lot_type_id]
    else:
        own_rows = LotContent.objects.select_related('item_type').filter(lot=lot).order_by('pk')
        type_rows = LotRequirements.objects.select_related('item_type').filter(lot_type_id=lot.lot_type_id).order_by('pk')
    own = {row.item_type_id: row for row in own_rows}
    rows = []
    for requirement in type_rows:
        custom = own.pop(requirement.item_type_id, None)
        location = requirement.location if custom is None or custom.location is None else custom.location
        extra = custom.extra if custom else 0
        quantity = requirement.quantity + extra - (custom.removed if custom else 0)
        if quantity <= 0:
            continue  # reparti dans un autre sac de son lot global (LotContent.removed)
        rows.append(Requirement(requirement.item_type, quantity, location, extra,
                                not strict and optional_in_lot(lot, requirement.item_type, requirement.optional)))
    for custom in own.values():
        if custom.extra > 0:
            rows.append(Requirement(custom.item_type, custom.extra, custom.location or '', custom.extra,
                                    not strict and optional_in_lot(lot, custom.item_type)))
    return with_units(sort_requirements(rows, lot_locations(lot)), preload)


def distribution_sacs(lot):
    """Sacs d'un lot global par referentiel : ses sous-lots actifs dont le type est un rangement (le sac de soin et
    le sac d'O2 du B+), {referentiel: [sacs]} dans l'ordre des noms."""
    groups = defaultdict(list)
    for child in (lot.children.select_related('lot_type__part_of').filter(active=True, lot_type__part_of__isnull=False)
                  .order_by('name', 'id')):
        groups[child.lot_type.part_of].append(child)
    return groups


def unplaced_lines(lot):
    """Lot d'un referentiel reparti entre ses sacs (le B+) : lignes de la liste que ses sacs ne placent pas en
    entier, attendues nulle part ([{type, type_name, missing}]). Le lot est incomplet tant qu'il y en a."""
    if not lot.lot_type.referentiel or lot.lot_type.storage or not lot.active:
        return []
    result = []
    for group in lot_distribution(lot):
        for line in group['lines']:
            if line['placed'] < line['required']:
                result.append({'type': line['type'], 'type_name': line['type_name'],
                               'missing': line['required'] - line['placed']})
    return result


def lot_distribution(lot):
    """Repartition du contenu d'un referentiel entre les sacs d'un lot global : pour chaque ligne du referentiel, la
    quantite et l'emplacement dans chaque sac (contenu attendu du sac : son type, puis ce que ce lot y change), et ce
    qui est place en tout. Le type de chaque rangement donne la repartition par defaut."""
    result = []
    for referentiel, sacs in sorted(distribution_sacs(lot).items(), key=lambda pair: pair[0].name.lower()):
        placed = {sac.id: {row.item_type_id: row for row in lot_requirements(sac, strict=True)} for sac in sacs}
        defaults = {}
        for row in LotRequirements.objects.filter(lot_type_id__in={sac.lot_type_id for sac in sacs}):
            defaults[(row.lot_type_id, row.item_type_id)] = row.location
        lines = []
        for requirement in ordered_requirements(referentiel):
            type_id = requirement.item_type_id
            places = []
            for sac in sacs:
                row = placed[sac.id].get(type_id)
                places.append({'lot': sac.id, 'quantity': row.quantity if row else 0,
                               'location': row.location if row else defaults.get((sac.lot_type_id, type_id), '')})
            lines.append({'type': type_id, 'type_name': requirement.item_type.name, 'required': requirement.quantity,
                          'placed': sum(place['quantity'] for place in places), 'places': places})
        result.append({
            'referentiel': referentiel.type, 'referentiel_name': referentiel.name,
            'sacs': [{'id': sac.id, 'name': sac.name, 'lot_type': sac.lot_type_id, 'lot_type_name': sac.lot_type.name,
                      'locations': list(dict.fromkeys([*lot_locations(sac), *sac.lot_type.locations]))}
                     for sac in sacs],
            'lines': lines,
        })
    return result


def carried_by_rangements(lot_type, preload=None) -> bool:
    """Un referentiel dont des rangements se partagent la liste (ex : le B+, reparti entre le sac de soin et le sac
    O2) : ses lots n'attendent rien eux-memes, leur contenu est porte par leurs sous-lots."""
    if not lot_type.referentiel:
        return False
    if preload is not None:
        return lot_type.type in preload.carried
    return LotType.objects.filter(part_of=lot_type).exists()


def share_packs(items):
    """Items (lus sans select_related) dont chaque pack et chaque type d'item sont lus une fois et partages :
    select_related('pack__item_type') construit un pack et un ItemType complet (en decodant ses champs JSON) pour
    chaque item, ce qui dominait le temps de l'« État des lots » (2 400 items pour 600 packs et 40 types). Les items
    ne sont que lus."""
    items = list(items)
    packs = ItemsPacks.objects.in_bulk({item.pack_id for item in items})
    types = ItemType.objects.in_bulk({pack.item_type_id for pack in packs.values()})
    for pack in packs.values():
        pack.item_type = types[pack.item_type_id]
    for item in items:
        item.pack = packs[item.pack_id]
    return items


class LotPreload:
    """Ce que l'etat d'une liste de lots demande, lu en quelques requetes pour tous les lots a la fois (pages
    « État des lots », Gestion de la TECH, liste des lots du poste) au lieu de cinq requetes par lot : items actifs
    par lot, exigences par type de lot, contenu propre par lot, et l'arborescence (parents, sous-lots actifs)."""

    def __init__(self, lots):
        ids = [lot.id for lot in lots]
        self.settings = ServerSettings.get()  # credit de confiance du reassort (restock_credit)
        self.items = defaultdict(list)
        for item in share_packs(Items.objects
                                .filter(location_id__in=ids, status=ItemStatus.ACTIVE).order_by('iid')):
            self.items[item.location_id].append(item)
        self.requirements = defaultdict(list)
        for row in (LotRequirements.objects.select_related('item_type')
                    .filter(lot_type_id__in={lot.lot_type_id for lot in lots}).order_by('pk')):
            self.requirements[row.lot_type_id].append(row)
        # signalements ouverts sur les lots eux-memes (ou un type d'item d'un lot) : lot non utilisable s'il y en a un
        # inutilisable (voir reports.lot_reports)
        self.reports = defaultdict(list)
        for report in (ProblemReport.objects.select_related('item_type')
                       .filter(lot_id__in=ids, item__isnull=True, sealed_pack__isnull=True)
                       .exclude(status=ReportStatus.RESOLVED).order_by('usable', 'created', 'id')):
            self.reports[report.lot_id].append(report)
        # referentiels repartis entre des rangements (voir carried_by_rangements)
        self.carried = set(LotType.objects.filter(part_of__isnull=False).values_list('part_of_id', flat=True))
        self.content = defaultdict(list)
        for row in LotContent.objects.select_related('item_type').filter(lot_id__in=ids).order_by('pk'):
            self.content[row.lot_id].append(row)
        # arborescence : tous les lots (un parent peut etre hors de la liste), quelques champs seulement
        self.tree = {lot.id: lot for lot in Lots.objects.only('id', 'name', 'parent_id', 'active', 'lot_type_id', 'hidden_in_verif',
                                                                  'assembled')}
        # lots exiges par type (voir fixed_slots_missing)
        self.slots = defaultdict(list)
        for slot in LotTypeSlot.objects.all():
            self.slots[slot.lot_type_id].append(slot)
        self.unit_members = unit_members()  # sous-types des types a unites (attentes en mL)
        self.children = defaultdict(list)
        for lot in self.tree.values():
            if lot.active and lot.parent_id is not None:
                self.children[lot.parent_id].append(lot)

    def ancestors(self, lot):
        """Comme Lots.ancestors : du parent direct au lot global."""
        chain = []
        seen = {lot.id}
        parent = self.tree.get(lot.parent_id)
        while parent is not None and parent.id not in seen:
            chain.append(parent)
            seen.add(parent.id)
            parent = self.tree.get(parent.parent_id)
        return chain


def assign_slots(slots, children):
    """Repartit les sous-lots actifs d'un lot entre les lots exiges de son type, dans l'ordre des places : une place
    fixe prend les sous-lots ranges pour de bon, une place a l'assemblage les sous-lots assembles, d'un type accepte.
    Renvoie [(place, [sous-lots])] et les sous-lots sans place (simples sous-lots, comme avant)."""
    left = sorted(children, key=lambda child: child.name.lower())
    result = []
    for slot in slots:
        assembly = slot.mode == SlotMode.ASSEMBLY
        taken = [child for child in left
                 if child.lot_type_id in slot.accepted and bool(child.assembled) == assembly][:slot.quantity]
        left = [child for child in left if child not in taken]
        result.append((slot, taken))
    return result, left


def fixed_slots_missing(lot, preload=None):
    """Places fixes non facultatives du type du lot qui ne sont pas toutes occupees : « Sac de soin » ou
    « Kit AES (3/5) ». Un lot auquel il en manque est incomplet."""
    slots = preload.slots.get(lot.lot_type_id, []) if preload is not None else list(lot.lot_type.slots.all())
    slots = [slot for slot in slots if slot.mode == SlotMode.FIXED and not slot.optional]
    if not slots:
        return []
    children = (preload.children.get(lot.id, []) if preload is not None
                else list(lot.children.filter(active=True).only('id', 'name', 'lot_type_id', 'assembled')))
    missing = []
    for slot, taken in assign_slots(slots, children)[0]:
        if len(taken) < slot.quantity:
            missing.append(slot.name if slot.quantity == 1 else f'{slot.name} ({len(taken)}/{slot.quantity})')
    return missing


def fixed_place(lot):
    """Le lot tient-il une place fixe dans son parent ? Un lot fixe n'est jamais propose ailleurs (assemblage,
    remplacement) : on ne part pas avec le sac du VPS pour un lot A."""
    if lot.parent_id is None or lot.assembled:
        return False
    parent = lot.parent
    slots = list(parent.lot_type.slots.filter(mode=SlotMode.FIXED))
    if not slots:
        return False
    children = list(parent.children.filter(active=True).only('id', 'name', 'lot_type_id', 'assembled'))
    return any(lot.id in {child.id for child in taken} for _, taken in assign_slots(slots, children)[0])


def slot_candidates(lot, slot):
    """Lots qu'on peut mettre dans une place du lot : pour une place a l'assemblage, les lots libres (sans parent)
    d'un type accepte ; pour une place fixe (remplacement), tous les lots d'un type accepte qui ne tiennent pas
    eux-memes une place fixe. Jamais le lot lui-meme, ses lots parents ou ses sous-lots."""
    tree = {other.id for other in lot.descendants()} | {other.id for other in lot.ancestors()} | {lot.id}
    queryset = (Lots.objects.select_related('lot_type', 'parent__lot_type')
                .filter(active=True, lot_type_id__in=slot.accepted).exclude(id__in=tree).order_by('name'))
    if slot.mode == SlotMode.ASSEMBLY:
        return list(queryset.filter(parent__isnull=True))
    return [other for other in queryset if not fixed_place(other)]


def coverage_lots(referentiel):
    """Lots qui doivent couvrir la liste d'un referentiel : ses lots (ex : B+ 1, B+ 2), et les lots d'un de ses
    rangements qui ne sont pas dans un lot du referentiel (ex : un kit range sans lot global)."""
    lots = list(Lots.objects.select_related('lot_type', 'parent').filter(
        Q(lot_type=referentiel) | Q(lot_type__part_of=referentiel), active=True).order_by('name', 'id'))
    result = []
    for lot in lots:
        if lot.lot_type_id != referentiel.type and any(
                parent.lot_type_id == referentiel.type for parent in lot.ancestors()):
            continue  # sac d'un B+ : couvert avec son B+
        result.append(lot)
    return result


def lot_coverage(lot, referentiel):
    """Compare la liste d'un referentiel au contenu attendu d'un lot et de ses sous-lots (ses sacs, rangements par
    emplacements) : pour chaque ligne du referentiel, la quantite placee et ou elle l'est, puis ce qui est en plus.
    Un sous-lot d'un autre referentiel (le B+ d'un VPS) n'est pas compte : il couvre sa propre liste."""
    placed = defaultdict(int)
    places = defaultdict(list)
    names = {}
    converted = {}  # sous-type -> type a unites dans lequel il est compte
    stack = [lot]
    while stack:
        node = stack.pop()
        if node.lot_type.storage:
            continue
        if node is not lot and (
                (node.lot_type.referentiel and node.lot_type_id != referentiel.type)
                or (node.lot_type.part_of_id not in (None, referentiel.type))):
            continue
        node_rows = lot_requirements(node, strict=True)
        node_types = {requirement.item_type_id for requirement in node_rows}
        for requirement in node_rows:
            type_id = requirement.item_type_id
            group = requirement.item_type.group
            if group is not None and group.unit and group.type not in node_types:
                # sous-type range sans attendu de son type a unites dans ce sac (8 sachets de 25 mL) : il couvre
                # la ligne du type a unites (200 mL de serum phy)
                converted[type_id] = group.type
                names[group.type] = group.name
                placed[group.type] += requirement.quantity * max(1, requirement.item_type.fills)
                places[group.type].append({'lot': node.id, 'lot_name': node.name, 'location': requirement.location,
                                           'quantity': requirement.quantity * max(1, requirement.item_type.fills)})
            names[type_id] = requirement.item_type.name
            placed[type_id] += requirement.quantity
            places[type_id].append({'lot': node.id, 'lot_name': node.name, 'location': requirement.location,
                                    'quantity': requirement.quantity})
        stack.extend(reversed(list(node.children.select_related('lot_type').filter(active=True).order_by(
            F('sort_order').asc(nulls_last=True), 'name'))))
    lines = []
    wanted = set()
    for requirement in ordered_requirements(referentiel):
        type_id = requirement.item_type_id
        wanted.add(type_id)
        lines.append({'type': type_id, 'type_name': requirement.item_type.name, 'required': requirement.quantity,
                      'placed': placed.get(type_id, 0), 'places': places.get(type_id, []),
                      'unit': requirement.item_type.unit})
    wanted_groups = {line['type'] for line in lines if line['unit']}
    extra = [{'type': type_id, 'type_name': names[type_id], 'required': 0, 'placed': placed[type_id],
              'places': places[type_id]}
             for type_id in sorted(set(placed) - wanted, key=lambda key: (name_key(names[key]), key))
             if converted.get(type_id) not in wanted_groups]
    missing = [line for line in lines if line['placed'] < line['required']]
    return {'id': lot.id, 'name': lot.name, 'type': lot.lot_type_id, 'type_name': lot.lot_type.name,
            'complete': not missing, 'missing': len(missing), 'lines': lines, 'extra': extra}


def lot_locations(lot):
    """Ordre des emplacements d'un lot : le sien s'il en a un, sinon celui de son type."""
    return list(lot.locations or lot.lot_type.locations or [])


def location_order(rows, order=(), known=()):
    """{cle: emplacement} des emplacements utilises par des exigences [{location}], dans l'ordre demande puis dans
    l'ordre d'apparition des nouveaux. Deux emplacements qui ne different que par la casse ou les accents sont le meme
    (l'orthographe deja connue, celle de known, est gardee)."""
    locations = [clean_location(row.get('location')) for row in rows]
    used = {name_key(location) for location in locations if location}
    spelling = {}
    for location in [*map(clean_location, known), *map(clean_location, order), *locations]:
        spelling.setdefault(name_key(location), location)
    result = {}
    for location in [*map(clean_location, order), *locations]:
        if name_key(location) in used:
            result.setdefault(name_key(location), spelling[name_key(location)])
    return result


def requirements_status(lot, today=None, items=None, preload=None, strict=False):
    """Compare le contenu d'un lot aux exigences de son type.

    Seuls les items vus lors de la derniere verif (ou ajoutes depuis) comptent comme presents : un item
    manque a la derniere verif est encore attendu mais n'est plus considere comme fiable. `items` : items actifs
    du lot deja lus (evite une requete), `preload` : voir LotPreload.
    """
    today = today or timezone.localdate()
    fresh = defaultdict(int)
    expired = defaultdict(int)
    unconfirmed = defaultdict(int)
    if items is None:
        items = _items_queryset().filter(location=lot, status=ItemStatus.ACTIVE)
    for item in items:
        if item.missed_verifs > 0:
            unconfirmed[item.pack.item_type_id] += 1
        elif item.needs_replacement(today):
            expired[item.pack.item_type_id] += 1
        else:
            fresh[item.pack.item_type_id] += 1
    rows = []
    unchecked = set(lot.unchecked or [])
    requirements = lot_requirements(lot, preload, strict)
    unit_rows = {row.item_type_id: row for row in requirements if row.fills}
    within = {code: row.item_type_id for row in unit_rows.values() for code in row.fills}

    def units(counts, requirement):
        """Items d'un attendu : son type, plus les unites de ses sous-types pour un type a unites."""
        return counts.get(requirement.item_type_id, 0) + sum(
            counts.get(code, 0) * value for code, value in requirement.fills.items())
    for requirement in requirements:
        type_id = requirement.item_type_id
        if not requirement.item_type.physical:
            # item a cocher : la a moins d'avoir ete laisse non coche a la derniere vérif
            rows.append({
                'type': type_id, 'type_name': requirement.item_type.name, 'location': requirement.location,
                'required': requirement.required, 'expected': requirement.quantity, 'optional': requirement.optional,
                'extra': requirement.extra, 'check': True, 'tasks': list(requirement.item_type.tasks or []),
                'present': 0 if type_id in unchecked else requirement.quantity, 'expired': 0, 'unconfirmed': 0,
            })
            continue
        rows.append({
            'type': type_id,
            'type_name': requirement.item_type.name,
            'location': requirement.location,
            # minimum qui retient le lot : 0 pour un attendu facultatif, expected est alors le nombre prevu
            'required': requirement.required,
            'expected': requirement.quantity,
            'optional': requirement.optional,
            'extra': requirement.extra,  # dont en plus du type de lot (contenu propre du lot)
            'present': units(fresh, requirement),
            'expired': units(expired, requirement),
            'unconfirmed': units(unconfirmed, requirement),
        })
        if requirement.fills:
            # type a unites : quantites en unites (unit), ce que remplit chaque sous-type, et le sous-type a proposer
            # pour ce qui manque (sous-type par defaut, sinon celui qui en remplit le plus)
            row = rows[-1]
            row['unit'] = requirement.unit
            row['fills'] = dict(requirement.fills)
            row['subtypes'] = [{'type': code, 'type_name': requirement.names.get(code, code), 'fills': value,
                                'present': fresh.get(code, 0)}
                               for code, value in sorted(requirement.fills.items(), key=lambda pair: (-pair[1], pair[0]))]
            lacking = row['required'] - row['present']
            if lacking > 0:
                default = requirement.item_type.default_subtype_id
                code = default if default in requirement.fills else row['subtypes'][0]['type']
                row['suggest'] = {'type': code, 'type_name': requirement.names.get(code, code),
                                  'count': -(-lacking // requirement.fills[code])}
        elif type_id in within:
            # sous-type d'un type a unites aussi attendu dans ce lot : minimum compris dedans (« dont 2 sachets »)
            parent = unit_rows[within[type_id]]
            rows[-1]['within'] = parent.item_type_id
            rows[-1]['within_name'] = parent.item_type.name
            rows[-1]['within_units'] = parent.fills[type_id]
            rows[-1]['unit'] = parent.unit
    return rows


def verif_lots(lots):
    """Lots couverts par une verif : chaque lot demande suivi de ses sous-lots actifs, sans doublon."""
    result = []
    seen = set()
    for lot in lots:
        for sub_lot in lot.descendants():
            if sub_lot.id not in seen:
                seen.add(sub_lot.id)
                result.append(sub_lot)
    return result


def _required_by_lot(group, strict=False):
    """{lot id: {type d'item: quantite}} d'apres le contenu attendu de chaque lot (son type et son contenu propre) ;
    0 pour un attendu facultatif, sauf en vérif complete (strict). Et {lot id: {attendu a unites: {sous-type:
    unites}}} : un attendu d'un type a unites se compte en unites (voir row_weights)."""
    required, fills = {}, {}
    for lot in group:
        rows = [row for row in lot_requirements(lot, strict=strict) if row.item_type.physical]
        required[lot.id] = {row.item_type_id: row.required for row in rows}
        fills[lot.id] = {row.item_type_id: row.fills for row in rows if row.fills}
    return required, fills


def checks_by_lot(group, strict=False):
    """{lot id: [types d'items a cocher qui retiennent le lot]} (attendus non physiques, hors facultatifs)."""
    return {lot.id: [row.item_type_id for row in lot_requirements(lot, strict=strict)
                     if not row.item_type.physical and row.required > 0] for lot in group}


def assign_items(group, scanned, today, places=None, cascade=False, strict=False):
    """Lot de destination de chaque item scanne pendant une verif de plusieurs lots.

    Un item deja range dans un des lots y reste (on l'a trouve la, meme scanne pendant qu'on verifiait un autre
    lot). Vérif en cascade (`cascade`) : `places` est le lot ou l'on etait entre au scan, un item range dans un lot
    qui n'est pas celui-ci ni un de ses sous-lots y a ete trouve, il y va comme un item venu d'ailleurs. Un item venu d'ailleurs (stock, autre lot hors de la verif) va dans le lot dont l'etiquette (publique ou
    cle) a ete scannee en dernier avant lui (`places` : {iid: id du lot}) : ce lot ou un de ses sous-lots, celui qui
    en attend encore, sinon le premier qui en contient dans sa definition, sinon ce lot lui-meme. Sans lot connu :
    le premier lot de la verif qui en attend encore, sinon le premier qui en contient, sinon le premier lot.
    Meme regle dans les fronts (planSession du front web, App::plan_verif du poste).
    """
    places = places or {}
    ids = {lot.id for lot in group}
    parent = {lot.id: lot.parent_id for lot in group}

    def under(lot_id, place):
        """lot_id est le lot `place` ou un de ses sous-lots (dans la verif)."""
        while lot_id is not None:
            if lot_id == place:
                return True
            lot_id = parent.get(lot_id)
        return False

    required, fills = _required_by_lot(group, strict)
    deficit = {lot_id: dict(rows) for lot_id, rows in required.items()}

    def weights(lot_id, type_id):
        return row_weights(fills[lot_id], required[lot_id], type_id)

    def take(lot_id, type_id):
        """L'item remplit un manque de ce lot (en unites pour un type a unites) : il y est compte."""
        rows = weights(lot_id, type_id)
        if not any(deficit[lot_id][row] > 0 for row, _ in rows):
            return False
        for row, units in rows:
            deficit[lot_id][row] -= units
        return True
    target = {}
    newcomers = []
    for item in scanned:
        place = places.get(item.iid)
        if item.location_id in ids and (not cascade or place not in ids or under(item.location_id, place)):
            target[item.iid] = item.location_id
            if not item.needs_replacement(today):
                take(item.location_id, item.pack.item_type_id)
        else:
            newcomers.append(item)
    # items frais d'abord : ce sont eux qui remplissent les manques
    newcomers.sort(key=lambda item: (item.needs_replacement(today), item.iid))
    for item in newcomers:
        type_id = item.pack.item_type_id
        place = places.get(item.iid)
        scope = [lot.id for lot in group if place not in ids or under(lot.id, place)]
        candidates = [lot_id for lot_id in scope if weights(lot_id, type_id)]
        choice = None
        if not item.needs_replacement(today):
            choice = next((lot_id for lot_id in candidates if take(lot_id, type_id)), None)
        if choice is None:
            choice = candidates[0] if candidates else scope[0]
        target[item.iid] = choice
    return target


def _with_ancestors(lots):
    """Les lots et leurs lots parents : ouvrir un sous-lot suppose d'ouvrir le lot qui le contient."""
    result = {}
    for lot in lots:
        result[lot.id] = lot
        for parent in lot.ancestors():
            result.setdefault(parent.id, parent)
    return list(result.values())


def perform_verif(lot, iids, identity, partial=False, participants=(), places=None, sealed=(), exclude=(),
                  strict=False, checked=None):
    """Verifie un lot, plusieurs lots d'un meme lot global, ou le stock (lot None), a partir des iids scannes.

    - lot peut etre un lot ou une liste de lots : chaque lot est verifie avec ses sous-lots ;
    - les items scannes sont places dans leur lot (voir assign_items) et marques presents. Pour le stock,
      un item range dans un rangement du stock y reste ;
    - un item perime scanne alors que des items frais du meme type sont arrives dans le meme lot est
      considere comme remplace (il sort du lot) ;
    - les items attendus mais non scannes voient leur compteur de verifs manquees augmenter, et
      passent en 'disparu' s'ils sont perimes ou manques trop souvent ;
    - verif partielle : seuls les lots que les items scannes rendent complets sont verifies. Les items scannes
      destines aux autres lots y sont ajoutes comme un reassort (lot « verif recommandee »), sans toucher au
      reste de leur contenu ;
    - un item a etiquette a dechirer (ItemType.tear_off) non scanne est utilise : il passe tout de suite en
      'disparu' (l'etiquette est arrachee a l'ouverture du sachet, les restes du sachet sont perdus) ;
    - un item non consommable (ItemType.consumable faux) non scanne dans un lot ordinaire verifie en a ete retire :
      il passe « hors lot » (ni dans un lot ni en stock), sans compter comme utilise ni manquant. Il rejoint le stock
      quand il y est scanne ou remis a la main. Dans un rangement du stock, il manque comme les autres ;
    - participants : autres personnes de la verif a plusieurs (identites stockees), enregistrees sur la verif ;
    - places : {iid: id du lot} dernier lot dont l'etiquette a ete scannee avant l'item (voir assign_items) ;
    - sealed : lots scelles dont l'etiquette du scelle a ete scannee pendant la verif (scelle intact constate, voir
      views.sealed_targets). Ils comptent comme verifies avec leurs sous-lots, sans etre ouverts ni scannes : leur
      contenu n'est ni attendu ni signale manquant, et ils ne recoivent pas d'items. Un lot dont un item est scanne
      a ete ouvert : il est verifie normalement ;
    - un item facultatif (attendu facultatif ou sous-lot facultatif, voir optional_types) non scanne reste ou il
      etait : ni manquant, ni hors lot, et il ne retient pas le lot. Scanne, il est compte et place comme les autres ;
    - strict : « vérif complète » demandee par un admin, rien n'est facultatif (attendus et sous-lots facultatifs
      sont obligatoires le temps de cette verif) ;
    - exclude : sous-lots des lots verifies laisses de cote avec leurs propres sous-lots (vérif en cascade : deja
      valides pendant cette vérif, pas encore faits, ou faits par un autre participant). Ils ne sont ni attendus, ni
      verifies, ni ouverts ;
    - checked : {lot id: [types]} items a cocher (ItemType.physical faux) coches pendant la vérif. Ceux d'un lot
      verifie qui ne le sont pas manquent jusqu'a sa prochaine vérif (Lots.unchecked). None (ancien front) : rien ne
      change ; en verif partielle, un lot n'est complet que si ses items a cocher le sont.
    """
    now = timezone.now()
    today = timezone.localdate()
    requested = list(dict.fromkeys(iids))
    participants = [value[:64] for value in dict.fromkeys(participants) if value and value != identity]
    threshold = qrprotec_setting('MISSING_AFTER_VERIFS')
    if lot is None:
        group = None
    else:
        group = verif_lots(lot if isinstance(lot, (list, tuple)) else [lot])
        if exclude:
            left_out = {sub_lot.id for excluded in exclude for sub_lot in excluded.descendants()}
            group = [sub_lot for sub_lot in group if sub_lot.id not in left_out]
    by_id = {sub_lot.id: sub_lot for sub_lot in group or []}

    with transaction.atomic():
        scanned = list(_items_queryset().select_for_update(of=('self',)).filter(iid__in=requested).order_by('iid'))
        scanned_ids = {item.iid for item in scanned}
        unknown = [iid for iid in requested if iid not in scanned_ids]

        # Lots scelles intacts (etiquette du scelle scannee) : eux et leurs sous-lots sont mis de cote
        intact = []
        if group is not None:
            opened_from = {item.location_id for item in scanned if item.location_id}
            for sealed_lot in sealed:
                subtree = sealed_lot.descendants()
                if sealed_lot.is_sealed and not opened_from & {sub_lot.id for sub_lot in subtree}:
                    intact += [sub_lot for sub_lot in subtree if sub_lot.id not in {done.id for done in intact}]
        intact_ids = {sub_lot.id for sub_lot in intact}
        if intact_ids:
            depths = {sub_lot.id: sub_lot.depth for sub_lot in group}
            for sub_lot in intact:
                sub_lot.depth = depths.get(sub_lot.id, sub_lot.depth)
            group = [sub_lot for sub_lot in group if sub_lot.id not in intact_ids]
            by_id = {sub_lot.id: sub_lot for sub_lot in group}
            if scanned and not group:
                raise ValueError('Items scannés hors des lots scellés : vérifiez aussi le lot qui doit les recevoir')
        scope = Q(location_id__in=list(by_id)) if group is not None else Q(location__isnull=True)
        not_seen = list(
            _items_queryset().select_for_update(of=('self',))
            .filter(scope, status__in=[ItemStatus.ACTIVE, ItemStatus.MISSING])
            .exclude(iid__in=scanned_ids)
            .exclude(reported=True)  # signale (signalement ouvert) : jamais marque disparu
            .order_by('iid')
        )
        # items facultatifs non scannes (casques du VPS) : ils restent ou ils etaient, sans manquer ni retenir le lot
        optional_checks = {sub_lot.id: optional_types(sub_lot, strict=strict)[1] for sub_lot in group or []}
        not_seen = [item for item in not_seen
                    if not (item.location_id in optional_checks and optional_checks[item.location_id](item))]

        # Destination de chaque item scanne
        if group is None:
            target = {
                item.iid: item.location_id if item.location_id and item.location.lot_type.storage else None
                for item in scanned
            }
        else:
            # un seul lot verifie avec ses sous-lots : vérif en cascade des fronts (plusieurs lots : vérif groupee)
            cascade = not isinstance(lot, (list, tuple)) or len(lot) == 1
            target = assign_items(group, scanned, today, places, cascade, strict)

        # Detection des remplacements, par lot et par type. Un perime (inutilisable, desactive) scanne est retire du
        # lot, a jeter, des que le lot a son minimum d'items frais de ce type (meme deja presents avant la verif) ;
        # sinon il reste a remplacer. Un perime scanne qui n'etait pas dans ce lot (stock, autre lot, hors lot) n'y
        # entre jamais : il est a jeter. Un item a la revision en retard n'est pas jete : seulement remplace par un
        # item frais nouvellement arrive. Meme regle dans les fronts (planSession, App::plan_verif).
        # Type a unites (attentes en mL) : les comptes se font en unites de chaque attendu (row_weights), et un
        # perime est remplace par des unites fraiches de son attendu principal (main_row).
        required, fills = _required_by_lot(group, strict) if group is not None else ({}, {})

        def weights(lot_id, type_id):
            return row_weights(fills.get(lot_id, {}), required.get(lot_id, {}), type_id)
        fresh = defaultdict(int)
        new_fresh = defaultdict(int)
        expired_by_key = defaultdict(list)
        for item in scanned:
            lot_id, type_id = target[item.iid], item.pack.item_type_id
            if item.needs_replacement(today):
                expired_by_key[(lot_id, type_id)].append(item)
                continue
            fresh_new = item.location_id != lot_id or item.status != ItemStatus.ACTIVE
            rows = weights(lot_id, type_id) or [(type_id, 1)]
            for row, units in rows:
                fresh[(lot_id, row)] += units
            row, units = main_row(rows, type_id)
            if fresh_new:
                new_fresh[(lot_id, row)] += units
        replaced = set()
        for key, expired_items in expired_by_key.items():
            lot_id, type_id = key
            rows = weights(lot_id, type_id) or [(type_id, 1)]
            covered = all(fresh[(lot_id, row)] >= required.get(lot_id, {}).get(row, 0) for row, _ in rows)
            main, units = main_row(rows, type_id)
            for item in expired_items:
                if discardable(item, today) and (covered or item.location_id != lot_id):
                    replaced.add(item.iid)
                elif new_fresh.get((lot_id, main), 0) >= units:
                    replaced.add(item.iid)
                    new_fresh[(lot_id, main)] -= units

        # Lots verifies : tous, ou seulement ceux que la verif partielle rend complets
        if group is None:
            verified_ids = set()
        elif partial:
            verified_ids = _complete_lots(group, scanned, not_seen, target, today, strict, checked)
        else:
            verified_ids = set(by_id)
        if group is not None and partial and not verified_ids and not scanned and not intact and not checked:
            raise ValueError('Rien à valider : aucun item scanné')
        # lots caches (B+ du VPS) et lots de referentiel : pas de vérif a eux, valides par leurs sous-lots (plus bas)
        derived = {lot_id for lot_id, sub_lot in by_id.items() if hidden_in_verif(sub_lot) or virtual_lot(sub_lot)}
        verified_ids -= derived
        not_seen = [item for item in not_seen if group is None or item.location_id in verified_ids]
        # items non consommables absents d'un lot ordinaire verifie : retires du lot, ils passent hors lot
        loose = [item for item in not_seen if leaves_lot(item)]
        not_seen = [item for item in not_seen if not leaves_lot(item)]

        # Scelles : lots verifies, lots ou des items sont ajoutes, lots d'ou des items sont retires, et leurs parents
        opened_ids = set(verified_ids)
        opened_ids |= {target[item.iid] for item in scanned if target[item.iid] is not None}
        opened_ids |= {item.location_id for item in scanned if item.location_id and item.location_id != target[item.iid]}
        opened = list(Lots.objects.select_related('parent').filter(id__in=opened_ids))
        unsealed, broken = [], []
        for sealed in _with_ancestors(opened):
            if sealed.is_sealed:
                if sealed.id in verified_ids:
                    reason = 'vérif'
                elif sealed.id in by_id:
                    reason = 'ajout'
                else:
                    reason = 'retrait' if sealed.id in opened_ids else 'ouverture d\'un sous-lot'
                break_seal(sealed, identity, reason, now, notify=False)
                unsealed.append(sealed.name)
                broken.append((sealed, reason))
                if sealed.id in by_id:
                    by_id[sealed.id].refresh_from_db(fields=['is_sealed', 'seal_code', 'unsealed', 'unsealed_by'])
        notify_seals_broken(broken, identity)
        was_missing = {item.iid for item in not_seen if item.status == ItemStatus.MISSING}

        verifs = {}
        if group is None:
            verifs[None] = Verifs.objects.create(lot=None, datetime=now, by=identity, participants=participants)
        for lot_id in verified_ids:
            verifs[lot_id] = Verifs.objects.create(lot=by_id[lot_id], datetime=now, by=identity,
                                                   participants=participants,
                                                   lot_type_version=by_id[lot_id].lot_type.version,
                                                   lot_version=by_id[lot_id].version)

        report = {'present': [], 'expired': [], 'replaced': [], 'missing': [], 'unknown': unknown, 'reactivated': [],
                  'restocked': [], 'torn': [], 'unusable': [], 'loose': [item.iid for item in loose]}
        entries = []
        moves = []
        origin = {item.iid: item.location_id for item in scanned}
        # items retrouves apres avoir ete notes absents : leur absence ne compte plus comme une utilisation
        movements.found_again([item.iid for item in scanned + loose if movements.may_be_absent(item)], now, identity)
        counts = defaultdict(lambda: defaultdict(int))  # lot id -> compteurs de la verif
        restocked = defaultdict(int)
        restocked_items = defaultdict(list)  # lot id -> items ranges par reassort (journal)
        for item in scanned:
            destination = target[item.iid]
            expired = item.needs_replacement(today)
            if group is not None and destination not in verified_ids:
                # reassort (verif partielle) : l'item est range dans son lot sans verif de ce lot
                if item.location_id != destination:
                    restocked[destination] += 1
                    restocked_items[destination].append(item)
                    report['restocked'].append(item.iid)
                item.touch(identity, SeenWhile.ADD, now)
                item.missed_verifs = 0
                item.status = ItemStatus.ACTIVE
                item.location = by_id[destination]
                continue
            verif = verifs[destination if group is not None else None]
            item.touch(identity, SeenWhile.VERIF, now)
            item.missed_verifs = 0
            if item.status != ItemStatus.ACTIVE:
                report['reactivated'].append(item.iid)
            if item.iid in replaced:
                item.status = ItemStatus.REPLACED
                item.location = None
                report['replaced'].append(item.iid)
                counts[destination]['replaced'] += 1
                entries.append(VerifItem(verif=verif, item=item, result=VerifResult.REPLACED, expired=expired))
            else:
                item.status = ItemStatus.ACTIVE
                if group is not None:
                    item.location = by_id[destination]
                elif destination is None:
                    item.location = None  # sinon : reste dans son rangement du stock
                report['present'].append(item.iid)
                counts[destination]['present'] += 1
                if expired:
                    report['expired'].append(item.iid)
                    counts[destination]['expired'] += 1
                if item.unusable or item.disabled:
                    # signale inutilisable ou desactive : a remplacer (compte aussi dans expired)
                    report['unusable'].append(item.iid)
                entries.append(VerifItem(verif=verif, item=item, result=VerifResult.PRESENT, expired=expired))
        for item in not_seen:
            expired = item.needs_replacement(today)
            verif = verifs[item.location_id if group is not None else None]
            if item.missed_verifs == 0:
                moves.append(movements.absence(item, now, identity, verif))
            item.missed_verifs += 1
            torn = item.pack.item_type.tear_off
            if expired or torn or item.missed_verifs >= threshold:
                item.status = ItemStatus.MISSING
            if torn:
                report['torn'].append(item.iid)
            report['missing'].append(item.iid)
            counts[item.location_id]['missing'] += 1
            entries.append(VerifItem(verif=verif, item=item, result=VerifResult.MISSING, expired=expired))

        save_items(scanned, ['status', 'location', 'missed_verifs', 'last_seen', 'last_seen_by', 'last_seen_while'])
        save_items(not_seen, ['status', 'missed_verifs'])
        _set_loose(loose, identity, now)
        for item in scanned:
            if item.status == ItemStatus.REPLACED:
                moves.append(movements.movement(item, MovementKind.REPLACED, now, identity, origin[item.iid]))
            elif item.location_id != origin[item.iid]:
                moves.append(movements.movement(item, MovementKind.MOVE, now, identity, origin[item.iid],
                                                  item.location_id))
        ItemMovement.objects.bulk_create(moves)
        by_iid_location = {item.iid: item.location_id for item in not_seen}
        newly_missing = [item for item in not_seen if item.status == ItemStatus.MISSING and item.iid not in was_missing]
        VerifItem.objects.bulk_create(entries)

        lots_report = []
        complete = True
        for sub_lot in intact:
            # scelle intact constate : le lot (et ses sous-lots) est valide sans etre ouvert
            lots_report.append({'id': sub_lot.id, 'name': sub_lot.name, 'parent': sub_lot.parent_id,
                                'depth': sub_lot.depth, 'verified': False, 'sealed': True, 'complete': True,
                                'restocked': 0})
            journal.record(OperationKind.VERIF, identity, 'scellé intact constaté pendant une vérif', lot=sub_lot,
                           at=now, sealed_intact=True)
        for sub_lot in group or []:
            if sub_lot.id in derived:
                continue  # ligne ajoutee s'il est valide par ses sous-lots (verify_hidden_parents)
            row = {'id': sub_lot.id, 'name': sub_lot.name, 'parent': sub_lot.parent_id, 'depth': sub_lot.depth,
                   'verified': sub_lot.id in verified_ids, 'restocked': restocked.get(sub_lot.id, 0)}
            if sub_lot.id in verified_ids:
                sub_lot.last_verif = now
                sub_lot.last_verif_by = identity
                sub_lot.last_verif_while = SeenWhile.VERIF
                sub_lot.last_used = now
                sub_lot.last_used_by = identity
                sub_lot.verif_recommended = False
                sub_lot.restocked_count = 0
                sub_lot.outings = 0
                sub_lot.back_from_list = ''
                sub_lot.marked_used = None
                sub_lot.marked_used_by = ''
                if checked is not None:
                    ticked = set(checked.get(sub_lot.id, ()))
                    sub_lot.unchecked = [type_id for type_id in checks_by_lot([sub_lot], strict)[sub_lot.id]
                                         if type_id not in ticked]
                sub_lot.save(update_fields=['last_verif', 'last_verif_by', 'last_verif_while', 'last_used',
                                            'last_used_by', 'verif_recommended', 'restocked_count', 'outings',
                                            'back_from_list', 'marked_used', 'marked_used_by', 'unchecked'])
                requirements = requirements_status(sub_lot, today, strict=strict)
                lot_expired = [iid for iid in report['expired'] if target.get(iid) == sub_lot.id]
                # complet des que le minimum d'items frais est la, meme s'il reste un perime en trop
                lot_complete = all(r['present'] >= r['required'] for r in requirements)
                verif = verifs[sub_lot.id]
                verif.complete = lot_complete
                verif.present_count = counts[sub_lot.id]['present']
                verif.missing_count = counts[sub_lot.id]['missing']
                verif.expired_count = counts[sub_lot.id]['expired']
                verif.replaced_count = counts[sub_lot.id]['replaced']
                verif.save()
                row.update({'complete': lot_complete, 'requirements': requirements, 'verif_id': verif.id})
                _record_verif(sub_lot, identity, now, verif, requirements, partial,
                              [iid for iid in report['missing'] if by_iid_location.get(iid) == sub_lot.id],
                              lot_expired)
                if not lot_complete:
                    lot_missing = [item for item in newly_missing if item.location_id == sub_lot.id]
                    notify_verif_problem(sub_lot, identity, requirements,
                                         {'expired': lot_expired}, lot_missing)
            else:
                complete = False
                row['complete'] = None
                if restocked.get(sub_lot.id):
                    sub_lot.last_used = now
                    sub_lot.last_used_by = identity
                    fields = ['last_used', 'last_used_by'] + apply_restock(sub_lot, identity, now,
                                                                           restocked[sub_lot.id])
                    sub_lot.save(update_fields=fields)
                    items = restocked_items[sub_lot.id]
                    journal.record(OperationKind.RESTOCK, identity, f"{journal.types_summary(items)} (vérif partielle)",
                                   lot=sub_lot, at=now, count=len(items), items=journal.item_ids(items),
                                   partial_verif=True)
            complete = complete and row['complete'] is not False
            lots_report.append(row)

        if group is None:
            verif = verifs[None]
            verif.complete = True
            verif.present_count = len(report['present'])
            verif.missing_count = len(report['missing'])
            verif.expired_count = len(report['expired'])
            verif.replaced_count = len(report['replaced'])
            verif.save()
            _record_verif(None, identity, now, verif, [], False, report['missing'], report['expired'])

        if group is not None:
            # lots caches dans la vérif en cascade (B+ du VPS) : valides avec le dernier de leurs sous-lots
            for parent, verif in verify_hidden_parents([by_id[lot_id] for lot_id in verified_ids] + intact, identity, now):
                lots_report.append({'id': parent.id, 'name': parent.name, 'parent': parent.parent_id,
                                    'depth': by_id[parent.id].depth if parent.id in by_id else 0, 'verified': True,
                                    'complete': verif.complete, 'requirements': [], 'verif_id': verif.id,
                                    'restocked': 0})

        affected = {item.pack.item_type_id for item in scanned + not_seen + loose}
        notifications.check_stock_levels(affected)

    verified_rows = [row for row in lots_report if row['verified']]
    report['lots'] = lots_report
    report['partial'] = group is not None and any(not row['verified'] and not row.get('sealed') for row in lots_report)
    report['sealed_lots'] = [row['name'] for row in lots_report if row.get('sealed')]
    report['complete'] = complete
    report['verif_id'] = verifs[None].id if group is None else (verified_rows[0]['verif_id'] if verified_rows else None)
    # exigences des lots verifies (nom du lot precise quand la verif en couvre plusieurs)
    report['requirements'] = [
        dict(requirement, lot=row['id'], lot_name=row['name']) if len(lots_report) > 1 else requirement
        for row in verified_rows for requirement in row['requirements']
    ]
    report['unsealed'] = bool(unsealed)
    report['unsealed_lots'] = unsealed
    return report


def discardable(item, today=None) -> bool:
    """Perime, signale inutilisable ou desactive : a retirer du lot et a jeter. Un item dont seule la revision est
    en retard n'est pas jete (il part en revision)."""
    return item.is_expired(today) or item.unusable or item.disabled


def leaves_lot(item) -> bool:
    """Item non consommable range dans un lot ordinaire : absent a la verif du lot, il en a ete retire et passe hors
    lot (un lecteur de glycemie ne s'use pas). Dans un rangement du stock (ou hors lot), il manque normalement."""
    return (not item.pack.item_type.consumable and item.location_id is not None
            and not item.location.lot_type.storage)


def _set_loose(items, identity, now):
    """Items non consommables absents a la verif de leur lot : sortis du lot, « hors lot » (pas encore en stock)."""
    if not items:
        return
    ItemMovement.objects.bulk_create([movements.movement(item, MovementKind.MOVE, now, identity, item.location_id)
                                      for item in items])
    by_origin = defaultdict(list)
    for item in items:
        by_origin[item.location_id].append(item)
    for origin_items in by_origin.values():
        journal.record(OperationKind.REMOVE, identity,
                       f"{journal.types_summary(origin_items)} (non consommable absent à la vérif : hors lot)",
                       lot=origin_items[0].location, at=now, count=len(origin_items),
                       items=journal.item_ids(origin_items), non_consumable=True)
    for item in items:
        item.location = None
        item.status = ItemStatus.LOOSE
        item.missed_verifs = 0
    save_items(items, ['location', 'status', 'missed_verifs'])


def _record_verif(lot, identity, now, verif, requirements, partial, missing, expired):
    """Verif d'un lot (ou du stock, lot None) dans le journal des operations."""
    counts = [(verif.present_count, 'présent'), (verif.missing_count, 'manquant'), (verif.expired_count, 'périmé'),
              (verif.replaced_count, 'remplacé')]
    parts = [journal.plural(count, word) for count, word in counts if count]
    lacking = [{'type': row['type'], 'type_name': row['type_name'], 'required': row['required'],
                'present': row['present'],
                # type a unites : unite et sous-type a prendre (compte rendu « Manque 50 mL de Sérum phy »)
                **{key: row[key] for key in ('unit', 'within', 'suggest', 'subtypes') if key in row}}
               for row in requirements if row['present'] < row['required']]
    if lot is None:
        summary = ', '.join(parts) or 'aucun item'
    else:
        summary = ('complète' if verif.complete else 'incomplète') + (' : ' + ', '.join(parts) if parts else '')
    if partial:
        summary += ' (vérif partielle)'
    if verif.participants:
        summary += f' (à {len(verif.participants) + 1})'
    journal.record(OperationKind.STOCK_VERIF if lot is None else OperationKind.VERIF, identity, summary, lot=lot,
                   at=now, verif=verif.id, participants=verif.participants, complete=verif.complete, partial=partial, present=verif.present_count,
                   missing=verif.missing_count, expired=verif.expired_count, replaced=verif.replaced_count,
                   lacking=lacking, missing_items=missing[:journal.MAX_LISTED_ITEMS],
                   expired_items=expired[:journal.MAX_LISTED_ITEMS])


def _complete_lots(group, scanned, not_seen, target, today, strict=False, checked=None):
    """Lots que les items scannes rendent complets (verif partielle). Un lot sans aucun item scanne n'est retenu
    que s'il ne contient rien : sinon tout son contenu serait signale manquant. Un rangement du stock n'a pas de
    contenu attendu : ses items sont ses attendus, il n'est complet que si tous ont ete scannes. Un item a
    etiquette a dechirer (utilise) ou non consommable (passe hors lot) non scanne ne compte pas. Meme regle dans
    les fronts (planSession, App::plan_verif)."""
    required, fills = _required_by_lot(group, strict)
    fresh = defaultdict(lambda: defaultdict(int))
    touched = set()
    for item in scanned:
        destination = target[item.iid]
        touched.add(destination)
        if not item.needs_replacement(today):
            type_id = item.pack.item_type_id
            for row, units in (row_weights(fills.get(destination, {}), required.get(destination, {}), type_id)
                               or [(type_id, 1)]):
                fresh[destination][row] += units
    # un item a dechirer ou non consommable (passe hors lot) non scanne ne retient pas son lot
    holding = {item.location_id for item in not_seen if not item.pack.item_type.tear_off and not leaves_lot(item)}
    checks = checks_by_lot(group, strict) if checked is not None else {}
    complete = set()
    for lot in group:
        if lot.id in holding and (lot.id not in touched or lot.lot_type.storage):
            continue
        if checked is not None and not set(checks[lot.id]) <= set(checked.get(lot.id, ())):
            continue  # un item a cocher ne l'est pas
        # un perime en trop ne retient pas le lot : seul compte le minimum d'items frais
        if all(fresh[lot.id][type_id] >= quantity for type_id, quantity in required[lot.id].items()):
            complete.add(lot.id)
    return complete


def notify_verif_problem(lot, identity, requirements, report, newly_missing):
    """Vérif incomplète : notification web (type verif_problem, désactivé par défaut), qui ouvre la fiche du lot."""
    parts = []
    lacking = [lacking_text(row, ' ') for row in requirements if row['present'] < row['required']]
    if lacking:
        parts.append('manque ' + ', '.join(lacking))
    if report['expired']:
        parts.append(journal.plural(len(report['expired']), 'périmé'))
    if newly_missing:
        parts.append(journal.plural(len(newly_missing), 'item signalé disparu', 'items signalés disparus'))
    return notifications.notify_admins(
        'verif_problem', 'vérif incomplète',
        f'vérif du lot {lot.name} incomplète ({short_identity_name(identity)}) : ' + ' ; '.join(parts),
        url=f'../verif?lot={lot.id}', tag=f'verif-{lot.id}', tab='lot')


def seal_lot(lot, identity, seal_number='', force=False):
    """Scelle un lot : il reste valide sans verif tant que le scelle n'est pas brise."""
    if not lot.active:
        raise ValueError('Lot inactif')
    if not force:
        # un lot global est scelle avec ses sous-lots : tous doivent etre complets
        today = timezone.localdate()
        for sub_lot in lot.descendants():
            rows = requirements_status(sub_lot, today)
            expired = Items.objects.filter(Q(pack__peremption__lt=today) | Q(unusable=True) | Q(disabled=True)
                                           | revision_blocked_q(today),
                                           location=sub_lot,
                                           status=ItemStatus.ACTIVE).exists()
            if expired or any(row['present'] < row['required'] for row in rows):
                what = 'Lot' if sub_lot.id == lot.id else f'Sous-lot {sub_lot.name}'
                raise ValueError(f'{what} incomplet ou contenant des périmés : faites une vérif complète avant de '
                                 'sceller (ou forcez le scellage)')
    now = timezone.now()
    lot.is_sealed = True
    lot.sealed = now
    lot.sealed_by = identity
    lot.seal_number = seal_number[:32]
    lot.seal_code = generate_key(16)
    lot.seal_open_code = generate_key(16)
    lot.unsealed = None
    lot.unsealed_by = ''
    lot.last_used = now
    lot.last_used_by = identity
    lot.save()
    number = f'scellé n°{lot.seal_number}' if lot.seal_number else 'scellé sans numéro'
    journal.record(OperationKind.SEAL, identity, number + (' (forcé)' if force else ''), lot=lot, at=now,
                   seal_number=lot.seal_number, forced=force)
    return lot


def break_seal(lot, identity, reason, now=None, notify=True):
    """Brise le scelle d'un lot (ouverture, vérif, ajout ou retrait d'items). notify=False : l'appelant envoie une
    seule notification pour tous les scelles ouverts par la meme operation (notify_seals_broken)."""
    if not lot.is_sealed:
        return False
    lot.is_sealed = False
    lot.seal_code = ''
    lot.unsealed = now or timezone.now()
    lot.unsealed_by = identity
    lot.save(update_fields=['is_sealed', 'seal_code', 'unsealed', 'unsealed_by'])
    number = f' n°{lot.seal_number}' if lot.seal_number else ''
    journal.record(OperationKind.UNSEAL, identity, f'scellé{number} ouvert ({reason})', lot=lot, at=lot.unsealed,
                   seal_number=lot.seal_number, reason=reason)
    if notify:
        notify_seals_broken([(lot, reason)], identity)
    return True


def notify_seals_broken(opened, identity):
    """Scelles ouverts par une meme operation ([(lot, raison)], le lot le plus bas d'abord) : une notification aux
    admins (notification web), qui ouvre la fiche du premier lot."""
    if not opened:
        return 0
    who = short_identity_name(identity)
    names = [f'{lot.name}' + (f' (scellé n°{lot.seal_number})' if lot.seal_number else '') for lot, _ in opened]
    first, reason = opened[0]
    text = f'{names[0]} ouvert ({reason}) par {who}'
    if len(opened) > 1:
        text += ', avec ' + ', '.join(names[1:])
    return notifications.notify_admins('seal_broken', 'scellé ouvert', f'scellé ouvert : {text}',
                                       url=f'../verif?lot={first.id}', tag=f'seal-{first.id}', tab='lot')


# Scelle ouvert par l'etiquette d'ouverture sans utilisateur identifie
SEAL_OPENED_ANONYMOUSLY = f"{SOURCE_DECLARED}:{ANONYMOUS_OPENING}"
# delai pour signer une ouverture anonyme (l'utilisateur se connecte juste apres le scan)
SEAL_OPENING_SIGN_DELAY = timedelta(hours=1)


def open_seal_with_label(lot, identity):
    """Etiquette d'ouverture scannee (elle est a l'interieur du lot) : le scan suffit a ouvrir le scelle, que
    quelqu'un soit identifie ou non. Une ouverture anonyme peut etre signee juste apres (connexion apres le scan).

    Renvoie 'opened' (scelle ouvert par ce scan), 'signed' (ouverture anonyme attribuee) ou 'already'.
    """
    if lot.is_sealed:
        now = timezone.now()
        # un sous-lot est range dans son lot parent : l'ouvrir suppose d'ouvrir aussi le parent
        opened, broken = [], []
        for sealed in _with_ancestors([lot]):
            if sealed.is_sealed:
                reason = "étiquette d'ouverture" if sealed.id == lot.id else "ouverture d'un sous-lot"
                break_seal(sealed, identity or SEAL_OPENED_ANONYMOUSLY, reason, now, notify=False)
                opened.append(sealed)
                broken.append((sealed, reason))
        notify_seals_broken(broken, identity or SEAL_OPENED_ANONYMOUSLY)
        # le contenu a pu etre utilise : verif recommandee (orange) pour le lot, ses sous-lots (ils etaient sous
        # le scelle) et les lots parents ouverts avec lui
        recommended = {sub_lot.id: sub_lot for sub_lot in lot.descendants()}
        recommended.update({sealed.id: sealed for sealed in opened})
        for target in recommended.values():
            if not target.verif_recommended or target.back_from_list or target.marked_used:
                target.verif_recommended = True
                target.back_from_list = ''
                target.marked_used = None
                target.marked_used_by = ''
                target.save(update_fields=['verif_recommended', 'back_from_list', 'marked_used', 'marked_used_by'])
        return 'opened'
    if (identity and lot.unsealed_by == SEAL_OPENED_ANONYMOUSLY and lot.unsealed
            and timezone.now() - lot.unsealed <= SEAL_OPENING_SIGN_DELAY):
        # les lots parents ouverts par le meme scan sont signes aussi
        for opened in _with_ancestors([lot]):
            if opened.unsealed_by == SEAL_OPENED_ANONYMOUSLY and opened.unsealed == lot.unsealed:
                opened.unsealed_by = identity
                opened.save(update_fields=['unsealed_by'])
                for operation in Operation.objects.filter(kind=OperationKind.UNSEAL, lot=opened, at=opened.unsealed,
                                                          by=SEAL_OPENED_ANONYMOUSLY):
                    operation.by = identity
                    operation.details['signed_after_scan'] = True
                    operation.save(update_fields=['by', 'details'])
        # la notification web de l'ouverture (meme tag) est remplacee par une version signee
        webpush.notify_event('seal_broken', 'QRProtec : scellé ouvert',
                             f"scellé ouvert : {lot.name} ouvert (étiquette d'ouverture) par {short_identity_name(identity)}",
                             url=f'../verif?lot={lot.id}', tag=f'seal-{lot.id}', tab='lot')
        return 'signed'
    return 'already'


def mark_used(lot, identity):
    """Bouton « Marquer ce lot comme utilisé » (tout secouriste) : le lot et ses sous-lots non scelles passent en
    verif recommandee (orange) pour les suivants, jusqu'a la prochaine verif. Un lot scelle s'ouvre avec l'etiquette
    d'ouverture (rangee a l'interieur), pas avec ce bouton. Renvoie les lots marques."""
    if not lot.active:
        raise ValueError('Lot inactif')
    if lot.is_sealed:
        raise ValueError("Lot scellé : scannez l'étiquette d'ouverture rangée à l'intérieur pour l'ouvrir")
    now = timezone.now()
    marked = []
    for target in lot.descendants():
        if target.is_sealed or target.lot_type.storage or not target.active:
            continue
        target.verif_recommended = True
        target.marked_used = now
        target.marked_used_by = identity
        target.last_used = now
        target.last_used_by = identity
        target.outings += 1  # une sortie de plus pour le credit de confiance du reassort
        target.save(update_fields=['verif_recommended', 'marked_used', 'marked_used_by', 'last_used', 'last_used_by',
                                   'outings'])
        marked.append(target)
    if not marked:
        raise ValueError('Rien à marquer : rangement du stock ou sous-lots scellés')
    others = [target.name for target in marked if target.id != lot.id]
    journal.record(OperationKind.LOT_USED, identity,
                   'vérif recommandée' + (f" (avec {', '.join(others)})" if others else ''), lot=lot, at=now,
                   lots=[target.id for target in marked])
    return marked


def move_refusal(item, lot, today, declared=False, tree=()):
    """Raison pour laquelle un item ne peut pas etre range dans `lot` (None : en stock), ou '' s'il peut l'etre.

    - un item supprime ou remplace (jete) ne revient jamais par un reassort : la gestion le restaure d'abord ;
    - un item perime, signale inutilisable ou desactive n'entre pas dans un lot ordinaire (il est a jeter). Un
      rangement du stock ou le stock l'acceptent : il y attend d'etre jete ;
    - sous un nom declare (cle du lot, sans badge), seuls les items en stock ou deja dans le lot (ou ses sous-lots)
      peuvent y etre ranges : la cle d'un lot ne permet pas de vider un autre lot, ni d'en briser le scelle.
    Les options de debug du poste (admins seulement) levent ces refus, voir views.debug_options."""
    if item.status == ItemStatus.DELETED:
        return 'supprimé : à restaurer depuis la gestion'
    if item.status == ItemStatus.REPLACED:
        return 'remplacé (jeté)'
    if lot is not None and not lot.lot_type.storage and discardable(item, today):
        if item.disabled:
            return 'désactivé : à jeter, pas dans un lot'
        if item.unusable:
            return 'signalé inutilisable : à jeter, pas dans un lot'
        return 'périmé : à jeter, pas dans un lot'
    if declared and item.location_id and item.location_id not in tree and not item.location.lot_type.storage:
        return f'dans le lot {item.location.name} : scannez votre badge pour le déplacer'
    return ''


def move_items(iids, lot, identity, context, declared=False, debug=False):
    """Deplace des items dans un lot (ou en stock si lot est None) sans toucher aux autres.

    Les items qui ne peuvent pas y aller (voir move_refusal) restent ou ils sont et sont rendus dans "refused" ;
    si aucun item ne bouge a cause d'eux, l'operation est refusee en entier. declared : identite declaree (nom, sans
    badge). debug : options de debug d'un admin au poste, aucun refus (lots inacceptables en vrai, pour tester)."""
    now = timezone.now()
    today = timezone.localdate(now)
    requested = list(dict.fromkeys(iids))
    with transaction.atomic():
        candidates = list(Items.objects.select_for_update(of=('self',))
                          .select_related('pack__item_type', 'location__lot_type').filter(iid__in=requested))
        found = {item.iid for item in candidates}
        tree = {sub_lot.id for sub_lot in lot.descendants()} if lot is not None and declared else set()
        items, refused = [], []
        for item in candidates:
            reason = '' if debug else move_refusal(item, lot, today, declared, tree)
            if reason:
                refused.append({'iid': item.iid, 'reason': reason})
            else:
                items.append(item)
        if refused and not items:
            reasons = ', '.join(f"{row['iid']} ({row['reason']})" for row in refused)
            raise ValueError(('Aucun item ajouté : ' if lot is not None else 'Aucun item remis en stock : ') + reasons)
        # ajouter ou retirer des items d'un lot scelle brise son scelle
        touched_lots = {item.location_id for item in items if item.location_id and item.location_id != (lot.id if lot else None)}
        if lot is not None and items:
            touched_lots.add(lot.id)
        # un sous-lot est range dans son lot parent : l'ouvrir brise aussi le scelle du parent
        broken = []
        for sealed in _with_ancestors(Lots.objects.select_related('parent').filter(id__in=touched_lots)):
            if not sealed.is_sealed:
                continue
            if lot is not None and sealed.id == lot.id:
                reason = 'ajout'
            else:
                reason = 'retrait' if sealed.id in touched_lots else "ouverture d'un sous-lot"
            break_seal(sealed, identity, reason, now, notify=False)
            broken.append((sealed, reason))
        notify_seals_broken(broken, identity)
        if lot is not None:
            lot.refresh_from_db(fields=['is_sealed', 'seal_code', 'unsealed', 'unsealed_by'])
        movements.found_again([item.iid for item in items if movements.may_be_absent(item)], now, identity)
        moves = [
            movements.movement(item, MovementKind.MOVE, now, identity, item.location_id, lot.id if lot else None)
            for item in items if item.location_id != (lot.id if lot else None)
        ]
        ItemMovement.objects.bulk_create(moves)
        _record_moves(items, lot, identity, context, now)
        for item in items:
            item.location = lot
            item.status = ItemStatus.ACTIVE
            item.missed_verifs = 0
            item.touch(identity, context, now)
        save_items(items, ['location', 'status', 'missed_verifs', 'last_seen', 'last_seen_by', 'last_seen_while'])
        if lot is not None:
            lot.last_used = now
            lot.last_used_by = identity
            fields = ['last_used', 'last_used_by']
            if items:
                # reassort sans verif complete (un rangement du stock n'a pas de contenu attendu : y ranger des items
                # est l'usage normal)
                fields += apply_restock(lot, identity, now, len(items))
            lot.save(update_fields=fields)
        notifications.check_stock_levels({item.pack.item_type_id for item in items})
    moved = {item.iid for item in items}
    return {'moved': [iid for iid in requested if iid in moved], 'refused': refused,
            'unknown': [iid for iid in requested if iid not in found]}


def _record_moves(items, lot, identity, context, now):
    """Reassort (items ranges dans un lot) ou retrait vers le stock (un par lot d'origine) dans le journal."""
    destination = lot.id if lot else None
    moved = [item for item in items if item.location_id != destination]
    if lot is not None:
        if moved:
            origins = sorted({item.location.name for item in moved if item.location_id})
            journal.record(OperationKind.RESTOCK, identity, journal.types_summary(moved), lot=lot, at=now,
                           count=len(moved), items=journal.item_ids(moved), origins=origins, context=context)
        return
    by_origin = defaultdict(list)
    for item in moved:
        by_origin[item.location_id].append(item)
    for origin_items in by_origin.values():
        journal.record(OperationKind.REMOVE, identity, journal.types_summary(origin_items),
                       lot=origin_items[0].location, at=now, count=len(origin_items),
                       items=journal.item_ids(origin_items))


def mark_out(iids, identity):
    """Items sortis du stock sans lot (ex : pris pour une intervention) : ils passent « sorti » et quittent le stock.

    Seuls les items en stock (hors lot ou dans un rangement du stock) peuvent sortir ; un item deja sorti repart pour
    un nouveau delai. Un item sorti qui reapparait (verif, ajout a un lot, retour en stock) redevient actif. Sinon il
    est compte comme utilise (ou jete s'il est perime) apres QRPROTEC['OUT_DAYS'] jours ou a sa peremption, voir
    expire_out_items. Un item non consommable ne s'use pas : il reste « sorti » jusqu'a son retour.
    """
    now = timezone.now()
    requested = list(dict.fromkeys(iids))
    with transaction.atomic():
        items = list(_items_queryset().select_for_update(of=('self',)).filter(iid__in=requested).order_by('iid'))
        found = {item.iid for item in items}
        refused = []
        out = []
        for item in items:
            if item.status not in (ItemStatus.ACTIVE, ItemStatus.OUT):
                refused.append({'iid': item.iid, 'reason': f'{item.get_status_display().lower()} : pas en stock'})
            elif item.location_id and not item.location.lot_type.storage:
                refused.append({'iid': item.iid,
                                'reason': f'dans le lot {item.location.name} : scannez son étiquette pour le retirer'})
            else:
                out.append(item)
        if not out:
            if refused:
                raise ValueError('Aucun item sorti : ' + ', '.join(f"{row['iid']} ({row['reason']})" for row in refused))
            raise ValueError('Rien à sortir : aucun item scanné')
        moves = [movements.movement(item, MovementKind.OUT, now, identity, item.location_id) for item in out]
        ItemMovement.objects.bulk_create(moves)
        origins = sorted({item.location.name for item in out if item.location_id})
        journal.record(OperationKind.ITEM_OUT, identity, journal.types_summary(out), at=now, count=len(out),
                       items=journal.item_ids(out), origins=origins)
        for item in out:
            item.location = None
            item.status = ItemStatus.OUT
            item.missed_verifs = 0
            item.touch(identity, SeenWhile.OUT, now)
        save_items(out, ['location', 'status', 'missed_verifs', 'last_seen', 'last_seen_by',
                                        'last_seen_while'])
        notifications.check_stock_levels({item.pack.item_type_id for item in out})
    return {
        'out': [item.iid for item in out],
        'refused': refused,
        'unknown': [iid for iid in requested if iid not in found],
        'until': _date_iso(out_deadline(out[0], now)) if len(out) == 1 else None,
        'days': qrprotec_setting('OUT_DAYS'),
    }


def _date_iso(value):
    return value.isoformat() if value else None


def out_deadline(item, since=None):
    """Date a laquelle un item sorti non revu compte comme utilise : OUT_DAYS jours apres sa sortie, ou sa
    peremption si elle tombe avant. None pour un item non consommable : il ne s'use pas, il reste sorti."""
    if not item.pack.item_type.consumable:
        return None
    deadline = timezone.localdate(since or item.last_seen) + timedelta(days=qrprotec_setting('OUT_DAYS'))
    peremption = item.pack.peremption
    return min(deadline, peremption) if peremption else deadline


def expire_out_items(now=None):
    """Items sortis non revus dans le delai (ou arrives a peremption) : comptes comme utilises (ils etaient bons a la
    sortie), statut « disparu ». S'ils reapparaissent ensuite, l'utilisation est annulee comme apres une verif."""
    now = now or timezone.now()
    today = timezone.localdate(now)
    expired = []
    with transaction.atomic():
        for item in _items_queryset().select_for_update(of=('self',)).filter(status=ItemStatus.OUT).order_by('iid'):
            deadline = out_deadline(item)
            if deadline is not None and deadline <= today:
                expired.append(item)
        if not expired:
            return []
        moves = []
        for item in expired:
            moves.append(movements.movement(item, MovementKind.USED, now, item.last_seen_by))
            item.status = ItemStatus.MISSING
            item.missed_verifs = 1  # absence deja comptee : une verif du stock n'en ajoute pas une autre
        ItemMovement.objects.bulk_create(moves)
        save_items(expired, ['status', 'missed_verifs'])
        journal.record(OperationKind.ITEM_OUT_USED, '',
                       f"{journal.types_summary(expired)} : {journal.pl(len(expired), 'non revu', 'non revus')} après la sortie, "
                       f"{journal.pl(len(expired), 'compté utilisé', 'comptés utilisés')}",
                       at=now, count=len(expired), items=journal.item_ids(expired))
        notifications.check_stock_levels({item.pack.item_type_id for item in expired})
    return [item.iid for item in expired]


def mark_deleted(item, identity, reason):
    now = timezone.now()
    ItemMovement.objects.create(item=item, item_type_id=item.pack.item_type_id, kind=MovementKind.DELETED, at=now,
                                by=identity[:64], from_lot_id=item.location_id)
    journal.record(OperationKind.ITEM_DELETE, identity, f'{item.pack.item_type.name} {item.iid} : {reason}',
                   lot=item.location, at=now, items=[item.iid], reason=reason)
    item.status = ItemStatus.DELETED
    item.deleted = now
    item.deleted_by = identity
    item.deleted_reason = reason[:128]
    item.location = None
    item.touch(identity, SeenWhile.DELETE, now)
    item.save()
    notifications.check_stock_levels([item.pack.item_type_id])


def set_disabled(items, identity, disabled, reason=''):
    """Desactive (rappel produit, non-conformite) ou reactive des items. Desactive, un item est invalide comme un
    perime : il ne compte plus dans son lot et doit etre remplace (le remplacant le sort du lot a la verif)."""
    reason = str(reason or '').strip()[:128]
    if disabled and not reason:
        raise ValueError('Une raison est obligatoire pour désactiver un item')
    items = [item for item in items if item.disabled != bool(disabled)]
    if not items:
        return []
    now = timezone.now()
    with transaction.atomic():
        for item in items:
            item.disabled = bool(disabled)
            item.disabled_reason = reason if disabled else ''
            item.disabled_at = now if disabled else None
            item.disabled_by = identity[:64] if disabled else ''
            item.save(update_fields=['disabled', 'disabled_reason', 'disabled_at', 'disabled_by'])
        first = items[0]
        what = first.pack.item_type.name + (f' ({len(items)} items)' if len(items) > 1 else f' {first.iid}')
        if disabled:
            journal.record(OperationKind.ITEM_DISABLE, identity, f'{what} : {reason}', at=now,
                           items=[item.iid for item in items], reason=reason)
        else:
            journal.record(OperationKind.ITEM_ENABLE, identity, what, at=now, items=[item.iid for item in items])
    notifications.check_stock_levels(sorted({item.pack.item_type_id for item in items}))
    return items


def revise_item(item, identity, tasks=None, next_due=None, note=''):
    """Revision d'un item (materiel non consommable a reviser) : toutes ses taches de revision cochees. La prochaine
    revision est `next_due` (date saisie) ou, par defaut, aujourd'hui + la periodicite du type."""
    item_type = item.pack.item_type
    if not item_type.has_revision:
        raise ValueError("Ce type d'item n'a pas de révision")
    if item.status in (ItemStatus.DELETED, ItemStatus.REPLACED):
        raise ValueError("Item supprimé : pas de révision")
    expected = list(item_type.revision_tasks or [])
    done = [task for task in expected if task in (tasks or [])]
    if len(done) != len(expected):
        raise ValueError('Cochez toutes les tâches de la révision')
    now = timezone.now()
    today = timezone.localdate(now)
    due = next_due or add_months(today, item_type.revision_months)
    if due <= today:
        raise ValueError('La prochaine révision doit être après aujourd\'hui')
    note = str(note or '').strip()[:256]
    with transaction.atomic():
        ItemRevision.objects.create(item=item, at=now, by=identity[:64], tasks=done, next_due=due, note=note)
        item.revision_due = due
        item.revised_at = now
        item.revised_by = identity[:64]
        item.save(update_fields=['revision_due', 'revised_at', 'revised_by'])
        journal.record(OperationKind.REVISION, identity, f'{item_type.name} {item.iid} : prochaine révision le '
                       f'{due.strftime("%d/%m/%Y")}' + (f' ({note})' if note else ''), at=now, items=[item.iid],
                       next_due=due.isoformat())
    return item


def revisions_due(today=None, days=30):
    """Items actifs a reviser en retard ou dans les `days` jours, prochaine revision d'abord."""
    today = today or timezone.localdate()
    return list(Items.objects.select_related('pack__item_type', 'location')
                .filter(status__in=(ItemStatus.ACTIVE, ItemStatus.OUT, ItemStatus.LOOSE), revision_due__isnull=False,
                        revision_due__lte=today + timedelta(days=days), pack__item_type__revision_months__gt=0,
                        pack__item_type__consumable=False)
                .order_by('revision_due', 'iid'))


def restore(item, identity):
    now = timezone.now()
    movements.found_again([item.iid], now, identity)
    ItemMovement.objects.create(item=item, item_type_id=item.pack.item_type_id, kind=MovementKind.RESTORED, at=now,
                                by=identity[:64], to_lot_id=item.location_id)
    journal.record(OperationKind.ITEM_RESTORE, identity, f'{item.pack.item_type.name} {item.iid}', lot=item.location,
                   at=now, items=[item.iid])
    item.status = ItemStatus.ACTIVE
    item.deleted = None
    item.deleted_by = ''
    item.deleted_reason = ''
    item.missed_verifs = 0
    item.touch(identity, SeenWhile.RESTORE)
    item.save()
    notifications.check_stock_levels([item.pack.item_type_id])


def stock_status(soon_days=30):
    """Etat des stocks par type d'item (une ligne par ItemType)."""
    today = timezone.localdate()
    soon = today + timedelta(days=soon_days)
    items = 'itemspacks__items'
    active = Q(**{f'{items}__status': ItemStatus.ACTIVE})
    # un item desactive (rappel produit, non-conformite) compte avec les perimes
    disabled = Q(**{f'{items}__disabled': True}) | Q(**{f'{items}__revision_due__lt': today, 'revision_months__gt': 0,
                                                         'consumable': False, 'revision_usable': False})
    fresh = (Q(itemspacks__peremption__isnull=True) | Q(itemspacks__peremption__gte=today)) & ~disabled
    expired = Q(itemspacks__peremption__lt=today) | disabled
    # les rangements du stock (armoires, tiroirs) comptent dans le stock, pas dans les lots
    in_stock = in_stock_q(f'{items}__')
    in_lot = Q(**{f'{items}__location__isnull': False, f'{items}__location__lot_type__storage': False})
    soon_filter = Q(itemspacks__peremption__gte=today) & Q(itemspacks__peremption__lte=soon)
    queryset = ItemType.objects.filter(archived=False, physical=True).order_by('name').annotate(
        stock_fresh=Count(items, filter=active & fresh & in_stock),
        stock_expired=Count(items, filter=active & expired & in_stock),
        lots_fresh=Count(items, filter=active & fresh & in_lot),
        lots_expired=Count(items, filter=active & expired & in_lot),
        expiring_soon=Count(items, filter=active & soon_filter),
        # parts du stock / des lots non perimes qui periment bientot (tri « bientot vide »)
        stock_expiring_soon=Count(items, filter=active & fresh & soon_filter & in_stock),
        lots_expiring_soon=Count(items, filter=active & fresh & soon_filter & in_lot),
        missing=Count(items, filter=Q(**{f'{items}__status': ItemStatus.MISSING})),
        out=Count(items, filter=Q(**{f'{items}__status': ItemStatus.OUT})),
        loose=Count(items, filter=Q(**{f'{items}__status': ItemStatus.LOOSE})),
    )
    rows = [
        {
            'type': row.type,
            'name': row.name,
            'min_quantity': row.min_quantity,
            'perissable': row.perissable,
            'stock_fresh': row.stock_fresh,
            'stock_expired': row.stock_expired,
            'lots_fresh': row.lots_fresh,
            'lots_expired': row.lots_expired,
            'expiring_soon': row.expiring_soon,
            'stock_expiring_soon': row.stock_expiring_soon,
            'lots_expiring_soon': row.lots_expiring_soon,
            'missing': row.missing,
            'out': row.out,
            'loose': row.loose,
            # attentes en quantite : un type a unites compte en unites ce que ses sous-types remplissent
            'unit': row.unit,
            'group': row.group_id or '',
            'fills': row.fills,
            'default_subtype': row.default_subtype_id or '',
        }
        for row in queryset
    ]
    return with_unit_totals(rows)


STOCK_COUNTS = ('stock_fresh', 'stock_expired', 'lots_fresh', 'lots_expired', 'expiring_soon', 'stock_expiring_soon',
                'lots_expiring_soon', 'missing', 'out', 'loose')


def with_unit_totals(rows):
    """Lignes de l'etat des stocks : celle d'un type a unites (« Sérum phy », mL) additionne les unites de ses
    sous-types (2 sachets de 25 mL + 1 flacon de 50 mL : 100 mL) ; son minimum est en unites, celui de chaque
    sous-type en items."""
    by_type = {row['type']: row for row in rows}
    for row in rows:
        group = by_type.get(row['group'])
        if group is None or not group['unit']:
            continue
        for key in STOCK_COUNTS:
            group[key] += row[key] * max(1, row['fills'])
    return rows
