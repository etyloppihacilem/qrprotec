# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          front_config.py
#       -\-    _|__
#        |\___/  . \        Created on 7 Nov. 2026 at 13:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Fichier de configuration chiffre d'un front distant (`frontkey add NOM --config FICHIER`).

Le fichier contient tout ce qu'il faut a un front d'une autre machine pour se connecter : URL du
serveur, cle du front et, avec l'autorite interne de Caddy, son certificat. Il est importe dans le
front (Reglages > Serveur ou fenetre « Connexion au serveur », sans badge tant que le serveur est
injoignable), qui demande le mot de passe choisi a la creation.

Format (texte JSON, lisible par le front C++ avec OpenSSL) :

    {"qrprotec": "front-config", "version": 1,
     "kdf": "PBKDF2-HMAC-SHA256", "iterations": 600000, "salt": "<base64, 16 octets>",
     "cipher": "AES-256-GCM", "nonce": "<base64, 12 octets>",
     "data": "<base64 : texte chiffre suivi de l'etiquette GCM de 16 octets>"}

La cle AES (32 octets) est derivee du mot de passe par PBKDF2 ; la donnee associee GCM est la
chaine fixe `qrprotec-front-config-v1`. Un sel, un nombre d'iterations ou un nonce modifie change la
cle ou l'etiquette : le dechiffrement echoue comme avec un mauvais mot de passe. Le texte clair est
un JSON :

    {"name": "accueil", "api_url": "https://...", "api_key": "qrpf_...",
     "api_ca_pem": "-----BEGIN CERTIFICATE-----...", "created": "2026-11-07T13:00:00+01:00"}
"""

import base64
import hashlib
import json
import os

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

MAGIC = 'front-config'
VERSION = 1
KDF = 'PBKDF2-HMAC-SHA256'
CIPHER = 'AES-256-GCM'
ITERATIONS = 600_000
MIN_ITERATIONS = 100_000
MAX_ITERATIONS = 10_000_000
MIN_PASSWORD_LENGTH = 8
SUFFIX = '.qrprotec'


class FrontConfigError(ValueError):
    pass


def _derive(password, salt, iterations):
    return hashlib.pbkdf2_hmac('sha256', password.encode('utf-8'), salt, iterations, dklen=32)


AAD = b'qrprotec-front-config-v1'


def encrypt(payload, password, iterations=ITERATIONS):
    """Texte du fichier chiffre (str) pour le dict `payload`."""
    if len(password) < MIN_PASSWORD_LENGTH:
        raise FrontConfigError(f"Mot de passe trop court ({MIN_PASSWORD_LENGTH} caracteres au moins)")
    salt = os.urandom(16)
    nonce = os.urandom(12)
    header = {
        'qrprotec': MAGIC, 'version': VERSION,
        'kdf': KDF, 'iterations': iterations, 'salt': base64.b64encode(salt).decode('ascii'),
        'cipher': CIPHER, 'nonce': base64.b64encode(nonce).decode('ascii'),
    }
    plain = json.dumps(payload, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    data = AESGCM(_derive(password, salt, iterations)).encrypt(nonce, plain, AAD)
    return json.dumps({**header, 'data': base64.b64encode(data).decode('ascii')}, indent=1) + '\n'


def decrypt(text, password):
    """dict du fichier `text` (meme verifications que le front)."""
    try:
        document = json.loads(text)
    except ValueError as error:
        raise FrontConfigError("Ce n'est pas un fichier de configuration QRProtec") from error
    if not isinstance(document, dict) or document.get('qrprotec') != MAGIC:
        raise FrontConfigError("Ce n'est pas un fichier de configuration QRProtec")
    if document.get('version') != VERSION or document.get('kdf') != KDF or document.get('cipher') != CIPHER:
        raise FrontConfigError("Version du fichier de configuration non prise en charge")
    iterations = document.get('iterations')
    if not isinstance(iterations, int) or not MIN_ITERATIONS <= iterations <= MAX_ITERATIONS:
        raise FrontConfigError("Fichier de configuration invalide (iterations)")
    try:
        salt = base64.b64decode(document['salt'], validate=True)
        nonce = base64.b64decode(document['nonce'], validate=True)
        data = base64.b64decode(document['data'], validate=True)
    except (KeyError, TypeError, ValueError) as error:
        raise FrontConfigError("Fichier de configuration invalide") from error
    if len(nonce) != 12 or len(salt) < 8 or len(data) < 16:
        raise FrontConfigError("Fichier de configuration invalide")
    try:
        plain = AESGCM(_derive(password, salt, iterations)).decrypt(nonce, data, AAD)
    except InvalidTag as error:
        raise FrontConfigError("Mot de passe incorrect ou fichier modifie") from error
    return json.loads(plain.decode('utf-8'))
