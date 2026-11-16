# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          views/lists.py
#       -\-    _|__
#        |\___/  . \        Created on 16 Nov. 2026 at 19:45
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Listes de materiel (checklists.py) : utilisateur connecte (badge ou session), droits de modification par liste."""

from .common import *  # noqa: F401,F403
from .common import _checklist_call
from .public import *  # noqa: F401,F403
from .local import *  # noqa: F401,F403
from .notif_settings import *  # noqa: F401,F403
from .douchette import *  # noqa: F401,F403


# Listes de materiel (checklists.py) : utilisateur connecte (badge ou session), droits de modification par liste
# ----------------------------------------------------------------------------------------------------------------------

def _checklist(checklist_id):
    from ..models import Checklist
    return get_object_or_404(Checklist.objects.select_related('owner').prefetch_related(
        'members', 'lines__lot__lot_type', 'lines__item_type', 'scans__item__pack__item_type', 'scans__lot'),
        id=checklist_id)




@api_view(['POST'])
@handle_errors
def checklists_list(request):
    """Listes : {"user", "show": "mine" (les siennes et celles qui lui sont attribuees) | "templates" (les modeles) |
    "all" (sans les modeles) | "assigned" (attribuees et pas encore rentrees, page « Départ »)} ; modeles et toutes les listes pour les chefs, la gestion et les admins."""
    from .. import checklists
    user = connected_user(request)
    show = str(request.data.get('show') or 'mine')
    rows = [checklists.checklist_dict(row, user, full=False) for row in checklists.visible(user, show)[:300]]
    # a traiter en haut : listes sorties (retour a faire), puis a preparer, et les listes rentrees ; dans chaque groupe,
    # la date la plus proche d'aujourd'hui en haut (a egalite, la date a venir avant la date passee)
    rank = {'validated': 0, 'draft': 1}
    rows.sort(key=lambda row: (rank.get(row['status'], 2), *checklists.date_distance(row['date'])))
    can_make = checklists.can_make_lists(user)
    return Response({'lists': rows, 'can_see_all': can_make, 'can_create': can_make})


@api_view(['POST'])
@handle_errors
def checklists_users(request):
    """Recherche de secouristes par prenom pour les attribuer a une liste : {"user", "q"} (chefs, gestion, admins)."""
    from .. import checklists
    user = connected_user(request)
    return Response({'users': _checklist_call(lambda: checklists.search_users(user, request.data.get('q')))})


def _option_lot(lot):
    """Lot de l'editeur de liste ; un sac d'un B+ de referentiel (sans etiquette) donne son B+ (`unit`) : le choisir
    ajoute le B+ entier."""
    from .. import checklists
    row = {'id': lot.id, 'name': lot.name, 'lot_type_name': lot.lot_type.name, 'description': lot.lot_type.description}
    unit = checklists.lot_unit(lot)
    if unit.id != lot.id:
        row['unit'] = {'id': unit.id, 'name': unit.name, 'lot_type_name': unit.lot_type.name}
    return row


@api_view(['POST'])
@handle_errors
def checklists_options(request):
    """Choix de l'editeur de liste : lots actifs (hors rangements du stock) et types d'items non archives."""
    from .. import checklists
    from ..models import ItemType
    user = connected_user(request)
    if not checklists.can_make_lists(user):
        raise ApiError('Réservé aux chefs, à la gestion et aux admins', status.HTTP_403_FORBIDDEN)
    lots = (Lots.objects.select_related('lot_type').filter(active=True, lot_type__storage=False)
            .order_by('name'))
    types = ItemType.objects.filter(archived=False).order_by('name')
    lot_types = LotType.objects.filter(archived=False, storage=False).order_by('name')
    # « Lot au choix » : seulement s'il y a un choix. Un type de lot unique (un VPS, meme referentiel) ou qui n'a
    # qu'un lot n'est pas propose : on ajoute son lot, propose comme lot.
    counts = {}
    for code in Lots.objects.filter(active=True, lot_type__storage=False).values_list('lot_type_id', flat=True):
        counts[code] = counts.get(code, 0) + 1
    lot_types = [row for row in lot_types if not row.unique and counts.get(row.type, 0) != 1]
    return Response({
        # description : cherchee aussi, apres les noms (marques, autres noms) ; un lot prend celle de son type
        'lots': [_option_lot(lot) for lot in lots],
        'types': [{'type': row.type, 'name': row.name, 'consumable': row.consumable, 'description': row.description,
                   'is_group': row.is_unit_group, 'unit': row.unit}  # type parent, a unites (serum phy en mL) : quantite en unites
                  for row in types],
        # lignes « n'importe quel lot de ce type »
        'lot_types': [{'type': row.type, 'name': row.name, 'description': row.description} for row in lot_types],
    })


@api_view(['POST'])
@handle_errors
def checklists_departure(request):
    """« Partir avec » (tout utilisateur connecte) : sans lot_type, les types de lots proposes avec le nombre de lots
    disponibles ; avec {"lot_type"}, cree la liste de depart (checklists.departure) et la renvoie. Page « Départ » :
    {"blocked": true} renvoie les lots qu'on ne peut pas cocher, {"lots": [ids]} cree la liste des lots coches."""
    from .. import checklists
    user = connected_user(request)
    if request.data.get('blocked'):  # page « Départ » : lots qu'on ne peut pas cocher
        return Response({'blocked': checklists.departure_blocked()})
    if request.data.get('lots') is not None:  # page « Départ » : liste des lots coches
        lot_ids = request.data.get('lots')
        if not isinstance(lot_ids, list):
            raise ApiError('lots : liste d\'identifiants de lots attendue', status.HTTP_400_BAD_REQUEST)
        checklist = _checklist_call(lambda: checklists.departure_lots(user, lot_ids))
        return Response(checklists.checklist_dict(_checklist(checklist.id), user), status=status.HTTP_201_CREATED)
    code = str(request.data.get('lot_type') or '')
    if code:
        lot_type = get_object_or_404(LotType, type=code)
        checklist = _checklist_call(lambda: checklists.departure(user, lot_type))
        return Response(checklists.checklist_dict(_checklist(checklist.id), user), status=status.HTTP_201_CREATED)
    rows = []
    for lot_type in LotType.objects.filter(archived=False, storage=False).order_by('name'):
        free = lot_type.lots_set.filter(active=True, parent__isnull=True).exists()
        if not (lot_type.referentiel or lot_type.slots.exists() or free):
            continue  # sac toujours range dans un autre lot : on part avec son lot
        available = checklists.available_lots([lot_type.type], limit=1000)
        rows.append({'type': lot_type.type, 'name': lot_type.name, 'available': len(available),
                     'available_ok': sum(1 for row in available if row['state']['kind'] == 'ok'),
                     'lines': len(checklists.departure_lines(lot_type))})
    return Response({'lot_types': rows, 'full_verif': ServerSettings.get().departure_full_verif})


@api_view(['POST'])
@handle_errors
def checklists_create(request):
    """Nouvelle liste : {"user", "name", "note", "template", "date" (AAAA-MM-JJ, aujourd'hui par defaut),
    "lines": [{"lot"} | {"type", "quantity"} | {"task"}]}."""
    from .. import checklists
    user = connected_user(request)
    data = request.data
    checklist = _checklist_call(lambda: checklists.create(user, data.get('name'), data.get('note', ''),
                                                          data.get('template', False), data.get('lines') or [],
                                                          data.get('date')))
    return Response(checklists.checklist_dict(_checklist(checklist.id), user), status=status.HTTP_201_CREATED)


@api_view(['POST'])
@handle_errors
def checklist_action(request, checklist_id, action=''):
    """Une liste : {"user"} lit ; update {name, note, date, template, lines} ; duplicate {name, template} ; delete ;
    scan / unscan {item | lot} ; check {line, done} (tache libre) ; validate {partial} ; reopen (depart valide par
    erreur, 30 min) ; return-scan {item | lot} ; close-return ; add-lot {lot} (lot hors liste ajoute, ou a la place
    d'un lot du meme type pas encore coche : reponse a ask_add d'un scan) ; members {add: [matricules], remove: [matricules]}. A la lecture, {"open": true} (liste ouverte par
    son lien ou son QR code) l'ajoute aux listes d'un secouriste."""
    from .. import checklists
    user = connected_user(request)
    checklist = _checklist(checklist_id)
    data = request.data
    _checklist_call(lambda: checklists.access(checklist, user, join=not action and bool(data.get('open'))))
    iid, lot = str(data.get('item') or ''), str(data.get('lot') or '')
    message = ''
    extra = {}
    if action == 'update':
        _checklist_call(lambda: checklists.update(checklist, user, data))
    elif action == 'members':
        add, remove = data.get('add') or [], data.get('remove') or []
        if not isinstance(add, list) or not isinstance(remove, list):
            raise ApiError('add, remove : listes de matricules attendues')
        _checklist_call(lambda: checklists.set_members(checklist, user, add, remove))
    elif action == 'duplicate':
        copy = _checklist_call(lambda: checklists.duplicate(checklist, user, data.get('name'),
                                                            data.get('template', False)))
        return Response(checklists.checklist_dict(_checklist(copy.id), user), status=status.HTTP_201_CREATED)
    elif action == 'delete':
        _checklist_call(lambda: checklists.delete(checklist, user))
        return Response({'deleted': checklist_id})
    elif action in ('scan', 'unscan', 'return-scan'):
        if not iid and not lot:
            raise ApiError('Item ou lot attendu')
        if action == 'scan':
            try:
                message = checklists.scan(checklist, user, iid, lot)
            except checklists.NotInList as exc:
                # lot hors liste : le front demande « Ajouter ce lot à la liste ? » (add-lot)
                message, extra = str(exc), {'ask_add': exc.proposal}
        elif action == 'unscan':
            checklists.unscan(checklist, user, iid, lot)
        else:
            message = checklists.return_scan(checklist, user, iid, lot)
    elif action == 'add-lot':  # reponse oui a « Ajouter ce lot à la liste ? »
        if not lot:
            raise ApiError('Lot attendu')
        message = _checklist_call(lambda: checklists.add_lot(checklist, user, lot))
    elif action == 'remove-part':  # croix d'un sac d'un B+ de referentiel : le B+ separe, ce sac retire
        if not lot:
            raise ApiError('Lot attendu')
        message = _checklist_call(lambda: checklists.remove_part(checklist, user, lot))
    elif action == 'check':
        try:
            line = int(data.get('line'))
        except (TypeError, ValueError):
            raise ApiError('line : identifiant de la tâche attendu')
        message = checklists.check_task(checklist, user, line, data.get('done', True) is not False)
    elif action == 'validate':
        checklists.validate(checklist, user, bool(data.get('partial')))
    elif action == 'reopen':
        checklists.reopen(checklist, user)
    elif action == 'close-return':
        extra = checklists.close_return(checklist, user)
    elif action:
        raise ApiError('Action inconnue', status.HTTP_404_NOT_FOUND)
    return Response({**checklists.checklist_dict(_checklist(checklist_id), user), 'message': message, **extra})
