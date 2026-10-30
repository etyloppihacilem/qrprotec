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
from dataclasses import dataclass
from datetime import timedelta

from django.db import transaction
from django.db.models import Count, Q
from django.utils import timezone

from . import journal, movements, notifications, webpush
from .idendity import ANONYMOUS_OPENING, SOURCE_DECLARED, parse_identity, short_identity_name
from .models import (
    ItemMovement, ItemRevision, ItemStatus, Items, ItemType, LotContent, LotRequirements, Lots, MovementKind, OperationKind,
    SeenWhile, VerifItem, Operation, VerifResult, Verifs, add_months, generate_key, name_key, qrprotec_setting,
    revision_blocked_q,
)


def display_name(identity):
    return parse_identity(identity)['display_name'] if identity else '?'


def _items_queryset():
    return Items.objects.select_related('pack__item_type', 'location__lot_type')


def _location_filter(lot):
    return Q(location=lot) if lot is not None else Q(location__isnull=True)


def in_stock_q(prefix=''):
    """Items en stock : hors de tout lot, ou ranges dans un rangement du stock (armoire, tiroir...)."""
    return Q(**{f'{prefix}location__isnull': True}) | Q(**{f'{prefix}location__lot_type__storage': True})


def clean_location(location):
    return ' '.join(str(location or '').split())[:64]


@dataclass
class Requirement:
    """Exigence d'un lot : celle de son type, avec les items en plus du lot (extra) et son emplacement dans ce lot."""
    item_type: ItemType
    quantity: int
    location: str
    extra: int = 0

    @property
    def item_type_id(self):
        return self.item_type.pk


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


def lot_requirements(lot):
    """Contenu attendu d'un lot (Requirement) : celui de son type, plus son contenu propre (LotContent : items en
    plus, emplacements de ce lot), dans l'ordre des emplacements du lot (Lots.locations, sinon ceux du type)."""
    if lot.lot_type.storage:
        return []  # un rangement du stock n'attend rien
    own = {row.item_type_id: row for row in LotContent.objects.select_related('item_type').filter(lot=lot)}
    rows = []
    for requirement in LotRequirements.objects.select_related('item_type').filter(lot_type_id=lot.lot_type_id):
        custom = own.pop(requirement.item_type_id, None)
        location = requirement.location if custom is None or custom.location is None else custom.location
        extra = custom.extra if custom else 0
        rows.append(Requirement(requirement.item_type, requirement.quantity + extra, location, extra))
    for custom in own.values():
        if custom.extra > 0:
            rows.append(Requirement(custom.item_type, custom.extra, custom.location or '', custom.extra))
    return sort_requirements(rows, lot_locations(lot))


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


def requirements_status(lot, today=None):
    """Compare le contenu d'un lot aux exigences de son type.

    Seuls les items vus lors de la derniere verif (ou ajoutes depuis) comptent comme presents : un item
    manque a la derniere verif est encore attendu mais n'est plus considere comme fiable.
    """
    today = today or timezone.localdate()
    fresh = defaultdict(int)
    expired = defaultdict(int)
    unconfirmed = defaultdict(int)
    for item in _items_queryset().filter(location=lot, status=ItemStatus.ACTIVE):
        if item.missed_verifs > 0:
            unconfirmed[item.pack.item_type_id] += 1
        elif item.needs_replacement(today):
            expired[item.pack.item_type_id] += 1
        else:
            fresh[item.pack.item_type_id] += 1
    rows = []
    for requirement in lot_requirements(lot):
        type_id = requirement.item_type_id
        rows.append({
            'type': type_id,
            'type_name': requirement.item_type.name,
            'location': requirement.location,
            'required': requirement.quantity,
            'extra': requirement.extra,  # dont en plus du type de lot (contenu propre du lot)
            'present': fresh.get(type_id, 0),
            'expired': expired.get(type_id, 0),
            'unconfirmed': unconfirmed.get(type_id, 0),
        })
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


def _required_by_lot(group):
    """{lot id: {type d'item: quantite}} d'apres le contenu attendu de chaque lot (son type et son contenu propre)."""
    return {lot.id: {row.item_type_id: row.quantity for row in lot_requirements(lot)} for lot in group}


def assign_items(group, scanned, today, places=None):
    """Lot de destination de chaque item scanne pendant une verif de plusieurs lots.

    Un item deja range dans un des lots y reste (on l'a trouve la, meme scanne pendant qu'on verifiait un autre
    lot). Un item venu d'ailleurs (stock, autre lot hors de la verif) va dans le lot dont l'etiquette (publique ou
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

    required = _required_by_lot(group)
    deficit = {lot_id: dict(rows) for lot_id, rows in required.items()}
    target = {}
    newcomers = []
    for item in scanned:
        if item.location_id in ids:
            target[item.iid] = item.location_id
            type_id = item.pack.item_type_id
            if not item.needs_replacement(today) and deficit[item.location_id].get(type_id, 0) > 0:
                deficit[item.location_id][type_id] -= 1
        else:
            newcomers.append(item)
    # items frais d'abord : ce sont eux qui remplissent les manques
    newcomers.sort(key=lambda item: (item.needs_replacement(today), item.iid))
    for item in newcomers:
        type_id = item.pack.item_type_id
        place = places.get(item.iid)
        scope = [lot.id for lot in group if place not in ids or under(lot.id, place)]
        candidates = [lot_id for lot_id in scope if type_id in required[lot_id]]
        choice = None
        if not item.needs_replacement(today):
            choice = next((lot_id for lot_id in candidates if deficit[lot_id].get(type_id, 0) > 0), None)
            if choice is not None:
                deficit[choice][type_id] -= 1
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


def perform_verif(lot, iids, identity, partial=False, participants=(), places=None, sealed=(), exclude=()):
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
    - exclude : sous-lots des lots verifies laisses de cote avec leurs propres sous-lots (vérif en cascade : deja
      valides pendant cette vérif, pas encore faits, ou faits par un autre participant). Ils ne sont ni attendus, ni
      verifies, ni ouverts.
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

        # Destination de chaque item scanne
        if group is None:
            target = {
                item.iid: item.location_id if item.location_id and item.location.lot_type.storage else None
                for item in scanned
            }
        else:
            target = assign_items(group, scanned, today, places)

        # Detection des remplacements, par lot et par type. Un perime (inutilisable, desactive) scanne est retire du
        # lot, a jeter, des que le lot a son minimum d'items frais de ce type (meme deja presents avant la verif) ;
        # sinon il reste a remplacer. Un item a la revision en retard n'est pas jete : seulement remplace par un
        # item frais nouvellement arrive. Meme regle dans les fronts (planSession, App::plan_verif).
        required = _required_by_lot(group) if group is not None else {}
        fresh = defaultdict(int)
        new_fresh = defaultdict(int)
        expired_by_key = defaultdict(list)
        for item in scanned:
            key = (target[item.iid], item.pack.item_type_id)
            if item.needs_replacement(today):
                expired_by_key[key].append(item)
            else:
                fresh[key] += 1
                if item.location_id != target[item.iid] or item.status != ItemStatus.ACTIVE:
                    new_fresh[key] += 1
        replaced = set()
        for key, expired_items in expired_by_key.items():
            lot_id, type_id = key
            covered = fresh[key] >= required.get(lot_id, {}).get(type_id, 0)
            slots = new_fresh.get(key, 0)
            for item in expired_items:
                if covered and discardable(item, today):
                    replaced.add(item.iid)
                elif slots > 0:
                    replaced.add(item.iid)
                    slots -= 1

        # Lots verifies : tous, ou seulement ceux que la verif partielle rend complets
        if group is None:
            verified_ids = set()
        elif partial:
            verified_ids = _complete_lots(group, scanned, not_seen, target, today)
        else:
            verified_ids = set(by_id)
        if group is not None and partial and not verified_ids and not scanned and not intact:
            raise ValueError('Rien à valider : aucun item scanné')
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
                                                   participants=participants)

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

        Items.objects.bulk_update(
            scanned, ['status', 'location', 'missed_verifs', 'last_seen', 'last_seen_by', 'last_seen_while']
        )
        Items.objects.bulk_update(not_seen, ['status', 'missed_verifs'])
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
                sub_lot.back_from_list = ''
                sub_lot.marked_used = None
                sub_lot.marked_used_by = ''
                sub_lot.save(update_fields=['last_verif', 'last_verif_by', 'last_verif_while', 'last_used',
                                            'last_used_by', 'verif_recommended', 'restocked_count',
                                            'back_from_list', 'marked_used', 'marked_used_by'])
                requirements = requirements_status(sub_lot, today)
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
                    fields = ['last_used', 'last_used_by']
                    if not sub_lot.lot_type.storage:
                        # reassort sans verif complete : la prochaine personne devra verifier tout le lot
                        sub_lot.verif_recommended = True
                        sub_lot.restocked = now
                        sub_lot.restocked_by = identity
                        sub_lot.restocked_count += restocked[sub_lot.id]
                        fields += ['verif_recommended', 'restocked', 'restocked_by', 'restocked_count']
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
    Items.objects.bulk_update(items, ['location', 'status', 'missed_verifs'])


def _record_verif(lot, identity, now, verif, requirements, partial, missing, expired):
    """Verif d'un lot (ou du stock, lot None) dans le journal des operations."""
    counts = [(verif.present_count, 'présent'), (verif.missing_count, 'manquant'), (verif.expired_count, 'périmé'),
              (verif.replaced_count, 'remplacé')]
    parts = [journal.plural(count, word) for count, word in counts if count]
    lacking = [{'type': row['type'], 'type_name': row['type_name'], 'required': row['required'],
                'present': row['present']} for row in requirements if row['present'] < row['required']]
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


def _complete_lots(group, scanned, not_seen, target, today):
    """Lots que les items scannes rendent complets (verif partielle). Un lot sans aucun item scanne n'est retenu
    que s'il ne contient rien : sinon tout son contenu serait signale manquant. Un rangement du stock n'a pas de
    contenu attendu : ses items sont ses attendus, il n'est complet que si tous ont ete scannes. Un item a
    etiquette a dechirer (utilise) ou non consommable (passe hors lot) non scanne ne compte pas. Meme regle dans
    les fronts (planSession, App::plan_verif)."""
    required = _required_by_lot(group)
    fresh = defaultdict(lambda: defaultdict(int))
    touched = set()
    for item in scanned:
        destination = target[item.iid]
        touched.add(destination)
        if not item.needs_replacement(today):
            fresh[destination][item.pack.item_type_id] += 1
    # un item a dechirer ou non consommable (passe hors lot) non scanne ne retient pas son lot
    holding = {item.location_id for item in not_seen if not item.pack.item_type.tear_off and not leaves_lot(item)}
    complete = set()
    for lot in group:
        if lot.id in holding and (lot.id not in touched or lot.lot_type.storage):
            continue
        # un perime en trop ne retient pas le lot : seul compte le minimum d'items frais
        if all(fresh[lot.id][type_id] >= quantity for type_id, quantity in required[lot.id].items()):
            complete.add(lot.id)
    return complete


def notify_verif_problem(lot, identity, requirements, report, newly_missing):
    """Vérif incomplète : notification web (type verif_problem, désactivé par défaut), qui ouvre la fiche du lot."""
    parts = []
    lacking = [f"{row['required'] - row['present']} {row['type_name']}"
               for row in requirements if row['present'] < row['required']]
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
        target.save(update_fields=['verif_recommended', 'marked_used', 'marked_used_by', 'last_used', 'last_used_by'])
        marked.append(target)
    if not marked:
        raise ValueError('Rien à marquer : rangement du stock ou sous-lots scellés')
    others = [target.name for target in marked if target.id != lot.id]
    journal.record(OperationKind.LOT_USED, identity,
                   'vérif recommandée' + (f" (avec {', '.join(others)})" if others else ''), lot=lot, at=now,
                   lots=[target.id for target in marked])
    return marked


def move_items(iids, lot, identity, context):
    """Deplace des items dans un lot (ou en stock si lot est None) sans toucher aux autres."""
    now = timezone.now()
    requested = list(dict.fromkeys(iids))
    with transaction.atomic():
        items = list(Items.objects.select_for_update(of=('self',)).select_related('pack__item_type', 'location')
                     .filter(iid__in=requested))
        found = {item.iid for item in items}
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
        Items.objects.bulk_update(items, ['location', 'status', 'missed_verifs', 'last_seen', 'last_seen_by', 'last_seen_while'])
        if lot is not None:
            lot.last_used = now
            lot.last_used_by = identity
            fields = ['last_used', 'last_used_by']
            if items and not lot.lot_type.storage:
                # reassort sans verif complete : la prochaine personne devra verifier tout le lot
                # (un rangement du stock n'a pas de contenu attendu : y ranger des items est l'usage normal)
                lot.verif_recommended = True
                lot.restocked = now
                lot.restocked_by = identity
                lot.restocked_count += len(items)
                fields += ['verif_recommended', 'restocked', 'restocked_by', 'restocked_count']
            lot.save(update_fields=fields)
        notifications.check_stock_levels({item.pack.item_type_id for item in items})
    return {'moved': [iid for iid in requested if iid in found], 'unknown': [iid for iid in requested if iid not in found]}


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
        Items.objects.bulk_update(out, ['location', 'status', 'missed_verifs', 'last_seen', 'last_seen_by',
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
        Items.objects.bulk_update(expired, ['status', 'missed_verifs'])
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
    queryset = ItemType.objects.filter(archived=False).order_by('name').annotate(
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
    return [
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
        }
        for row in queryset
    ]
