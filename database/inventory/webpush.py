# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          webpush.py
#       -\-    _|__
#        |\___/  . \        Created on 01 Oct. 2026 at 12:30
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Notifications web (Web Push) : alternative aux SMS, quel que soit l'operateur.

Un admin connecte sur le front web active les notifications depuis l'onglet Stock : le navigateur
fournit un abonnement (PushSubscription), que le serveur utilise pour envoyer les alertes de stock bas
et de stock vide, meme page fermee.

- Chiffrement du message : RFC 8291 (aes128gcm, RFC 8188), avec un seul enregistrement.
- Authentification du serveur aupres du service de notifications : VAPID (RFC 8292, jeton ES256).
  Les cles sont generees au premier usage et gardees en base (PushKeys).

Le module `cryptography` (paquet python3-cryptography) est necessaire ; sans lui, les notifications
web sont simplement indisponibles.
"""

import base64
import hashlib
import hmac
import json
import logging
import os
import struct
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

from django.db import IntegrityError, close_old_connections, transaction
from django.utils import timezone

from .models import PushKeys, PushSubscription, Role, qrprotec_setting

logger = logging.getLogger(__name__)

RECORD_SIZE = 4096
MAX_PAYLOAD = 3000          # bien en dessous de RECORD_SIZE - 17 (tag + delimiteur)
TTL = 24 * 3600             # duree de garde par le service si le telephone est hors ligne
JWT_VALIDITY = 12 * 3600    # au plus 24 h (RFC 8292)


def available() -> bool:
    try:
        import cryptography  # noqa: F401
    except ImportError:
        return False
    return True


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode()


def b64url_decode(text) -> bytes:
    text = str(text).strip()
    return base64.urlsafe_b64decode(text + '=' * (-len(text) % 4))


# ----------------------------------------------------------------------------------------------------------------------
# Cles VAPID

def _public_point(private_key) -> bytes:
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
    return private_key.public_key().public_bytes(Encoding.X962, PublicFormat.UncompressedPoint)


def server_keys():
    """(cle privee, cle publique base64url), generees et enregistrees au premier appel."""
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.serialization import (
        Encoding, NoEncryption, PrivateFormat, load_pem_private_key,
    )
    row = PushKeys.objects.filter(pk=1).first()
    if row is None:
        private_key = ec.generate_private_key(ec.SECP256R1())
        pem = private_key.private_bytes(Encoding.PEM, PrivateFormat.PKCS8, NoEncryption()).decode()
        try:
            with transaction.atomic():
                row = PushKeys.objects.create(pk=1, private_key=pem, public_key=b64url(_public_point(private_key)))
        except IntegrityError:  # creee en parallele par une autre requete
            row = PushKeys.objects.get(pk=1)
    return load_pem_private_key(row.private_key.encode(), password=None), row.public_key


def public_key() -> str:
    return server_keys()[1]


def subject() -> str:
    return qrprotec_setting('WEB_PUSH_SUBJECT') or qrprotec_setting('PUBLIC_BASE_URL')


def vapid_authorization(endpoint, now=None) -> str:
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
    private_key, public = server_keys()
    url = urllib.parse.urlsplit(endpoint)
    header = b64url(json.dumps({'typ': 'JWT', 'alg': 'ES256'}, separators=(',', ':')).encode())
    claims = b64url(json.dumps({
        'aud': f'{url.scheme}://{url.netloc}',
        'exp': int(now if now is not None else time.time()) + JWT_VALIDITY,
        'sub': subject(),
    }, separators=(',', ':')).encode())
    signing_input = f'{header}.{claims}'
    r, s = decode_dss_signature(private_key.sign(signing_input.encode(), ec.ECDSA(hashes.SHA256())))
    signature = b64url(r.to_bytes(32, 'big') + s.to_bytes(32, 'big'))
    return f'vapid t={signing_input}.{signature}, k={public}'


# ----------------------------------------------------------------------------------------------------------------------
# Chiffrement (RFC 8291)

def _hkdf(salt, ikm, info, length):
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    return hmac.new(prk, info + b'\x01', hashlib.sha256).digest()[:length]


def encrypt(payload: bytes, p256dh, auth, salt=None, sender_key=None) -> bytes:
    """Corps aes128gcm pour l'abonnement (p256dh, auth en base64url). salt et sender_key : pour les tests."""
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    receiver_point = b64url_decode(p256dh)
    receiver = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), receiver_point)
    sender_key = sender_key or ec.generate_private_key(ec.SECP256R1())
    sender_point = _public_point(sender_key)
    shared = sender_key.exchange(ec.ECDH(), receiver)
    ikm = _hkdf(b64url_decode(auth), shared, b'WebPush: info\x00' + receiver_point + sender_point, 32)
    salt = salt or os.urandom(16)
    key = _hkdf(salt, ikm, b'Content-Encoding: aes128gcm\x00', 16)
    nonce = _hkdf(salt, ikm, b'Content-Encoding: nonce\x00', 12)
    record = AESGCM(key).encrypt(nonce, payload + b'\x02', None)  # \x02 : dernier enregistrement
    return salt + struct.pack('!IB', RECORD_SIZE, len(sender_point)) + sender_point + record


def valid_subscription_keys(p256dh, auth) -> bool:
    try:
        return len(b64url_decode(p256dh)) == 65 and len(b64url_decode(auth)) == 16
    except (ValueError, TypeError):
        return False


# ----------------------------------------------------------------------------------------------------------------------
# Envoi

def post_push(endpoint, body: bytes, headers, timeout=10):
    """POST vers le service de notifications du navigateur ; retourne le code HTTP. Remplacee dans les tests."""
    request = urllib.request.Request(endpoint, data=body, headers=headers, method='POST')
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status
    except urllib.error.HTTPError as exc:
        return exc.code


def send_one(subscription, message: dict):
    """Envoie a un abonnement ; retourne (ok, statut lisible). Supprime l'abonnement s'il a expire."""
    payload = json.dumps(message, ensure_ascii=False).encode()
    if len(payload) > MAX_PAYLOAD:
        message = {**message, 'body': message.get('body', '')[:MAX_PAYLOAD // 4] + '…'}
        payload = json.dumps(message, ensure_ascii=False).encode()
    try:
        code = post_push(subscription.endpoint, encrypt(payload, subscription.p256dh, subscription.auth), {
            'Authorization': vapid_authorization(subscription.endpoint),
            'Content-Encoding': 'aes128gcm',
            'Content-Type': 'application/octet-stream',
            'TTL': str(TTL),
            'Urgency': 'high',
        })
    except (urllib.error.URLError, OSError, ValueError) as exc:
        return False, f'Réseau : {exc}'
    if code in (404, 410):  # abonnement expire ou annule par le navigateur
        subscription.delete()
        return False, 'Abonnement expiré (supprimé)'
    return 200 <= code < 300, 'Envoyé' if 200 <= code < 300 else f'Code {code}'


def _deliver(jobs):
    close_old_connections()
    try:
        for subscription_id, message in jobs:
            subscription = PushSubscription.objects.filter(id=subscription_id).first()
            if subscription is None:
                continue
            ok, status = send_one(subscription, message)
            if not ok:
                logger.warning('Notification web vers %s : %s', subscription.user_id, status)
            if subscription.pk is not None:
                subscription.last_sent = timezone.now()
                subscription.last_status = status[:128]
                subscription.save(update_fields=['last_sent', 'last_status'])
    finally:
        close_old_connections()


def _dispatch(jobs):
    # meme reglage que les SMS : envoi dans la requete (tests) ou dans un thread
    if qrprotec_setting('SMS_SYNC'):
        _deliver(jobs)
    else:
        threading.Thread(target=_deliver, args=(jobs,), daemon=True).start()


def queue(jobs):
    """jobs : [(id d'abonnement, message)], envoyes apres la validation de la transaction."""
    if jobs and available():
        transaction.on_commit(lambda: _dispatch(jobs))
    return len(jobs)


def active_subscriptions():
    # un admin retrogade ou desactive ne recoit plus rien (son abonnement reste, inactif)
    return PushSubscription.objects.filter(user__active=True, user__role=Role.ADMIN)


def notify_stock(low, empty):
    """low : [(nom, stock, minimum)] passes sous le minimum ; empty : [nom] arrives a 0.

    Un message par abonnement, selon ses choix ; un type a la fois vide et bas n'apparait qu'une fois
    pour qui a choisi les deux.
    """
    if not low and not empty:
        return 0
    jobs = []
    for subscription in active_subscriptions():
        lines = []
        empty_names = set(empty) if subscription.stock_empty else set()
        if empty_names:
            lines.append('Stock vide : ' + ', '.join(empty))
        if subscription.stock_low:
            rows = [f'{name} {count}/{minimum}' for name, count, minimum in low if name not in empty_names]
            if rows:
                lines.append('Stock bas : ' + ', '.join(rows))
        if lines:
            jobs.append((subscription.id, {
                'title': 'QRProtec : stock vide' if empty_names else 'QRProtec : stock bas',
                'body': '\n'.join(lines),
                'tag': 'stock-empty' if empty_names else 'stock-low',
                'url': '../#stock',
            }))
    return queue(jobs)
