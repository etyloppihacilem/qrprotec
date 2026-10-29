# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          checklists.py
#       -\-    _|__
#        |\___/  . \        Created on 16 Oct. 2026 at 08:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Listes de materiel a prendre (ex : un chef demande du materiel a ses equipiers).

Une liste a des lignes : des lots, des types d'items avec une quantite, et des taches libres a cocher (ni lot ni item,
ex : « vérifier le niveau d'eau dans le sous-sol »). Elle se partage par lien et QR code
(`<base>/liste?id=ID`) et se verifie en direct, a plusieurs : chaque scan est enregistre tout de suite sur le serveur
(ChecklistScan, phase out), les pages relisent la liste. Validee, ses items sont sortis du stock (statut « sorti », sans
lot) et elle ne peut plus etre verifiee. Au retour, on scanne ce qui rentre (phase back) : l'item revient en stock tout
de suite. « Clôturer le retour » compte les consommables non rentres comme utilises ; un non consommable reste sorti
jusqu'a ce qu'on le revoie. Sans retour clos, la regle des sorties s'applique (utilise apres OUT_DAYS jours).

Droits : les chefs, la gestion et les admins (LIST_ROLES) creent des listes, voient toutes les listes et tous les
modeles (partages entre eux). Un chef modifie ses listes et tous les modeles, la gestion et les admins modifient tout.
Un secouriste ne cree pas de liste et ne voit que celles qui lui sont attribuees (members) : un chef l'y ajoute, ou il
ouvre son lien ou son QR code (il peut toujours participer a une liste dont il a le lien, et la partager). Il ne voit
pas les modeles. Un modele (is_template) ne se verifie pas : on le duplique, et il n'a pas de secouristes attribues.
"""

from django.db import transaction
from django.utils import timezone

from . import journal, movements, notifications, services
from django.db.models import Q

from .idendity import SOURCE_VERIFIED, _fold, format_identity_verified, identity_name
from .models import (
    Checklist, ChecklistLine, ChecklistScan, ChecklistStatus, ItemMovement, ItemStatus, Items, ItemType, Lots,
    MovementKind, OperationKind, Role, Secouristes, SeenWhile,
)
from .serializers import _date, public_url

MAX_LINES = 100
MAX_QUANTITY = 999
MAX_MEMBERS = 100
LIST_ROLES = (Role.CHEF, Role.GESTION, Role.ADMIN)  # creent des listes, voient toutes les listes et les modeles


def identity_of(user):
    return f'{SOURCE_VERIFIED}:{user.matricule}'


def can_make_lists(user):
    return user.role in LIST_ROLES


def can_edit(checklist, user):
    """Gestion et admin : tout ; chef : ses listes et tous les modeles (partages) ; l'auteur garde la main sur les
    siennes meme s'il n'est plus chef."""
    if user.role in (Role.GESTION, Role.ADMIN) or checklist.owner_id == user.matricule:
        return True
    return user.role == Role.CHEF and checklist.is_template


def _require_edit(checklist, user):
    if not can_edit(checklist, user):
        raise PermissionError("Seul l'auteur de la liste, la gestion ou un admin peut la modifier")


def _require_make(user):
    if not can_make_lists(user):
        raise PermissionError('Seuls les chefs, la gestion et les admins créent des listes')


def is_member(checklist, user):
    return checklist.owner_id == user.matricule or any(
        member.matricule == user.matricule for member in checklist.members.all())


def access(checklist, user, join=False):
    """Ouverture d'une liste par son identifiant (lien, QR code, ecran des listes). Un secouriste n'ouvre pas les
    modeles ; une autre liste, il peut y participer : ouverte par son lien (`join`), elle s'ajoute a ses listes."""
    if can_make_lists(user):
        return
    if checklist.is_template:
        raise PermissionError('Modèle réservé aux chefs, à la gestion et aux admins')
    if join and not is_member(checklist, user):
        checklist.members.add(user)


def set_members(checklist, user, add=(), remove=()):
    """Secouristes attribues a la liste (pas a un modele) : elle apparait dans leurs listes."""
    _require_edit(checklist, user)
    if checklist.is_template:
        raise ValueError("Un modèle n'a pas de secouristes : dupliquez-le pour en faire une liste")
    add = [str(matricule) for matricule in (add or [])]
    found = list(Secouristes.objects.filter(matricule__in=add, active=True))
    if len(found) != len(set(add)):
        raise ValueError('Utilisateur inconnu ou inactif')
    if checklist.members.count() + len(found) > MAX_MEMBERS:
        raise ValueError(f'Au plus {MAX_MEMBERS} secouristes par liste')
    checklist.members.add(*found)
    checklist.members.remove(*Secouristes.objects.filter(matricule__in=[str(m) for m in (remove or [])]))


def search_users(user, query, limit=10):
    """Secouristes actifs dont un prenom commence par la recherche (sans casse ni accents), pour les attribuer a une
    liste. Noms courts sur le web (identity_name dans le contexte anonymise), jamais de recherche par nom."""
    _require_make(user)
    words = _fold(str(query or '')).split()
    if not words:
        return []
    rows = []
    for matricule, prenom in Secouristes.objects.filter(active=True).values_list('matricule', 'prenom'):
        parts = _fold(prenom).replace('-', ' ').split()
        if all(any(part.startswith(word) for part in parts) for word in words):
            rows.append(matricule)
    users = [{'matricule': matricule, 'name': identity_name(format_identity_verified(matricule))} for matricule in rows]
    users.sort(key=lambda row: _fold(row['name']))
    return users[:limit]


def _clean_text(value, length):
    return ' '.join(str(value or '').split())[:length]


def set_lines(checklist, lines):
    """Remplace les lignes : [{"lot": id} | {"type": code, "quantity": n} | {"task": texte}], dans l'ordre. Un lot, un
    type ou une tache n'apparait qu'une fois (les quantites d'un meme type s'ajoutent). Une tache deja cochee le reste."""
    if not isinstance(lines, list):
        raise ValueError('lines : liste de lignes attendue')
    checked = {line.task: line for line in checklist.lines.all() if line.task and line.done} if checklist.pk else {}
    rows, lots, types, tasks = [], set(), {}, set()
    for line in lines:
        if not isinstance(line, dict):
            raise ValueError('lines : chaque ligne est un lot, un type avec une quantité ou une tâche')
        if 'task' in line:
            task = _clean_text(line['task'], 200)
            if not task:
                raise ValueError('Tâche vide : écrivez ce qu\'il faut faire')
            if task not in tasks:
                tasks.add(task)
                old = checked.get(task)
                rows.append(ChecklistLine(checklist=checklist, task=task, quantity=1, done=old.done if old else None,
                                          done_by=old.done_by if old else ''))
        elif line.get('lot'):
            lot = Lots.objects.filter(id=str(line['lot']), active=True).first()
            if lot is None:
                raise ValueError(f"Lot inconnu ou archivé : {line['lot']}")
            if lot.id not in lots:
                lots.add(lot.id)
                rows.append(ChecklistLine(checklist=checklist, lot=lot, quantity=1))
        elif line.get('type'):
            item_type = ItemType.objects.filter(type=str(line['type']), archived=False).first()
            if item_type is None:
                raise ValueError(f"Type d'item inconnu ou archivé : {line['type']}")
            try:
                quantity = int(line.get('quantity', 1))
            except (TypeError, ValueError):
                raise ValueError('quantity : entier attendu')
            if quantity < 1 or quantity > MAX_QUANTITY:
                raise ValueError(f'Quantité entre 1 et {MAX_QUANTITY}')
            if item_type.type in types:
                types[item_type.type].quantity = min(MAX_QUANTITY, types[item_type.type].quantity + quantity)
            else:
                types[item_type.type] = ChecklistLine(checklist=checklist, item_type=item_type, quantity=quantity)
                rows.append(types[item_type.type])
        else:
            raise ValueError('lines : chaque ligne est un lot, un type avec une quantité ou une tâche')
    if len(rows) > MAX_LINES:
        raise ValueError(f'Au plus {MAX_LINES} lignes par liste')
    for position, row in enumerate(rows):
        row.position = position
    checklist.lines.all().delete()
    ChecklistLine.objects.bulk_create(rows)


def create(user, name, note='', template=False, lines=None):
    _require_make(user)
    name = _clean_text(name, 80)
    if not name:
        raise ValueError('Nom de la liste obligatoire')
    with transaction.atomic():
        checklist = Checklist.objects.create(name=name, note=str(note or '').strip()[:500], owner=user,
                                             is_template=bool(template))
        set_lines(checklist, lines or [])
    return checklist


def update(checklist, user, data):
    """Nom, remarque, modele, lignes : tant que la liste n'est pas validee."""
    _require_edit(checklist, user)
    if checklist.status != ChecklistStatus.DRAFT:
        raise ValueError('Liste déjà validée : elle ne se modifie plus (dupliquez-la)')
    with transaction.atomic():
        if 'name' in data:
            name = _clean_text(data['name'], 80)
            if not name:
                raise ValueError('Nom de la liste obligatoire')
            checklist.name = name
        if 'note' in data:
            checklist.note = str(data['note'] or '').strip()[:500]
        if 'template' in data:
            template = bool(data['template'])
            if template and checklist.scans.exists():
                raise ValueError('Liste déjà commencée : dupliquez-la pour en faire un modèle')
            checklist.is_template = template
        if 'lines' in data:
            set_lines(checklist, data['lines'])
            # un type retire ou une quantite baissee : les scans en trop sont retires
            _drop_extra_scans(checklist)
        checklist.updated = timezone.now()
        checklist.save()
    return checklist


def duplicate(checklist, user, name=None, template=False):
    """Copie (nouvelle liste a preparer, a l'utilisateur) : modele -> liste a verifier, ou liste a refaire."""
    lines = [{'task': line.task} if line.task else {'lot': line.lot_id} if line.lot_id
             else {'type': line.item_type_id, 'quantity': line.quantity}
             for line in checklist.lines.all() if (line.lot_id is None or line.lot.active)
             and (line.item_type_id is None or not line.item_type.archived)]
    _require_make(user)
    copy = create(user, name or checklist.name, checklist.note, template, lines)
    copy.source = checklist
    copy.save(update_fields=['source'])
    return copy


def delete(checklist, user):
    _require_edit(checklist, user)
    if checklist.status == ChecklistStatus.VALIDATED:
        raise ValueError('Liste sortie : faites son retour avant de la supprimer')
    checklist.delete()


def _out_scans(checklist):
    return [scan for scan in checklist.scans.all() if scan.phase == ChecklistScan.OUT]


def _drop_extra_scans(checklist):
    wanted = {line.item_type_id: line.quantity for line in checklist.lines.all() if line.item_type_id}
    lots = {line.lot_id for line in checklist.lines.all() if line.lot_id}
    counts = {}
    for scan in _out_scans(checklist):
        if scan.lot_id:
            if scan.lot_id not in lots:
                scan.delete()
            continue
        type_id = scan.item.pack.item_type_id
        counts[type_id] = counts.get(type_id, 0) + 1
        if counts[type_id] > wanted.get(type_id, 0):
            scan.delete()


def _require_open(checklist):
    if checklist.is_template:
        raise ValueError("C'est un modèle : dupliquez-le pour faire une liste à vérifier")
    if checklist.status != ChecklistStatus.DRAFT:
        raise ValueError('Liste déjà validée : elle ne peut plus être vérifiée')


def scan(checklist, user, iid='', lot_id=''):
    """Scan a la verif d'une liste : un item du stock d'un type demande (dans la quantite), ou l'etiquette d'un lot de
    la liste. Repond un message pour le scanneur."""
    _require_open(checklist)
    identity = identity_of(user)
    with transaction.atomic():
        checklist = Checklist.objects.select_for_update().get(id=checklist.id)
        _require_open(checklist)
        lines = list(checklist.lines.select_related('lot', 'item_type'))
        scans = _out_scans(checklist)
        if lot_id:
            line = next((line for line in lines if line.lot_id == lot_id), None)
            if line is None:
                raise ValueError("Ce lot n'est pas dans la liste")
            if any(row.lot_id == lot_id for row in scans):
                return f'{line.lot.name} : déjà coché'
            ChecklistScan.objects.create(checklist=checklist, lot=line.lot, by=identity)
            message = f'{line.lot.name} : coché'
        else:
            item = Items.objects.select_related('pack__item_type', 'location__lot_type').filter(iid=iid).first()
            if item is None:
                raise ValueError('Item inconnu')
            name = item.pack.item_type.name
            line = next((line for line in lines if line.item_type_id == item.pack.item_type_id), None)
            if line is None:
                raise ValueError(f"{name} : pas demandé dans cette liste")
            if any(row.item_id == item.iid for row in scans):
                return f'{name} : déjà scanné'
            if item.status != ItemStatus.ACTIVE or (item.location_id and not item.location.lot_type.storage):
                where = f'dans le lot {item.location.name}' if item.location_id else item.get_status_display().lower()
                raise ValueError(f'{name} : pas en stock ({where}), prenez-en un dans le stock')
            if item.needs_replacement():
                raise ValueError(f'{name} : périmé ou inutilisable, prenez-en un autre')
            other = (ChecklistScan.objects.filter(item=item, phase=ChecklistScan.OUT,
                                                  checklist__status=ChecklistStatus.DRAFT)
                     .exclude(checklist=checklist).select_related('checklist').first())
            if other is not None:
                raise ValueError(f'{name} : déjà pris pour la liste « {other.checklist.name} »')
            taken = sum(1 for row in scans if row.item_id and row.item.pack.item_type_id == line.item_type_id)
            if taken >= line.quantity:
                raise ValueError(f'{name} : déjà {line.quantity}/{line.quantity}, rien de plus à prendre')
            ChecklistScan.objects.create(checklist=checklist, item=item, by=identity)
            message = f'{name} : {taken + 1}/{line.quantity}'
        checklist.updated = timezone.now()
        checklist.save(update_fields=['updated'])
    return message


def unscan(checklist, user, iid='', lot_id=''):
    """Retire un scan (erreur de scan, item reposé)."""
    _require_open(checklist)
    query = checklist.scans.filter(phase=ChecklistScan.OUT)
    query = query.filter(lot_id=lot_id) if lot_id else query.filter(item_id=iid)
    if not query.delete()[0]:
        raise ValueError("Ce scan n'est pas dans la liste")
    checklist.updated = timezone.now()
    checklist.save(update_fields=['updated'])


def check_task(checklist, user, line_id, done=True):
    """Coche (ou decoche) une tache libre de la liste a sa verif."""
    _require_open(checklist)
    line = checklist.lines.filter(id=line_id).exclude(task='').first()
    if line is None:
        raise ValueError("Cette tâche n'est pas dans la liste")
    line.done = timezone.now() if done else None
    line.done_by = identity_of(user) if done else ''
    line.save(update_fields=['done', 'done_by'])
    checklist.updated = timezone.now()
    checklist.save(update_fields=['updated'])
    return f'{line.task} : ' + ('fait' if done else 'à faire')


def progress(checklist):
    """Avancement de chaque ligne a la verif : (ligne, scans) et complet. Une tache cochee compte comme un scan."""
    scans = _out_scans(checklist)
    rows = []
    for line in checklist.lines.all():
        if line.task:
            done = [line] if line.done else []
        elif line.lot_id:
            done = [scan for scan in scans if scan.lot_id == line.lot_id]
        else:
            done = [scan for scan in scans if scan.item_id and scan.item.pack.item_type_id == line.item_type_id]
        rows.append((line, done))
    complete = bool(rows) and all(len(done) >= line.quantity for line, done in rows)
    return rows, complete


def validate(checklist, user, partial=False):
    """Liste verifiee : ses items sortent du stock (statut sorti) et elle ne peut plus etre verifiee. Incomplete, il
    faut `partial` (ce qui manque n'est pas pris)."""
    _require_open(checklist)
    identity = identity_of(user)
    with transaction.atomic():
        checklist = Checklist.objects.select_for_update().get(id=checklist.id)
        _require_open(checklist)
        rows, complete = progress(checklist)
        if not any(done for _, done in rows):
            raise ValueError('Rien de fait : scannez le matériel ou cochez les tâches de la liste avant de la valider')
        if not complete and not partial:
            raise ValueError('Liste incomplète : validez-la incomplète ou finissez de la scanner')
        iids = [scan.item_id for scan in _out_scans(checklist) if scan.item_id]
        if iids:
            result = services.mark_out(iids, identity)
            refused = [scan for scan in _out_scans(checklist) if scan.item_id and scan.item_id not in result['out']]
            for scan_row in refused:
                scan_row.delete()  # plus en stock entre-temps : pas sorti avec la liste
        now = timezone.now()
        went_out = bool(_out_scans(checklist))
        checklist.status = ChecklistStatus.VALIDATED
        checklist.validated = now
        checklist.validated_by = identity
        if not went_out:
            # que des taches : rien n'est sorti, pas de retour a faire
            checklist.status = ChecklistStatus.RETURNED
            checklist.returned = now
            checklist.returned_by = identity
        checklist.updated = now
        checklist.save()
        journal.record(OperationKind.CHECKLIST, identity,
                       f'liste « {checklist.name} » ' + ('sortie' if went_out else 'faite')
                       + ('' if complete else ' (incomplète)'), at=now, checklist=checklist.id, items=iids)
    return checklist


def _require_return(checklist):
    if checklist.status != ChecklistStatus.VALIDATED:
        raise ValueError("Pas de retour à faire : la liste n'est pas sortie" if checklist.status == ChecklistStatus.DRAFT
                         else 'Retour déjà clos')


def return_scan(checklist, user, iid='', lot_id=''):
    """Retour : un item sorti avec la liste revient en stock tout de suite ; l'etiquette d'un lot de la liste le note
    rentre."""
    _require_return(checklist)
    identity = identity_of(user)
    with transaction.atomic():
        back = [scan for scan in checklist.scans.all() if scan.phase == ChecklistScan.BACK]
        if lot_id:
            line = checklist.lines.filter(lot_id=lot_id).select_related('lot').first()
            if line is None or not any(scan.lot_id == lot_id for scan in _out_scans(checklist)):
                raise ValueError("Ce lot n'est pas sorti avec la liste")
            if any(scan.lot_id == lot_id for scan in back):
                return f'{line.lot.name} : déjà rentré'
            ChecklistScan.objects.create(checklist=checklist, lot=line.lot, phase=ChecklistScan.BACK, by=identity)
            lot = line.lot
            if lot.is_sealed:
                message = f'{lot.name} : rentré, toujours scellé'
            else:
                # le contenu a pu servir : verif recommandee (orange) jusqu'a la prochaine verif
                now = timezone.now()
                lot.verif_recommended = True
                lot.back_from_list = checklist.name
                lot.restocked = now
                lot.restocked_by = identity
                lot.save(update_fields=['verif_recommended', 'back_from_list', 'restocked', 'restocked_by'])
                message = f'{lot.name} : rentré, vérif recommandée'
        else:
            out = next((scan for scan in _out_scans(checklist) if scan.item_id == iid), None)
            if out is None:
                raise ValueError("Cet item n'est pas sorti avec la liste")
            name = out.item.pack.item_type.name
            if any(scan.item_id == iid for scan in back):
                return f'{name} : déjà rentré'
            item = out.item
            if item.status in (ItemStatus.OUT, ItemStatus.MISSING) or item.location_id is None:
                services.move_items([iid], None, identity, SeenWhile.CHECKLIST)  # retour en stock
            ChecklistScan.objects.create(checklist=checklist, item=item, phase=ChecklistScan.BACK, by=identity)
            message = f'{name} : remis en stock'
        checklist.updated = timezone.now()
        checklist.save(update_fields=['updated'])
    return message


def close_return(checklist, user):
    """Retour clos : les consommables sortis et pas rentres sont comptes utilises, les non consommables restent
    sortis (ils rentreront quand on les reverra). La liste ne sert plus."""
    _require_return(checklist)
    identity = identity_of(user)
    now = timezone.now()
    with transaction.atomic():
        back = {scan.item_id for scan in checklist.scans.all() if scan.phase == ChecklistScan.BACK and scan.item_id}
        missing = [scan.item for scan in _out_scans(checklist) if scan.item_id and scan.item_id not in back]
        used = [item for item in missing if item.status == ItemStatus.OUT and item.pack.item_type.consumable]
        if used:
            ItemMovement.objects.bulk_create([movements.movement(item, MovementKind.USED, now, identity)
                                              for item in used])
            for item in used:
                item.status = ItemStatus.MISSING
                item.missed_verifs = 1  # absence deja comptee : une verif du stock n'en ajoute pas une autre
            Items.objects.bulk_update(used, ['status', 'missed_verifs'])
            notifications.check_stock_levels({item.pack.item_type_id for item in used})
        checklist.status = ChecklistStatus.RETURNED
        checklist.returned = now
        checklist.returned_by = identity
        checklist.updated = now
        checklist.save()
        journal.record(OperationKind.CHECKLIST, identity,
                       f'retour de la liste « {checklist.name} » : {journal.plural(len(back), "rentré")}, '
                       f'{journal.plural(len(used), "utilisé")}',
                       at=now, checklist=checklist.id, items=[item.iid for item in used])
    return {'used': [item.iid for item in used], 'still_out': [item.iid for item in missing if item not in used]}


def _scan_dict(scan):
    data = {'phase': scan.phase, 'at': _date(scan.at), 'by': identity_name(scan.by)}
    if scan.item_id:
        item = scan.item
        data.update({'iid': item.iid, 'type': item.pack.item_type_id, 'type_name': item.pack.item_type.name,
                     'peremption': _date(item.pack.peremption), 'status': item.status,
                     'consumable': item.pack.item_type.consumable})
    else:
        data.update({'lot': scan.lot_id, 'lot_name': scan.lot.name})
    return data


def checklist_dict(checklist, user=None, full=True):
    owner = checklist.owner
    data = {
        'id': checklist.id,
        'name': checklist.name,
        'note': checklist.note,
        'owner': owner.matricule,
        'owner_name': identity_name(format_identity_verified(owner.matricule)),
        'template': checklist.is_template,
        'status': checklist.status,
        'status_label': 'Modèle' if checklist.is_template else 'Terminée' if (
            checklist.status == ChecklistStatus.RETURNED and not _out_scans(checklist)) else checklist.get_status_display(),
        'went_out': bool(_out_scans(checklist)),
        'created': _date(checklist.created),
        'updated': _date(checklist.updated),
        'validated': _date(checklist.validated),
        'validated_by': identity_name(checklist.validated_by),
        'returned': _date(checklist.returned),
        'returned_by': identity_name(checklist.returned_by),
        'url': public_url('liste', id=checklist.id),
        'can_edit': bool(user and can_edit(checklist, user)),
        'can_duplicate': bool(user and can_make_lists(user)),
        'source': checklist.source_id,
    }
    rows, complete = progress(checklist)
    data['line_count'] = len(rows)
    data['done_count'] = sum(1 for line, done in rows if len(done) >= line.quantity)
    data['complete'] = complete
    if not full:
        return data
    back = [scan for scan in checklist.scans.all() if scan.phase == ChecklistScan.BACK]
    lines = []
    for line, done in rows:
        row = {'id': line.id, 'quantity': line.quantity, 'scanned': len(done)}
        if line.task:
            row.update({'kind': 'task', 'task': line.task, 'scans': [], 'done': _date(line.done),
                        'done_by': identity_name(line.done_by), 'back': 0})
            lines.append(row)
            continue
        row['scans'] = [_scan_dict(scan) for scan in done]
        if line.lot_id:
            row.update({'kind': 'lot', 'lot': line.lot_id, 'lot_name': line.lot.name,
                        'lot_type_name': line.lot.lot_type.name,
                        'back': any(scan.lot_id == line.lot_id for scan in back)})
        else:
            row.update({'kind': 'type', 'type': line.item_type_id, 'type_name': line.item_type.name,
                        'back': sum(1 for scan in back if scan.item_id
                                    and scan.item.pack.item_type_id == line.item_type_id)})
        lines.append(row)
    data['lines'] = lines
    data['members'] = sorted(({'matricule': member.matricule,
                               'name': identity_name(format_identity_verified(member.matricule))}
                              for member in checklist.members.all()), key=lambda row: _fold(row['name']))
    data['back'] = [_scan_dict(scan) for scan in back]
    return data


def visible(user, show='mine'):
    """Listes affichees : les siennes et celles qui lui sont attribuees, les modeles (de tous, seulement dans l'onglet
    Modeles), ou toutes les listes ; modeles et toutes les listes pour les chefs, la gestion et les admins. Les
    modeles ne sont ni dans « Mes listes » ni dans « Toutes »."""
    queryset = Checklist.objects.select_related('owner').prefetch_related(
        'lines__lot', 'lines__item_type', 'scans__item__pack__item_type', 'scans__lot')
    if show == 'templates':
        return queryset.filter(is_template=True) if can_make_lists(user) else queryset.none()
    queryset = queryset.filter(is_template=False)
    if show == 'all' and can_make_lists(user):
        return queryset
    return queryset.filter(Q(owner=user) | Q(members=user)).distinct()
