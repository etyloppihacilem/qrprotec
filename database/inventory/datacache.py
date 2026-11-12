"""Cache des reponses lourdes en lecture (« État des lots », liste des lots du poste), tant que la base n'a pas change.

Ces listes sont recalculees pour chaque telephone qui ouvre l'accueil et pour chaque poste toutes les deux minutes,
alors que la base change bien plus rarement. Sur un processeur de portable (Python sur un coeur), un calcul coute
plusieurs dizaines de millisecondes pour 80 lots : on le garde tant que rien n'a ete ecrit.

Le changement est detecte par SQLite lui-meme : `PRAGMA data_version` d'une connexion qui n'ecrit jamais change des
qu'une AUTRE connexion valide une ecriture, quel que soit le processus (serve, commandes cron, sauvegarde restauree).
Aucune ecriture ne peut donc etre oubliee. En plus, une entree ne vit que MAX_AGE secondes : ce qui depend de l'heure
(credit de confiance du reassort, cle qui expire) reste a jour a la minute pres, et le jour fait partie de la cle.
Base en memoire (tests) ou autre moteur : pas de cache.
"""

import sqlite3
import threading
import time

from django.conf import settings
from django.db import connection

MAX_AGE = 30.0  # secondes

_lock = threading.Lock()
_watcher = None  # (chemin, connexion)
_entries = {}  # cle -> (data_version, instant, valeur)


def _database_path():
    database = settings.DATABASES['default']
    if database['ENGINE'] != 'django.db.backends.sqlite3' or connection.settings_dict['NAME'] != database['NAME']:
        return None  # base de test (renommee par le lanceur de tests)
    name = str(database['NAME'])
    if not name or name == ':memory:' or 'mode=memory' in name:
        return None
    return name


def data_version():
    """Numero qui change a chaque ecriture validee dans la base, None si on ne peut pas le savoir."""
    global _watcher
    path = _database_path()
    if path is None:
        return None
    with _lock:
        try:
            if _watcher is None or _watcher[0] != path:
                if _watcher is not None:
                    _watcher[1].close()
                _watcher = (path, sqlite3.connect(path, check_same_thread=False, isolation_level=None, timeout=5))
            return _watcher[1].execute('PRAGMA data_version').fetchone()[0]
        except sqlite3.Error:
            _watcher = None
            return None


def cached(key, compute):
    """Valeur de compute() pour cette cle, recalculee seulement si la base a change (ou apres MAX_AGE secondes).
    La valeur rendue est partagee : l'appelant ne doit pas la modifier."""
    version = data_version()
    if version is None:
        return compute()
    now = time.monotonic()
    with _lock:
        entry = _entries.get(key)
    if entry is not None and entry[0] == version and now - entry[1] < MAX_AGE:
        return entry[2]
    value = compute()
    # la version lue AVANT le calcul : une ecriture pendant le calcul fera recalculer la prochaine fois
    with _lock:
        _entries[key] = (version, now, value)
    return value


def clear():
    with _lock:
        _entries.clear()
