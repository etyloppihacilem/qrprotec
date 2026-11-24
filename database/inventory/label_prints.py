# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          label_prints.py
#       -\-    _|__
#        |\___/  . \        Created on 07 Nov. 2026 at 11:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Impressions demandees au poste depuis ailleurs, lancees dans sa fenetre « Impressions en attente » (voir
models.LabelPrint).

- Web (gestion et admins, Gestion de la TECH, bouton +) : nouvelles etiquettes d'items d'un type existant, nombre,
  peremption si le type est perissable. « Lancer l'impression » cree les items (reception au nom de qui les a
  demandes) puis imprime.
- Web (gestion et admins, fiche d'un paquet ferme) : etiquettes des items du paquet qu'on ouvre (items deja crees a
  la reception), ou son etiquette de paquet ouvert (paquet pre-etiquete, voir packs.py).
- Front d'un autre ordinateur (API distante) : « Imprimer » envoie les etiquettes deja rendues (images 1 bit), que le
  poste imprime telles quelles. Le poste est le poste principal : front de l'API locale du
  serveur, ou kiosk d'une autre machine dont la cle est marquee poste (FrontKey.poste, back sans kiosk).

La fenetre du poste s'ouvre d'elle-meme, meme sans personne de connecte. Une demande pas imprimee expire en 24 h.

Files d'impression : une par personne qui demande (LabelPrint.queue), pour que chacun retrouve son ruban quand plusieurs
portables envoient en meme temps. Au poste, on choisit la file (ou on scanne son badge) et on n'imprime qu'elle. Les
admins voient les files de n'importe ou (web, autre ordinateur), les annulent ou les lancent a distance : le poste
imprime une file lancee a son prochain passage.

Le poste interroge la file toutes les 20 s ; apres une premiere demande, il recoit les suivantes en direct pendant
15 min par WebSocket (voir print_live.py) : chaque changement de la file le lui signale (live.changed()).
"""

from __future__ import annotations

import base64
import binascii
from datetime import timedelta
from typing import TYPE_CHECKING, Any

from django.db import transaction
from django.db.models import Q
from django.utils import timezone

from . import journal
from . import notifications
from .print_live import hub as live
from . import serializers as ser
from .idendity import SOURCE_VERIFIED, identity_name
from . import packs
from .models import ItemsPacks, Items, ItemType, LabelPrint, LabelPrintKind, OperationKind

if TYPE_CHECKING:
    from datetime import date, datetime
    from django.db.models.query import QuerySet

MAX_COUNT = 200          # etiquettes par demande (rouleau de l'imprimante)
RECENT_HOURS = 24        # impressions recentes montrees au poste (reimpression si le rouleau a coince)
WEB_RECENT_DAYS = 7      # impressions faites montrees sur le web
MAX_LABEL_PIXELS = 1200 * 1200  # image d'une etiquette d'un front (largeur x hauteur, 1 bit par pixel)


class PrintError(Exception):
    def __init__(self, message: str, code: int = 400) -> None:
        super().__init__(message)
        self.message = message
        self.code = code


def _expiry() -> datetime:
    return timezone.now() - timedelta(hours=LabelPrint.EXPIRY_HOURS)


def pending_rows() -> QuerySet:
    """Demandes a imprimer : ni imprimees, ni annulees, ni expirees (plus de 24 h)."""
    return LabelPrint.objects.select_related('item_type').filter(printed__isnull=True, cancelled__isnull=True,
                                                                 created__gte=_expiry())


def what(row: LabelPrint) -> str:
    """« 5 × Compresses, péremption 31/12/2027 », ou « 3 étiquettes : Lot B+ 1 (clé) » pour celles d'un front."""
    if row.kind == LabelPrintKind.LABELS:
        return journal.plural(row.count, 'étiquette') + (f' : {row.title}' if row.title else '')
    if row.kind == LabelPrintKind.PACK and row.open_label:
        return f'Étiquette de paquet ouvert : {row.item_type.name}'
    text = f'{row.count} × {row.item_type.name}'
    if row.peremption:
        text += f", péremption {row.peremption.strftime('%d/%m/%Y')}"
    return text


def queue_name(row: LabelPrint) -> str:
    """Nom de la file : la personne qui demande, sinon l'ordinateur d'ou vient l'envoi."""
    if row.by:
        return identity_name(row.by)
    return row.front or 'Demandeur inconnu'


def queues(rows: list[LabelPrint], matricules: bool = False) -> list[dict[str, Any]]:
    """Demandes en attente groupees par file, la plus ancienne demande d'abord. matricules : avec le matricule de qui
    demande, pour que le poste retrouve la file d'un badge scanne (fronts seulement)."""
    groups: dict[str, dict[str, Any]] = {}
    for row in sorted(rows, key=lambda row: row.created):
        group = groups.get(row.queue)
        if group is None:
            group = groups[row.queue] = {'queue': row.queue, 'name': queue_name(row), 'requests': 0, 'labels': 0,
                                         'oldest': ser._date(row.created), 'launch_requested': None,
                                         'launch_requested_by': '', 'ids': []}
            if matricules:
                source, _, payload = row.by.partition(':')
                group['matricule'] = payload if source == SOURCE_VERIFIED else ''
        group['requests'] += 1
        group['labels'] += row.count
        group['ids'].append(row.id)
        if row.launch_requested is not None:
            group['launch_requested'] = ser._date(row.launch_requested)
            group['launch_requested_by'] = identity_name(row.launch_requested_by) if row.launch_requested_by else ''
    return list(groups.values())


def print_dict(row: LabelPrint, full: bool = False) -> dict[str, Any]:
    """full : avec les items crees (etiquettes d'items) ou les images (etiquettes d'un front), pour imprimer."""
    items_kind = row.kind in (LabelPrintKind.ITEMS, LabelPrintKind.PACK)
    data = {
        'id': row.id,
        'kind': row.kind,
        'what': what(row),
        'type': row.item_type_id,
        'type_name': row.item_type.name if items_kind else '',
        'count': row.count,
        'peremption': ser._date(row.peremption),
        'title': row.title,
        'front': row.front,
        'created': ser._date(row.created),
        'by': identity_name(row.by) if row.by else '',
        'printed': ser._date(row.printed),
        'printed_on': row.printed_on,
        'cancelled': ser._date(row.cancelled),
        'cancelled_by': identity_name(row.cancelled_by) if row.cancelled_by else '',
        'expired': row.expired,
        'pending': row.pending,
        'queue': row.queue,
        'queue_name': queue_name(row),
        'launch_requested': ser._date(row.launch_requested),
        'launch_requested_by': identity_name(row.launch_requested_by) if row.launch_requested_by else '',
        'pack': row.sealed_pack_id or '',
        'open_label': row.open_label,
    }
    if full and row.open_label and row.sealed_pack is not None:
        # etiquette de paquet ouvert : imprimee avec la cle actuelle du paquet (une reimpression apres un nouveau
        # pre-etiquetage prend la nouvelle)
        data['pack_data'] = ser.sealed_pack_dict(row.sealed_pack, with_items=False)
        data['pack_data']['open_url'] = ser.pack_open_url(row.sealed_pack)
    elif full and items_kind:
        found = {item.iid: item for item in
                 Items.objects.select_related('pack__item_type', 'location__lot_type').filter(iid__in=row.items)}
        today = timezone.localdate()
        data['items'] = [ser.item_dict(found[iid], today) for iid in row.items if iid in found]
    if full and not items_kind:
        data['labels'] = row.labels
    return data


def create(by: str, type_code: str, count: int, peremption: date | None) -> LabelPrint:
    item_type = ItemType.objects.filter(type=type_code).first()
    if item_type is None:
        raise PrintError("Type d'item inconnu", 404)
    if item_type.archived:
        raise PrintError(f"Le type « {item_type.name} » est archivé")
    if not item_type.physical:
        raise PrintError(f"« {item_type.name} » est un item à cocher : il n'a pas d'étiquette")
    if not 1 <= count <= MAX_COUNT:
        raise PrintError(f"Entre 1 et {MAX_COUNT} étiquettes par demande")
    if item_type.perissable and peremption is None:
        raise PrintError(f"« {item_type.name} » est périssable : indiquez la date de péremption")
    row = LabelPrint.objects.create(by=by[:64], item_type=item_type, count=count,
                                    peremption=peremption if item_type.perissable else None)
    live.changed()
    return row


def _check_label(label: dict[str, Any]) -> dict[str, Any]:
    """Etiquette rendue par un front : {description, width, height, ppm, bits} (bits : base64, 1 bit par pixel,
    lignes de gauche a droite, octets de poids fort d'abord)."""
    if not isinstance(label, dict):
        raise PrintError('labels : liste d\'étiquettes attendue')
    try:
        width, height, ppm = int(label.get('width')), int(label.get('height')), float(label.get('ppm', 8))
    except (TypeError, ValueError):
        raise PrintError('Étiquette : largeur, hauteur ou résolution invalide')
    if not (0 < width and 0 < height and width * height <= MAX_LABEL_PIXELS and 1 <= ppm <= 50):
        raise PrintError('Étiquette : dimensions hors limites')
    try:
        bits = base64.b64decode(str(label.get('bits', '')), validate=True)
    except (binascii.Error, ValueError):
        raise PrintError('Étiquette : image illisible')
    if len(bits) != (width * height + 7) // 8:
        raise PrintError('Étiquette : taille de l\'image incohérente')
    checked = {'description': str(label.get('description', ''))[:128], 'width': width, 'height': height, 'ppm': ppm,
               'bits': str(label['bits'])}
    label_type = _check_label_type(label.get('label_type'))
    if label_type:
        checked['label_type'] = label_type
    return checked


def _check_label_type(label_type: dict[str, Any] | None) -> dict[str, Any] | None:
    """Type d'etiquette du modele (rouleau) : le poste imprime sur ce rouleau et attend qu'il soit charge.
    Absent ou vide : format libre."""
    if label_type in (None, '', {}):
        return None
    if not isinstance(label_type, dict) or not str(label_type.get('name', '')).strip():
        raise PrintError('Étiquette : type d\'étiquette invalide')
    try:
        width, height = float(label_type.get('width_mm')), float(label_type.get('height_mm'))
        rotation = int(label_type.get('rotation', 0))
    except (TypeError, ValueError):
        raise PrintError('Étiquette : type d\'étiquette invalide')
    support = label_type.get('support', 'direct')
    if not (0 < width <= 300 and 0 < height <= 500 and rotation in (0, 90, 180, 270)
            and support in ('direct', 'transfer')):
        raise PrintError('Étiquette : type d\'étiquette hors limites')
    checked = {'name': str(label_type['name']).strip()[:64], 'width_mm': width, 'height_mm': height,
               'rotation': rotation, 'support': support}
    # marges (mm) : rien n'est imprime en dehors ; margin_mm pour les quatre cotes, puis une cle par cote si
    # elles different ; absentes, le poste prend sa valeur par defaut
    for key in ('margin_mm', 'margin_top_mm', 'margin_bottom_mm', 'margin_left_mm', 'margin_right_mm'):
        if label_type.get(key) is None:
            continue
        try:
            margin = float(label_type[key])
        except (TypeError, ValueError):
            raise PrintError('Étiquette : type d\'étiquette invalide')
        if not 0 <= margin <= 10:
            raise PrintError('Étiquette : type d\'étiquette hors limites')
        checked[key] = margin
    return checked


def create_labels(by: str, front: str, title: str, labels: list[dict[str, Any]]) -> LabelPrint:
    """« Imprimer » sur le front d'un autre ordinateur : etiquettes deja rendues, envoyees au poste."""
    if not isinstance(labels, list) or not labels:
        raise PrintError('Aucune étiquette à imprimer')
    if len(labels) > MAX_COUNT:
        raise PrintError(f'Au plus {MAX_COUNT} étiquettes par envoi')
    checked = [_check_label(label) for label in labels]
    row = LabelPrint.objects.create(by=by[:64], kind=LabelPrintKind.LABELS, count=len(checked),
                                    title=str(title or '')[:128], front=str(front or '')[:64], labels=checked)
    live.changed()
    return row


def web_list() -> dict[str, Any]:
    """Types proposes (non archives) et etiquettes d'items demandees depuis le web : en attente, puis recentes."""
    since = timezone.now() - timedelta(days=WEB_RECENT_DAYS)
    rows = LabelPrint.objects.select_related('item_type').filter(
        kind__in=[LabelPrintKind.ITEMS, LabelPrintKind.PACK]).filter(
        Q(created__gte=since) | Q(printed__isnull=True, cancelled__isnull=True, created__gte=_expiry()))
    rows = sorted(rows, key=lambda row: (not row.pending, -row.created.timestamp()))
    types = [{'type': t.type, 'name': t.name, 'perissable': t.perissable, 'description': t.description}
             for t in ItemType.objects.filter(archived=False, physical=True).order_by('name')]
    return {'types': types, 'prints': [print_dict(row) for row in rows], 'max_count': MAX_COUNT,
            'expiry_hours': LabelPrint.EXPIRY_HOURS}


def cancel(print_id: int, by: str) -> LabelPrint:
    with transaction.atomic():
        row = LabelPrint.objects.select_related('item_type').filter(id=print_id).first()
        if row is None:
            raise PrintError('Impression introuvable', 404)
        if row.printed is not None:
            raise PrintError('Déjà imprimée au poste', 409)
        if row.cancelled is None:
            row.cancelled = timezone.now()
            row.cancelled_by = by[:64]
            row.save(update_fields=['cancelled', 'cancelled_by'])
            live.changed()
    return row


def poste_list() -> dict[str, Any]:
    """Fenetre du poste (interrogee toutes les 20 s, donc sans les images ni les items) : demandes en attente (les
    plus anciennes d'abord) et impressions des dernieres 24 h."""
    since = timezone.now() - timedelta(hours=RECENT_HOURS)
    recent = LabelPrint.objects.select_related('item_type').filter(printed__gte=since).order_by('-printed')
    pending = list(pending_rows().order_by('created'))
    return {'pending': [print_dict(row) for row in pending], 'queues': queues(pending, matricules=True),
            'recent': [print_dict(row) for row in recent], 'expiry_hours': LabelPrint.EXPIRY_HOURS}


def admin_queues(matricules: bool = False) -> dict[str, Any]:
    """Admins, de n'importe ou (web, autre ordinateur) : files en attente au poste, avec leurs demandes."""
    pending = list(pending_rows().order_by('created'))
    listed = queues(pending, matricules)
    by_queue: dict[str, list[dict[str, Any]]] = {}
    for row in pending:
        by_queue.setdefault(row.queue, []).append(print_dict(row))
    for group in listed:
        group['prints'] = by_queue.get(group['queue'], [])
    return {'queues': listed, 'expiry_hours': LabelPrint.EXPIRY_HOURS}


def _queue_rows(queue: str) -> list[LabelPrint]:
    if not isinstance(queue, str):
        raise PrintError('queue : file attendue')
    rows = [row for row in pending_rows().order_by('created') if row.queue == queue]
    if not rows:
        raise PrintError("Plus rien en attente dans cette file (déjà imprimée, annulée ou expirée)", 404)
    return rows


def launch_queue(queue: str, by: str) -> int:
    """« Lancer l'impression » a distance (admins) : le poste imprime ces demandes a son prochain passage, sans que
    personne n'appuie sur le bouton. Repond le nombre de demandes lancees."""
    with transaction.atomic():
        rows = _queue_rows(queue)
        now = timezone.now()
        LabelPrint.objects.filter(id__in=[row.id for row in rows]).update(launch_requested=now,
                                                                          launch_requested_by=by[:64])
        live.changed()
    return len(rows)


def cancel_queue(queue: str, by: str) -> int:
    """Annule toute une file (admins, de n'importe ou). Repond le nombre de demandes annulees."""
    with transaction.atomic():
        rows = _queue_rows(queue)
        LabelPrint.objects.filter(id__in=[row.id for row in rows]).update(cancelled=timezone.now(),
                                                                          cancelled_by=by[:64])
        live.changed()
    return len(rows)


def detail(print_id: int) -> dict[str, Any]:
    """Demande complete (items ou images), pour « Réimprimer » au poste."""
    row = LabelPrint.objects.select_related('item_type').filter(id=print_id).first()
    if row is None:
        raise PrintError('Impression introuvable', 404)
    return print_dict(row, full=True)


def print_now(print_id: int, front_name: str = '') -> dict[str, Any]:
    """« Lancer l'impression » au poste. Etiquettes d'items : cree les items (reception au nom de qui les a
    demandes, au stock). Repond la demande complete, dont le poste imprime les etiquettes. Une seule fois : un second
    poste qui lance la meme demande est refuse, comme une demande annulee ou expiree."""
    with transaction.atomic():
        row = LabelPrint.objects.select_for_update().select_related('item_type').filter(id=print_id).first()
        if row is None:
            raise PrintError('Impression introuvable', 404)
        if row.cancelled is not None:
            raise PrintError(f'Impression annulée par {identity_name(row.cancelled_by)}', 409)
        if row.printed is not None:
            raise PrintError('Déjà imprimée' + (f' sur {row.printed_on}' if row.printed_on else ''), 409)
        if row.expired:
            raise PrintError(f'Demande expirée (pas imprimée en {LabelPrint.EXPIRY_HOURS} h) : refaites-la', 410)
        row.printed = timezone.now()
        row.printed_on = front_name[:64]
        if row.kind == LabelPrintKind.ITEMS:
            if row.item_type.perissable and row.peremption is None:
                raise PrintError(f"« {row.item_type.name} » est devenu périssable : refaites la demande")
            created = ItemsPacks.objects.add_items(row.item_type, row.peremption, row.count, row.by[:32])
            row.items = [item.iid for item in created]
            summary = what(row) + ' (étiquettes demandées depuis le web, imprimées au poste'
            summary += f' {front_name})' if front_name else ')'
            journal.record(OperationKind.RECEPTION, row.by, summary, type=row.item_type_id, count=row.count,
                           peremption=row.peremption.isoformat() if row.peremption else None,
                           items=journal.item_ids(created), label_print=row.id)
            notifications.check_stock_levels([row.item_type_id])
        if row.kind == LabelPrintKind.PACK and row.sealed_pack is not None and not row.open_label:
            # etiquettes des items du paquet ouvert depuis le web (crees a la reception)
            row.items = list(Items.objects.filter(sealed_pack=row.sealed_pack, deleted__isnull=True)
                             .order_by('iid').values_list('iid', flat=True))
            packs.mark_labels_printed(row.sealed_pack, row.printed)
        if row.kind == LabelPrintKind.PACK and row.sealed_pack is not None and row.open_label \
                and not row.sealed_pack.open_label_key:
            raise PrintError("Ce paquet n'est plus pré-étiqueté : refaites la demande depuis sa fiche", 409)
        row.save(update_fields=['printed', 'printed_on', 'items'])
        live.changed()
    return print_dict(row, full=True)


def pending_tasks() -> list[tuple[LabelPrint, str]]:
    """Liste de taches de la gestion (De temps en temps) : demandes pas encore imprimees au poste."""
    return [(row, what(row)) for row in pending_rows().order_by('created')]
