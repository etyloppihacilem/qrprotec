# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          tasks.py
#       -\-    _|__
#        |\___/  . \        Created on 16 Oct. 2026 at 21:30
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Liste de taches de la gestion du stock (« Gestion de la TECH », roles gestion et admin) : ce qu'il y a a faire,
par priorite, pour qu'une personne qui reprend la gestion ne soit pas perdue.

Ordre des categories (choix d'Hippolyte) : signalements, lots a sceller, lots invalides, verifs recommandees,
revisions du materiel, verifs du stock, commandes et deplacements recommandes, puis les taches de temps en temps
(etiquettes et badges qui expirent, perimes du stock, listes pas rentrees, materiel hors lot, verifs de routine), et
en tout dernier le materiel du stock sans rangement a ranger.

Les peremptions des 30 jours ne sont pas des taches (choix d'Hippolyte, 2026-11-11) : les secouristes remplacent les
items une fois perimes, pendant la verif du lot rouge. Seuls comptent ce qu'elles demandent au TECH : une commande
(categorie orders) ou un echange pour utiliser plus tot ce qui va perimer (transfers). Elles restent consultables dans
`upcoming` (« Peremptions a venir », repliee en bas de la liste, hors du compte des taches).

Chaque tache : {category, level ('bad' rouge, 'warn' orange, 'info'), title, detail, date, lot, item, report, link}.
`link` dit ou la traiter : 'lot' (fiche du lot), 'item' (fiche de l'item), 'reports', 'forecast', 'lists', 'users',
'inventory', 'lot_admin', 'label_prints' (fenetres du poste ou pages du web), 'consumables' (poste seulement).
"""

from collections import Counter, defaultdict
from datetime import datetime, timedelta

from django.db.models import Max
from django.utils import timezone

from . import consumables
from . import forecast as stock_forecast
from . import label_prints
from . import serializers as ser
from . import services
from .idendity import format_identity_verified, identity_name
from .journal import plural
from .models import (
    Checklist, ChecklistStatus, ItemMovement, Items, ItemStatus, Lots, MovementKind, ProblemReport, ReportStatus,
    STOCK_LOT_TYPE, Secouristes, ServerSettings, StockOrder, Verifs, lot_keys_expire, qrprotec_setting,
)
from .reports import target_name

CATEGORIES = [
    ('reports', 'Signalements', 'Problèmes signalés par les secouristes : les prendre en charge, puis les résoudre.'),
    ('seal', 'Lots à sceller', 'Lots à scellé obligatoire ouverts, ou vérifiés sans scellé : vérif complète puis scellé.'),
    ('invalid', 'Lots invalides', 'Lots incomplets, avec des périmés ou jamais vérifiés : les compléter, puis une vérif.'),
    ('recommended', 'Vérifs recommandées', 'Lots réassortis, ouverts, rentrés d\'une liste ou marqués utilisés : une '
                                          'vérif complète les remet au vert.'),
    ('revisions', 'Révisions du matériel', 'Matériel à réviser en retard ou dans les 30 jours.'),
    ('stock_verif', 'Vérifs du stock', 'Beaucoup de sorties d\'un rangement (ou du stock sans rangement) depuis sa '
                                       'dernière vérif : au retour, chaque item est remis à sa place, une vérif de '
                                       'temps en temps contrôle qu\'il y est bien.'),
    ('orders', 'Commandes', 'Commandes à passer dans les 30 jours pour ne pas passer sous le minimum (prévisions). '
                            'Cochez « Commandé » une fois passée : elle attend sa livraison et se clôt à la réception.'),
    ('transfers', 'Déplacements', 'Échanges qui évitent de laisser périmer du matériel dans les 30 jours (prévisions).'),
    ('periodic', 'De temps en temps', 'Étiquettes à imprimer au poste, étiquettes et badges qui expirent, '
                                      'périmés du stock, listes pas rentrées, matériel hors lot, vérifs de routine.'),
    ('unsorted', 'Sans rangement', 'Matériel en stock rangé nulle part : le ranger dans un rangement du stock quand '
                                   'il y a le temps.'),
]
ORDER_WINDOW_DAYS = 30
REVISION_WINDOW_DAYS = 30
LIST_RETURN_DAYS = 2  # liste sortie depuis plus longtemps : retour a faire
LEVEL_ORDER = {'bad': 0, 'warn': 1, 'info': 2}
SEAL_ORDER = {'seal_opened': 0, 'must_seal': 1}
INVALID_ORDER = {'unusable': 0, 'sealed_expired': 1, 'incomplete': 2, 'never': 3}
ORDER_DELIVERY_DAYS = 30  # commande passee pas livree au bout de 30 jours : de nouveau une tache
UPCOMING_DAYS = 30  # « Peremptions a venir » : items qui periment dans les 30 jours
UPCOMING_YELLOW_DAYS = 7  # jaune dans les 7 jours
UPCOMING_ORANGE_DAYS = 2  # orange dans les 2 jours


def _iso(value):
    if value is None or isinstance(value, str):
        return value
    if isinstance(value, datetime):
        return timezone.localtime(value).isoformat(timespec='seconds')
    return value.isoformat()


def _day(value):
    """JJ/MM/AAAA d'une date ou d'un horodatage (texte des taches)."""
    if isinstance(value, datetime):
        value = timezone.localtime(value).date()
    return value.strftime('%d/%m/%Y')


def task(category, level, title, detail='', date=None, link='', lot=None, item=None, report=None, sort=0):
    return {'category': category, 'level': level, 'title': title, 'detail': detail, 'date': _iso(date), 'link': link,
            'lot': lot, 'item': item, 'report': report, '_sort': sort}


def _report_tasks():
    tasks = []
    for report in ProblemReport.objects.select_related('item_type').exclude(status=ReportStatus.RESOLVED):
        fresh = report.status == ReportStatus.NEW
        what = 'à prendre en charge' if fresh else 'en cours de résolution'
        usable = 'toujours utilisable' if report.usable else 'inutilisable'
        detail = f'{what} · {usable} · signalé par {identity_name(report.by)}'
        if report.comment:
            detail += f' : « {report.comment[:120]} »'
        level = 'bad' if fresh or not report.usable else 'warn'
        tasks.append(task('reports', level, target_name(report), detail, report.created, 'reports', lot=report.lot_id,
                          item=report.item_id, report=report.id, sort=(0 if fresh else 1, report.created.timestamp())))
    return tasks


def _missing_text(row):
    parts = [services.lacking_text(req) for req in row['requirements'] if req['present'] < req['required']]
    text = ', '.join(parts[:4]) + (f" et {plural(len(parts) - 4, 'autre')}" if len(parts) > 4 else '')
    return f'manque {text}' if parts else ''


def _recommended_reason(row):
    if row['restocked_count']:
        return (f"réassort de {plural(row['restocked_count'], 'item')}"
                + (f" par {row['restocked_by']}" if row['restocked_by'] else ''), row['restocked'])
    if row['back_from_list']:
        return f"sorti avec la liste « {row['back_from_list']} »", None
    if row.get('marked_used'):
        return ('marqué utilisé' + (f" par {row['marked_used_by']}" if row['marked_used_by'] else ''),
                row['marked_used'])
    return ('scellé ouvert' + (f" par {row['unsealed_by']}" if row['unsealed_by'] else ''), row['unsealed'])


def _lot_tasks(rows, today):
    tasks = []
    routine_days = qrprotec_setting('ROUTINE_VERIF_DAYS')
    has_children = {row['parent'] for row in rows if row['parent']}
    for row in rows:
        # comme le voyant « lots a traiter » du poste : ni les rangements du stock, ni les simples regroupements
        if row['storage'] or (not row['requirements'] and not row['item_count'] and row['id'] in has_children):
            continue
        state = row.get('effective') or row['state']
        code = state['code']
        where = ' › '.join([part['name'] for part in row.get('path', [])] + [row['name']])
        common = {'link': 'lot', 'lot': row['id']}
        if code in SEAL_ORDER:
            detail = ('scellé ouvert : vérif complète, puis le resceller' if code == 'seal_opened'
                      else 'vérifié complet : poser un scellé')
            tasks.append(task('seal', 'bad', where, detail, row['unsealed'] or row['last_verif'], **common,
                              sort=SEAL_ORDER[code]))
        elif state['kind'] == 'bad':
            parts = []
            if code == 'unusable':
                parts.append('signalé non utilisable : voir les signalements')
            elif code == 'never':
                parts.append('jamais vérifié')
            elif code == 'sealed_expired':
                parts.append('scellé, contient des périmés : ouvrir et remplacer')
            missing = _missing_text(row)
            if missing:
                parts.append(missing)
            if row['expired_count'] and code != 'sealed_expired':
                parts.append(f"{plural(row['expired_count'], 'item')} à remplacer (périmé, inutilisable ou désactivé)")
            tasks.append(task('invalid', 'bad', where, ' · '.join(parts) or state['label'], row['last_verif'],
                              **common, sort=INVALID_ORDER.get(code, 3)))
        elif state['kind'] == 'warn':
            reason, when = _recommended_reason(row)
            tasks.append(task('recommended', 'warn', where, reason, when, **common, sort=-(row['item_count'])))
        elif code == 'verified' and routine_days and row['last_verif']:
            last = datetime.fromisoformat(row['last_verif']).date()
            if (today - last).days >= routine_days:
                tasks.append(task('periodic', 'info', f'Vérif de routine : {where}',
                                  f'dernière vérif il y a {(today - last).days} jours', row['last_verif'], **common,
                                  sort=(3, last.toordinal())))
    return tasks


def _revision_tasks(today):
    tasks = []
    for item in services.revisions_due(today, REVISION_WINDOW_DAYS):
        late = item.revision_due < today
        place = item.location.name if item.location_id else 'stock' if item.status == ItemStatus.ACTIVE \
            else item.get_status_display().lower()
        if late:
            detail = f'en retard depuis le {_day(item.revision_due)}'
            if not item.pack.item_type.revision_usable:
                detail += ' : inutilisable tant que pas révisé'
        else:
            detail = f'à faire avant le {_day(item.revision_due)}'
        tasks.append(task('revisions', 'bad' if late and not item.pack.item_type.revision_usable else
                          'warn' if late else 'info', f'{item.pack.item_type.name} {item.iid}',
                          f'{detail} · {place}', item.revision_due, 'item', lot=item.location_id, item=item.iid,
                          sort=item.revision_due.toordinal()))
    return tasks


def _stock_verif_tasks():
    """Sorties du stock (a la main ou par une liste de materiel) depuis la derniere verif du stock, par lieu : le
    stock hors rangement, ou chaque rangement (sa propre verif compte aussi). Au retour d'une liste, chaque item est
    remis dans le rangement d'ou il est sorti (on fait confiance aux secouristes) : la vérif n'est proposee qu'au bout
    de ServerSettings.storage_verif_outings sorties (une sortie : les items sortis ensemble, liste ou sortie a la
    main), sinon c'est beaucoup de travail pour le TECH (Hippolyte, 2026-11-11)."""
    threshold = max(1, ServerSettings.get().storage_verif_outings)
    last_stock = Verifs.objects.filter(lot__isnull=True).aggregate(last=Max('datetime'))['last']
    outs = ItemMovement.objects.select_related('item_type', 'from_lot__lot_type').filter(kind=MovementKind.OUT)
    if last_stock:
        outs = outs.filter(at__gt=last_stock)
    by_place = defaultdict(list)
    for move in outs.order_by('at'):
        lot = move.from_lot
        if lot is not None and (not lot.lot_type.storage or not lot.active):
            continue
        if lot is not None and lot.last_verif and move.at <= lot.last_verif:
            continue
        by_place[lot.id if lot else None].append(move)
    lots = {lot.id: lot for lot in Lots.objects.filter(id__in=[key for key in by_place if key])}
    tasks = []
    for place, moves in by_place.items():
        outings = len({move.at for move in moves})
        if outings < threshold:
            continue
        lot = lots.get(place)
        counts = Counter(move.item_type.name for move in moves)
        summary = ', '.join(f'{count} × {name}' for name, count in counts.most_common(4))
        if len(counts) > 4:
            summary += f" et {plural(len(counts) - 4, 'autre type', 'autres types')}"
        people = sorted({identity_name(move.by) for move in moves if move.by})
        last = (lot.last_verif if lot and lot.last_verif and (not last_stock or lot.last_verif > last_stock)
                else last_stock)
        since = f'depuis la dernière vérif ({_day(last)})' if last else 'et jamais vérifié'
        detail = (f"{plural(outings, 'sortie')} {since}, {plural(len(moves), 'item sorti', 'items sortis')} : "
                  f"{summary}")
        if people:
            detail += ' · par ' + ', '.join(people[:3]) + ('…' if len(people) > 3 else '')
        tasks.append(task('stock_verif', 'warn', f'Vérifier {lot.name}' if lot else 'Vérifier le stock', detail,
                          moves[-1].at, 'lot' if lot else 'stock', lot=place, sort=-outings))
    return tasks


def open_orders():
    """Commandes passees en cours (case « Commandé »), par type d'item. Clot au passage celles qui sont livrees : les
    items de ce type receptionnes depuis la commande la remplissent, les plus anciennes commandes d'abord."""
    by_type = defaultdict(list)
    for order in StockOrder.objects.select_related('item_type').filter(delivered__isnull=True, cancelled__isnull=True):
        by_type[order.item_type_id].append(order)
    now = timezone.now()
    for type_id, orders in list(by_type.items()):
        orders.sort(key=lambda order: order.placed)
        received = Items.objects.filter(pack__item_type_id=type_id, added__gt=orders[0].placed).count()
        still = []
        for order in orders:
            order.received = min(received, order.quantity)
            received -= order.received
            if order.received >= order.quantity:
                order.delivered = now
                order.save(update_fields=['delivered'])
            else:
                still.append(order)
        by_type[type_id] = still
    return {type_id: orders for type_id, orders in by_type.items() if orders}


def order_dict(order, today):
    days = (today - timezone.localtime(order.placed).date()).days
    return {'id': order.id, 'type': order.item_type_id, 'name': order.item_type.name, 'quantity': order.quantity,
            'received': order.received, 'placed': _iso(order.placed), 'placed_by': identity_name(order.placed_by),
            'days': days, 'late': days >= ORDER_DELIVERY_DAYS}


def _forecast_tasks(today, orders):
    data = stock_forecast.forecast(months=6)
    limit = today + timedelta(days=ORDER_WINDOW_DAYS)
    tasks = []
    for row in data['types']:
        order = row['order']
        # deja commande (case « Commandé ») : la commande attend sa livraison, pas de nouvelle suggestion
        if not order or order['before'] > limit or row['type'] in orders:
            continue
        detail = ('dès que possible' if order['urgent'] else f"avant le {_day(order['before'])}")
        if order['below_min']:
            detail += f" · sous le minimum le {_day(order['below_min'])}"
        detail += f" · stock {row['stock_now']}, minimum {row['min_quantity']}"
        row_task = task('orders', 'warn' if order['urgent'] else 'info', f"Commander {order['quantity']} × {row['name']}",
                        detail, order['before'], 'forecast', sort=order['before'].toordinal())
        row_task.update(type=row['type'], name=row['name'], quantity=order['quantity'])  # case « Commandé »
        tasks.append(row_task)
    # commandes passees pas livrees au bout de 30 jours : relancer le fournisseur, ou la decocher
    for type_orders in orders.values():
        for order in type_orders:
            info = order_dict(order, today)
            if not info['late']:
                continue
            got = f" (reçu {order.received}/{order.quantity})" if order.received else ''
            row_task = task('orders', 'warn', f'Commande pas livrée : {order.quantity} × {order.item_type.name}',
                            f"commandée le {_day(order.placed)} par {info['placed_by'] or '?'}{got} : relancer le "
                            'fournisseur, ou la décocher si elle ne viendra pas', order.placed, 'forecast',
                            sort=order.placed.toordinal() - 10 ** 6)
            row_task.update(type=order.item_type_id, order=order.id)
            tasks.append(row_task)
    for group in data['transfers']:
        if group['before'] > limit:
            continue
        moves = [f"{move['type_name']} ×{len(move['take'])} → {move['target']['path']}" for move in group['moves']]
        source = group['source']
        when = ('scellé, à ouvrir avant le ' if source['sealed'] else 'avant le ') + _day(group['before'])
        tasks.append(task('transfers', 'warn' if source['sealed'] else 'info', f"Échanges dans {source['path']}",
                          f"{when} · " + ', '.join(moves[:3]) + (f" et {plural(len(moves) - 3, 'autre')}" if len(moves) > 3 else ''),
                          group['before'], 'forecast', lot=source['id'], sort=group['before'].toordinal()))
    return tasks


def _periodic_tasks(today):
    tasks = []
    # etiquettes d'items demandees depuis le web, pas encore imprimees au poste
    for row, text in label_prints.pending_tasks():
        who = identity_name(row.by) if row.by else ''
        if row.front:
            who = f'{who} sur {row.front}' if who else row.front
        tasks.append(task('periodic', 'info', f'Étiquettes à imprimer : {text}',
                          f'demandées par {who or "?"} le {_day(row.created)} : au poste, fenêtre « Impressions en '
                          'attente », Lancer l\'impression (expire après 24 h)', row.created,
                          'label_prints' if row.kind == 'items' else '', sort=(-1, row.created.timestamp())))
    warning = timedelta(days=qrprotec_setting('KEY_EXPIRY_WARNING_DAYS'))
    # etiquettes privees des lots (cle) qui expirent : a renouveler et reimprimer (seulement si le nom declare est
    # autorise, sinon elles n'expirent plus)
    lots = Lots.objects.filter(active=True, verif_key_expires__isnull=False, verif_key_expires__lte=today + warning)
    for lot in lots.order_by('verif_key_expires') if lot_keys_expire() else ():
        expired = lot.verif_key_expires < today
        tasks.append(task('periodic', 'warn' if expired else 'info', f'Étiquette clé : {lot.name}',
                          ('expirée depuis le ' if expired else 'expire le ') + _day(lot.verif_key_expires)
                          + ' : la renouveler et la réimprimer (Gestion des lots)', lot.verif_key_expires,
                          'lot_admin', lot=lot.id, sort=(0, lot.verif_key_expires.toordinal())))
    # badges qui expirent
    for user in Secouristes.objects.filter(active=True, key_expires__isnull=False,
                                           key_expires__lte=today + warning).order_by('key_expires'):
        expired = user.key_expires < today
        tasks.append(task('periodic', 'warn' if expired else 'info', f'Badge : {identity_name(format_identity_verified(user.matricule))}',
                          ('expiré depuis le ' if expired else 'expire le ') + _day(user.key_expires)
                          + ' : renouveler la clé et réimprimer le badge (Utilisateurs, admin)', user.key_expires,
                          'users', sort=(0, user.key_expires.toordinal())))
    # perimes (ou inutilisables) qui restent dans le stock
    expired_items = [item for item in Items.objects.select_related('pack__item_type', 'location__lot_type')
                     .filter(services.in_stock_q(), status=ItemStatus.ACTIVE) if item.needs_replacement(today)]
    if expired_items:
        counts = Counter(item.pack.item_type.name for item in expired_items)
        summary = ', '.join(f'{count} × {name}' for name, count in counts.most_common(5))
        tasks.append(task('periodic', 'warn', 'Retirer les périmés du stock',
                          f"{plural(len(expired_items), 'item périmé, inutilisable ou désactivé', 'items périmés, inutilisables ou désactivés')} : "
                          f'{summary}',
                          None, 'inventory', sort=(1, 0)))
    # listes de materiel sorties et pas rentrees
    since = timezone.now() - timedelta(days=LIST_RETURN_DAYS)
    for checklist in Checklist.objects.filter(is_template=False, status=ChecklistStatus.VALIDATED,
                                              validated__lte=since).order_by('validated'):
        tasks.append(task('periodic', 'info', f'Retour de la liste « {checklist.name} »',
                          f'sortie le {_day(checklist.validated)} par {identity_name(checklist.validated_by)} : '
                          'rescanner ce qui rentre puis clôturer le retour', checklist.validated, 'lists',
                          sort=(1, checklist.validated.timestamp())))
    # materiel non consommable retire d'un lot, ni dans un lot ni en stock
    loose = list(Items.objects.select_related('pack__item_type').filter(status=ItemStatus.LOOSE))
    if loose:
        counts = Counter(item.pack.item_type.name for item in loose)
        summary = ', '.join(f'{count} × {name}' for name, count in counts.most_common(5))
        tasks.append(task('periodic', 'info', 'Ranger le matériel hors lot',
                          f"{plural(len(loose), 'item retiré', 'items retirés')} d'un lot, ni en stock ni dans un lot : {summary}", None,
                          'inventory', sort=(1, 1)))
    return tasks


def _unsorted_tasks():
    """Items en stock sans rangement, quand il y a des rangements ou les mettre : derniere priorite du TECH."""
    if not Lots.objects.filter(active=True, lot_type__storage=True).exclude(lot_type_id=STOCK_LOT_TYPE).exists():
        return []  # pas de rangements : le stock est en vrac, rien a ranger
    items = list(Items.objects.select_related('pack__item_type').filter(location__isnull=True,
                                                                       status=ItemStatus.ACTIVE))
    if not items:
        return []
    counts = Counter(item.pack.item_type.name for item in items)
    summary = ', '.join(f'{count} × {name}' for name, count in counts.most_common(5))
    if len(counts) > 5:
        summary += f" et {plural(len(counts) - 5, 'autre type', 'autres types')}"
    return [task('unsorted', 'info', 'Ranger le matériel sans rangement',
                 f"{plural(len(items), 'item')} en stock sans rangement : {summary}", None, 'inventory')]


def upcoming_expiries(rows, today):
    """Peremptions a venir (pas des taches) : items actifs, non desactives, qui periment dans les 30 jours, une ligne
    par lieu, type et date, ce qui perime en premier en tete. level : 'warn' (orange, 2 jours), 'soon' (jaune,
    7 jours), 'info'."""
    places = {row['id']: ' › '.join([part['name'] for part in row.get('path', [])] + [row['name']]) for row in rows}
    items = (Items.objects.select_related('pack__item_type')
             .filter(status=ItemStatus.ACTIVE, disabled=False, pack__peremption__gte=today,
                     pack__peremption__lte=today + timedelta(days=UPCOMING_DAYS))
             .exclude(location__active=False))
    groups = Counter((item.pack.peremption, item.location_id or '', item.pack.item_type.name) for item in items)
    result = []
    for (date, lot, name), count in sorted(groups.items(), key=lambda pair: (pair[0][0], places.get(pair[0][1], ''),
                                                                            pair[0][2])):
        days = (date - today).days
        when = ("aujourd'hui" if days == 0 else 'demain' if days == 1 else f'dans {days} jours')
        level = ('warn' if days <= UPCOMING_ORANGE_DAYS else 'soon' if days <= UPCOMING_YELLOW_DAYS else 'info')
        place = places.get(lot, '') if lot else 'stock'
        result.append({'level': level, 'title': f'{count} × {name}', 'detail': f'périme {when} ({_day(date)}) · '
                       + (place or 'lot'), 'date': _iso(date), 'days': days, 'link': 'lot' if lot else 'stock',
                       'lot': lot or None})
    return result


def _consumable_tasks(today):
    """Bobines ou rubans a commander (poste seulement : le web ne montre pas les consommables)."""
    return [task('orders', row['level'], row['title'], row['detail'], None, 'consumables', sort=row['autonomy'])
            for row in consumables.order_tasks(today)]


def todo(today=None, consumables_tasks=False):
    """Taches a faire, par categorie dans l'ordre de priorite, la plus urgente en tete de chaque categorie.
    consumables_tasks : bobines et rubans a commander (fronts du poste, pas le web)."""
    today = today or timezone.localdate()
    lots = Lots.objects.select_related('lot_type').filter(active=True).order_by('lot_type__name', 'name')
    rows = ser.lot_list(lots, today=today)
    orders = open_orders()
    tasks = (_report_tasks() + _lot_tasks(rows, today) + _revision_tasks(today) + _stock_verif_tasks()
             + _forecast_tasks(today, orders) + (_consumable_tasks(today) if consumables_tasks else [])
             + _periodic_tasks(today) + _unsorted_tasks())
    rank = {key: index for index, (key, _, _) in enumerate(CATEGORIES)}
    tasks.sort(key=lambda row: (rank[row['category']], LEVEL_ORDER[row['level']], row['_sort']))
    for row in tasks:
        del row['_sort']
    categories = []
    for key, title, help_text in CATEGORIES:
        rows_of = [row for row in tasks if row['category'] == key]
        level = min((row['level'] for row in rows_of), key=LEVEL_ORDER.get, default='')
        categories.append({'key': key, 'title': title, 'help': help_text, 'count': len(rows_of), 'level': level})
    # peremptions a venir : a part, repliees en bas, hors du compte des taches
    # commandes passees en attente de livraison : sous les commandes, hors du compte (sauf en retard : tache)
    placed = sorted((order_dict(order, today) for type_orders in orders.values() for order in type_orders),
                    key=lambda row: row['placed'])
    return {'today': today, 'count': len(tasks), 'categories': categories, 'tasks': tasks, 'orders_placed': placed,
            'upcoming': upcoming_expiries(rows, today)}
