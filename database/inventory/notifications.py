# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          notifications.py
#       -\-    _|__
#        |\___/  . \        Created on 30 Sep. 2026 at 10:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Notifications SMS par l'API de Free Mobile (les notifications web sont dans webpush.py).

    https://smsapi.free-mobile.fr/sendmsg?user=IDENTIFIANT&pass=CLE&msg=MESSAGE

Chaque destinataire (SmsRecipient) a son propre couple identifiant / cle : l'API de Free n'envoie
qu'au titulaire de la ligne. Les envois partent dans un thread apres la validation de la transaction,
pour ne pas ralentir l'API ; le resultat est garde dans last_status.
"""

import logging
import threading
import urllib.error
import urllib.parse
import urllib.request

from django.db import close_old_connections, transaction
from django.utils import timezone

from . import webpush
from .models import ItemStatus, ItemType, Items, NotificationSettings, SmsRecipient, qrprotec_setting

logger = logging.getLogger(__name__)

FREE_SMS_URL = 'https://smsapi.free-mobile.fr/sendmsg'
MAX_LENGTH = 900  # l'API refuse les messages trop longs

# Codes de retour documentes par Free
STATUS_MESSAGES = {
    200: 'Envoyé',
    400: 'Paramètre manquant',
    402: "Trop d'envois en peu de temps",
    403: "Service non activé sur l'espace abonné ou identifiants incorrects",
    500: 'Erreur du serveur Free, réessayez plus tard',
}

EVENTS = ('stock_low', 'verif_problem', 'seal_broken', 'expired_daily')


def send_free_sms(user, password, message, timeout=10):
    """Envoie un SMS ; retourne (ok, statut lisible). Remplacee dans les tests."""
    query = urllib.parse.urlencode({'user': user, 'pass': password, 'msg': message})
    try:
        with urllib.request.urlopen(f'{FREE_SMS_URL}?{query}', timeout=timeout) as response:
            code = response.status
    except urllib.error.HTTPError as exc:
        code = exc.code
    except (urllib.error.URLError, OSError) as exc:
        return False, f'Réseau : {exc}'
    return code == 200, STATUS_MESSAGES.get(code, f'Code {code}')


def _deliver(recipient_ids, message):
    close_old_connections()
    try:
        for recipient in SmsRecipient.objects.filter(id__in=recipient_ids):
            ok, status = send_free_sms(recipient.user, recipient.password, message)
            if not ok:
                logger.warning('SMS vers %s : %s', recipient.name, status)
            recipient.last_sent = timezone.now()
            recipient.last_status = status[:128]
            recipient.save(update_fields=['last_sent', 'last_status'])
    finally:
        close_old_connections()


def _dispatch(recipient_ids, message):
    if qrprotec_setting('SMS_SYNC'):
        _deliver(recipient_ids, message)
    else:
        threading.Thread(target=_deliver, args=(recipient_ids, message), daemon=True).start()


def send(message, recipients=None):
    """Envoie a tous les destinataires actifs (ou a ceux donnes), sans verifier les reglages."""
    if recipients is None:
        recipients = SmsRecipient.objects.filter(active=True)
    recipient_ids = [recipient.id for recipient in recipients]
    if not recipient_ids:
        return 0
    message = message.strip()
    if len(message) > MAX_LENGTH:
        message = message[:MAX_LENGTH - 3] + '...'
    transaction.on_commit(lambda: _dispatch(recipient_ids, message))
    return len(recipient_ids)


def notify(event, message):
    """Notification d'un evenement, si les SMS et cet evenement sont actives dans les reglages."""
    assert event in EVENTS, event
    settings_row = NotificationSettings.get()
    if not settings_row.enabled or not getattr(settings_row, event):
        return 0
    return send(f'QRProtec : {message}')


def check_stock_levels(type_ids=None):
    """Alerte quand le stock (hors lots, non perime) d'un type passe sous son minimum ou arrive a 0.

    SMS (stock bas) et notifications web (stock bas, stock vide) : une seule alerte par passage sous le
    seuil. ItemType.low_notified est remis a zero quand le stock remonte au minimum, empty_notified
    quand il redevient positif.
    """
    today = timezone.localdate()
    queryset = ItemType.objects.filter(min_quantity__gt=0)
    if type_ids is not None:
        queryset = queryset.filter(type__in=set(type_ids))
    low, empty = [], []
    for item_type in queryset:
        count = Items.objects.filter(
            pack__item_type=item_type, status=ItemStatus.ACTIVE, location__isnull=True
        ).exclude(pack__peremption__lt=today).count()
        changed = []
        if count < item_type.min_quantity and not item_type.low_notified:
            low.append((item_type.name, count, item_type.min_quantity))
            item_type.low_notified = True
            changed.append('low_notified')
        elif count >= item_type.min_quantity and item_type.low_notified:
            item_type.low_notified = False
            changed.append('low_notified')
        if count == 0 and not item_type.empty_notified:
            empty.append(item_type.name)
            item_type.empty_notified = True
            changed.append('empty_notified')
        elif count > 0 and item_type.empty_notified:
            item_type.empty_notified = False
            changed.append('empty_notified')
        if changed:
            item_type.save(update_fields=changed)
    low_text = [f'{name} {count}/{minimum}' for name, count, minimum in low]
    if low_text:
        notify('stock_low', 'stock bas : ' + ', '.join(low_text))
    webpush.notify_stock(low, empty)
    return low_text
