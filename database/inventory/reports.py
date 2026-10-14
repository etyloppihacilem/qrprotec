# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          reports.py
#       -\-    _|__
#        |\___/  . \        Created on 14 Oct. 2026 at 10:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Signalements de problemes (ProblemReport) : un utilisateur connecte signale un item, un type d'item d'un lot ou
un lot, pendant une verif ou depuis sa fiche. Les admins sont prevenus (SMS et notifications web) et suivent le
signalement : nouveau (rouge), en cours de resolution (orange si l'objet est inutilisable, vert s'il l'est toujours),
resolu (vert). Resolu, un signalement d'item indique son issue : item remis en service, desactive (non conforme :
invalide comme un perime, remplace a la prochaine verif) ou retire de l'inventaire.

Un item signale inutilisable doit etre remplace comme un perime tant que son signalement est ouvert
(Items.unusable) ; un item signale n'est pas marque disparu par une verif (Items.reported).
"""

from django.db import transaction
from django.utils import timezone

from . import journal, notifications, services
from .idendity import identity_name
from .models import Items, ItemStatus, ItemType, Lots, OperationKind, ProblemReport, ReportResolution, ReportStatus

COMMENT_LENGTH = 1000
NOTE_LENGTH = 1000


def refresh_item(item):
    """Met a jour les drapeaux de l'item d'apres ses signalements ouverts."""
    if item is None:
        return
    reports = ProblemReport.objects.filter(item=item).exclude(status=ReportStatus.RESOLVED)
    reported = reports.exists()
    unusable = reports.filter(usable=False).exists()
    if (item.reported, item.unusable) != (reported, unusable):
        item.reported, item.unusable = reported, unusable
        item.save(update_fields=['reported', 'unusable'])


def target_name(report):
    """« Tensiomètre (B+ · Sac de soin) », « Sac de soin » : ce qui est signale."""
    if report.item_id or report.item_type_id:
        type_name = report.item_type.name if report.item_type_id else 'Item'
        return f'{type_name} ({report.lot_name})' if report.lot_name else type_name
    return report.lot_name or 'Lot inconnu'


def report_dict(report):
    data = {
        'id': report.id,
        'created': report.created.isoformat() if report.created else None,
        'by': identity_name(report.by),
        'lot': report.lot_id,
        'lot_name': report.lot_name,
        'item': report.item_id,
        'type': report.item_type_id,
        'type_name': report.item_type.name if report.item_type_id else '',
        'target': target_name(report),
        'comment': report.comment,
        'usable': report.usable,
        'status': report.status,
        'status_label': ReportStatus(report.status).label,
        'color': report.color,
        'note': report.note,
        'updated': report.updated.isoformat() if report.updated else None,
        'updated_by': identity_name(report.updated_by),
        'resolved': report.resolved.isoformat() if report.resolved else None,
        'resolved_by': identity_name(report.resolved_by),
        'resolution': report.resolution,
        'resolution_label': ReportResolution(report.resolution).label if report.resolution else '',
    }
    return data


def create(identity, lot_id='', iid='', type_code='', comment='', usable=True):
    """Nouveau signalement ; retourne (signalement, item ou None). L'item donne son lot et son type."""
    item = None
    lot = None
    item_type = None
    if iid:
        item = Items.objects.select_related('pack__item_type', 'location').filter(iid=iid).first()
        if item is None:
            raise ValueError('Item inconnu')
        item_type = item.pack.item_type
        lot = item.location
    if lot_id:
        lot = Lots.objects.filter(id=lot_id).first()
        if lot is None:
            raise ValueError('Lot inconnu')
    if type_code and item_type is None:
        item_type = ItemType.objects.filter(type=type_code).first()
        if item_type is None:
            raise ValueError("Type d'item inconnu")
    if item is None and lot is None:
        raise ValueError('Indiquez le lot ou l\'item concerné')
    comment = str(comment or '').strip()[:COMMENT_LENGTH]
    with transaction.atomic():
        report = ProblemReport.objects.create(
            by=identity, lot=lot, lot_name=lot.name if lot else '', item=item, item_type=item_type,
            comment=comment, usable=bool(usable),
        )
        refresh_item(item)
        state = 'utilisable' if report.usable else 'inutilisable'
        journal.record(OperationKind.REPORT, identity, f'{target_name(report)} : problème signalé ({state})',
                       lot=lot, report=report.id, item=item.iid if item else '', usable=report.usable,
                       comment=comment[:200])
        who = identity_name(identity)
        text = f'{who} signale un problème sur {target_name(report)} ({state})' + (f' : {comment}' if comment else '')
        notifications.notify_admins('problem_report', 'problème signalé', text, url='../#signalements',
                                    tag=f'report-{report.id}', tab='reports')
    return report, item


def update(report, identity, status=None, usable=None, note=None, resolution=None):
    """Prise en charge par un admin : statut, objet utilisable ou non, reponse. En resolvant le signalement d'un
    item, `resolution` dit ce qu'il devient : 'ok' (remis en service), 'disabled' (desactive, non conforme) ou
    'removed' (retire de l'inventaire, deja jete)."""
    changes = []
    now = timezone.now()
    if status is not None and status not in ReportStatus.values:
        raise ValueError('Statut inconnu (new, progress ou resolved)')
    if resolution is not None:
        resolution = str(resolution or '')
        if resolution not in ReportResolution.values:
            raise ValueError('Issue inconnue (ok, disabled ou removed)')
        if resolution and (status or report.status) != ReportStatus.RESOLVED:
            raise ValueError("L'issue se donne en résolvant le signalement")
        if resolution in (ReportResolution.DISABLED, ReportResolution.REMOVED) and report.item is None:
            raise ValueError("Ce signalement ne porte pas sur un item : il ne peut pas être désactivé ni retiré")
    if status is not None and status != report.status:
        report.status = status
        changes.append(ReportStatus(status).label.lower())
        if status == ReportStatus.RESOLVED:
            report.resolved = now
            report.resolved_by = identity
        else:
            report.resolved = None
            report.resolved_by = ''
            report.resolution = ReportResolution.NONE
    if usable is not None and bool(usable) != report.usable:
        report.usable = bool(usable)
        changes.append('utilisable' if report.usable else 'inutilisable')
    resolution_changed = bool(resolution) and resolution != report.resolution
    if resolution_changed:
        report.resolution = resolution
        changes.append(ReportResolution(resolution).label.lower())
    if note is not None:
        note = str(note).strip()[:NOTE_LENGTH]
        if note != report.note:
            report.note = note
            changes.append('réponse')
    if not changes:
        return report
    report.updated = now
    report.updated_by = identity
    with transaction.atomic():
        report.save()
        item = report.item
        if item is not None and resolution_changed:
            reason = f'Non conforme (signalement n°{report.id})'
            if report.comment:
                reason += f' : {report.comment}'
            if resolution == ReportResolution.DISABLED:
                services.set_disabled([item], identity, True, reason)
            elif resolution == ReportResolution.REMOVED and item.status != ItemStatus.DELETED:
                services.mark_deleted(item, identity, reason)
        refresh_item(item)
        journal.record(OperationKind.REPORT, identity, f'{target_name(report)} : ' + ', '.join(changes),
                       lot=report.lot, report=report.id, status=report.status, usable=report.usable,
                       resolution=report.resolution)
    return report


def listing(show='open', limit=200):
    """Signalements ouverts (show='open') ou tous, les plus recents d'abord ; compteurs par etat."""
    queryset = ProblemReport.objects.select_related('item_type').order_by('-created', '-id')
    if show == 'open':
        queryset = queryset.exclude(status=ReportStatus.RESOLVED)
    counts = {
        'new': ProblemReport.objects.filter(status=ReportStatus.NEW).count(),
        'progress': ProblemReport.objects.filter(status=ReportStatus.IN_PROGRESS).count(),
    }
    return {'counts': counts, 'reports': [report_dict(report) for report in queryset[:limit]]}
