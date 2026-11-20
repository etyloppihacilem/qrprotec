"""Annulation des operations du journal par un admin (page Journal du poste).

Chaque requete qui ecrit (POST...) est enregistree ligne par ligne : des declencheurs SQLite temporaires, propres a la
connexion, notent chaque ligne ajoutee, modifiee ou supprimee dans une table temporaire. A la fin de la requete, si
elle a ajoute des lignes au journal, ces ecritures sont gardees dans OperationChange sous un numero de lot
(Operation.batch) commun a toutes les operations de la requete (une verif en cascade, ses reassorts, ses retraits).
Sinon elles sont oubliees : ce qui n'est pas dans le journal ne s'annule pas.

Annuler une operation, c'est defaire les ecritures de son lot, de la derniere a la premiere :
- une ligne ajoutee est supprimee, une ligne supprimee est remise ;
- une colonne modifiee reprend son ancienne valeur, sauf si une operation posterieure (encore valable) l'a ecrite
  depuis, ou si elle a change autrement : la valeur la plus recente reste (une verif annulee ne defait pas la verif
  faite apres elle) ;
- les operations posterieures qui dependent d'une ligne ajoutee (reponse a un signalement annule, ligne qui y fait
  reference) sont annulees avec elle : c'est la liste montree avant de confirmer.

L'annulation est elle-meme une operation du journal (kind CANCEL), enregistree de la meme facon : l'annuler defait ses
propres ecritures, ce qui retablit l'operation annulee (et efface sa marque « annulee »). Les annulations sont
transparentes pour la regle des valeurs plus recentes : elles ne font que remettre des valeurs passees.
"""

import json
import re
import threading
from collections import defaultdict
from contextlib import contextmanager

from django.apps import apps
from django.db import connection, transaction
from django.utils import timezone

from . import journal

LOG = 'qrp_undo_log'
FLAG = 'qrp_undo_on'
SAFE_METHODS = ('GET', 'HEAD', 'OPTIONS', 'TRACE')
# compteurs et traces techniques : jamais remis en arriere (un numero d'item ou de lot ne doit pas resservir). Passkey :
# un identifiant de connexion, jamais recree ni supprime par une annulation (une passkey supprimee par un admin ne doit
# pas revenir)
IGNORED_MODELS = ('Sequence', 'LotSequence', 'PushKeys', 'PushSubscription', 'ServerProblem', 'FrontConfigDownload',
                  'OperationChange', 'Passkey')
# lignes ajoutees dont les modifications posterieures ne font pas dependre l'operation qui les fait : une absence
# notee par une verif (ItemMovement) est « annulee » par la verif suivante qui retrouve l'item, cette verif ne depend
# pas de la premiere
NO_DEPENDENCY = ('ItemMovement',)
# reglages crees a leur premiere lecture (ServerSettings.get()) : jamais supprimes par une annulation
SINGLETONS = ('ServerSettings', 'NotificationSettings')
IGNORED_COLUMNS = {
    'Secouristes': ('pin_failures', 'pin_failures_total', 'pin_locked_until', 'pin_blocked', 'pin_forgotten',
                    'pin_reset_notified', 'name_login_failures', 'name_login_locked_until', 'key_expiry_stage'),
    'Lots': ('key_expiry_stage',),
    'FrontKey': ('last_used', 'last_address'),
    'ItemsPacks': ('last_sequence',),
}
_SET_COLUMN = re.compile(r'(?:\bSET |, )"(\w+)" = ')
_WRITE = re.compile(r'^\s*(INSERT INTO|UPDATE|DELETE FROM) "(\w+)"', re.IGNORECASE)

_state = threading.local()


class Table:
    def __init__(self, model):
        self.model = model
        self.name = model._meta.db_table
        self.columns = [field.column for field in model._meta.concrete_fields]
        self.pk = model._meta.pk.column
        ignored = IGNORED_COLUMNS.get(model.__name__, ())
        self.ignored = {model._meta.get_field(name).column for name in ignored}
        # colonnes qui font reference a une autre table : {colonne: (table visee, colonne visee)}
        self.references = {field.column: (field.related_model._meta.db_table, field.target_field.column)
                           for field in model._meta.concrete_fields
                           if field.is_relation and field.related_model is not None}


_tables = None


def tables():
    """{nom de table: Table} des modeles de l'inventaire suivis (tables des ManyToMany comprises)."""
    global _tables
    if _tables is None:
        found = {}
        for model in apps.get_app_config('inventory').get_models(include_auto_created=True):
            if model.__name__ in IGNORED_MODELS or not model._meta.managed:
                continue
            found[model._meta.db_table] = Table(model)
        _tables = found
    return _tables


def operation_table():
    from .models import Operation
    return Operation._meta.db_table


# ----------------------------------------------------------------------------------------------- enregistrement

def _quote(name):
    return '"' + name.replace('"', '""') + '"'


def _row_json(prefix, columns):
    return 'json_array(' + ', '.join(f'{prefix}.{_quote(column)}' for column in columns) + ')'


def _installed():
    """Tables dont les declencheurs existent sur la connexion en cours (les objets temporaires meurent avec elle)."""
    raw = connection.connection
    state = getattr(connection, '_qrp_undo', None)
    if state is None or state[0] is not raw:
        state = (raw, set())
        connection._qrp_undo = state
    return state[1]


def _exists(cursor, kind, name):
    """L'objet temporaire existe encore (name None : la table du journal des ecritures)."""
    cursor.execute('SELECT 1 FROM sqlite_temp_master WHERE type = %s AND name = %s',
                   ['table', LOG] if name is None else [kind, name])
    return cursor.fetchone() is not None


def _ready():
    """Capture deja installee sur cette connexion, et toujours la (une transaction annulee l'emporte avec elle)."""
    if connection.connection is None or not _installed():
        return False
    with connection.cursor() as cursor:
        if _exists(cursor, 'table', None):
            return True
    _installed().clear()
    return False


def _install(cursor, name):
    """Declencheurs d'une table, crees a sa premiere ecriture sur la connexion : une requete qui ne fait que lire (le
    web lit aussi en POST) ne paie rien, une ecriture seulement les tables qu'elle touche."""
    installed = _installed()
    if installed and connection.in_atomic_block and not _exists(cursor, 'trigger', f'qrp_undo_{name}_I'
                                                                 if name in installed else None):
        installed.clear()  # crees dans une transaction annulee depuis : disparus avec elle
    if not installed:
        cursor.execute(f'CREATE TEMP TABLE IF NOT EXISTS {LOG} '
                       '(seq INTEGER PRIMARY KEY, tbl TEXT, act TEXT, old TEXT, new TEXT)')
        cursor.execute(f'CREATE TEMP TABLE IF NOT EXISTS {FLAG} (on_ INTEGER)')
        cursor.execute(f'INSERT INTO {FLAG} VALUES (1)')  # appele pendant une capture
        installed.add('')
    if name in installed:
        return
    table = tables()[name]
    when = f'WHEN EXISTS (SELECT 1 FROM {FLAG})'
    for event, act, old, new in (('INSERT', 'I', 'NULL', _row_json('NEW', table.columns)),
                                 ('UPDATE', 'U', _row_json('OLD', table.columns), _row_json('NEW', table.columns)),
                                 ('DELETE', 'D', _row_json('OLD', table.columns), 'NULL')):
        cursor.execute(
            f'CREATE TEMP TRIGGER IF NOT EXISTS {_quote("qrp_undo_" + name + "_" + act)} AFTER {event} '
            f"ON main.{_quote(name)} {when} BEGIN INSERT INTO {LOG} (tbl, act, old, new) "
            f"VALUES ('{name}', '{act}', {old}, {new}); END")
    installed.add(name)


def _watch_writes(execute, sql, params, many, context):
    """Avant chaque ecriture d'une table suivie : ses declencheurs, et pour un UPDATE les colonnes qu'il ecrit (meme
    sans changer de valeur), notees dans une ligne « S » avant ses lignes « U »."""
    if getattr(_state, 'depth', 0) and not getattr(_state, 'inside', False):
        match = _WRITE.match(sql)
        if match:
            name = match.group(2)
            if name in tables():
                _state.inside = True
                try:
                    with connection.cursor() as cursor:
                        _install(cursor, name)
                        if match.group(1).upper() == 'UPDATE':
                            where = sql.upper().find(' WHERE ')
                            written = _SET_COLUMN.findall(sql[:where] if where > 0 else sql)
                            cursor.execute(f"INSERT INTO {LOG} (tbl, act, new) VALUES (%s, 'S', %s)",
                                           [name, json.dumps(written)])
                finally:
                    _state.inside = False
    return execute(sql, params, many, context)


@contextmanager
def capture():
    """Enregistre les ecritures du bloc ; a la fin, celles d'un bloc qui a ecrit dans le journal sont gardees."""
    if connection.vendor != 'sqlite':
        yield
        return
    if getattr(_state, 'depth', 0):
        _state.depth += 1
        try:
            yield
        finally:
            _state.depth -= 1
        return
    if _ready():  # connexion gardee d'une capture precedente
        with connection.cursor() as cursor:
            cursor.execute(f'DELETE FROM {LOG}')
            cursor.execute(f'INSERT INTO {FLAG} VALUES (1)')
    _state.depth = 1
    try:
        with connection.execute_wrapper(_watch_writes):
            yield
    finally:
        _state.depth = 0
        if _ready():  # rien n'a ete ecrit sinon
            try:
                _finish()
            finally:
                with connection.cursor() as cursor:
                    cursor.execute(f'DELETE FROM {FLAG}')
                    cursor.execute(f'DELETE FROM {LOG}')


def _finish():
    with connection.cursor() as cursor:
        cursor.execute(f'DELETE FROM {FLAG}')  # les ecritures qui suivent ne sont plus enregistrees
        cursor.execute(f'SELECT seq, tbl, act, old, new FROM {LOG} ORDER BY seq')
        log = cursor.fetchall()
    operations_table = operation_table()
    operation_ids = []
    changes = []
    written = []
    for seq, name, act, old, new in log:
        if act == 'S':
            written = json.loads(new)
            continue
        table = tables().get(name)
        if table is None:
            continue
        old = json.loads(old) if old else None
        new = json.loads(new) if new else None
        if name == operations_table and act == 'I':
            operation_ids.append(new[table.columns.index(table.pk)])
            continue
        if act != 'U' and table.model.__name__ in SINGLETONS:
            continue
        row = dict(zip(table.columns, new or old))
        if act == 'U':
            before, after = dict(zip(table.columns, old)), dict(zip(table.columns, new))
            changed = [column for column in table.columns if before[column] != after[column]
                       and column not in table.ignored]
            columns = [column for column in written if column in before and column not in table.ignored]
            if not changed and not columns:
                continue
            old = {column: before[column] for column in changed}
            new = {column: after[column] for column in changed}
            changes.append((seq, name, after[table.pk], act, old, new, sorted(set(columns) | set(changed))))
        else:
            changes.append((seq, name, row[table.pk], act, old and dict(zip(table.columns, old)),
                            new and dict(zip(table.columns, new)), []))
    if not operation_ids:
        return
    from .models import Operation, OperationChange, Sequence
    batch = Sequence.next('operation_batch')
    now = timezone.now()
    OperationChange.objects.bulk_create([
        OperationChange(batch=batch, seq=index, at=now, table=name, row=str(pk), act=act, old=old, new=new,
                        written=columns)
        for index, (_, name, pk, act, old, new, columns) in enumerate(changes)
    ], batch_size=200)
    Operation.objects.filter(id__in=operation_ids).update(batch=batch)


class CaptureMiddleware:
    """Enregistre les ecritures de chaque requete qui peut ecrire, pour pouvoir annuler ses operations."""

    def __init__(self, get_response):
        self.get_response = get_response

    def __call__(self, request):
        if request.method in SAFE_METHODS:
            return self.get_response(request)
        with capture():
            return self.get_response(request)


# ------------------------------------------------------------------------------------------------- annulation

def _batch_operations(batches):
    from .models import Operation
    by_batch = defaultdict(list)
    for operation in Operation.objects.filter(batch__in=batches).order_by('at', 'id'):
        by_batch[operation.batch].append(operation)
    return by_batch


def _active_ordinary(batches):
    """Lots d'operations encore valables (ni annules, ni annulations) parmi `batches`."""
    from .models import OperationKind
    return {batch for batch, operations in _batch_operations(batches).items()
            if operations and all(op.cancelled_by_id is None and op.kind != OperationKind.CANCEL
                                  for op in operations)}


def _dependents(batch, excluded):
    """Lots posterieurs encore valables qui dependent d'une ligne ajoutee par `batch` : ils l'ont modifiee ou
    supprimee, ou ils ont ajoute ou modifie une ligne qui y fait reference."""
    from .models import OperationChange
    guest = _guest_dependents(batch, excluded)
    inserted = defaultdict(list)  # table -> lignes ajoutees (completes)
    for name, row in OperationChange.objects.filter(batch=batch, act='I').values_list('table', 'new'):
        if name in tables() and tables()[name].model.__name__ not in NO_DEPENDENCY:
            inserted[name].append(row)
    if not inserted:
        return guest
    later = OperationChange.objects.filter(batch__gt=batch).exclude(batch__in=excluded)
    candidates = set()
    for name, rows in inserted.items():
        pk = tables()[name].pk
        keys = [str(row[pk]) for row in rows]
        for start in range(0, len(keys), 500):
            candidates |= set(later.filter(table=name, row__in=keys[start:start + 500], act__in=('U', 'D'))
                              .values_list('batch', flat=True))
    # references : colonnes d'autres tables qui visent une ligne ajoutee
    targets = {}  # (table visee, colonne visee) -> valeurs ajoutees
    referring = defaultdict(list)
    for table in tables().values():
        for column, target in table.references.items():
            if target[0] in inserted:
                if target not in targets:
                    targets[target] = {str(row.get(target[1])) for row in inserted[target[0]]}
                referring[table.name].append((column, target))
    for change in later.filter(table__in=list(referring), act__in=('I', 'U')).exclude(batch__in=candidates).only(
            'batch', 'table', 'new'):
        for column, target in referring[change.table]:
            value = (change.new or {}).get(column)
            if value is not None and str(value) in targets[target]:
                candidates.add(change.batch)
                break
    return _active_ordinary(candidates - {batch}) | guest


def _guest_dependents(batch, excluded):
    """Activation (ou reactivation) d'une carte d'invite dans `batch` : toutes les operations faites ensuite par cet
    invite, jusqu'a la prochaine activation de la carte, en dependent. Annuler l'activation les annule avec elle."""
    from .models import Operation, OperationKind
    activations = Operation.objects.filter(batch=batch, kind=OperationKind.USER, details__guest_activation=True)
    found = set()
    for activation in activations:
        matricule = activation.details.get('matricule')
        if not matricule:
            continue
        operations = Operation.objects.filter(by=f'M:{matricule}', batch__gt=batch).exclude(batch__in=excluded)
        following = (Operation.objects.filter(kind=OperationKind.USER, details__guest_activation=True,
                                              details__matricule=matricule, batch__gt=batch)
                     .exclude(cancelled_by__isnull=False).order_by('batch').first())
        if following is not None:
            operations = operations.filter(batch__lt=following.batch)
        found |= set(operations.values_list('batch', flat=True))
    return _active_ordinary(found - {batch}) if found else set()


def plan(operation):
    """Lots d'operations a annuler pour annuler `operation` (le sien et ceux qui en dependent), du plus recent au
    plus ancien. ValueError si elle ne peut pas l'etre."""
    from .models import OperationChange
    if operation.cancelled_by_id is not None:
        raise ValueError('Opération déjà annulée')
    if operation.batch is None:
        raise ValueError("Opération enregistrée avant l'annulation des opérations : elle ne peut pas être annulée")
    if not OperationChange.objects.filter(batch=operation.batch).exists():
        raise ValueError("Rien à annuler : cette opération n'a rien changé dans la base")
    closure, pending = {operation.batch}, [operation.batch]
    while pending:
        found = _dependents(pending.pop(), closure)
        pending.extend(found - closure)
        closure |= found
    return sorted(closure, reverse=True)


def preview(operation):
    """Operations annulees ensemble (la sienne d'abord) : {"operations": [...]} pour la confirmation."""
    batches = plan(operation)
    by_batch = _batch_operations(batches)
    own = by_batch.pop(operation.batch, [])
    others = [op for batch in sorted(by_batch) for op in by_batch[batch]]
    return own, others


def _later_writes(table, row, batch, excluded, cache):
    """Colonnes ecrites depuis `batch` par des operations encore valables (hors annulations et lots exclus). None :
    ligne supprimee puis remise, tout est considere ecrit."""
    from .models import OperationChange
    later = list(OperationChange.objects.filter(table=table, row=row, batch__gt=batch).exclude(batch__in=excluded)
                 .values_list('batch', 'act', 'written'))
    batches = {entry[0] for entry in later} - set(cache)
    if batches:
        active = _active_ordinary(batches)
        cache.update({value: value in active for value in batches})
    columns = set()
    for later_batch, act, written in later:
        if not cache[later_batch]:
            continue
        if act != 'U':
            return None
        columns |= set(written)
    return columns


def _undo_batch(batch, excluded, cache):
    """Defait les ecritures d'un lot d'operations. Renvoie (lignes defaites, colonnes laissees a une valeur plus
    recente)."""
    from .models import OperationChange
    done = kept = 0
    with connection.cursor() as cursor:
        for change in OperationChange.objects.filter(batch=batch).order_by('-seq'):
            table = tables().get(change.table)
            if table is None:
                continue
            pk = _quote(table.pk)
            if change.act == 'I':
                cursor.execute(f'DELETE FROM {_quote(table.name)} WHERE {pk} = %s', [change.new[table.pk]])
                done += cursor.rowcount > 0
            elif change.act == 'D':
                row = change.old
                cursor.execute(f'SELECT 1 FROM {_quote(table.name)} WHERE {pk} = %s', [row[table.pk]])
                if cursor.fetchone():
                    kept += 1
                    continue
                columns = [column for column in table.columns if column in row]
                cursor.execute(f'INSERT INTO {_quote(table.name)} ({", ".join(map(_quote, columns))}) '
                               f'VALUES ({", ".join(["%s"] * len(columns))})', [row[column] for column in columns])
                done += 1
            else:
                columns = [column for column in change.new if column in table.columns]
                if not columns:
                    continue
                # valeurs lues comme les declencheurs les ont notees (texte JSON), pas converties par Django
                cursor.execute(f'SELECT {_row_json(_quote(table.name), columns)} FROM {_quote(table.name)} '
                               f'WHERE {pk} = %s', [_pk_value(table, change.row)])
                current = cursor.fetchone()
                if current is None:
                    kept += 1
                    continue
                current = json.loads(current[0])
                written = _later_writes(change.table, change.row, batch, excluded, cache)
                restore = {}
                for column, value in zip(columns, current):
                    if (written is None or column in written) or value != change.new[column]:
                        kept += 1
                        continue
                    restore[column] = change.old[column]
                if restore:
                    assignments = ', '.join(f'{_quote(column)} = %s' for column in restore)
                    cursor.execute(f'UPDATE {_quote(table.name)} SET {assignments} WHERE {pk} = %s',
                                   [*restore.values(), _pk_value(table, change.row)])
                    done += 1
    return done, kept


def _pk_value(table, row):
    field = table.model._meta.pk
    return int(row) if field.get_internal_type() in ('AutoField', 'BigAutoField', 'IntegerField',
                                                     'PositiveIntegerField') else row


def _when(operation):
    local = timezone.localtime(operation.at)
    return local.strftime('%d/%m/%Y à %H:%M')


def describe(operation):
    """« vérif de Sac de soin du 12/11/2026 à 14:05 » (ligne visee par une annulation)."""
    from .models import OperationKind
    if operation.kind == OperationKind.CANCEL:
        return f'annulation du {_when(operation)}'
    label = operation.get_kind_display()
    label = label[:1].lower() + label[1:]
    return label + (f' ({operation.lot_name})' if operation.lot_name else '') + f' du {_when(operation)}'


def cancel(operation, identity, reason):
    """Annule `operation` et celles qui en dependent, au nom de `identity`, avec un motif. Renvoie la ligne
    d'annulation du journal. A appeler pendant une requete (capture) : l'annulation peut etre annulee a son tour."""
    from .models import Operation, OperationKind
    reason = (reason or '').strip()
    if not reason:
        raise ValueError("Un motif est obligatoire pour annuler une opération")
    with transaction.atomic():
        operation = Operation.objects.select_for_update().get(pk=operation.pk)
        batches = plan(operation)
        cancelled = [op for ops in _batch_operations(batches).values() for op in ops]
        cache = {}
        done = kept = 0
        for batch in batches:
            batch_done, batch_kept = _undo_batch(batch, set(batches), cache)
            done += batch_done
            kept += batch_kept
        names = sorted({tables()[name].name for name in tables()})
        connection.check_constraints(table_names=names)
        others = [op for op in cancelled if op.batch != operation.batch]
        if operation.kind == OperationKind.CANCEL:
            target = operation.details.get('target')
            original = Operation.objects.filter(id=target).first() if target else None
            summary = f'annulation annulée ({describe(original) if original else describe(operation)} rétablie)'
        else:
            summary = describe(operation) + ' annulée'
        if others:
            verb = 'dépendaient' if len(others) > 1 else 'dépendait'
            summary += f" (et {journal.plural(len(others), 'opération')} qui en {verb})"
        summary = summary[:1].upper() + summary[1:]
        line = journal.record(OperationKind.CANCEL, identity, f'{summary} : {reason}', lot=operation.lot,
                              target=operation.id, reason=reason, cancels=[op.id for op in cancelled],
                              undone=done, kept=kept)
        Operation.objects.filter(id__in=[op.id for op in cancelled]).update(cancelled_by=line)
    return line
