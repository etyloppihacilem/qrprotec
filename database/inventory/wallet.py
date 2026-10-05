# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          wallet.py
#       -\-    _|__
#        |\___/  . \        Created on 05 Oct. 2026 at 08:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Badge dans Apple Wallet et Google Wallet.

Le QR code du pass est celui du badge imprime (badge?m=..&key=..) : la cle n'etant connue que du badge,
le front web la renvoie (badge scanne) pour generer le pass. Le pass expire avec le badge ; renouveler
le badge donne un nouveau pass (l'ancien QR code est refuse par le serveur).

- Apple : fichier .pkpass (zip : pass.json, images, manifest.json et sa signature PKCS#7 detachee),
  signe avec le certificat « Pass Type ID » de l'equipe Apple Developer et le certificat Apple WWDR.
- Google : lien « Enregistrer dans Google Wallet » (https://pay.google.com/gp/v/save/<JWT>), JWT RS256
  signe avec la cle du compte de service autorise sur l'emetteur ; la classe et l'objet sont crees par
  Google a l'enregistrement.

Le module `cryptography` est necessaire ; sans lui (ou sans configuration), l'export est indisponible.
"""

import base64
import hashlib
import io
import json
import time
import zipfile
from datetime import datetime, timedelta, time as dtime
from pathlib import Path
from urllib.parse import urlsplit

from django.utils import timezone

from .models import qrprotec_setting
from .serializers import public_url

ASSETS_DIR = Path(__file__).resolve().parent / 'wallet_assets'
APPLE_IMAGES = ('icon.png', 'icon@2x.png', 'icon@3x.png', 'logo.png', 'logo@2x.png', 'logo@3x.png')
PKPASS_TYPE = 'application/vnd.apple.pkpass'
GOOGLE_SAVE_URL = 'https://pay.google.com/gp/v/save/'
GOOGLE_CLASS_SUFFIX = 'qrprotec_badge'
# OID des attributs du sujet d'un certificat Pass Type ID : UID = identifiant du type de pass
OID_UID = '0.9.2342.19200300.100.1.1'

ROLE_LABELS = {'normal': 'Secouriste', 'gestion': 'Gestion', 'admin': 'Administrateur'}
BACKGROUND = (0, 46, 108)     # bleu de la protection civile
FOREGROUND = (255, 255, 255)
LABEL = (255, 196, 0)


class WalletError(Exception):
    pass


def _crypto_available() -> bool:
    try:
        import cryptography  # noqa: F401
    except ImportError:
        return False
    return True


def apple_enabled() -> bool:
    return bool(qrprotec_setting('WALLET_APPLE_CERT') and qrprotec_setting('WALLET_APPLE_WWDR')) and _crypto_available()


def google_enabled() -> bool:
    return bool(qrprotec_setting('WALLET_GOOGLE_ISSUER_ID')
                and qrprotec_setting('WALLET_GOOGLE_SERVICE_ACCOUNT')) and _crypto_available()


def status() -> dict:
    return {'apple': apple_enabled(), 'google': google_enabled()}


# ----------------------------------------------------------------------------------------------------------------------
# Contenu commun

def badge_url(user, key) -> str:
    return public_url('badge', m=user.matricule, key=key)


def expires_at(user) -> datetime:
    """Fin de validite du badge : il est valable jusqu'au jour key_expires inclus (heure locale)."""
    return timezone.make_aware(datetime.combine(user.key_expires + timedelta(days=1), dtime.min))


def serial(user) -> str:
    """Identifiant du pass : change a chaque renouvellement du badge (nouvelle cle)."""
    return f'{user.matricule}-{user.key_hash[:16]}'


def _rgb(color) -> str:
    return 'rgb({}, {}, {})'.format(*color)


def _hex(color) -> str:
    return '#{:02x}{:02x}{:02x}'.format(*color)


def _read(path, what):
    try:
        return Path(path).read_bytes()
    except OSError as exc:
        raise WalletError(f'{what} illisible ({path}) : {exc.strerror}')


# ----------------------------------------------------------------------------------------------------------------------
# Apple Wallet

def _load_certificate(data):
    from cryptography import x509
    return x509.load_pem_x509_certificate(data) if b'-----BEGIN' in data else x509.load_der_x509_certificate(data)


def apple_credentials():
    """(certificat du pass, cle privee, certificat WWDR) depuis la configuration."""
    from cryptography.hazmat.primitives.serialization import load_pem_private_key, pkcs12

    cert_path = qrprotec_setting('WALLET_APPLE_CERT')
    password = (qrprotec_setting('WALLET_APPLE_PASSWORD') or '').encode() or None
    data = _read(cert_path, 'Certificat Apple Wallet')
    try:
        if b'-----BEGIN' in data:
            certificate = _load_certificate(data)
            key_path = qrprotec_setting('WALLET_APPLE_KEY')
            key = load_pem_private_key(_read(key_path, 'Cle Apple Wallet') if key_path else data, password)
        else:
            key, certificate, _ = pkcs12.load_key_and_certificates(data, password)
        wwdr = _load_certificate(_read(qrprotec_setting('WALLET_APPLE_WWDR'), 'Certificat Apple WWDR'))
    except (ValueError, TypeError) as exc:
        raise WalletError(f'Certificats Apple Wallet invalides : {exc}')
    if key is None or certificate is None:
        raise WalletError('Certificat Apple Wallet : cle privee ou certificat absent du fichier')
    return certificate, key, wwdr


def apple_identifiers(certificate):
    """(passTypeIdentifier, teamIdentifier) lus dans le sujet du certificat Pass Type ID."""
    from cryptography.x509.oid import NameOID, ObjectIdentifier

    subject = certificate.subject
    uid = subject.get_attributes_for_oid(ObjectIdentifier(OID_UID))
    team = subject.get_attributes_for_oid(NameOID.ORGANIZATIONAL_UNIT_NAME)
    if not uid or not team:
        raise WalletError("Certificat Apple Wallet : ce n'est pas un certificat « Pass Type ID »")
    return uid[0].value, team[0].value


def pass_json(user, key, pass_type, team) -> dict:
    organization = qrprotec_setting('WALLET_ORGANIZATION')
    expires = expires_at(user)
    return {
        'formatVersion': 1,
        'passTypeIdentifier': pass_type,
        'teamIdentifier': team,
        'serialNumber': serial(user),
        'organizationName': organization,
        'description': f'Badge {organization}',
        'logoText': organization,
        'backgroundColor': _rgb(BACKGROUND),
        'foregroundColor': _rgb(FOREGROUND),
        'labelColor': _rgb(LABEL),
        'expirationDate': expires.isoformat(),
        'sharingProhibited': True,
        'barcodes': [{'format': 'PKBarcodeFormatQR', 'message': badge_url(user, key),
                      'messageEncoding': 'iso-8859-1', 'altText': user.matricule}],
        'generic': {
            'primaryFields': [{'key': 'name', 'label': 'Secouriste', 'value': f'{user.prenom} {user.nom}'}],
            'secondaryFields': [
                {'key': 'matricule', 'label': 'Matricule', 'value': user.matricule},
                {'key': 'role', 'label': 'Rôle', 'value': ROLE_LABELS.get(user.role, user.role)},
            ],
            'auxiliaryFields': [
                {'key': 'expires', 'label': "Valable jusqu'au", 'value': user.key_expires.isoformat(),
                 'dateStyle': 'PKDateStyleMedium', 'ignoresTimeZone': True},
            ],
            'backFields': [
                {'key': 'usage', 'label': 'Utilisation',
                 'value': "Présentez ce QR code comme le badge imprimé (caméra du front web, ou douchette si elle "
                          "lit les écrans). Il est refusé dès que le badge est renouvelé ou expiré."},
                {'key': 'site', 'label': 'Site', 'value': qrprotec_setting('PUBLIC_BASE_URL')},
            ],
        },
    }


def apple_pass(user, key) -> bytes:
    """Fichier .pkpass du badge (cle en clair, verifiee par l'appelant)."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.serialization import Encoding, pkcs7

    certificate, private_key, wwdr = apple_credentials()
    pass_type, team = apple_identifiers(certificate)
    files = {'pass.json': json.dumps(pass_json(user, key, pass_type, team), ensure_ascii=False).encode()}
    for name in APPLE_IMAGES:
        files[name] = (ASSETS_DIR / name).read_bytes()
    manifest = json.dumps({name: hashlib.sha1(data).hexdigest() for name, data in files.items()}).encode()
    signature = (
        pkcs7.PKCS7SignatureBuilder()
        .set_data(manifest)
        .add_signer(certificate, private_key, hashes.SHA256())
        .add_certificate(wwdr)
        .sign(Encoding.DER, [pkcs7.PKCS7Options.DetachedSignature])
    )
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            archive.writestr(name, data)
        archive.writestr('manifest.json', manifest)
        archive.writestr('signature', signature)
    return buffer.getvalue()


# ----------------------------------------------------------------------------------------------------------------------
# Google Wallet

def _b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode()


def google_service_account() -> dict:
    try:
        account = json.loads(_read(qrprotec_setting('WALLET_GOOGLE_SERVICE_ACCOUNT'), 'Compte de service Google'))
    except ValueError:
        raise WalletError('Compte de service Google : fichier JSON invalide')
    if not isinstance(account, dict) or not account.get('client_email') or not account.get('private_key'):
        raise WalletError('Compte de service Google : client_email ou private_key manquant')
    return account


def generic_object(user, key) -> dict:
    issuer = qrprotec_setting('WALLET_GOOGLE_ISSUER_ID')
    organization = qrprotec_setting('WALLET_ORGANIZATION')
    base = qrprotec_setting('PUBLIC_BASE_URL')
    if not base.endswith('/'):
        base += '/'

    def text(value):
        return {'defaultValue': {'language': 'fr', 'value': value}}

    return {
        'id': f'{issuer}.{serial(user)}',
        'classId': f'{issuer}.{GOOGLE_CLASS_SUFFIX}',
        'state': 'ACTIVE',
        'cardTitle': text(organization),
        'subheader': text('Secouriste'),
        'header': text(f'{user.prenom} {user.nom}'),
        'hexBackgroundColor': _hex(BACKGROUND),
        'logo': {'sourceUri': {'uri': f'{base}web/icon-192.png'}},
        'barcode': {'type': 'QR_CODE', 'value': badge_url(user, key), 'alternateText': user.matricule},
        'textModulesData': [
            {'id': 'matricule', 'header': 'Matricule', 'body': user.matricule},
            {'id': 'role', 'header': 'Rôle', 'body': ROLE_LABELS.get(user.role, user.role)},
            {'id': 'expires', 'header': "Valable jusqu'au", 'body': user.key_expires.strftime('%d/%m/%Y')},
        ],
        'validTimeInterval': {'end': {'date': expires_at(user).isoformat()}},
    }


def google_save_url(user, key) -> str:
    """Lien « Enregistrer dans Google Wallet » du badge (JWT signe, la cle est dans le QR code)."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding
    from cryptography.hazmat.primitives.serialization import load_pem_private_key

    account = google_service_account()
    try:
        private_key = load_pem_private_key(account['private_key'].encode(), None)
    except (ValueError, TypeError) as exc:
        raise WalletError(f'Compte de service Google : cle privee invalide ({exc})')
    issuer = qrprotec_setting('WALLET_GOOGLE_ISSUER_ID')
    origin = urlsplit(qrprotec_setting('PUBLIC_BASE_URL'))
    claims = {
        'iss': account['client_email'],
        'aud': 'google',
        'typ': 'savetowallet',
        'iat': int(time.time()),
        'origins': [f'{origin.scheme}://{origin.netloc}'] if origin.netloc else [],
        'payload': {
            'genericClasses': [{'id': f'{issuer}.{GOOGLE_CLASS_SUFFIX}'}],
            'genericObjects': [generic_object(user, key)],
        },
    }
    header = {'alg': 'RS256', 'typ': 'JWT'}
    if account.get('private_key_id'):
        header['kid'] = account['private_key_id']
    signing_input = '.'.join(_b64url(json.dumps(part, separators=(',', ':'), ensure_ascii=False).encode())
                             for part in (header, claims))
    signature = private_key.sign(signing_input.encode(), padding.PKCS1v15(), hashes.SHA256())
    return GOOGLE_SAVE_URL + signing_input + '.' + _b64url(signature)
