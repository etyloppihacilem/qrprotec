"""Passkeys du front web : faux authentificateur (vraie cle P-256, reponses WebAuthn signees comme un telephone),
creation, connexion, refus (signature, domaine, origine, defi rejoue, compteur), nom + PIN coupes, carte gardee, QR
code de connexion au poste depuis une session passkey, suppression par un admin."""

from __future__ import annotations

import hashlib
import json
import struct
import time
from unittest import mock
from urllib.parse import parse_qs, urlparse
from typing import Any

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from django.core.cache import cache
from django.test import override_settings

from .. import passkeys as webauthn
from .. import views
from ..models import Operation, Passkey
from .base import ApiTestCase, quiet_stderr

ORIGIN = 'https://example.com'  # PUBLIC_BASE_URL des tests


def cbor(value: Any) -> bytes:
    """Encodeur CBOR minimal (entiers, octets, texte, listes, maps) pour fabriquer les reponses."""
    def head(major: int, argument: int) -> bytes:
        if argument < 24:
            return bytes([major << 5 | argument])
        for info, size in ((24, 1), (25, 2), (26, 4), (27, 8)):
            if argument < 1 << (8 * size):
                return bytes([major << 5 | info]) + argument.to_bytes(size, 'big')
    if value is None:
        return b'\xf6'
    if isinstance(value, bool):
        return bytes([0xf5 if value else 0xf4])
    if isinstance(value, int):
        return head(0, value) if value >= 0 else head(1, -1 - value)
    if isinstance(value, bytes):
        return head(2, len(value)) + value
    if isinstance(value, str):
        data = value.encode()
        return head(3, len(data)) + data
    if isinstance(value, list):
        return head(4, len(value)) + b''.join(cbor(item) for item in value)
    if isinstance(value, dict):
        return head(5, len(value)) + b''.join(cbor(key) + cbor(item) for key, item in value.items())
    raise TypeError(value)


class FakeAuthenticator:
    """Telephone qui cree et utilise une passkey (attestation « none », comme demande par le serveur)."""

    def __init__(self, rp: str = 'example.com', origin: str = ORIGIN) -> None:
        self.rp, self.origin = rp, origin
        self.key = ec.generate_private_key(ec.SECP256R1())
        self.credential_id = b'cred-' + bytes(range(27))
        self.count = 0
        self.user_handle = b''

    def cose(self) -> bytes:
        numbers = self.key.public_key().public_numbers()
        return cbor({1: 2, 3: -7, -1: 1, -2: numbers.x.to_bytes(32, 'big'), -3: numbers.y.to_bytes(32, 'big')})

    def auth_data(self, flags: int, credential: bool = False) -> bytes:
        data = hashlib.sha256(self.rp.encode()).digest() + bytes([flags]) + struct.pack('>I', self.count)
        if credential:
            data += bytes(16) + struct.pack('>H', len(self.credential_id)) + self.credential_id + self.cose()
        return data

    def client_data(self, kind: str, challenge: str, origin: str | None = None) -> bytes:
        return json.dumps({'type': kind, 'challenge': challenge, 'origin': origin or self.origin}).encode()

    def create(self, options: dict[str, Any], flags: int = 0x45, origin: str | None = None) -> dict[str, Any]:
        self.user_handle = webauthn.b64decode(options['user']['id'])
        client = self.client_data('webauthn.create', options['challenge'], origin)
        attestation = cbor({'fmt': 'none', 'attStmt': {}, 'authData': self.auth_data(flags, credential=True)})
        return {'id': webauthn.b64encode(self.credential_id), 'type': 'public-key',
                'response': {'clientDataJSON': webauthn.b64encode(client),
                             'attestationObject': webauthn.b64encode(attestation), 'transports': ['internal']}}

    def get(self, options: dict[str, Any], flags: int = 0x05, origin: str | None = None, count_step: int = 1,
            tamper: bool = False) -> dict[str, Any]:
        self.count += count_step
        client = self.client_data('webauthn.get', options['challenge'], origin)
        auth = self.auth_data(flags)
        signature = self.key.sign(auth + hashlib.sha256(client).digest(), ec.ECDSA(hashes.SHA256()))
        if tamper:
            auth = auth[:-1] + bytes([auth[-1] ^ 1])
        return {'id': webauthn.b64encode(self.credential_id), 'type': 'public-key',
                'response': {'clientDataJSON': webauthn.b64encode(client),
                             'authenticatorData': webauthn.b64encode(auth),
                             'signature': webauthn.b64encode(signature),
                             'userHandle': webauthn.b64encode(self.user_handle)}}


class PasskeyTests(ApiTestCase):

    def setUp(self) -> None:
        super().setUp()
        cache.clear()  # essais de connexion comptes par adresse (limite de 10)
        self.user.set_pin('432100')
        self.user.save()
        self.phone = FakeAuthenticator()

    def card(self) -> dict[str, str]:
        return {'matricule': 'M001', 'key': self.user.new_key, 'session': views.session_token(self.user)}

    def register(self, phone: FakeAuthenticator | None = None, **kwargs: Any) -> tuple[int, Any]:
        phone = phone or self.phone
        code, body = self.call('POST', '/api/me/passkeys/', {'user': self.card(), 'action': 'options'}, local=False)
        self.assertEqual(code, 200, body)
        options = body['publicKey']
        self.assertEqual(options['rp']['id'], 'example.com')
        self.assertEqual(options['authenticatorSelection']['userVerification'], 'required')
        self.assertNotIn('Dupont', json.dumps(options))  # jamais de nom de famille sur le web
        credential = phone.create(options, **kwargs)
        return self.call('POST', '/api/me/passkeys/', {'user': self.card(), 'action': 'create',
                                                       'credential': credential}, local=False)

    def login(self, phone: str | None = None, **kwargs: Any) -> tuple[int, dict[str, Any]]:
        phone = phone or self.phone
        code, body = self.call('POST', '/api/auth/passkey/options/', {}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(body['publicKey']['rpId'], 'example.com')
        return self.call('POST', '/api/auth/passkey/', {'credential': phone.get(body['publicKey'], **kwargs)},
                         local=False)

    def name_login(self) -> tuple[int, dict[str, Any]]:
        return self.call('POST', '/api/auth/name/', {'nom': 'Dupont', 'prenom': 'Jeanne', 'pin': '432100'}, local=False)

    def test_register_then_login(self) -> None:
        code, body = self.register()
        self.assertEqual(code, 200, body)
        self.assertEqual(len(body['passkeys']), 1)
        self.assertTrue(Operation.objects.filter(summary__contains='passkey créée').exists())
        code, user = self.login()
        self.assertEqual(code, 200, user)
        self.assertEqual(user['matricule'], 'M001')
        self.assertTrue(user['passkey_session'])
        self.assertNotIn('nom', user)
        passkey = Passkey.objects.get()
        self.assertEqual(passkey.sign_count, 1)
        self.assertIsNotNone(passkey.last_used)
        # la session sert d'identifiant comme apres une connexion par nom et PIN
        code, _ = self.call('POST', '/api/me/prefs/', {'user': {'matricule': 'M001', 'session': user['session']},
                                                       'web_prefs': {'scan_bubble': True}}, local=False)
        self.assertEqual(code, 200)

    def test_passkey_disables_name_login_but_not_the_card(self) -> None:
        code, _ = self.name_login()
        self.assertEqual(code, 200)
        self.register()
        code, body = self.name_login()
        self.assertEqual(code, 403)
        self.assertTrue(body['passkey_required'])
        self.assertIn('passkey', body['error'])
        # PIN faux : refus general, qui ne dit rien de la passkey
        code, body = self.call('POST', '/api/auth/name/', {'nom': 'Dupont', 'prenom': 'Jeanne', 'pin': '000000'},
                               local=False)
        self.assertEqual(code, 403)
        self.assertNotIn('passkey_required', body)
        # matricule seul + PIN : coupe aussi
        code, body = self.call('POST', '/api/auth/', {'matricule': 'M001', 'pin': '432100'}, local=False)
        self.assertEqual(code, 403)
        self.assertTrue(body.get('passkey_required'))
        # au poste (kiosk ou poste distant avec sa cle de front), le nom et le PIN marchent toujours
        code, body = self.call('POST', '/api/auth/name/', {'nom': 'Dupont', 'prenom': 'Jeanne', 'pin': '432100'},
                               session=False)
        self.assertEqual(code, 200, body)
        self.assertEqual(body['matricule'], 'M001')
        code, body = self.call('POST', '/api/auth/', {'matricule': 'M001', 'pin': '432100'}, session=False)
        self.assertEqual(code, 200, body)
        # la carte marche toujours
        code, _ = self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.new_key, 'pin': '432100'},
                            local=False)
        self.assertEqual(code, 200)
        # sans passkey (supprimee par l'utilisateur), le nom et le PIN remarchent
        passkey_id = Passkey.objects.get().pk
        code, body = self.call('POST', '/api/me/passkeys/', {'user': self.card(), 'action': 'delete', 'id': passkey_id},
                               local=False)
        self.assertEqual(code, 200)
        self.assertEqual(body['passkeys'], [])
        code, _ = self.name_login()
        self.assertEqual(code, 200)

    def test_refusals(self) -> None:
        with quiet_stderr():
            # utilisateur non verifie (pas d'empreinte ni de code)
            code, _ = self.register(flags=0x41)
            self.assertEqual(code, 400)
            # autre origine
            code, _ = self.register(origin='https://evil.example.org')
            self.assertEqual(code, 400)
            # passkey creee pour un autre domaine
            code, _ = self.register(phone=FakeAuthenticator(rp='evil.example.org'))
            self.assertEqual(code, 400)
            self.assertFalse(Passkey.objects.exists())
            self.register()
            # signature fausse
            with self.assertLogs('inventory.passkeys', 'WARNING') as logs:
                code, body = self.login(tamper=True)
            self.assertEqual(code, 403)
            self.assertIn('signature fausse', logs.output[0])
            self.assertNotIn('signature', body['error'])
            # compteur qui recule (passkey clonee)
            code, _ = self.login()
            self.assertEqual(code, 200)
            code, _ = self.login(count_step=0)
            self.assertEqual(code, 403)
            # defi rejoue
            code, options = self.call('POST', '/api/auth/passkey/options/', {}, local=False)
            first = self.phone.get(options['publicKey'])
            self.assertEqual(self.call('POST', '/api/auth/passkey/', {'credential': first}, local=False)[0], 200)
            again = self.phone.get(options['publicKey'])
            self.assertEqual(self.call('POST', '/api/auth/passkey/', {'credential': again}, local=False)[0], 403)
            # defi expire
            code, options = self.call('POST', '/api/auth/passkey/options/', {}, local=False)
            late = self.phone.get(options['publicKey'])
            with mock.patch('django.core.signing.time.time', return_value=time.time() + 400):
                self.assertEqual(self.call('POST', '/api/auth/passkey/', {'credential': late}, local=False)[0], 403)
            # compte desactive
            self.user.active = False
            self.user.save()
            self.assertEqual(self.login()[0], 403)

    def test_unknown_passkey(self) -> None:
        with quiet_stderr():
            code, body = self.login()
        self.assertEqual(code, 403)
        self.assertTrue(body['passkey_unknown'])

    @override_settings(DEBUG=True)
    def test_localhost_in_development(self) -> None:
        phone = FakeAuthenticator(rp='localhost', origin='http://localhost:8000')
        _, body = self.call('POST', '/api/me/passkeys/', {'user': self.card(), 'action': 'options'}, local=False)
        self.assertEqual(body['publicKey']['rp']['id'], 'example.com')  # hote testserver : domaine configure
        options = self.client.post('/api/me/passkeys/', {'user': self.card(), 'action': 'options'},
                                   content_type='application/json', HTTP_HOST='localhost:8000').json()['publicKey']
        self.assertEqual(options['rp']['id'], 'localhost')
        response = self.client.post('/api/me/passkeys/', {'user': self.card(), 'action': 'create',
                                                          'credential': phone.create(options)},
                                    content_type='application/json', HTTP_HOST='localhost:8000')
        self.assertEqual(response.status_code, 200, response.content)

    def test_poste_login_qr_only_from_a_passkey_session(self) -> None:
        self.register()
        # session par la carte : pas de QR code
        code, _ = self.call('POST', '/api/me/poste-login/', {'user': self.card()}, local=False)
        self.assertEqual(code, 403)
        _, user = self.login()
        credentials = {'matricule': 'M001', 'session': user['session']}
        code, body = self.call('POST', '/api/me/poste-login/', {'user': credentials}, local=False)
        self.assertEqual(code, 200, body)
        self.assertEqual(body['seconds'], 300)
        query = parse_qs(urlparse(body['url']).query)
        token = query['t'][0]
        self.assertEqual(query['m'][0], 'M001')
        # un telephone ne s'en sert pas : c'est pour la douchette d'un poste
        code, _ = self.call('POST', '/api/auth/temp/', {'matricule': 'M001', 'token': token}, local=False)
        self.assertEqual(code, 403)
        # 5 min seulement
        with mock.patch('django.core.signing.time.time', return_value=time.time() + 400):
            code, _ = self.call('POST', '/api/auth/temp/', {'matricule': 'M001', 'token': token}, session=False)
        self.assertEqual(code, 403)
        code, poste = self.call('POST', '/api/auth/temp/', {'matricule': 'M001', 'token': token}, session=False)
        self.assertEqual(code, 200, poste)
        self.assertNotIn('temporary', poste)  # session normale du poste
        self.assertEqual(poste['nom'], 'Dupont')
        self.assertTrue(Operation.objects.filter(summary__contains='QR code de son téléphone').exists())
        # une seule fois
        code, _ = self.call('POST', '/api/auth/temp/', {'matricule': 'M001', 'token': token}, session=False)
        self.assertEqual(code, 403)

    def test_admin_deletes_passkeys(self) -> None:
        self.register()
        admin = {'matricule': 'P001', 'key': self.operator.new_key, 'session': self.operator_session}
        code, _ = self.call('POST', '/api/debug/passkeys/', {'user': self.card(), 'matricule': 'M001'}, local=False)
        self.assertEqual(code, 403)  # pas un admin
        code, body = self.call('POST', '/api/debug/passkeys/', {'user': admin, 'matricule': 'M001'}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(len(body['passkeys']), 1)
        code, body = self.call('POST', '/api/debug/passkeys/', {'user': admin, 'matricule': 'M001', 'action': 'delete'},
                               local=False)
        self.assertEqual(body['passkeys'], [])
        self.assertEqual(self.name_login()[0], 200)
        self.assertTrue(Operation.objects.filter(summary__contains='1 passkey de').exists())
        # au poste : nombre de passkeys dans la fenetre Utilisateurs, suppression par PATCH
        self.register(phone=FakeAuthenticator())
        code, users = self.call('GET', '/api/users/')
        self.assertEqual(next(row for row in users if row['matricule'] == 'M001')['passkeys'], 1)
        code, row = self.call('PATCH', '/api/users/M001/', {'delete_passkeys': True})
        self.assertEqual(code, 200)
        self.assertEqual(row['passkeys'], 0)

    def test_first_login_passkey_before_pin(self) -> None:
        """Premiere connexion au web : passkey creee avec la carte seule tant que le code obligatoire n'est pas
        choisi, puis le code (Hippolyte, 2026-11-21)."""
        self.user.pin_hash = ''
        self.user.set_pin_mode('required')
        self.user.save()
        card = {'matricule': 'M001', 'key': self.user.new_key}
        code, body = self.call('POST', '/api/auth/', card, local=False)
        self.assertEqual(code, 403)
        self.assertTrue(body['pin_setup_required'])
        self.assertTrue(body['passkey_offer'])
        # la carte seule : options et creation, rien d'autre (ni la liste, ni la suppression)
        code, body = self.call('POST', '/api/me/passkeys/', {'user': card, 'action': 'list'}, local=False)
        self.assertEqual(code, 403)
        code, body = self.call('POST', '/api/me/passkeys/', {'user': card, 'action': 'options'}, local=False)
        self.assertEqual(code, 200, body)
        credential = self.phone.create(body['publicKey'])
        code, body = self.call('POST', '/api/me/passkeys/', {'user': card, 'action': 'create',
                                                             'credential': credential}, local=False)
        self.assertEqual(code, 200, body)
        self.assertEqual(len(body['passkeys']), 1)
        # ensuite le code obligatoire, avec la carte, comme avant ; plus de passkey a proposer
        code, body = self.call('POST', '/api/auth/', card, local=False)
        self.assertEqual(code, 403)
        self.assertFalse(body['passkey_offer'])
        code, body = self.call('POST', '/api/auth/', {**card, 'new_pin': '135790'}, local=False)
        self.assertEqual(code, 200, body)
        self.assertTrue(body['has_passkey'])
        # code choisi : la carte seule ne suffit plus pour creer une passkey
        code, _ = self.call('POST', '/api/me/passkeys/', {'user': card, 'action': 'options'}, local=False)
        self.assertEqual(code, 403)

    def test_first_login_passkey_offer_once(self) -> None:
        self.user.pin_hash = ''
        self.user.set_pin_mode('required')
        self.user.web_prefs = {'passkey_offered': True}
        self.user.save()
        code, body = self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.new_key}, local=False)
        self.assertEqual(code, 403)
        self.assertFalse(body['passkey_offer'])


class CborTests(ApiTestCase):

    def test_decoder(self) -> None:
        value = {1: -7, 'a': [b'\x01', 'é', True, None], -3: 2 ** 40}
        self.assertEqual(webauthn.cbor_loads(cbor(value)), value)
        with self.assertRaises(webauthn.PasskeyError):
            webauthn.cbor_loads(cbor(value)[:-1])
        with self.assertRaises(webauthn.PasskeyError):
            webauthn.cbor_loads(cbor(value) + b'\x00')
