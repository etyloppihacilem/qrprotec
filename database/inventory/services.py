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
from datetime import timedelta

from django.db import transaction
from django.db.models import Count, Q
from django.utils import timezone

from . import journal, movements, notifications
from .idendity import SOURCE_DECLARED, parse_identity
from .models import (
    ItemMovement, ItemStatus, Items, ItemType, LotRequirements, Lots, MovementKind, OperationKind, SeenWhile, VerifItem,
    Operation, VerifResult, Verifs, generate_key, qrprotec_setting,
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
        elif item.is_expired(today):
            expired[item.pack.item_type_id] += 1
        else:
            fresh[item.pack.item_type_id] += 1
    rows = []
    requirements = LotRequirements.objects.select_related('item_type').filter(lot_type=lot.lot_type)
    for requirement in requirements.order_by('location', 'item_type__name'):
        type_id = requirement.item_type_id
        rows.append({
            'type': type_id,
            'type_name': requirement.item_type.name,
            'location': requirement.location,
            'required': requirement.quantity,
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
    """{lot id: {type d'item: quantite}} d'apres les exigences du type de chaque lot."""
    by_type = defaultdict(dict)
    for row in LotRequirements.objects.filter(lot_type_id__in={lot.lot_type_id for lot in group}):
        by_type[row.lot_type_id][row.item_type_id] = row.quantity
    return {lot.id: dict(by_type.get(lot.lot_type_id, {})) for lot in group}


def assign_items(group, scanned, today):
    """Lot de destination de chaque item scanne pendant une verif de plusieurs lots.

    Un item deja range dans un des lots y reste. Un item venu d'ailleurs (stock, autre lot) va dans le
    premier lot qui en attend encore, sinon dans le premier lot qui en contient dans sa definition, sinon dans
    le premier lot de la verif. Meme regle dans les fronts (aperçu de la vérif partielle).
    """
    ids = {lot.id for lot in group}
    required = _required_by_lot(group)
    deficit = {lot_id: dict(rows) for lot_id, rows in required.items()}
    target = {}
    newcomers = []
    for item in scanned:
        if item.location_id in ids:
            target[item.iid] = item.location_id
            type_id = item.pack.item_type_id
            if not item.is_expired(today) and deficit[item.location_id].get(type_id, 0) > 0:
                deficit[item.location_id][type_id] -= 1
        else:
            newcomers.append(item)
    # items frais d'abord : ce sont eux qui remplissent les manques
    newcomers.sort(key=lambda item: (item.is_expired(today), item.iid))
    for item in newcomers:
        type_id = item.pack.item_type_id
        candidates = [lot.id for lot in group if type_id in required[lot.id]]
        choice = None
        if not item.is_expired(today):
            choice = next((lot_id for lot_id in candidates if deficit[lot_id].get(type_id, 0) > 0), None)
            if choice is not None:
                deficit[choice][type_id] -= 1
        if choice is None:
            choice = candidates[0] if candidates else group[0].id
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


def perform_verif(lot, iids, identity, partial=False):
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
      'disparu' (l'etiquette est arrachee a l'ouverture du sachet, les restes du sachet sont perdus).
    """
    now = timezone.now()
    today = timezone.localdate()
    requested = list(dict.fromkeys(iids))
    threshold = qrprotec_setting('MISSING_AFTER_VERIFS')
    if lot is None:
        group = None
    else:
        group = verif_lots(lot if isinstance(lot, (list, tuple)) else [lot])
    by_id = {sub_lot.id: sub_lot for sub_lot in group or []}

    with transaction.atomic():
        scanned = list(_items_queryset().select_for_update(of=('self',)).filter(iid__in=requested).order_by('iid'))
        scanned_ids = {item.iid for item in scanned}
        unknown = [iid for iid in requested if iid not in scanned_ids]
        scope = Q(location_id__in=list(by_id)) if group is not None else Q(location__isnull=True)
        not_seen = list(
            _items_queryset().select_for_update(of=('self',))
            .filter(scope, status__in=[ItemStatus.ACTIVE, ItemStatus.MISSING])
            .exclude(iid__in=scanned_ids)
            .order_by('iid')
        )

        # Destination de chaque item scanne
        if group is None:
            target = {
                item.iid: item.location_id if item.location_id and item.location.lot_type.storage else None
                for item in scanned
            }
        else:
            target = assign_items(group, scanned, today)

        # Detection des remplacements : par lot et par type, nombre d'items frais nouvellement arrives
        new_fresh = defaultdict(int)
        expired_by_key = defaultdict(list)
        for item in scanned:
            key = (target[item.iid], item.pack.item_type_id)
            if item.is_expired(today):
                expired_by_key[key].append(item)
            elif item.location_id != target[item.iid] or item.status != ItemStatus.ACTIVE:
                new_fresh[key] += 1
        replaced = set()
        for key, expired_items in expired_by_key.items():
            for item in expired_items[:new_fresh.get(key, 0)]:
                replaced.add(item.iid)

        # Lots verifies : tous, ou seulement ceux que la verif partielle rend complets
        if group is None:
            verified_ids = set()
        elif partial:
            verified_ids = _complete_lots(group, scanned, not_seen, target, replaced, today)
        else:
            verified_ids = set(by_id)
        if group is not None and partial and not verified_ids and not scanned:
            raise ValueError('Rien à valider : aucun item scanné')
        not_seen = [item for item in not_seen if group is None or item.location_id in verified_ids]

        # Scelles : lots verifies, lots ou des items sont ajoutes, lots d'ou des items sont retires, et leurs parents
        opened_ids = set(verified_ids)
        opened_ids |= {target[item.iid] for item in scanned if target[item.iid] is not None}
        opened_ids |= {item.location_id for item in scanned if item.location_id and item.location_id != target[item.iid]}
        opened = list(Lots.objects.select_related('parent').filter(id__in=opened_ids))
        unsealed = []
        for sealed in _with_ancestors(opened):
            if sealed.is_sealed:
                if sealed.id in verified_ids:
                    reason = 'vérif'
                elif sealed.id in by_id:
                    reason = 'ajout'
                else:
                    reason = 'retrait' if sealed.id in opened_ids else 'ouverture d\'un sous-lot'
                break_seal(sealed, identity, reason, now)
                unsealed.append(sealed.name)
                if sealed.id in by_id:
                    by_id[sealed.id].refresh_from_db(fields=['is_sealed', 'seal_code', 'unsealed', 'unsealed_by'])
        was_missing = {item.iid for item in not_seen if item.status == ItemStatus.MISSING}

        verifs = {}
        if group is None:
            verifs[None] = Verifs.objects.create(lot=None, datetime=now, by=identity)
        for lot_id in verified_ids:
            verifs[lot_id] = Verifs.objects.create(lot=by_id[lot_id], datetime=now, by=identity)

        report = {'present': [], 'expired': [], 'replaced': [], 'missing': [], 'unknown': unknown, 'reactivated': [],
                  'restocked': [], 'torn': []}
        entries = []
        moves = []
        origin = {item.iid: item.location_id for item in scanned}
        # items retrouves apres avoir ete notes absents : leur absence ne compte plus comme une utilisation
        movements.found_again([item.iid for item in scanned if movements.may_be_absent(item)], now, identity)
        counts = defaultdict(lambda: defaultdict(int))  # lot id -> compteurs de la verif
        restocked = defaultdict(int)
        restocked_items = defaultdict(list)  # lot id -> items ranges par reassort (journal)
        for item in scanned:
            destination = target[item.iid]
            expired = item.is_expired(today)
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
                entries.append(VerifItem(verif=verif, item=item, result=VerifResult.PRESENT, expired=expired))
        for item in not_seen:
            expired = item.is_expired(today)
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
                sub_lot.save(update_fields=['last_verif', 'last_verif_by', 'last_verif_while', 'last_used',
                                            'last_used_by', 'verif_recommended', 'restocked_count'])
                requirements = requirements_status(sub_lot, today)
                lot_expired = [iid for iid in report['expired'] if target.get(iid) == sub_lot.id]
                lot_complete = all(r['present'] >= r['required'] for r in requirements) and not lot_expired
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

        affected = {item.pack.item_type_id for item in scanned} | {item.pack.item_type_id for item in not_seen}
        notifications.check_stock_levels(affected)

    verified_rows = [row for row in lots_report if row['verified']]
    report['lots'] = lots_report
    report['partial'] = bool(group) and len(verified_rows) < len(lots_report)
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
    journal.record(OperationKind.STOCK_VERIF if lot is None else OperationKind.VERIF, identity, summary, lot=lot,
                   at=now, verif=verif.id, complete=verif.complete, partial=partial, present=verif.present_count,
                   missing=verif.missing_count, expired=verif.expired_count, replaced=verif.replaced_count,
                   lacking=lacking, missing_items=missing[:journal.MAX_LISTED_ITEMS],
                   expired_items=expired[:journal.MAX_LISTED_ITEMS])


def _complete_lots(group, scanned, not_seen, target, replaced, today):
    """Lots que les items scannes rendent complets (verif partielle). Un lot sans aucun item scanne n'est retenu
    que s'il ne contient rien : sinon tout son contenu serait signale manquant. Un item a etiquette a dechirer
    non scanne ne compte pas (il a ete utilise). Meme regle dans les fronts (planSession, App::plan_verif)."""
    required = _required_by_lot(group)
    fresh = defaultdict(lambda: defaultdict(int))
    expired = defaultdict(int)
    touched = set()
    for item in scanned:
        destination = target[item.iid]
        touched.add(destination)
        if item.iid in replaced:
            continue
        if item.is_expired(today):
            expired[destination] += 1
        else:
            fresh[destination][item.pack.item_type_id] += 1
    holding = {item.location_id for item in not_seen if not item.pack.item_type.tear_off}
    complete = set()
    for lot in group:
        if lot.id not in touched and lot.id in holding:
            continue
        if expired[lot.id]:
            continue
        if all(fresh[lot.id][type_id] >= quantity for type_id, quantity in required[lot.id].items()):
            complete.add(lot.id)
    return complete


def notify_verif_problem(lot, identity, requirements, report, newly_missing):
    parts = []
    lacking = [f"{row['required'] - row['present']} {row['type_name']}"
               for row in requirements if row['present'] < row['required']]
    if lacking:
        parts.append('manque ' + ', '.join(lacking))
    if report['expired']:
        parts.append(f"{len(report['expired'])} périmé(s)")
    if newly_missing:
        parts.append(f'{len(newly_missing)} item(s) signalé(s) disparu(s)')
    notifications.notify(
        'verif_problem', f'vérif du lot {lot.name} incomplète ({display_name(identity)}) : ' + ' ; '.join(parts)
    )


def seal_lot(lot, identity, seal_number='', force=False):
    """Scelle un lot : il reste valide sans verif tant que le scelle n'est pas brise."""
    if not lot.active:
        raise ValueError('Lot inactif')
    if not force:
        # un lot global est scelle avec ses sous-lots : tous doivent etre complets
        today = timezone.localdate()
        for sub_lot in lot.descendants():
            rows = requirements_status(sub_lot, today)
            expired = Items.objects.filter(location=sub_lot, status=ItemStatus.ACTIVE, pack__peremption__lt=today).exists()
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


def break_seal(lot, identity, reason, now=None):
    """Brise le scelle d'un lot (ouverture, vérif, ajout ou retrait d'items)."""
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
    notifications.notify('seal_broken', f'scellé{number} du lot {lot.name} ouvert ({reason}) par {display_name(identity)}')
    return True


# Scelle ouvert par l'etiquette d'ouverture sans utilisateur identifie
SEAL_OPENED_ANONYMOUSLY = f"{SOURCE_DECLARED}:Anonyme (étiquette intérieure)"
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
        opened = []
        for sealed in _with_ancestors([lot]):
            if sealed.is_sealed:
                reason = "étiquette d'ouverture" if sealed.id == lot.id else "ouverture d'un sous-lot"
                break_seal(sealed, identity or SEAL_OPENED_ANONYMOUSLY, reason, now)
                opened.append(sealed)
        # le contenu a pu etre utilise : verif recommandee (orange) pour le lot, ses sous-lots (ils etaient sous
        # le scelle) et les lots parents ouverts avec lui
        recommended = {sub_lot.id: sub_lot for sub_lot in lot.descendants()}
        recommended.update({sealed.id: sealed for sealed in opened})
        for target in recommended.values():
            if not target.verif_recommended:
                target.verif_recommended = True
                target.save(update_fields=['verif_recommended'])
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
        return 'signed'
    return 'already'


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
        for sealed in _with_ancestors(Lots.objects.select_related('parent').filter(id__in=touched_lots)):
            if not sealed.is_sealed:
                continue
            if lot is not None and sealed.id == lot.id:
                reason = 'ajout'
            else:
                reason = 'retrait' if sealed.id in touched_lots else "ouverture d'un sous-lot"
            break_seal(sealed, identity, reason, now)
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
    expire_out_items.
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
        'until': out_deadline(out[0], now).isoformat() if len(out) == 1 else None,
        'days': qrprotec_setting('OUT_DAYS'),
    }


def out_deadline(item, since=None):
    """Date a laquelle un item sorti non revu compte comme utilise : OUT_DAYS jours apres sa sortie, ou sa
    peremption si elle tombe avant."""
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
            if out_deadline(item) <= today:
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
                       f"{journal.types_summary(expired)} : non revu(s) après la sortie, compté(s) utilisé(s)",
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
    fresh = Q(itemspacks__peremption__isnull=True) | Q(itemspacks__peremption__gte=today)
    expired = Q(itemspacks__peremption__lt=today)
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
        missing=Count(items, filter=Q(**{f'{items}__status': ItemStatus.MISSING})),
        out=Count(items, filter=Q(**{f'{items}__status': ItemStatus.OUT})),
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
            'missing': row.missing,
            'out': row.out,
        }
        for row in queryset
    ]
