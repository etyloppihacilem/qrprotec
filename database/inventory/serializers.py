# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          serializers.py
#       -\-    _|__
#        |\___/  . \        Created on 29 Sep. 2026 at 14:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Conversion des modeles en dictionnaires JSON pour les API."""

from datetime import datetime, timedelta
from urllib.parse import urlencode

from django.utils import timezone

from .idendity import parse_identity
from .models import ItemStatus, qrprotec_setting
from .services import requirements_status


def public_url(path: str, **params) -> str:
    base = qrprotec_setting('PUBLIC_BASE_URL')
    if not base.endswith('/'):
        base += '/'
    return f"{base}{path}?{urlencode(params)}"


def _date(value):
    """Date seule (peremption, expiration) ou horodatage en heure locale (verifs, passages)."""
    if not value:
        return None
    if isinstance(value, datetime):
        return timezone.localtime(value).isoformat(timespec='seconds')
    return value.isoformat()


def _who(value):
    return parse_identity(value)['display_name'] if value else ''


def item_type_dict(item_type):
    return {
        'type': item_type.type,
        'name': item_type.name,
        'description': item_type.description,
        'min_quantity': item_type.min_quantity,
        'perissable': item_type.perissable,
        'default_pack_size': item_type.default_pack_size,
    }


def item_dict(item, today=None):
    today = today or timezone.localdate()
    return {
        'iid': item.iid,
        'type': item.pack.item_type_id,
        'type_name': item.pack.item_type.name,
        'peremption': _date(item.pack.peremption),
        'expired': item.is_expired(today),
        'status': item.status,
        'location': item.location_id,
        'location_name': item.location.name if item.location_id else '',
        'sealed_pack': item.sealed_pack_id,
        'missed_verifs': item.missed_verifs,
        'last_seen': _date(item.last_seen),
        'last_seen_by': _who(item.last_seen_by),
        'last_seen_while': item.last_seen_while,
        'added': _date(item.added),
        'added_by': _who(item.added_by),
        'deleted_reason': item.deleted_reason,
    }


def sealed_pack_dict(sealed_pack, with_items=True):
    data = {
        'id': sealed_pack.id,
        'type': sealed_pack.item_type_id,
        'type_name': sealed_pack.item_type.name,
        'peremption': _date(sealed_pack.peremption),
        'count': sealed_pack.count,
        'created': _date(sealed_pack.created),
        'created_by': _who(sealed_pack.created_by),
        'opened': _date(sealed_pack.opened),
        'opened_by': _who(sealed_pack.opened_by),
        'url': public_url('pack', id=sealed_pack.id),
    }
    if with_items:
        data['items'] = [item.iid for item in sealed_pack.items.order_by('iid')]
    return data


def lot_type_dict(lot_type):
    return {
        'type': lot_type.type,
        'name': lot_type.name,
        'description': lot_type.description,
        'version': lot_type.version,
        'requirements': [
            {
                'type': requirement.item_type_id,
                'type_name': requirement.item_type.name,
                'quantity': requirement.quantity,
                'location': requirement.location,
            }
            for requirement in lot_type.requirements.select_related('item_type').order_by('location', 'item_type__name')
        ],
    }


def lot_dict(lot, local=False, with_items=False, today=None):
    today = today or timezone.localdate()
    items = list(lot.items.select_related('pack__item_type').filter(status=ItemStatus.ACTIVE).order_by('iid'))
    requirements = requirements_status(lot, today)
    expired = [item for item in items if item.is_expired(today)]
    soon_limit = today + timedelta(days=30)
    soon = [item for item in items if item.pack.peremption and today <= item.pack.peremption <= soon_limit]
    data = {
        'id': lot.id,
        'lot_type': lot.lot_type_id,
        'lot_type_name': lot.lot_type.name,
        'name': lot.name,
        'name_short': lot.name_short,
        'version': lot.version,
        'active': lot.active,
        'is_sealed': lot.is_sealed,
        'sealed': _date(lot.sealed) if lot.is_sealed else None,
        'sealed_by': _who(lot.sealed_by) if lot.is_sealed else '',
        'seal_number': lot.seal_number if lot.is_sealed else '',
        'unsealed': _date(lot.unsealed),
        'unsealed_by': _who(lot.unsealed_by),
        # un lot scelle reste valide jusqu'a la premiere peremption de son contenu
        'valid_until': _date(min((item.pack.peremption for item in items if item.pack.peremption), default=None)),
        'created': _date(lot.created),
        'last_verif': _date(lot.last_verif),
        'last_verif_by': _who(lot.last_verif_by),
        'item_count': len(items),
        'expired_count': len(expired),
        'expiring_soon_count': len(soon),
        'requirements': requirements,
        'complete': all(row['present'] >= row['required'] for row in requirements) and not expired,
        'public_url': public_url('verif', lot=lot.id),
    }
    if with_items:
        data['items'] = [item_dict(item, today) for item in items]
        data['missing_items'] = [
            item_dict(item, today)
            for item in lot.items.select_related('pack__item_type').filter(status=ItemStatus.MISSING).order_by('iid')
        ]
    if local:
        data['verif_key'] = lot.verif_key
        data['verif_key_expires'] = _date(lot.verif_key_expires)
        data['private_url'] = public_url('verif', lot=lot.id, key=lot.verif_key)
        data['seal_url'] = public_url('seal', lot=lot.id, s=lot.seal_code) if lot.is_sealed else ''
    return data


def user_dict(user, local=False):
    data = {
        'matricule': user.matricule,
        'nom': user.nom,
        'prenom': user.prenom,
        'privileged': user.privileged,
        'active': user.active,
        'key_expires': _date(user.key_expires),
    }
    if local:
        data['key'] = user.key or ''
        data['badge_url'] = public_url('badge', m=user.matricule, key=user.key or '')
    return data


def verif_dict(verif):
    return {
        'id': verif.id,
        'lot': verif.lot_id,
        'datetime': _date(verif.datetime),
        'by': _who(verif.by),
        'complete': verif.complete,
        'present_count': verif.present_count,
        'missing_count': verif.missing_count,
        'expired_count': verif.expired_count,
        'replaced_count': verif.replaced_count,
    }


def recipient_dict(recipient):
    # la cle d'identification n'est jamais renvoyee
    return {
        'id': recipient.id,
        'name': recipient.name,
        'user': recipient.user,
        'has_password': bool(recipient.password),
        'active': recipient.active,
        'last_sent': _date(recipient.last_sent),
        'last_status': recipient.last_status,
    }


def notification_settings_dict(settings_row, recipients):
    return {
        'enabled': settings_row.enabled,
        'events': {
            'stock_low': settings_row.stock_low,
            'verif_problem': settings_row.verif_problem,
            'seal_broken': settings_row.seal_broken,
            'expired_daily': settings_row.expired_daily,
        },
        'recipients': [recipient_dict(recipient) for recipient in recipients],
    }
