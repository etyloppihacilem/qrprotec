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
(etiquettes et badges qui expirent, perimes du stock, listes pas rentrees, materiel hors lot, verifs de routine).

Chaque tache : {category, level ('bad' rouge, 'warn' orange, 'info'), title, detail, date, lot, item, report, link}.
`link` dit ou la traiter : 'lot' (fiche du lot), 'item' (fiche de l'item), 'reports', 'forecast', 'lists', 'users',
'inventory', 'lot_admin' (fenetres du poste ou pages du web).
"""

from collections import Counter, defaultdict
from datetime import datetime, timedelta

from django.db.models import Max
from django.utils import timezone

from . import forecast as stock_forecast
from . import serializers as ser
from . import services
from .idendity import identity_name
from .models import (
    Checklist, ChecklistStatus, ItemMovement, Items, ItemStatus, Lots, MovementKind, ProblemReport, ReportStatus,
    Secouristes, Verifs, qrprotec_setting,
)
from .reports import target_name

CATEGORIES = [
    ('reports', 'Signalements', 'Problèmes signalés par les secouristes : les prendre en charge, puis les résoudre.'),
    ('seal', 'Lots à sceller', 'Lots à scellé obligatoire ouverts, ou vérifiés sans scellé : vérif complète puis scellé.'),
    ('invalid', 'Lots invalides', 'Lots incomplets, avec des périmés ou jamais vérifiés : les compléter, puis une vérif.'),
    ('recommended', 'Vérifs recommandées', 'Lots réassortis, ouverts, rentrés d\'une liste ou marqués utilisés : une '
                                          'vérif complète les remet au vert.'),
    ('revisions', 'Révisions du matériel', 'Matériel à réviser en retard ou dans les 30 jours.'),
    ('stock_verif', 'Vérifs du stock', 'Du matériel est sorti du stock depuis sa dernière vérif (sortie à la main ou '
                                       'liste de matériel) : recompter.'),
    ('orders', 'Commandes', 'Commandes à passer dans les 30 jours pour ne pas passer sous le minimum (prévisions).'),
    ('transfers', 'Déplacements', 'Échanges qui évitent de laisser périmer du matériel dans les 30 jours (prévisions).'),
    ('periodic', 'De temps en temps', 'Étiquettes et badges qui expirent, périmés du stock, listes pas rentrées, '
                                      'matériel hors lot, vérifs de routine.'),
]
ORDER_WINDOW_DAYS = 30
REVISION_WINDOW_DAYS = 30
LIST_RETURN_DAYS = 2  # liste sortie depuis plus longtemps : retour a faire
LEVEL_ORDER = {'bad': 0, 'warn': 1, 'info': 2}
SEAL_ORDER = {'seal_opened': 0, 'must_seal': 1}
INVALID_ORDER = {'sealed_expired': 0, 'incomplete': 1, 'never': 2}


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
    parts = [f"{needed - present} × {name}" for name, needed, present in
             ((req['type_name'], req['required'], req['present']) for req in row['requirements'])
             if present < needed]
    text = ', '.join(parts[:4]) + (f' et {len(parts) - 4} autre(s)' if len(parts) > 4 else '')
    return f'manque {text}' if parts else ''


def _recommended_reason(row):
    if row['restocked_count']:
        return (f"réassort de {row['restocked_count']} item(s)"
                + (f" par {row['restocked_by']}" if row['restocked_by'] else ''), row['restocked'])
    if row['back_from_list']:
        return f"rentré avec la liste « {row['back_from_list']} »", None
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
            if code == 'never':
                parts.append('jamais vérifié')
            elif code == 'sealed_expired':
                parts.append('scellé, contient des périmés : ouvrir et remplacer')
            missing = _missing_text(row)
            if missing:
                parts.append(missing)
            if row['expired_count'] and code != 'sealed_expired':
                parts.append(f"{row['expired_count']} item(s) à remplacer (périmé, inutilisable ou désactivé)")
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
        if state['kind'] == 'ok' and not row['is_sealed'] and row['expiring_soon_count']:
            # perimes dans les 30 jours d'un lot valide (un lot invalide les a deja dans sa tache)
            tasks.append(task('periodic', 'info', f'Péremptions proches : {where}',
                              f"{row['expiring_soon_count']} item(s) périment dans les 30 jours : à remplacer "
                              'au prochain passage', None, **common, sort=(2, 0)))
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
    stock hors rangement, ou chaque rangement (sa propre verif compte aussi)."""
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
        lot = lots.get(place)
        counts = Counter(move.item_type.name for move in moves)
        summary = ', '.join(f'{count} × {name}' for name, count in counts.most_common(4))
        if len(counts) > 4:
            summary += f' et {len(counts) - 4} autre(s) type(s)'
        people = sorted({identity_name(move.by) for move in moves if move.by})
        last = (lot.last_verif if lot and lot.last_verif and (not last_stock or lot.last_verif > last_stock)
                else last_stock)
        since = f'depuis la dernière vérif ({_day(last)})' if last else 'et jamais vérifié'
        detail = f'{len(moves)} item(s) sorti(s) {since} : {summary}'
        if people:
            detail += ' · par ' + ', '.join(people[:3]) + ('…' if len(people) > 3 else '')
        tasks.append(task('stock_verif', 'warn', f'Vérifier {lot.name}' if lot else 'Vérifier le stock', detail,
                          moves[-1].at, 'lot' if lot else 'stock', lot=place, sort=-len(moves)))
    return tasks


def _forecast_tasks(today):
    data = stock_forecast.forecast(months=6)
    limit = today + timedelta(days=ORDER_WINDOW_DAYS)
    tasks = []
    for row in data['types']:
        order = row['order']
        if not order or order['before'] > limit:
            continue
        detail = ('dès que possible' if order['urgent'] else f"avant le {_day(order['before'])}")
        if order['below_min']:
            detail += f" · sous le minimum le {_day(order['below_min'])}"
        detail += f" · stock {row['stock_now']}, minimum {row['min_quantity']}"
        tasks.append(task('orders', 'warn' if order['urgent'] else 'info', f"Commander {order['quantity']} × {row['name']}",
                          detail, order['before'], 'forecast', sort=order['before'].toordinal()))
    for group in data['transfers']:
        if group['before'] > limit:
            continue
        moves = [f"{move['type_name']} ×{len(move['take'])} → {move['target']['path']}" for move in group['moves']]
        source = group['source']
        when = ('scellé, à ouvrir avant le ' if source['sealed'] else 'avant le ') + _day(group['before'])
        tasks.append(task('transfers', 'warn' if source['sealed'] else 'info', f"Échanges dans {source['path']}",
                          f"{when} · " + ', '.join(moves[:3]) + (f' et {len(moves) - 3} autre(s)' if len(moves) > 3 else ''),
                          group['before'], 'forecast', lot=source['id'], sort=group['before'].toordinal()))
    return tasks


def _periodic_tasks(today):
    tasks = []
    warning = timedelta(days=qrprotec_setting('KEY_EXPIRY_WARNING_DAYS'))
    # etiquettes privees des lots (cle) qui expirent : a renouveler et reimprimer
    for lot in Lots.objects.filter(active=True, verif_key_expires__isnull=False,
                                   verif_key_expires__lte=today + warning).order_by('verif_key_expires'):
        expired = lot.verif_key_expires < today
        tasks.append(task('periodic', 'warn' if expired else 'info', f'Étiquette clé : {lot.name}',
                          ('expirée depuis le ' if expired else 'expire le ') + _day(lot.verif_key_expires)
                          + ' : la renouveler et la réimprimer (Gestion des lots)', lot.verif_key_expires,
                          'lot_admin', lot=lot.id, sort=(0, lot.verif_key_expires.toordinal())))
    # badges qui expirent
    for user in Secouristes.objects.filter(active=True, key_expires__isnull=False,
                                           key_expires__lte=today + warning).order_by('key_expires'):
        expired = user.key_expires < today
        tasks.append(task('periodic', 'warn' if expired else 'info', f'Badge : {user.prenom} {user.nom}',
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
                          f'{len(expired_items)} item(s) périmés, inutilisables ou désactivés : {summary}',
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
                          f'{len(loose)} item(s) retirés d\'un lot, ni en stock ni dans un lot : {summary}', None,
                          'inventory', sort=(1, 1)))
    return tasks


def todo(today=None):
    """Taches a faire, par categorie dans l'ordre de priorite, la plus urgente en tete de chaque categorie."""
    today = today or timezone.localdate()
    lots = Lots.objects.select_related('lot_type').filter(active=True).order_by('lot_type__name', 'name')
    rows = ser.lot_list(lots, today=today)
    tasks = (_report_tasks() + _lot_tasks(rows, today) + _revision_tasks(today) + _stock_verif_tasks()
             + _forecast_tasks(today) + _periodic_tasks(today))
    rank = {key: index for index, (key, _, _) in enumerate(CATEGORIES)}
    tasks.sort(key=lambda row: (rank[row['category']], LEVEL_ORDER[row['level']], row['_sort']))
    for row in tasks:
        del row['_sort']
    categories = []
    for key, title, help_text in CATEGORIES:
        rows_of = [row for row in tasks if row['category'] == key]
        level = min((row['level'] for row in rows_of), key=LEVEL_ORDER.get, default='')
        categories.append({'key': key, 'title': title, 'help': help_text, 'count': len(rows_of), 'level': level})
    return {'today': today, 'count': len(tasks), 'categories': categories, 'tasks': tasks}
