# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          passkeys.py
#       -\-    _|__
#        |\___/  . \        Created on 20 Nov. 2026 at 11:30
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Passkeys (WebAuthn) : connexion au front web sans badge ni PIN, verifiee ici sans bibliotheque WebAuthn.

Creation (secouriste deja connecte) : `registration_options` -> navigator.credentials.create() -> `verify_registration`.
Connexion : `authentication_options` -> navigator.credentials.get() -> `verify_authentication`. Passkeys
« decouvrables » (le telephone propose le compte, pas de nom a taper), verification de l'utilisateur exigee
(empreinte, visage ou code du telephone), attestation « none » (on ne cherche pas a savoir quel telephone c'est).

Le defi est un jeton signe (5 min) et ne sert qu'une fois (cache). Le domaine des passkeys (RP ID) est
QRPROTEC_PASSKEY_RP_ID, sinon le nom d'hote de QRPROTEC_PUBLIC_BASE_URL ; en developpement (QRPROTEC_DEBUG=1) sur
localhost, « localhost ». Le navigateur exige HTTPS avec un certificat valide, sauf sur localhost.

Debogage : chaque refus leve PasskeyError dont le message dit pourquoi (journal du serveur) ; le front web n'en
montre qu'un message general. Voir docs/technique/passkeys.md.
"""

from __future__ import annotations

import base64
import hashlib
import json
import logging
import secrets
import struct
from urllib.parse import urlsplit
from typing import TYPE_CHECKING, Any

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec, ed25519, padding, rsa
from django.conf import settings
from django.core import signing
from django.core.cache import cache
from django.core.exceptions import DisallowedHost

from .models import qrprotec_setting

if TYPE_CHECKING:
    from inventory.models import Passkey, Secouristes
    from rest_framework.request import Request

log = logging.getLogger(__name__)

CHALLENGE_SALT = 'qrprotec.passkey-challenge'
CHALLENGE_MAX_AGE = 300
TIMEOUT_MS = CHALLENGE_MAX_AGE * 1000
RP_NAME = 'QRprotec'
# ES256 (presque tous les telephones), Ed25519, RS256 (Windows Hello)
ALGORITHMS = (-7, -8, -257)

FLAG_UP = 0x01  # utilisateur present
FLAG_UV = 0x04  # utilisateur verifie (empreinte, visage, code du telephone)
FLAG_AT = 0x40  # donnees de la nouvelle cle presentes (creation)


class PasskeyError(ValueError):
    """Reponse WebAuthn refusee : le message (technique) part dans le journal du serveur."""


# base64url sans remplissage, comme WebAuthn
# ----------------------------------------------------------------------------------------------------------------------

def b64encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode('ascii')


def b64decode(value: str) -> bytes:
    if not isinstance(value, str) or len(value) > 16384:
        raise PasskeyError('champ base64url absent ou trop long')
    try:
        return base64.urlsafe_b64decode(value + '=' * (-len(value) % 4))
    except (ValueError, TypeError) as exc:
        raise PasskeyError(f'base64url invalide : {exc}') from None


# CBOR (RFC 8949), juste ce que WebAuthn utilise : entiers, octets, texte, tableaux, maps, simples
# ----------------------------------------------------------------------------------------------------------------------

def cbor_decode(data: bytes, offset: int = 0, depth: int = 0) -> Any:
    """(valeur, position apres la valeur)."""
    if depth > 16:
        raise PasskeyError('CBOR trop imbrique')
    if offset >= len(data):
        raise PasskeyError('CBOR tronque')
    initial = data[offset]
    major, info = initial >> 5, initial & 0x1f
    offset += 1
    if info < 24:
        argument = info
    elif info in (24, 25, 26, 27):
        size = 1 << (info - 24)
        if offset + size > len(data):
            raise PasskeyError('CBOR tronque')
        argument = int.from_bytes(data[offset:offset + size], 'big')
        offset += size
    else:
        raise PasskeyError('CBOR de longueur indefinie non gere')
    if major == 0:
        return argument, offset
    if major == 1:
        return -1 - argument, offset
    if major in (2, 3):
        if offset + argument > len(data):
            raise PasskeyError('CBOR tronque')
        chunk = data[offset:offset + argument]
        return (bytes(chunk) if major == 2 else chunk.decode('utf-8')), offset + argument
    if major == 4:
        values = []
        for _ in range(argument):
            value, offset = cbor_decode(data, offset, depth + 1)
            values.append(value)
        return values, offset
    if major == 5:
        result = {}
        for _ in range(argument):
            key, offset = cbor_decode(data, offset, depth + 1)
            value, offset = cbor_decode(data, offset, depth + 1)
            result[key] = value
        return result, offset
    if major == 6:  # etiquette : ignoree
        return cbor_decode(data, offset, depth + 1)
    simple = {20: False, 21: True, 22: None}
    if info in simple:
        return simple[info], offset
    raise PasskeyError('CBOR : valeur simple non geree')


def cbor_loads(data: bytes) -> dict[str, Any]:
    value, end = cbor_decode(data)
    if end != len(data):
        raise PasskeyError('CBOR : octets en trop')
    return value


# Cles COSE (RFC 9053)
# ----------------------------------------------------------------------------------------------------------------------

def cose_public_key(cose: bytes | dict[int, Any]) -> tuple[Any, int]:
    """Cle publique `cryptography` et algorithme COSE d'une cle COSE (EC2 P-256, OKP Ed25519, RSA)."""
    key = cbor_loads(cose) if isinstance(cose, (bytes, bytearray, memoryview)) else cose
    if not isinstance(key, dict):
        raise PasskeyError('cle COSE invalide')
    kty, alg = key.get(1), key.get(3)
    try:
        if kty == 2 and alg == -7 and key.get(-1) == 1:
            x, y = key.get(-2), key.get(-3)
            if not (isinstance(x, bytes) and isinstance(y, bytes) and len(x) == len(y) == 32):
                raise PasskeyError('cle EC2 invalide')
            return ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b'\x04' + x + y), alg
        if kty == 1 and alg == -8 and key.get(-1) == 6:
            return ed25519.Ed25519PublicKey.from_public_bytes(key.get(-2)), alg
        if kty == 3 and alg == -257:
            n, e = int.from_bytes(key.get(-1), 'big'), int.from_bytes(key.get(-2), 'big')
            if n.bit_length() < 2048:
                raise PasskeyError('cle RSA trop courte')
            return rsa.RSAPublicNumbers(e, n).public_key(), alg
    except (TypeError, ValueError) as exc:
        if isinstance(exc, PasskeyError):
            raise
        raise PasskeyError(f'cle COSE invalide : {exc}') from None
    raise PasskeyError(f'algorithme de cle non gere (kty {kty}, alg {alg})')


def verify_signature(cose: bytes, signature: bytes, data: bytes) -> None:
    public_key, alg = cose_public_key(cose)
    try:
        if alg == -7:
            public_key.verify(signature, data, ec.ECDSA(hashes.SHA256()))
        elif alg == -8:
            public_key.verify(signature, data)
        else:
            public_key.verify(signature, data, padding.PKCS1v15(), hashes.SHA256())
    except InvalidSignature:
        raise PasskeyError('signature fausse') from None


# Domaine (RP ID) et origine
# ----------------------------------------------------------------------------------------------------------------------

def _request_hostname(request: Request) -> str:
    try:
        return (urlsplit('//' + request.get_host()).hostname or '').lower()
    except DisallowedHost:  # hote refuse par ALLOWED_HOSTS : pas de domaine de developpement
        return ''


def rp_id(request: Request | None = None) -> str:
    configured = str(qrprotec_setting('PASSKEY_RP_ID') or '').strip().lower()
    if configured:
        return configured
    if settings.DEBUG and request is not None and _request_hostname(request) in ('localhost', '127.0.0.1', '[::1]', '::1'):
        return 'localhost'  # localhost est un contexte securise : les passkeys marchent en HTTP
    return (urlsplit(str(qrprotec_setting('PUBLIC_BASE_URL'))).hostname or 'localhost').lower()


def origin_allowed(origin: str, rp: str) -> bool:
    parts = urlsplit(str(origin or ''))
    host = (parts.hostname or '').lower()
    if not host or not (host == rp or host.endswith('.' + rp)):
        return False
    return parts.scheme == 'https' or (parts.scheme == 'http' and host == 'localhost')


# Defis a usage unique
# ----------------------------------------------------------------------------------------------------------------------

def new_challenge(purpose: str, matricule: str = '') -> str:
    nonce = secrets.token_urlsafe(24)
    token = signing.dumps({'p': purpose, 'm': matricule, 'n': nonce}, salt=CHALLENGE_SALT, compress=False)
    return b64encode(token.encode('ascii'))


def use_challenge(challenge_b64: str, purpose: str, matricule: str = '') -> None:
    try:
        token = b64decode(challenge_b64).decode('ascii')
        data = signing.loads(token, salt=CHALLENGE_SALT, max_age=CHALLENGE_MAX_AGE)
    except (signing.BadSignature, UnicodeDecodeError):
        raise PasskeyError('defi inconnu ou expire (plus de 5 min)') from None
    if data.get('p') != purpose or data.get('m') != matricule:
        raise PasskeyError('defi destine a une autre operation ou a un autre compte')
    if not cache.add('qrprotec.passkey-challenge.' + data['n'], 1, CHALLENGE_MAX_AGE + 60):
        raise PasskeyError('defi deja utilise (rejeu)')


# Donnees de l'authentificateur
# ----------------------------------------------------------------------------------------------------------------------

def _client_data(response: dict[str, Any], expected_type: str, purpose: str, matricule: str, request: Request
                ) -> tuple[bytes, str]:
    raw = b64decode(response.get('clientDataJSON'))
    try:
        client = json.loads(raw)
    except ValueError:
        raise PasskeyError('clientDataJSON illisible') from None
    if not isinstance(client, dict) or client.get('type') != expected_type:
        raise PasskeyError(f"type {client.get('type') if isinstance(client, dict) else '?'} au lieu de {expected_type}")
    rp = rp_id(request)
    if not origin_allowed(client.get('origin'), rp):
        raise PasskeyError(f"origine {client.get('origin')!r} hors du domaine des passkeys {rp!r}")
    use_challenge(client.get('challenge'), purpose, matricule)
    return raw, rp


def _auth_data(data: bytes, rp: str, require_credential: bool = False) -> dict[str, Any]:
    if len(data) < 37:
        raise PasskeyError('authenticatorData trop court')
    if data[:32] != hashlib.sha256(rp.encode()).digest():
        raise PasskeyError(f'passkey creee pour un autre domaine que {rp!r}')
    flags = data[32]
    if not flags & FLAG_UP:
        raise PasskeyError('utilisateur absent (UP)')
    if not flags & FLAG_UV:
        raise PasskeyError("utilisateur non verifie (UV) : empreinte, visage ou code du telephone exige")
    sign_count = struct.unpack('>I', data[33:37])[0]
    result = {'flags': flags, 'sign_count': sign_count}
    if require_credential:
        if not flags & FLAG_AT or len(data) < 55:
            raise PasskeyError('pas de nouvelle cle dans la reponse (AT)')
        length = struct.unpack('>H', data[53:55])[0]
        credential_id = data[55:55 + length]
        if len(credential_id) != length or not length:
            raise PasskeyError('identifiant de cle tronque')
        _, end = cbor_decode(data, 55 + length)
        cose_bytes = bytes(data[55 + length:end])
        cose_public_key(cose_bytes)  # algorithme gere ?
        result.update(credential_id=credential_id, public_key=cose_bytes)
    return result


# Options et verifications
# ----------------------------------------------------------------------------------------------------------------------

def registration_options(request: Request, user: Secouristes, existing_ids: list[Any] = ()) -> dict[str, Any]:
    """publicKey de navigator.credentials.create() pour un secouriste connecte."""
    return {
        'challenge': new_challenge('register', user.matricule),
        'rp': {'id': rp_id(request), 'name': RP_NAME},
        # identifiant opaque (le matricule, jamais le nom de famille sur le web)
        'user': {'id': b64encode(user.matricule.encode()), 'name': f'{user.prenom} ({user.matricule})',
                 'displayName': user.prenom},
        'pubKeyCredParams': [{'type': 'public-key', 'alg': alg} for alg in ALGORITHMS],
        'timeout': TIMEOUT_MS,
        'attestation': 'none',
        'authenticatorSelection': {'residentKey': 'required', 'requireResidentKey': True,
                                   'userVerification': 'required'},
        'excludeCredentials': [{'type': 'public-key', 'id': credential_id} for credential_id in existing_ids],
    }


def verify_registration(request: Request, user: Secouristes, credential: dict[str, Any]) -> dict[str, Any]:
    """{credential_id (base64url), public_key (COSE), sign_count, transports} d'une creation valide."""
    if not isinstance(credential, dict) or not isinstance(credential.get('response'), dict):
        raise PasskeyError('reponse de creation absente')
    response = credential['response']
    _, rp = _client_data(response, 'webauthn.create', 'register', user.matricule, request)
    attestation = cbor_loads(b64decode(response.get('attestationObject')))
    if not isinstance(attestation, dict) or not isinstance(attestation.get('authData'), bytes):
        raise PasskeyError('attestationObject invalide')
    data = _auth_data(attestation['authData'], rp, require_credential=True)
    credential_id = b64encode(data['credential_id'])
    if credential.get('id') not in (None, credential_id):
        raise PasskeyError("l'identifiant annonce ne correspond pas a la cle creee")
    transports = response.get('transports') or credential.get('transports') or []
    transports = [str(value)[:16] for value in transports if isinstance(value, str)][:8] if isinstance(transports, list) else []
    return {'credential_id': credential_id, 'public_key': data['public_key'], 'sign_count': data['sign_count'],
            'transports': transports}


def authentication_options(request: Request) -> dict[str, Any]:
    """publicKey de navigator.credentials.get() : pas de liste de cles, le telephone propose ses passkeys."""
    return {
        'challenge': new_challenge('login'),
        'rpId': rp_id(request),
        'timeout': TIMEOUT_MS,
        'userVerification': 'required',
        'allowCredentials': [],
    }


def verify_authentication(request: Request, passkey: Passkey, credential: dict[str, Any]) -> int:
    """Verifie une reponse de connexion pour cette passkey ; renvoie le nouveau compteur de signatures."""
    response = credential.get('response') if isinstance(credential, dict) else None
    if not isinstance(response, dict):
        raise PasskeyError('reponse de connexion absente')
    client_raw, rp = _client_data(response, 'webauthn.get', 'login', '', request)
    auth_data = b64decode(response.get('authenticatorData'))
    data = _auth_data(auth_data, rp)
    handle = response.get('userHandle')
    if handle and b64decode(handle) != passkey.user.matricule.encode():
        raise PasskeyError("la passkey annonce un autre compte (userHandle)")
    verify_signature(bytes(passkey.public_key), b64decode(response.get('signature')),
                     auth_data + hashlib.sha256(client_raw).digest())
    count = data['sign_count']
    if (count or passkey.sign_count) and count <= passkey.sign_count:
        # compteur qui recule : la passkey a peut-etre ete copiee
        raise PasskeyError(f'compteur de signatures {count} <= {passkey.sign_count} (passkey clonee ?)')
    return count
