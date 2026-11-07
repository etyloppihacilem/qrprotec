# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          label_prints.py
#       -\-    _|__
#        |\___/  . \        Created on 07 Nov. 2026 at 11:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Impressions d'etiquettes d'items demandees depuis le web et lancees au poste (voir models.LabelPrint).

Web (admins, Gestion de la TECH, bouton +) : type d'item existant, nombre d'etiquettes, peremption si le type est
perissable. Poste : la fenetre « Impressions en attente » s'ouvre d'elle-meme, meme sans personne de connecte, et
« Lancer l'impression » cree les items (reception au nom de l'admin) puis envoie leurs etiquettes a l'imprimante.
"""

from datetime import timedelta

from django.db import transaction
from django.db.models import Q
from django.utils import timezone

from . import journal
from . import notifications
from . import serializers as ser
from .idendity import identity_name
from .models import ItemsPacks, Items, ItemType, LabelPrint, OperationKind

MAX_COUNT = 200          # etiquettes par demande (rouleau de l'imprimante)
RECENT_HOURS = 24        # impressions recentes montrees au poste (reimpression si le rouleau a coince)
WEB_RECENT_DAYS = 7      # impressions faites montrees sur le web


class PrintError(Exception):
    def __init__(self, message, code=400):
        super().__init__(message)
        self.message = message
        self.code = code


def what(row):
    """« 5 × Compresses, péremption 12/2027 »"""
    text = f'{row.count} × {row.item_type.name}'
    if row.peremption:
        text += f", péremption {row.peremption.strftime('%d/%m/%Y')}"
    return text


def print_dict(row, with_items=False):
    data = {
        'id': row.id,
        'type': row.item_type_id,
        'type_name': row.item_type.name,
        'count': row.count,
        'peremption': ser._date(row.peremption),
        'created': ser._date(row.created),
        'by': identity_name(row.by),
        'printed': ser._date(row.printed),
        'printed_on': row.printed_on,
        'cancelled': ser._date(row.cancelled),
        'cancelled_by': identity_name(row.cancelled_by) if row.cancelled_by else '',
        'pending': row.pending,
    }
    if with_items:
        found = {item.iid: item for item in
                 Items.objects.select_related('pack__item_type', 'location__lot_type').filter(iid__in=row.items)}
        today = timezone.localdate()
        data['items'] = [ser.item_dict(found[iid], today) for iid in row.items if iid in found]
    return data


def create(by, type_code, count, peremption):
    item_type = ItemType.objects.filter(type=type_code).first()
    if item_type is None:
        raise PrintError("Type d'item inconnu", 404)
    if item_type.archived:
        raise PrintError(f"Le type « {item_type.name} » est archivé")
    if not 1 <= count <= MAX_COUNT:
        raise PrintError(f"Entre 1 et {MAX_COUNT} étiquettes par demande")
    if item_type.perissable and peremption is None:
        raise PrintError(f"« {item_type.name} » est périssable : indiquez la date de péremption")
    return LabelPrint.objects.create(by=by[:64], item_type=item_type, count=count,
                                     peremption=peremption if item_type.perissable else None)


def web_list():
    """Types proposes (non archives) et demandes en attente puis recentes, pour le front web."""
    since = timezone.now() - timedelta(days=WEB_RECENT_DAYS)
    rows = LabelPrint.objects.select_related('item_type').filter(
        Q(created__gte=since) | Q(printed__isnull=True, cancelled__isnull=True))
    rows = sorted(rows, key=lambda row: (not row.pending, -row.created.timestamp()))
    types = [{'type': t.type, 'name': t.name, 'perissable': t.perissable}
             for t in ItemType.objects.filter(archived=False).order_by('name')]
    return {'types': types, 'prints': [print_dict(row) for row in rows], 'max_count': MAX_COUNT}


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
    """Fenetre du poste : demandes en attente (les plus anciennes d'abord) et impressions des dernieres 24 h."""
    since = timezone.now() - timedelta(hours=RECENT_HOURS)
    pending = LabelPrint.objects.select_related('item_type').filter(printed__isnull=True, cancelled__isnull=True)
    recent = LabelPrint.objects.select_related('item_type').filter(printed__gte=since).order_by('-printed')
    return {'pending': [print_dict(row) for row in pending.order_by('created')],
            'recent': [print_dict(row, with_items=True) for row in recent]}


def print_now(print_id, front_name=''):
    """« Lancer l'impression » au poste : cree les items (reception au nom de l'admin qui les a demandes, au stock)
    et repond leurs etiquettes. Une seule fois : un second poste qui lance la meme demande est refuse."""
    with transaction.atomic():
        row = LabelPrint.objects.select_for_update().select_related('item_type').filter(id=print_id).first()
        if row is None:
            raise PrintError('Impression introuvable', 404)
        if row.cancelled is not None:
            raise PrintError(f'Impression annulée par {identity_name(row.cancelled_by)}', 409)
        if row.printed is not None:
            raise PrintError('Déjà imprimée' + (f' sur {row.printed_on}' if row.printed_on else ''), 409)
        if row.item_type.perissable and row.peremption is None:
            raise PrintError(f"« {row.item_type.name} » est devenu périssable : refaites la demande sans attendre")
        created = ItemsPacks.objects.add_items(row.item_type, row.peremption, row.count, row.by[:32])
        row.printed = timezone.now()
        row.printed_on = front_name[:64]
        row.items = [item.iid for item in created]
        row.save(update_fields=['printed', 'printed_on', 'items'])
        summary = what(row) + ' (étiquettes demandées depuis le web, imprimées au poste'
        summary += f' {front_name})' if front_name else ')'
        journal.record(OperationKind.RECEPTION, row.by, summary, type=row.item_type_id, count=row.count,
                       peremption=row.peremption.isoformat() if row.peremption else None,
                       items=journal.item_ids(created), label_print=row.id)
        notifications.check_stock_levels([row.item_type_id])
    return print_dict(row, with_items=True)


def pending_tasks():
    """Liste de taches de la gestion (De temps en temps) : demandes pas encore imprimees au poste."""
    rows = LabelPrint.objects.select_related('item_type').filter(printed__isnull=True, cancelled__isnull=True)
    return [(row, what(row)) for row in rows.order_by('created')]
