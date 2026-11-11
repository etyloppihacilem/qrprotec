"""Cache de vérif des fronts : tout ce qu'une vérif lit au serveur, chargé d'un coup en arrière-plan.

Une partie de la réserve n'a ni réseau ni wifi : de « Lancer la vérif » à l'envoi, chaque scan doit se traiter sans
attendre le serveur (bip, signalement, désactivation, lot où l'item est rangé). Les fronts chargent donc, au début de
la vérif, le détail de tous les lots actifs, de tous les items qu'on peut encore scanner, des paquets fermés et de
tous les types d'items, puis ne redemandent que ce qui a changé.

Le cache est fait d'enregistrements (« i:<iid> » item, « l:<id> » lot, « p:<id> » paquet fermé, « t:<code> » type
d'item) répartis en BUCKETS seaux selon leur clé. Le front envoie l'empreinte de chacun de ses seaux ; le serveur
renvoie seulement les seaux dont l'empreinte a changé, avec tout leur contenu (un enregistrement absent d'un seau
renvoyé a disparu). Sans empreinte, tout est renvoyé. Rien n'est gardé côté serveur entre deux appels : plusieurs
processus (API publique, locale, distante) répondent pareil.

Les items d'un lot n'y sont que par leur iid (« items », « missing_items ») : le front les reprend dans les
enregistrements d'items, avec « optional » (items facultatifs dans ce lot). Les clés des lots et les codes des
scellés n'y sont jamais : ils restent contrôlés par le serveur.
"""
import hashlib
import json
import threading
import time
from datetime import timedelta

from django.utils import timezone

from . import serializers as ser
from .idendity import is_anonymized
from .models import ItemStatus, ItemType, Items, Lots, SealedPacks

BUCKETS = 256
# items qui ne sont plus dans le stock (supprimes, remplaces) : gardes tant qu'une vieille etiquette peut encore
# trainer dans un sac, pour afficher « Marqué supprimé » sans le serveur
GONE_DAYS = 365
# paquets ouverts depuis plus longtemps : leur etiquette n'est plus scannee
OPENED_PACK_DAYS = 180
# plusieurs fronts qui chargent ou se mettent a jour en meme temps : le meme calcul resert quelques secondes (le
# detail d'un item scanne, lui, est toujours redemande au serveur)
REUSE_SECONDS = 10

_lock = threading.Lock()
_last = {'at': 0.0, 'records': None, 'anonymized': None}


def _item_records(today):
    since = timezone.now() - timedelta(days=GONE_DAYS)
    queryset = (Items.objects.select_related('pack__item_type', 'location__lot_type')
                .exclude(status__in=(ItemStatus.DELETED, ItemStatus.REPLACED), last_seen__lt=since))
    return {f'i:{item.iid}': ser.item_dict(item, today) for item in queryset.iterator(chunk_size=500)}


def _strip_items(row):
    """Lignes d'items d'un lot (detail du lot) remplacees par leurs iids, « optional » a part."""
    optional = [item['iid'] for item in row.get('items', []) + row.get('missing_items', []) if item.get('optional')]
    row['items'] = [item['iid'] for item in row.get('items', [])]
    row['missing_items'] = [item['iid'] for item in row.get('missing_items', [])]
    row['optional_items'] = optional
    return row


def _lot_records(today):
    """Lots globaux et lots seuls, avec tous leurs sous-lots (« descendants ») : le front trouve un sous-lot dans
    l'arbre de son lot global, la vérif se fait toujours depuis le lot global (loadLot)."""
    records = {}
    roots = Lots.objects.select_related('lot_type', 'replaces').filter(active=True).exclude(parent__active=True)
    for lot in roots:
        # meme detail que lots/<id>/ (views.lot_detail), sans cle ni depart : la fiche reste en ligne
        data = ser.lot_dict(lot, with_items=True, today=today)
        _strip_items(data)
        for row in data.get('descendants', []):
            _strip_items(row)
        records[f'l:{lot.id}'] = data
    return records


def _pack_records():
    since = timezone.now() - timedelta(days=OPENED_PACK_DAYS)
    queryset = SealedPacks.objects.select_related('item_type').exclude(opened__lt=since)
    return {f'p:{pack.id}': ser.sealed_pack_dict(pack) for pack in queryset}


def _type_records():
    return {f't:{item_type.type}': ser.item_type_dict(item_type) for item_type in ItemType.objects.all()}


def build():
    """Tous les enregistrements du cache, {cle: valeur}."""
    with _lock:
        now = time.monotonic()
        # noms courts sur l'API publique, complets sur l'API locale ou distante : pas le meme calcul
        anonymous = is_anonymized()
        if (_last['records'] is not None and _last['anonymized'] == anonymous
                and now - _last['at'] < REUSE_SECONDS):
            return _last['records']
        today = timezone.localdate()
        records = {**_type_records(), **_item_records(today), **_pack_records(), **_lot_records(today)}
        _last.update(at=time.monotonic(), records=records, anonymized=anonymous)
        return records


def forget():
    """Prochain build() recalcule tout (tests, apres une ecriture)."""
    with _lock:
        _last.update(at=0.0, records=None)


def bucket_of(key: str) -> int:
    return hashlib.sha1(key.encode()).digest()[0] % BUCKETS


def _serialized(value) -> str:
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(',', ':'), default=str)


def snapshot(records):
    """{seau: (empreinte, {cle: valeur})} ; empreinte : sha1 des enregistrements du seau, tries par cle."""
    buckets = {}
    for key, value in records.items():
        buckets.setdefault(bucket_of(key), {})[key] = value
    result = {}
    for index in range(BUCKETS):
        content = buckets.get(index, {})
        digest = hashlib.sha1()
        for key in sorted(content):
            digest.update(key.encode())
            digest.update(b'\0')
            digest.update(_serialized(content[key]).encode())
            digest.update(b'\0')
        result[index] = (digest.hexdigest()[:16], content)
    return result


def diff(known):
    """Réponse du serveur au front qui a déjà les seaux `known` ({seau: empreinte}) : les seaux changés seulement.

    {"version": empreinte de tout le cache, "generated": date, "buckets": {seau: {"hash", "records"}}, "count"}"""
    known = known if isinstance(known, dict) else {}
    buckets = snapshot(build())
    changed = {}
    total = hashlib.sha1()
    for index in range(BUCKETS):
        digest, content = buckets[index]
        total.update(digest.encode())
        if str(known.get(str(index), known.get(index, ''))) != digest:
            changed[str(index)] = {'hash': digest, 'records': content}
    return {
        'version': total.hexdigest()[:16],
        'generated': timezone.now().isoformat(),
        'buckets_count': BUCKETS,
        'count': sum(len(content) for _, content in buckets.values()),
        'buckets': changed,
    }
