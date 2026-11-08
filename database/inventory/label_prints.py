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

- Web (admins, Gestion de la TECH, bouton +) : nouvelles etiquettes d'items d'un type existant, nombre, peremption si
  le type est perissable. « Lancer l'impression » cree les items (reception au nom de l'admin) puis imprime.
- Front d'un autre ordinateur (API distante) : « Imprimer » envoie les etiquettes deja rendues (images 1 bit), que le
  poste imprime telles quelles. Le poste est le poste principal : front de l'API locale du
  serveur, ou kiosk d'une autre machine dont la cle est marquee poste (FrontKey.poste, back sans kiosk).

La fenetre du poste s'ouvre d'elle-meme, meme sans personne de connecte. Une demande pas imprimee expire en 24 h.
"""

import base64
import binascii
from datetime import timedelta

from django.db import transaction
from django.db.models import Q
from django.utils import timezone

from . import journal
from . import notifications
from . import serializers as ser
from .idendity import identity_name
from .models import ItemsPacks, Items, ItemType, LabelPrint, LabelPrintKind, OperationKind

MAX_COUNT = 200          # etiquettes par demande (rouleau de l'imprimante)
RECENT_HOURS = 24        # impressions recentes montrees au poste (reimpression si le rouleau a coince)
WEB_RECENT_DAYS = 7      # impressions faites montrees sur le web
MAX_LABEL_PIXELS = 1200 * 1200  # image d'une etiquette d'un front (largeur x hauteur, 1 bit par pixel)


class PrintError(Exception):
    def __init__(self, message, code=400):
        super().__init__(message)
        self.message = message
        self.code = code


def _expiry():
    return timezone.now() - timedelta(hours=LabelPrint.EXPIRY_HOURS)


def pending_rows():
    """Demandes a imprimer : ni imprimees, ni annulees, ni expirees (plus de 24 h)."""
    return LabelPrint.objects.select_related('item_type').filter(printed__isnull=True, cancelled__isnull=True,
                                                                 created__gte=_expiry())


def what(row):
    """« 5 × Compresses, péremption 31/12/2027 », ou « 3 étiquettes : Lot B+ 1 (clé) » pour celles d'un front."""
    if row.kind == LabelPrintKind.LABELS:
        return journal.plural(row.count, 'étiquette') + (f' : {row.title}' if row.title else '')
    text = f'{row.count} × {row.item_type.name}'
    if row.peremption:
        text += f", péremption {row.peremption.strftime('%d/%m/%Y')}"
    return text


def print_dict(row, full=False):
    """full : avec les items crees (etiquettes d'items) ou les images (etiquettes d'un front), pour imprimer."""
    items_kind = row.kind == LabelPrintKind.ITEMS
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
    }
    if full and items_kind:
        found = {item.iid: item for item in
                 Items.objects.select_related('pack__item_type', 'location__lot_type').filter(iid__in=row.items)}
        today = timezone.localdate()
        data['items'] = [ser.item_dict(found[iid], today) for iid in row.items if iid in found]
    if full and not items_kind:
        data['labels'] = row.labels
    return data


def create(by, type_code, count, peremption):
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
    return LabelPrint.objects.create(by=by[:64], item_type=item_type, count=count,
                                     peremption=peremption if item_type.perissable else None)


def _check_label(label):
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
    return {'description': str(label.get('description', ''))[:128], 'width': width, 'height': height, 'ppm': ppm,
            'bits': str(label['bits'])}


def create_labels(by, front, title, labels):
    """« Imprimer » sur le front d'un autre ordinateur : etiquettes deja rendues, envoyees au poste."""
    if not isinstance(labels, list) or not labels:
        raise PrintError('Aucune étiquette à imprimer')
    if len(labels) > MAX_COUNT:
        raise PrintError(f'Au plus {MAX_COUNT} étiquettes par envoi')
    checked = [_check_label(label) for label in labels]
    return LabelPrint.objects.create(by=by[:64], kind=LabelPrintKind.LABELS, count=len(checked),
                                     title=str(title or '')[:128], front=str(front or '')[:64], labels=checked)


def web_list():
    """Types proposes (non archives) et etiquettes d'items demandees depuis le web : en attente, puis recentes."""
    since = timezone.now() - timedelta(days=WEB_RECENT_DAYS)
    rows = LabelPrint.objects.select_related('item_type').filter(kind=LabelPrintKind.ITEMS).filter(
        Q(created__gte=since) | Q(printed__isnull=True, cancelled__isnull=True, created__gte=_expiry()))
    rows = sorted(rows, key=lambda row: (not row.pending, -row.created.timestamp()))
    types = [{'type': t.type, 'name': t.name, 'perissable': t.perissable}
             for t in ItemType.objects.filter(archived=False, physical=True).order_by('name')]
    return {'types': types, 'prints': [print_dict(row) for row in rows], 'max_count': MAX_COUNT,
            'expiry_hours': LabelPrint.EXPIRY_HOURS}


def cancel(print_id, by):
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
    return row


def poste_list():
    """Fenetre du poste (interrogee toutes les 20 s, donc sans les images ni les items) : demandes en attente (les
    plus anciennes d'abord) et impressions des dernieres 24 h."""
    since = timezone.now() - timedelta(hours=RECENT_HOURS)
    recent = LabelPrint.objects.select_related('item_type').filter(printed__gte=since).order_by('-printed')
    return {'pending': [print_dict(row) for row in pending_rows().order_by('created')],
            'recent': [print_dict(row) for row in recent], 'expiry_hours': LabelPrint.EXPIRY_HOURS}


def detail(print_id):
    """Demande complete (items ou images), pour « Réimprimer » au poste."""
    row = LabelPrint.objects.select_related('item_type').filter(id=print_id).first()
    if row is None:
        raise PrintError('Impression introuvable', 404)
    return print_dict(row, full=True)


def print_now(print_id, front_name=''):
    """« Lancer l'impression » au poste. Etiquettes d'items : cree les items (reception au nom de l'admin qui les a
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
        row.save(update_fields=['printed', 'printed_on', 'items'])
    return print_dict(row, full=True)


def pending_tasks():
    """Liste de taches de la gestion (De temps en temps) : demandes pas encore imprimees au poste."""
    return [(row, what(row)) for row in pending_rows().order_by('created')]
