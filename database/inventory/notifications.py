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
from datetime import timedelta

from django.db import close_old_connections, transaction
from django.db.models import Q
from django.utils import timezone

from . import forecast, webpush
from .models import (
    ItemStatus, ItemType, Items, KeyExpiry, Lots, NotificationSettings, Secouristes, SmsRecipient, qrprotec_setting,
)

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

EVENTS = ('stock_low', 'verif_problem', 'seal_broken', 'expired_daily', 'pin_blocked',
          'lot_key_renewed', 'lot_key_expiring', 'badge_renewed', 'badge_expiring', 'order_due')


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
        in_stock = Q(location__isnull=True) | Q(location__lot_type__storage=True)  # rangements du stock compris
        count = Items.objects.filter(
            in_stock, pack__item_type=item_type, status=ItemStatus.ACTIVE
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


def notify_pin_blocked(user, url):
    """PIN bloque (50 essais faux ou code oublie) : SMS et notifications web des admins, avec le lien de deblocage.

    Une seule notification par blocage, quel que soit le nombre de tentatives ou de demandes (pas de spam
    possible) : pin_reset_notified n'est remis a zero que par la reinitialisation du PIN. Retourne True si les
    admins ont ete prevenus (maintenant ou avant).
    """
    # le drapeau est pris avant l'envoi : deux requetes simultanees n'envoient qu'une notification
    if user.pin_reset_notified or not Secouristes.objects.filter(
            pk=user.pk, pin_reset_notified=False).update(pin_reset_notified=True):
        user.pin_reset_notified = True
        return True
    reason = 'a oublié son code PIN' if user.pin_forgotten else 'a son code PIN bloqué après trop d\'essais'
    contact = user.pin_contact
    contact_text = f' (contact : {contact})' if contact is not None and contact.active else ''
    message = f'{user} ({user.matricule}) {reason}{contact_text}. Débloquer : {url}'
    sent = notify('pin_blocked', message) + webpush.notify_pin_blocked(f'{user} ({user.matricule}) {reason}', url)
    if not sent:  # aucun destinataire : la prochaine tentative reessaiera (ex. notifications activees entre-temps)
        Secouristes.objects.filter(pk=user.pk).update(pin_reset_notified=False)
        return False
    user.pin_reset_notified = True
    return True


def notify_admins(event, title, message):
    """Evenement envoye par SMS (reglages) et aux notifications web des admins qui l'ont choisi."""
    return notify(event, message) + webpush.notify_event(event, f'QRProtec : {title}', message)


def _day(value):
    return value.strftime('%d/%m/%Y')


def check_key_expirations(today=None):
    """Etiquettes privees de lot et badges qui expirent bientot (delai des Reglages, ou
    QRPROTEC_KEY_EXPIRY_WARNING_DAYS) ou ont expire.

    Une alerte par etape (bientot, puis expiree) : key_expiry_stage garde l'etape deja annoncee, le renouvellement
    la remet a zero. Les etapes avancent meme si l'evenement est desactive, pour ne pas envoyer d'un coup tout
    l'historique quand on l'active. Retourne {evenement: [lignes]}.
    """
    today = today or timezone.localdate()
    warning_days = NotificationSettings.get().expiry_warning_days
    limit = today + timedelta(days=warning_days)
    sources = (
        ('lot_key_expiring', 'étiquette privée de lot', 'étiquettes privées de lot', 'expirées',
         Lots.objects.filter(active=True, verif_key_expires__lte=limit).order_by('verif_key_expires', 'name'),
         'verif_key_expires', lambda lot: lot.name),
        ('badge_expiring', 'badge', 'badges', 'expirés',
         Secouristes.objects.filter(active=True, key_expires__isnull=False, key_expires__lte=limit)
         .order_by('key_expires', 'nom', 'prenom'),
         'key_expires', lambda user: f'{user} ({user.matricule})'),
    )
    report = {}
    for event, singular, plural, expired_word, queryset, field, label in sources:
        soon, expired = [], []
        for row in queryset:
            expires = getattr(row, field)
            stage = KeyExpiry.of(expires, today, warning_days)
            if stage <= row.key_expiry_stage:
                continue
            (expired if stage == KeyExpiry.EXPIRED else soon).append(f'{label(row)} ({_day(expires)})')
            row.key_expiry_stage = stage
            row.save(update_fields=['key_expiry_stage'])
        lines = []
        if soon:
            lines.append(f'{plural} qui expirent bientôt : ' + ', '.join(soon))
        if expired:
            lines.append(f'{plural} {expired_word} : ' + ', '.join(expired))
        if lines:
            notify_admins(event, f'{singular} à renouveler', ' ; '.join(lines))
        report[event] = soon + expired
    return report


def check_orders(today=None):
    """Commandes a passer d'apres les previsions de stock : types dont la date limite de commande
    (passage sous le minimum moins le delai de commande, ORDER_LEAD_DAYS) est atteinte.

    Une alerte par type et par commande : ItemType.order_notified est remis a zero quand plus aucune commande
    n'est due (stock recu, consommation revue). Le drapeau avance meme si l'evenement est desactive, comme
    pour les expirations. Retourne les lignes annoncees.
    """
    data = forecast.forecast(months=6)
    today = today or data['today']
    notified = set(ItemType.objects.filter(order_notified=True).values_list('type', flat=True))
    due, cleared = [], []
    for row in data['types']:
        order = row['order']
        urgent = order is not None and order['before'] <= today
        if urgent and row['type'] not in notified:
            due.append(row)
        elif not urgent and row['type'] in notified:
            cleared.append(row['type'])
    if cleared:
        ItemType.objects.filter(type__in=cleared).update(order_notified=False)
    if not due:
        return []
    ItemType.objects.filter(type__in=[row['type'] for row in due]).update(order_notified=True)
    lines = [f'{row["name"]} : {row["order"]["quantity"]} (sous le minimum le {_day(row["order"]["below_min"])})'
             for row in due]
    notify_admins('order_due', 'commande à passer', 'commande à passer : ' + ', '.join(lines))
    return lines
