import importlib
import json
import math
from datetime import datetime, timedelta
from io import StringIO

from django.apps import apps as django_apps

from django.conf import settings
from django.core.cache import cache
from django.core.management import call_command
from django.test import TestCase, override_settings
from django.utils import timezone

from . import serializers as ser
from . import views
from .base62 import decode_base62
from . import forecast
from . import idendity
from . import services
from .models import (
    ABSENCE_KINDS, FrontKey, ItemMovement, ItemStatus, Items, ItemsPacks, ItemType, LotRequirements, Lots, LotType, MovementKind,
    Operation, Secouristes, STOCK_LOT_TYPE, Verifs,
)

LOCAL = {'qrprotec.role': 'local'}


class ApiTestCase(TestCase):
    def call(self, method, path, data=None, local=True, session=None):
        """local : requete du poste, avec par defaut la session de l'administrateur connecte (session=False :
        personne de connecte, ou un autre jeton)."""
        extra = dict(LOCAL) if local else {}
        if local and session is not False:
            extra['HTTP_X_QRPROTEC_SESSION'] = session or self.operator_session
        body = json.dumps(data) if data is not None else None
        response = self.client.generic(method, path, body or '', content_type='application/json', **extra)
        is_json = response.get('Content-Type', '').startswith('application/json')
        return response.status_code, (response.json() if is_json else None)

    def setUp(self):
        self.today = timezone.localdate()
        self.compresses = ItemType.objects.create(type='compre', name='Compresses', perissable=True, min_quantity=10)
        self.garrot = ItemType.objects.create(type='garrot', name='Garrot', perissable=False, min_quantity=2)
        self.lot_type = LotType.objects.create(type='sacpse', name='Sac PSE')
        LotRequirements.objects.create(lot_type=self.lot_type, item_type=self.compresses, quantity=2)
        self.lot = Lots(lot_type=self.lot_type, name='Sac A', name_short='A', created_by='test')
        self.lot.save()
        self.user = Secouristes(matricule='M001', nom='Dupont', prenom='Jeanne')
        self.user.renew_key()
        self.user.save()
        # administrateur connecte sur le poste (jeton de session envoye par le front)
        self.operator = Secouristes(matricule='P001', nom='Poste', prenom='Admin', role='admin')
        self.operator.renew_key()
        self.operator.save()
        self.operator_session = views.session_token(self.operator)

    def create(self, item_type, peremption, count):
        return ItemsPacks.objects.add_items(item_type, peremption, count, 'test')


class IidTests(ApiTestCase):
    def test_iid_format(self):
        items = self.create(self.compresses, self.today + timedelta(days=100), 3)
        iid = items[2].iid
        self.assertEqual(len(iid), 22)
        self.assertEqual(iid[:6], 'compre')
        self.assertEqual(iid[6:14], (self.today + timedelta(days=100)).strftime('%Y%m%d'))
        self.assertEqual(decode_base62(iid[14:]), 3)
        # la sequence continue pour le meme type et la meme date
        more = self.create(self.compresses, self.today + timedelta(days=100), 1)
        self.assertEqual(decode_base62(more[0].iid[14:]), 4)

    def test_non_perishable_has_no_date(self):
        item = self.create(self.garrot, None, 1)[0]
        self.assertEqual(item.iid[6:14], '00000000')
        self.assertFalse(item.is_expired())

    def test_perishable_requires_date(self):
        with self.assertRaises(ValueError):
            self.create(self.compresses, None, 1)


class PublicApiTests(ApiTestCase):
    def test_management_routes_are_local_only(self):
        code, _ = self.call('GET', '/api/item-types/', local=False)
        self.assertEqual(code, 404)
        code, body = self.call('GET', '/api/item-types/')
        self.assertEqual(code, 200)
        self.assertEqual(len(body), 2)

    def test_public_verif_requires_lot_key(self):
        items = self.create(self.compresses, self.today + timedelta(days=10), 2)
        payload = {'items': [item.iid for item in items]}
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/verif/', payload, local=False)
        self.assertEqual(code, 403)
        payload['key'] = 'mauvaise'
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/verif/', payload, local=False)
        self.assertEqual(code, 403)
        payload['key'] = self.lot.verif_key
        payload['user'] = {'matricule': 'M001', 'key': self.user.new_key}
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/verif/', payload, local=False)
        self.assertEqual(code, 200)
        self.assertTrue(body['complete'])
        self.assertEqual(Items.objects.get(iid=items[0].iid).last_seen_by, 'M:M001')

    def test_public_lot_detail_hides_key(self):
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertEqual(code, 200)
        self.assertNotIn('verif_key', body)
        self.assertTrue(body['public_url'].endswith(f'verif?lot={self.lot.id}'))
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/')
        self.assertEqual(body['verif_key'], self.lot.verif_key)
        self.assertIn(f'key={self.lot.verif_key}', body['private_url'])

    def test_key_check_without_login(self):
        # poste sans personne de connecte : la cle n'est pas renvoyee, le serveur la compare (bug « clé périmée »)
        path = f'/api/lots/{self.lot.id}/?key='
        code, body = self.call('GET', path + self.lot.verif_key, session=False)
        self.assertEqual(code, 200)
        self.assertNotIn('verif_key', body)
        self.assertEqual(body['key_check'], 'valid')
        self.assertNotIn('key_error', body)
        code, body = self.call('GET', path + 'ancienne', session=False)
        self.assertEqual(body['key_check'], 'wrong')
        Lots.objects.filter(pk=self.lot.pk).update(verif_key_expires=self.today - timedelta(days=1))
        code, body = self.call('GET', path + self.lot.verif_key, local=False)
        self.assertEqual(body['key_check'], 'expired')
        self.assertIn('a expiré', body['key_error'])

    def test_missing_key_message_explains_label(self):
        Lots.objects.filter(pk=self.lot.pk).update(key_location='poche intérieure du couvercle')
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': []}, local=False)
        self.assertEqual(code, 403)
        self.assertIn("QR code rangé à l'intérieur du lot (poche intérieure du couvercle)", body['error'])
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertEqual(body['key_location'], 'poche intérieure du couvercle')

    def test_key_location_update(self):
        code, body = self.call('PATCH', f'/api/lots/{self.lot.id}/update/', {'key_location': '  sous le plateau  '})
        self.assertEqual(code, 200)
        self.assertEqual(body['key_location'], 'sous le plateau')

    def test_item_detail_last_location(self):
        # fiche d'item du front web : lot actuel, puis le lot quitte en dernier une fois l'item sorti
        sub = Lots(lot_type=self.lot_type, name='Poche', name_short='P', created_by='test', parent=self.lot)
        sub.save()
        stock_item, item = self.create(self.garrot, None, 2)
        code, body = self.call('GET', f'/api/items/{stock_item.iid}/', local=False)
        self.assertEqual(code, 200)
        self.assertIsNone(body['last_location'])
        self.call('POST', f'/api/lots/{sub.id}/add/', {'items': [item.iid]})
        code, body = self.call('GET', f'/api/items/{item.iid}/', local=False)
        self.assertEqual(body['last_location']['id'], sub.id)
        self.assertTrue(body['last_location']['current'])
        self.assertEqual(body['last_location']['path'], [{'id': self.lot.id, 'name': 'Sac A'}])
        self.call('POST', f'/api/items/{item.iid}/delete/', {'reason': 'cassé'})
        code, body = self.call('GET', f'/api/items/{item.iid}/', local=False)
        self.assertEqual(body['status'], 'deleted')
        self.assertEqual(body['last_location']['id'], sub.id)
        self.assertFalse(body['last_location']['current'])

    def test_default_lot_key_validity_is_one_year(self):
        self.assertEqual(self.lot.verif_key_expires, self.today + timedelta(days=365))

    def test_auth(self):
        code, body = self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.new_key}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(body['prenom'], 'Jeanne')
        code, _ = self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': 'x'}, local=False)
        self.assertEqual(code, 403)
        self.user.key_expires = self.today - timedelta(days=1)
        self.user.save()
        code, _ = self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.new_key}, local=False)
        self.assertEqual(code, 403)

    @override_settings(QRPROTEC={**__import__('django.conf').conf.settings.QRPROTEC, 'LOCAL_API_TOKEN': 'secret'})
    def test_local_token(self):
        code, _ = self.call('GET', '/api/stock/')
        self.assertEqual(code, 403)
        response = self.client.get('/api/stock/', HTTP_X_QRPROTEC_TOKEN='secret',
                                   HTTP_X_QRPROTEC_SESSION=self.operator_session, **LOCAL)
        self.assertEqual(response.status_code, 200)

    def test_remote_front_key(self):
        remote = {'qrprotec.role': 'remote', 'REMOTE_ADDR': '127.0.0.1'}
        # sans cle, ou avec le jeton local seulement : refuse
        self.assertEqual(self.client.get('/api/stock/', **remote).status_code, 401)
        self.assertEqual(self.client.get('/api/stock/', HTTP_X_QRPROTEC_KEY='qrpf_faux', **remote).status_code, 401)
        front, key = FrontKey.create('accueil')
        remote['HTTP_X_QRPROTEC_SESSION'] = self.operator_session
        response = self.client.get('/api/stock/', HTTP_X_QRPROTEC_KEY=key, HTTP_X_FORWARDED_FOR='192.0.2.7', **remote)
        self.assertEqual(response.status_code, 200)
        front.refresh_from_db()
        self.assertEqual(front.last_address, '192.0.2.7')
        health = self.client.get('/api/health/', HTTP_X_QRPROTEC_KEY=key, **remote).json()
        self.assertEqual((health['api'], health['front']), ('local', 'accueil'))
        self.assertIsNotNone(front.last_used)
        # la cle ne vaut que sur l'API distante, pas sur l'API publique
        self.assertEqual(self.client.get('/api/item-types/', HTTP_X_QRPROTEC_KEY=key).status_code, 404)
        front.revoked = True
        front.save()
        self.assertEqual(self.client.get('/api/stock/', HTTP_X_QRPROTEC_KEY=key, **remote).status_code, 401)
        # le front local fonctionne toujours sans cle, en meme temps qu'un front distant
        self.assertEqual(self.call('GET', '/api/stock/')[0], 200)

    def test_frontkey_command(self):
        from io import StringIO

        from django.core.management import CommandError, call_command

        out = StringIO()
        call_command('frontkey', 'add', 'accueil', stdout=out)
        key = next(word for word in out.getvalue().split() if word.startswith('qrpf_'))
        self.assertIsNotNone(FrontKey.authenticate(key))
        with self.assertRaises(CommandError):
            call_command('frontkey', 'add', 'accueil', stdout=StringIO())
        call_command('frontkey', 'revoke', 'accueil', stdout=StringIO())
        self.assertIsNone(FrontKey.authenticate(key))
        self.assertNotIn(key, FrontKey.objects.get().key_hash)


class DeclaredIdentityTests(ApiTestCase):
    """Identite declaree (nom sans badge) : reglage du serveur, desactive par defaut."""

    def verif(self, payload, local=False):
        items = self.create(self.compresses, self.today + timedelta(days=10), 2)
        payload = {'items': [item.iid for item in items], 'key': self.lot.verif_key, **payload}
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/verif/', payload, local=local, session=False)
        return code, body, items

    def test_disabled_by_default(self):
        code, health = self.call('GET', '/api/health/', local=False)
        self.assertFalse(health['declared_identity'])
        code, body, _ = self.verif({'name': 'Jean'})
        self.assertEqual(code, 403)
        self.assertTrue(body['login_required'])
        self.assertFalse(body['declared_identity'])
        # badge invalide : plus d'operation anonyme
        code, _, _ = self.verif({'user': {'matricule': 'M001', 'key': 'faux'}})
        self.assertEqual(code, 403)
        # poste sans utilisateur connecte (session expiree), avec la cle du lot
        code, _, _ = self.verif({'user': 'M001'}, local=True)
        self.assertEqual(code, 403)
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [], 'key': self.lot.verif_key,
                                                                      'name': 'Jean'}, local=False)
        self.assertEqual(code, 403)

    def test_enabled_by_admin(self):
        self.assertEqual(self.call('PATCH', '/api/server-settings/', {'declared_identity': True},
                                   session=views.session_token(self.user))[0], 403)
        code, body = self.call('PATCH', '/api/server-settings/', {'declared_identity': True})
        self.assertEqual(code, 200)
        self.assertTrue(body['declared_identity'])
        self.assertTrue(self.call('GET', '/api/health/', local=False)[1]['declared_identity'])
        code, body, items = self.verif({'name': '  Jean Martin '})
        self.assertEqual(code, 200)
        self.assertEqual(Items.objects.get(iid=items[0].iid).last_seen_by, 'D:Jean Martin')
        # le nom reste obligatoire, et la cle du lot aussi
        code, body, _ = self.verif({'name': ' '})
        self.assertEqual(code, 403)
        self.assertTrue(body['declared_identity'])
        code, _, _ = self.verif({'name': 'Jean', 'key': 'mauvaise'})
        self.assertEqual(code, 403)
        # un badge valide reste prioritaire sur le nom
        code, _, items = self.verif({'name': 'Jean', 'user': {'matricule': 'M001', 'key': self.user.new_key}})
        self.assertEqual(Items.objects.get(iid=items[0].iid).last_seen_by, 'M:M001')
        # poste sans utilisateur connecte : meme regle que l'API publique
        code, _, items = self.verif({'name': 'Jean'}, local=True)
        self.assertEqual(code, 200)
        self.assertEqual(Items.objects.get(iid=items[0].iid).last_seen_by, 'D:Jean')


class ScannerVolumeTests(ApiTestCase):
    """Volume de la douchette : choisi par chaque utilisateur, sinon defaut du serveur fixe par un admin."""

    def login(self, user):
        code, body = self.call('POST', '/api/auth/', {'matricule': user.matricule, 'key': user.new_key}, local=False)
        self.assertEqual(code, 200)
        return body

    def test_default_then_own_volume(self):
        self.assertEqual(self.call('GET', '/api/health/', local=False)[1]['scanner_volume_default'], 4)
        self.assertEqual(self.login(self.user)['scanner_volume'], 4)
        session = views.session_token(self.user)
        # un utilisateur normal regle son propre volume, repris a sa prochaine connexion
        code, body = self.call('PATCH', '/api/me/scanner-volume/', {'volume': 0}, session=session)
        self.assertEqual((code, body['scanner_volume'], body['custom']), (200, 0, True))
        self.assertEqual(self.login(self.user)['scanner_volume'], 0)
        self.assertEqual(self.call('PATCH', '/api/me/scanner-volume/', {'volume': 3}, session=session)[0], 400)
        self.assertEqual(self.call('PATCH', '/api/me/scanner-volume/', {'volume': 2}, session=False)[0], 403)
        # defaut du serveur : admin seulement, et sans effet sur un volume deja choisi
        self.assertEqual(self.call('PATCH', '/api/server-settings/', {'scanner_volume_default': 8},
                                   session=session)[0], 403)
        code, body = self.call('PATCH', '/api/server-settings/', {'scanner_volume_default': 8})
        self.assertEqual((code, body['scanner_volume_default']), (200, 8))
        self.assertEqual(self.login(self.user)['scanner_volume'], 0)
        self.call('PATCH', '/api/me/scanner-volume/', {'volume': None}, session=session)
        self.assertEqual(self.login(self.user)['scanner_volume'], 8)


class PublicNamesTests(ApiTestCase):
    """API publique : prenom seul, plus le debut du nom qui distingue les homonymes de meme prenom."""

    def test_short_names(self):
        people = [(1, 'Michel', 'Dubois'), (2, 'Julien', 'Dubois'), (3, 'Julien', 'Saxe')]
        self.assertEqual(idendity.short_names(people), {1: 'Michel', 2: 'Julien D.', 3: 'Julien S.'})
        people.append((4, 'julien', 'Deraine'))
        self.assertEqual(idendity.short_names(people),
                         {1: 'Michel', 2: 'Julien Du.', 3: 'Julien S.', 4: 'julien De.'})
        # accents et casse ignores ; homonymes complets et noms contenus dans un autre : nom entier
        self.assertEqual(idendity.short_names([(1, 'Émile', 'Roux'), (2, 'Emile', 'rousseau'),
                                               (3, 'Ana', 'Lima'), (4, 'Ana', 'Lima')]),
                         {1: 'Émile Roux', 2: 'Emile rous.', 3: 'Ana Lima', 4: 'Ana Lima'})
        self.assertEqual(idendity.short_names([(1, 'Yann', 'Le Gall'), (2, 'Yann', 'Le Roux')]),
                         {1: 'Yann Le G.', 2: 'Yann Le R.'})

    def test_declared_short_name(self):
        self.assertEqual(idendity.declared_short_name('Jean martin dupuis'), 'Jean M.')
        self.assertEqual(idendity.declared_short_name(' Jean '), 'Jean')

    def test_public_api_hides_last_names(self):
        Secouristes.objects.create(matricule='M002', nom='Durand', prenom='Jeanne')
        items = self.create(self.compresses, self.today + timedelta(days=10), 2)
        payload = {'items': [item.iid for item in items], 'key': self.lot.verif_key,
                   'user': {'matricule': 'M001', 'key': self.user.new_key}}
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/verif/', payload, local=False)
        self.assertEqual(code, 200)
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertEqual(body['last_verif_by'], 'Jeanne Dup.')
        self.assertEqual(self.call('GET', f'/api/items/{items[0].iid}/', local=False)[1]['last_seen_by'], 'Jeanne Dup.')
        # poste : nom complet
        self.assertEqual(self.call('GET', f'/api/lots/{self.lot.id}/')[1]['last_verif_by'], 'Jeanne Dupont')
        # identite declaree et matricule disparu
        Lots.objects.filter(pk=self.lot.pk).update(last_verif_by='D:Jean Martin')
        self.assertEqual(self.call('GET', f'/api/lots/{self.lot.id}/', local=False)[1]['last_verif_by'], 'Jean M.')
        Lots.objects.filter(pk=self.lot.pk).update(last_verif_by='M:X999')
        self.assertEqual(self.call('GET', f'/api/lots/{self.lot.id}/', local=False)[1]['last_verif_by'], 'Inconnu')
        # hors requete (notifications, journal) : nom complet
        self.assertEqual(ser._who('M:M001'), 'Jeanne Dupont')


class FrontSessionTests(ApiTestCase):
    """Le poste (API locale ou distante) n'a plus de privilege en soi : les routes de gestion exigent
    l'utilisateur connecte, reconnu par son jeton de session."""

    def session(self, matricule, role):
        user = Secouristes(matricule=matricule, nom='N', prenom='P', role=role)
        user.renew_key()
        user.save()
        return user, views.session_token(user)

    def test_routes_by_role(self):
        normal = views.session_token(self.user)
        _, gestion = self.session('G001', 'gestion')
        # personne de connecte : lectures du kiosk seulement, sans les cles des lots
        code, body = self.call('GET', '/api/stock/', session=False)
        self.assertEqual((code, body.get('login_required')), (403, True))
        code, lots = self.call('GET', '/api/lots/', session=False)
        self.assertEqual(code, 200)
        self.assertNotIn('verif_key', lots[0])
        self.assertNotIn('verif_key', self.call('GET', f'/api/lots/{self.lot.id}/', session=False)[1])
        self.assertEqual(self.call('GET', '/api/item-types/', session=False)[0], 200)
        self.assertEqual(self.call('POST', '/api/item-types/', {'type': 'serphy', 'name': 'x'}, session=False)[0], 403)
        self.assertEqual(self.call('GET', '/api/users/', session=False)[0], 403)
        self.assertEqual(self.call('POST', '/api/users/', {'matricule': 'X1', 'nom': 'a', 'prenom': 'b'},
                                   session=False)[0], 403)  # il existe deja un admin
        self.assertEqual(self.call('GET', '/api/users/', session='faux')[0], 403)
        # secouriste : pas de gestion
        self.assertEqual(self.call('GET', '/api/stock/', session=normal)[0], 403)
        self.assertNotIn('verif_key', self.call('GET', '/api/lots/', session=normal)[1][0])
        # gestion : inventaire, pas les utilisateurs ni les reglages
        self.assertEqual(self.call('GET', '/api/stock/', session=gestion)[0], 200)
        self.assertIn('verif_key', self.call('GET', '/api/lots/', session=gestion)[1][0])
        self.assertEqual(self.call('GET', '/api/users/', session=gestion)[0], 403)
        self.assertEqual(self.call('GET', '/api/notifications/', session=gestion)[0], 403)
        self.assertEqual(self.call('GET', '/api/users/')[0], 200)  # admin

    def test_identity_comes_from_session(self):
        items = self.create(self.compresses, self.today + timedelta(days=60), 2)
        normal = views.session_token(self.user)
        # le matricule envoye par le poste est ignore : c'est l'utilisateur de la session qui verifie
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid for item in items],
                                                                          'user': 'P001'}, session=normal)
        self.assertEqual(code, 200)
        self.lot.refresh_from_db()
        self.assertEqual(self.lot.last_verif_by, 'M:M001')
        # sans session, le poste est traite comme un telephone : cle du lot obligatoire
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [], 'user': 'P001'}, session=False)
        self.assertEqual(code, 403)

    def test_session_invalid_after_badge_renewal_or_deactivation(self):
        user, token = self.session('G002', 'gestion')
        self.assertEqual(self.call('GET', '/api/stock/', session=token)[0], 200)
        user.active = False
        user.save()
        self.assertEqual(self.call('GET', '/api/stock/', session=token)[0], 403)
        user.active = True
        user.renew_key()
        user.save()
        self.assertEqual(self.call('GET', '/api/stock/', session=token)[0], 403)

    def test_remote_needs_session_and_has_no_django_admin(self):
        _, key = FrontKey.create('accueil')
        remote = {'qrprotec.role': 'remote', 'REMOTE_ADDR': '127.0.0.1', 'HTTP_X_QRPROTEC_KEY': key}
        self.assertEqual(self.client.get('/api/stock/', **remote).status_code, 403)
        self.assertEqual(self.client.get('/api/users/', **remote).status_code, 403)
        self.assertEqual(self.client.get('/api/lots/', **remote).status_code, 200)
        self.assertEqual(self.client.get('/api/users/', HTTP_X_QRPROTEC_SESSION=self.operator_session,
                                         **remote).status_code, 200)
        self.assertEqual(self.client.get('/admin/', **remote).status_code, 404)

    def test_first_admin_without_session(self):
        self.operator.delete()
        code, body = self.call('POST', '/api/users/', {'matricule': 'R001', 'nom': 'a', 'prenom': 'b', 'role': 'normal'},
                               session=False)
        self.assertEqual((code, body['role']), (201, 'admin'))
        self.assertEqual(self.call('GET', '/api/users/', session=body['session'])[0], 200)
        code, _ = self.call('POST', '/api/users/', {'matricule': 'R002', 'nom': 'a', 'prenom': 'b'}, session=False)
        self.assertEqual(code, 403)


class VerifTests(ApiTestCase):
    def test_verif_replacement_and_missing(self):
        old = self.create(self.compresses, self.today - timedelta(days=1), 1)[0]
        kept = self.create(self.compresses, self.today + timedelta(days=30), 1)[0]
        lost = self.create(self.garrot, None, 1)[0]
        Items.objects.filter(iid__in=[old.iid, kept.iid, lost.iid]).update(location=self.lot)
        new = self.create(self.compresses, self.today + timedelta(days=300), 1)[0]

        # l'utilisateur scanne l'ancien perime + le nouveau : l'ancien est considere remplace
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/',
                                 {'items': [old.iid, kept.iid, new.iid, 'inconnu'], 'user': 'M001'})
        self.assertEqual(code, 200)
        self.assertEqual(report['replaced'], [old.iid])
        self.assertEqual(report['missing'], [lost.iid])
        self.assertEqual(report['unknown'], ['inconnu'])
        self.assertTrue(report['complete'])
        self.assertEqual(Items.objects.get(iid=old.iid).status, ItemStatus.REPLACED)
        # un item non vu ne compte plus dans les exigences
        LotRequirements.objects.create(lot_type=self.lot_type, item_type=self.garrot, quantity=1)
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/')
        garrot_row = next(row for row in body['requirements'] if row['type'] == 'garrot')
        self.assertEqual((garrot_row['present'], garrot_row['unconfirmed']), (0, 1))
        self.assertFalse(body['complete'])
        self.assertIsNone(Items.objects.get(iid=old.iid).location)
        self.assertEqual(Items.objects.get(iid=new.iid).location_id, self.lot.id)

        # non perime : disparu seulement apres plusieurs verifs manquees
        self.assertEqual(Items.objects.get(iid=lost.iid).status, ItemStatus.ACTIVE)
        for _ in range(2):
            self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [kept.iid, new.iid]})
        self.assertEqual(Items.objects.get(iid=lost.iid).status, ItemStatus.MISSING)

    def test_expired_not_replaced_stays(self):
        old = self.create(self.compresses, self.today - timedelta(days=1), 1)[0]
        Items.objects.filter(iid=old.iid).update(location=self.lot)
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [old.iid]})
        self.assertEqual(report['expired'], [old.iid])
        self.assertFalse(report['complete'])

    def test_add_items_and_back_to_stock(self):
        items = self.create(self.garrot, None, 2)
        iids = [item.iid for item in items]
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': iids})
        self.assertEqual(body['moved'], iids)
        self.assertEqual(Items.objects.filter(location=self.lot).count(), 2)
        code, body = self.call('POST', '/api/items/to-stock/', {'items': iids[:1]})
        self.assertEqual(Items.objects.filter(location=self.lot).count(), 1)


class ManagementTests(ApiTestCase):
    def test_batch_with_sealed_pack_and_stock(self):
        date = (self.today + timedelta(days=200)).isoformat()
        code, body = self.call('POST', '/api/items/batch/',
                               {'type': 'compre', 'peremption': date, 'count': 5, 'sealed_pack': True, 'user': 'M001'})
        self.assertEqual(code, 201)
        self.assertEqual(len(body['items']), 5)
        pack_id = body['sealed_pack']['id']
        code, pack = self.call('GET', f'/api/packs/{pack_id}/', local=False)
        self.assertEqual(len(pack['items']), 5)
        code, opened = self.call('POST', f'/api/packs/{pack_id}/open/', {'user': 'M001'})
        self.assertIsNotNone(opened['opened'])
        self.assertEqual(len(opened['items']), 5)
        # reouverture : reimpression, la date d'ouverture ne change pas
        code, again = self.call('POST', f'/api/packs/{pack_id}/open/', {'user': 'M001'})
        self.assertEqual(code, 200)
        self.assertEqual(again['opened'], opened['opened'])
        self.assertEqual(len(again['items']), 5)
        # refermer un paquet ouvert par erreur
        code, closed = self.call('POST', f'/api/packs/{pack_id}/close/', {'user': 'M001'})
        self.assertEqual(code, 200)
        self.assertIsNone(closed['opened'])
        code, _ = self.call('POST', f'/api/packs/{pack_id}/close/', {'user': 'M001'}, local=False)
        self.assertEqual(code, 404)
        code, stock = self.call('GET', '/api/stock/')
        row = next(row for row in stock if row['type'] == 'compre')
        self.assertEqual(row['stock_fresh'], 5)
        self.assertEqual(row['min_quantity'], 10)

    def test_create_entities(self):
        code, body = self.call('POST', '/api/item-types/', {'type': 'serphy', 'name': 'Serum phy', 'perissable': True})
        self.assertEqual(code, 201)
        code, _ = self.call('POST', '/api/item-types/', {'type': 'trop_long', 'name': 'x'})
        self.assertEqual(code, 400)
        code, body = self.call('POST', '/api/users/', {'matricule': 'M002', 'nom': 'Martin', 'prenom': 'Paul'})
        self.assertEqual(code, 201)
        self.assertEqual(len(body['key']), 24)
        self.assertIn('badge?m=M002', body['badge_url'])
        code, body = self.call('PUT', '/api/lot-types/sacpse/requirements/',
                               {'requirements': [{'type': 'serphy', 'quantity': 4}]})
        self.assertEqual(body['requirements'][0]['quantity'], 4)
        code, body = self.call('POST', '/api/lots/', {'lot_type': 'sacpse', 'name': 'Sac B'})
        self.assertEqual(code, 201)
        self.assertTrue(body['id'].startswith('sacpse'))
        self.assertEqual(len(body['id']), 14)

    def test_item_type_tasks(self):
        """Taches d'un type d'item (etiquette de taches) : nettoyees, exposees sur l'item avec l'URL de l'etiquette."""
        code, body = self.call('POST', '/api/item-types/', {'type': 'defibr', 'name': 'DAE',
                                                            'tasks': [' Voyant  vert ', '', 'Électrodes', 'Voyant vert']})
        self.assertEqual(code, 201)
        self.assertEqual(body['tasks'], ['Voyant vert', 'Électrodes'])
        code, _ = self.call('PATCH', '/api/item-types/defibr/', {'tasks': 'pas une liste'})
        self.assertEqual(code, 400)
        code, _ = self.call('PATCH', '/api/item-types/defibr/', {'tasks': [str(n) for n in range(30)]})
        self.assertEqual(code, 400)
        item = self.create(ItemType.objects.get(type='defibr'), None, 1)[0]
        code, body = self.call('GET', f'/api/items/{item.iid}/', local=False)
        self.assertEqual(body['tasks'], ['Voyant vert', 'Électrodes'])
        self.assertTrue(body['task_url'].endswith(f'task?id={item.iid}'))
        code, body = self.call('PATCH', '/api/item-types/defibr/', {'tasks': []})
        self.assertEqual(body['tasks'], [])
        code, body = self.call('GET', f'/api/items/{self.create(self.garrot, None, 1)[0].iid}/', local=False)
        self.assertEqual(body['tasks'], [])

    def test_delete_requires_reason(self):
        item = self.create(self.garrot, None, 1)[0]
        code, _ = self.call('POST', f'/api/items/{item.iid}/delete/', {})
        self.assertEqual(code, 400)
        code, body = self.call('POST', f'/api/items/{item.iid}/delete/', {'reason': 'casse'})
        self.assertEqual(body['status'], 'deleted')
        code, body = self.call('POST', f'/api/items/{item.iid}/restore/', {})
        self.assertEqual(body['status'], 'active')


class WebFrontTests(ApiTestCase):
    def test_qr_urls_serve_the_web_page(self):
        for url in ('/', '/verif', f'/verif?lot={self.lot.id}&key=x', '/badge?m=M001&key=x', '/pack?id=1',
                    '/item?id=compre20271231000000A1', '/task?id=compre20271231000000A1'):
            response = self.client.get(url)
            self.assertEqual(response.status_code, 200, url)
            self.assertIn(b'web/app.js', b''.join(response.streaming_content))
            self.assertEqual(response['Referrer-Policy'], 'no-referrer')

    def test_assets_whitelist(self):
        self.assertEqual(self.client.get('/web/app.js').status_code, 200)
        self.assertEqual(self.client.get('/web/vendor/jsQR.js').status_code, 200)
        self.assertEqual(self.client.get('/web/../views.py').status_code, 404)
        self.assertEqual(self.client.get('/web/index.html').status_code, 404)

    def test_douchette_easter_egg(self):
        """Le script est servi aux deux pages et ses phrases sont celles de l'app (codes.cpp)."""
        import re
        from pathlib import Path
        self.assertEqual(self.client.get('/web/douchette.js').status_code, 200)
        web = Path(__file__).resolve().parent / 'web'
        for page in ('index.html', 'scanner.html'):
            self.assertIn('web/douchette.js', (web / page).read_text(encoding='utf-8'))
        js = (web / 'douchette.js').read_text(encoding='utf-8')
        js_phrases = [a or b for a, b in re.findall(r"^\s+(?:'([^']*)'|\"([^\"]*)\"),$", js, re.M)]
        self.assertGreater(len(js_phrases), 1)
        codes = Path(__file__).resolve().parents[2] / 'app' / 'src' / 'core' / 'codes.cpp'
        if not codes.exists():
            self.skipTest("sources de l'app absentes")
        cpp = codes.read_text(encoding='utf-8')
        block = cpp[cpp.index('douchette_phrases()'):]
        block = block[:block.index('};')]
        cpp_phrases = re.findall(r'^\s+"(.*)",$', block, re.M)
        self.assertEqual(js_phrases, cpp_phrases)

    def test_icons_and_service_worker(self):
        for url in ('/web/sw.js', '/web/icon-192.png', '/web/icon-512.png', '/web/favicon.png', '/favicon.ico'):
            self.assertEqual(self.client.get(url).status_code, 200, url)
        self.assertEqual(self.client.get('/favicon.ico')['Content-Type'], 'image/png')


class SetupTests(ApiTestCase):
    def test_setup_and_createadmin(self):
        from io import StringIO
        from django.core.management import call_command
        self.operator.delete()
        code, body = self.call('GET', '/api/setup/', session=False)
        self.assertTrue(body['needs_admin'])
        self.assertEqual(self.call('GET', '/api/setup/', local=False)[0], 404)
        out = StringIO()
        call_command('createadmin', 'R001', 'Melica', 'Hippolyte', stdout=out)
        self.assertIn('badge?m=R001&key=', out.getvalue())
        code, body = self.call('GET', '/api/setup/', session=False)
        self.assertFalse(body['needs_admin'])
        admin = Secouristes.objects.get(matricule='R001')
        self.assertTrue(admin.privileged)
        self.assertEqual(admin.role, 'admin')
        old_hash = admin.key_hash
        call_command('createadmin', 'R001', stdout=StringIO())
        self.assertNotEqual(Secouristes.objects.get(matricule='R001').key_hash, old_hash)


# PIN haches avec un algorithme rapide : les tests de blocage font une cinquantaine d'essais
@override_settings(PASSWORD_HASHERS=['django.contrib.auth.hashers.MD5PasswordHasher'])
class PinTests(ApiTestCase):
    def auth(self, user, **extra):
        return self.call('POST', '/api/auth/', {'matricule': user.matricule, 'key': user.new_key, **extra}, local=False)

    def test_admin_sets_pin_at_first_login_then_needs_it(self):
        admin = Secouristes(matricule='A001', nom='Ad', prenom='Min', role='admin')
        admin.renew_key()
        admin.save()
        code, body = self.auth(admin)
        self.assertEqual(code, 403)
        self.assertTrue(body['pin_setup_required'])
        code, body = self.auth(admin, new_pin='12')
        self.assertEqual(code, 400)
        code, body = self.auth(admin, new_pin='4821')
        self.assertEqual(code, 200)
        self.assertTrue(body['has_pin'])
        code, body = self.auth(admin)
        self.assertEqual((code, body.get('pin_required')), (403, True))
        code, body = self.auth(admin, pin='4821')
        self.assertEqual(code, 200)
        # le jeton de session ouvre l'etat des stocks sans redemander le PIN
        badge = {'matricule': 'A001', 'key': admin.new_key}
        code, _ = self.call('POST', '/api/stock/summary/', {'user': badge}, local=False)
        self.assertEqual(code, 403)
        code, _ = self.call('POST', '/api/stock/summary/', {'user': {**badge, 'session': body['session']}}, local=False)
        self.assertEqual(code, 200)
        # un admin ne peut pas supprimer son PIN
        code, _ = self.call('PATCH', '/api/users/A001/', {'pin': ''})
        self.assertEqual(code, 400)

    def test_lockout_and_optional_pin(self):
        code, body = self.auth(self.user)
        self.assertEqual(code, 200)  # secouriste sans PIN
        self.assertFalse(body['pin_required'])
        code, body = self.call('PATCH', '/api/users/M001/', {'pin': '123456'})
        self.assertTrue(body['has_pin'])
        self.user.refresh_from_db()
        for _ in range(5):
            code, body = self.auth(self.user, pin='000000')
        self.assertTrue(body['pin_locked'])
        code, body = self.auth(self.user, pin='123456')  # bon PIN mais bloque
        self.assertEqual(code, 403)
        self.assertTrue(body['pin_locked'])
        self.call('PATCH', '/api/users/M001/', {'pin': ''})  # suppression (debloque)
        self.user.refresh_from_db()
        self.assertEqual(self.auth(self.user)[0], 200)

    def fail_pin(self, user, times):
        for _ in range(times):
            Secouristes.objects.filter(pk=user.pk).update(pin_locked_until=None)  # saute le blocage de 5 min
            code, body = self.auth(user, pin='000000')
        return code, body

    def test_pin_blocked_after_failures_until_admin_reset(self):
        admin = Secouristes(matricule='A001', nom='Ad', prenom='Min', role='admin')
        admin.renew_key()
        admin.set_pin('4821')
        admin.save()
        code, _ = self.call('PATCH', '/api/users/M001/', {'pin': '123456', 'pin_contact': 'M001'})
        self.assertEqual(code, 400)  # l'admin a contacter doit etre un administrateur
        code, body = self.call('PATCH', '/api/users/M001/', {'pin': '123456', 'pin_contact': 'A001'})
        self.assertEqual((body['pin_contact'], body['pin_contact_name']), ('A001', 'Min Ad'))
        code, session = self.auth(self.user, pin='123456')
        self.assertEqual(code, 200)

        # un PIN correct remet le compteur a zero
        self.fail_pin(self.user, 49)
        self.assertEqual(self.auth(self.user, pin='123456')[0], 200)
        code, body = self.fail_pin(self.user, 49)
        self.assertFalse(body.get('pin_blocked', False))
        code, body = self.fail_pin(self.user, 1)  # 50e echec
        self.assertEqual(code, 403)
        self.assertTrue(body['pin_blocked'])
        reset = body['pin_reset']
        self.assertIn('pinreset?m=M001&t=', reset['url'])
        self.assertEqual(reset['contact'], 'Min Ad')
        # bloque : meme le bon PIN est refuse, et la session deja ouverte ne vaut plus rien
        code, body = self.auth(self.user, pin='123456')
        self.assertEqual(code, 403)
        self.assertTrue(body['pin_blocked'])
        Secouristes.objects.filter(pk=self.user.pk).update(role='gestion')
        badge = {'matricule': 'M001', 'key': self.user.new_key, 'session': session['session']}
        self.assertEqual(self.call('POST', '/api/stock/summary/', {'user': badge}, local=False)[0], 403)

        token = reset['url'].split('t=')[1]
        from urllib.parse import unquote
        token = unquote(token)
        request = {'matricule': 'M001', 'token': token}
        # deblocage : admin connecte (badge + session PIN) seulement
        self.assertEqual(self.call('POST', '/api/pin-reset/', request, local=False)[0], 403)
        admin_badge = {'matricule': 'A001', 'key': admin.new_key}
        code, body = self.call('POST', '/api/pin-reset/', {**request, 'user': admin_badge}, local=False)
        self.assertEqual((code, body.get('pin_required')), (403, True))
        code, login = self.auth(admin, pin='4821')
        admin_badge['session'] = login['session']
        code, _ = self.call('POST', '/api/pin-reset/', {'matricule': 'M001', 'token': 'faux', 'user': admin_badge},
                            local=False)
        self.assertEqual(code, 404)
        code, body = self.call('POST', '/api/pin-reset/', {**request, 'user': admin_badge}, local=False)
        self.assertEqual((code, body['reset'], body['failures'], body['prenom']), (200, False, 50, 'Jeanne'))
        code, body = self.call('POST', '/api/pin-reset/', {**request, 'user': admin_badge, 'confirm': True}, local=False)
        self.assertEqual((code, body['reset']), (200, True))
        # lien a usage unique
        code, _ = self.call('POST', '/api/pin-reset/', {**request, 'user': admin_badge, 'confirm': True}, local=False)
        self.assertEqual(code, 404)
        # l'utilisateur choisit un nouveau PIN a sa prochaine connexion
        code, body = self.auth(self.user)
        self.assertEqual(code, 403)
        self.assertTrue(body['pin_setup_required'])
        code, body = self.auth(self.user, new_pin='2468')
        self.assertEqual(code, 200)
        self.assertFalse(body['pin_reset_required'])
        self.assertEqual(self.auth(self.user, pin='2468')[0], 200)

    def test_local_pin_reset(self):
        self.call('PATCH', '/api/users/M001/', {'pin': '123456'})
        self.user.refresh_from_db()
        self.fail_pin(self.user, 50)
        code, users = self.call('GET', '/api/users/')
        row = next(user for user in users if user['matricule'] == 'M001')
        self.assertTrue(row['pin_blocked'])
        self.assertEqual(row['pin_failures'], 50)
        code, body = self.call('PATCH', '/api/users/M001/', {'pin_reset': True})
        self.assertEqual((body['pin_blocked'], body['pin_reset_required'], body['has_pin']), (False, True, False))
        self.assertTrue(self.auth(self.user)[1]['pin_setup_required'])


    def test_forgotten_pin(self):
        # sans PIN : rien a oublier
        code, body = self.call('POST', '/api/pin-forgot/', {'matricule': 'M001', 'key': self.user.new_key}, local=False)
        self.assertEqual(code, 400)
        self.call('PATCH', '/api/users/M001/', {'pin': '123456'})
        code, _ = self.call('POST', '/api/pin-forgot/', {'matricule': 'M001', 'key': 'faux'}, local=False)
        self.assertEqual(code, 403)
        code, body = self.call('POST', '/api/pin-forgot/', {'matricule': 'M001', 'key': self.user.new_key}, local=False)
        self.assertEqual(code, 403)
        self.assertTrue(body['pin_blocked'])
        self.assertTrue(body['pin_reset']['forgotten'])
        self.assertFalse(body['pin_reset']['notified'])  # aucune notification configuree
        self.assertIn('oublié', body['error'])
        # bloque comme apres 50 essais, meme lien de deblocage
        code, again = self.auth(self.user, pin='123456')
        self.assertTrue(again['pin_blocked'])
        self.assertEqual(again['pin_reset']['url'], body['pin_reset']['url'])
        code, users = self.call('GET', '/api/users/')
        row = next(user for user in users if user['matricule'] == 'M001')
        self.assertTrue(row['pin_forgotten'])
        self.call('PATCH', '/api/users/M001/', {'pin_reset': True})
        self.user.refresh_from_db()
        self.assertFalse(self.user.pin_forgotten)
        self.assertTrue(self.auth(self.user)[1]['pin_setup_required'])

class PinModeTests(ApiTestCase):
    """PIN obligatoire ou desactive par secouriste, et PIN obligatoire par defaut (reglage du serveur)."""

    def auth(self, user, **extra):
        return self.call('POST', '/api/auth/', {'matricule': user.matricule, 'key': user.new_key, **extra}, local=False)

    def test_required_then_disabled(self):
        code, body = self.call('PATCH', '/api/users/M001/', {'pin_mode': 'required'})
        self.assertEqual((code, body['pin_mode'], body['pin_required']), (200, 'required', True))
        code, body = self.auth(self.user)
        self.assertEqual((code, body.get('pin_setup_required')), (403, True))
        self.assertEqual(self.auth(self.user, new_pin='2468')[0], 200)
        self.assertEqual(self.auth(self.user)[0], 403)
        # desactive : PIN supprime, plus demande
        code, body = self.call('PATCH', '/api/users/M001/', {'pin_mode': 'disabled'})
        self.assertEqual((code, body['has_pin'], body['pin_required']), (200, False, False))
        self.assertEqual(self.auth(self.user)[0], 200)
        # refuse pour un admin, et un mode inconnu
        self.assertEqual(self.call('PATCH', '/api/users/P001/', {'pin_mode': 'disabled'})[0], 400)
        self.assertEqual(self.call('PATCH', '/api/users/M001/', {'pin_mode': 'parfois'})[0], 400)
        # un PIN choisi par un admin reactive le PIN
        code, body = self.call('PATCH', '/api/users/M001/', {'pin': '1357'})
        self.assertEqual((body['pin_mode'], body['pin_required']), ('', True))

    def test_server_default(self):
        self.assertEqual(self.auth(self.user)[0], 200)
        code, body = self.call('PATCH', '/api/server-settings/', {'pin_required_default': True})
        self.assertEqual((code, body['pin_required_default']), (200, True))
        code, body = self.auth(self.user)
        self.assertEqual((code, body.get('pin_setup_required')), (403, True))
        # desactive pour ce secouriste : le reglage par defaut ne s'applique plus
        self.call('PATCH', '/api/users/M001/', {'pin_mode': 'disabled'})
        self.assertEqual(self.auth(self.user)[0], 200)
        # reservee aux admins
        self.assertEqual(self.call('PATCH', '/api/server-settings/', {'pin_required_default': False},
                                   session=views.session_token(self.user))[0], 403)


class NameLoginTests(ApiTestCase):
    """Connexion sans badge par nom, prenom et PIN : ne dit jamais si le nom existe, essais limites."""

    def setUp(self):
        super().setUp()
        cache.clear()
        self.user.set_pin('2468')
        self.user.save()

    def login(self, nom='Dupont', prenom='Jeanne', pin='2468', address='10.0.0.1'):
        response = self.client.post('/api/auth/name/', json.dumps({'nom': nom, 'prenom': prenom, 'pin': pin}),
                                    content_type='application/json', REMOTE_ADDR=address)
        return response.status_code, response.json()

    def test_login_and_session_as_badge(self):
        code, body = self.login(nom='DUPONT', prenom='jeanne')
        self.assertEqual((code, body['matricule']), (200, 'M001'))
        credentials = {'matricule': 'M001', 'session': body['session']}
        # le jeton remplace la cle du badge pour identifier les operations du telephone
        self.assertEqual(self.call('POST', '/api/verifs/recent/', {'user': credentials}, local=False)[0], 200)
        self.assertEqual(idendity.identity_from_request({'user': credentials}), 'M:M001')
        self.assertIsNone(idendity.identity_from_request({'user': {'matricule': 'M001', 'session': 'faux'}}))

    def test_refusals_look_the_same(self):
        unknown = self.login(nom='Personne')
        wrong = self.login(pin='1111')
        self.assertEqual(unknown, wrong)
        self.assertEqual(unknown[0], 403)
        # sans PIN, ou PIN desactive : meme refus
        self.call('PATCH', '/api/users/M001/', {'pin_mode': 'disabled'})
        self.assertEqual(self.login(), unknown)

    def test_failures_pause_without_blocking_badge_pin(self):
        for _ in range(5):
            self.login(pin='1111', address='10.0.0.2')
        self.user.refresh_from_db()
        self.assertIsNotNone(self.user.name_login_locked_until)
        self.assertEqual(self.user.pin_failures, 0)  # le PIN du badge n'est pas touche
        self.assertEqual(self.login(address='10.0.0.3')[0], 403)  # en pause, meme avec le bon PIN
        # une connexion avec le badge leve la pause
        self.assertEqual(self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.new_key,
                                                          'pin': '2468'}, local=False)[0], 200)
        self.assertEqual(self.login(address='10.0.0.3')[0], 200)

    def test_address_limit(self):
        for _ in range(10):
            self.login(nom='Personne', address='10.0.0.4')
        self.assertEqual(self.login(address='10.0.0.4')[0], 429)
        self.assertEqual(self.login(address='10.0.0.5')[0], 200)


class BadgeKeyTests(ApiTestCase):
    def test_badge_key_only_at_creation_and_renewal(self):
        code, created = self.call('POST', '/api/users/', {'matricule': 'M002', 'nom': 'Martin', 'prenom': 'Paul'})
        key = created['key']
        self.assertIn(f'key={key}', created['badge_url'])
        # ni la liste ni la fiche ne contiennent la cle, la base n'en garde que l'empreinte
        code, users = self.call('GET', '/api/users/')
        self.assertTrue(all('key' not in user and 'badge_url' not in user for user in users))
        code, body = self.call('GET', '/api/users/M002/')
        self.assertNotIn('key', body)
        stored = Secouristes.objects.get(matricule='M002')
        self.assertEqual(len(stored.key_hash), 64)
        self.assertNotIn(key, stored.key_hash)
        self.assertEqual(self.call('POST', '/api/auth/', {'matricule': 'M002', 'key': key}, local=False)[0], 200)
        # renouvellement : nouvelle cle affichee une fois, l'ancienne ne marche plus
        code, renewed = self.call('POST', '/api/users/M002/renew-key/')
        self.assertNotEqual(renewed['key'], key)
        self.assertEqual(self.call('POST', '/api/auth/', {'matricule': 'M002', 'key': key}, local=False)[0], 403)
        self.assertEqual(self.call('POST', '/api/auth/', {'matricule': 'M002', 'key': renewed['key']},
                                   local=False)[0], 200)
        self.assertEqual(self.call('POST', '/api/auth/', {'matricule': 'M002', 'key': None}, local=False)[0], 403)


class RestockTests(ApiTestCase):
    def test_restock_recommends_verif(self):
        new = self.create(self.compresses, self.today + timedelta(days=90), 2)
        code, lot = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertFalse(lot['verif_recommended'])
        # reassort par l'etiquette privee (API publique : cle du lot)
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/add/',
                               {'items': [item.iid for item in new], 'key': self.lot.verif_key,
                                'user': {'matricule': 'M001', 'key': self.user.new_key}}, local=False)
        self.assertEqual(code, 200)
        code, lot = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertTrue(lot['verif_recommended'])
        self.assertEqual(lot['restocked_count'], 2)
        self.assertIsNotNone(lot['restocked'])
        # une verif complete leve la recommandation
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid for item in new], 'user': 'M001'})
        code, lot = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertFalse(lot['verif_recommended'])
        self.assertEqual(lot['restocked_count'], 0)


class RoleTests(ApiTestCase):
    def badge(self, user):
        return {'matricule': user.matricule, 'key': user.new_key}

    def test_roles_and_public_summaries(self):
        code, body = self.call('POST', '/api/users/', {'matricule': 'G001', 'nom': 'Gest', 'prenom': 'Ion',
                                                       'role': 'gestion'})
        self.assertEqual(code, 201)
        self.assertEqual(body['role'], 'gestion')
        self.assertTrue(body['privileged'])
        gestion_key = body['key']  # cle du badge : seulement dans la reponse de creation
        code, _ = self.call('POST', '/api/users/', {'matricule': 'X001', 'nom': 'a', 'prenom': 'b', 'role': 'chef'})
        self.assertEqual(code, 400)
        gestion = Secouristes.objects.get(matricule='G001')
        gestion.new_key = gestion_key
        # etat des stocks : lecture seule, gestion ou admin seulement
        code, stock = self.call('POST', '/api/stock/summary/', {'user': self.badge(gestion)}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual({row['type'] for row in stock}, {'compre', 'garrot'})
        code, _ = self.call('POST', '/api/stock/summary/', {'user': self.badge(self.user)}, local=False)
        self.assertEqual(code, 403)
        code, _ = self.call('POST', '/api/stock/summary/', {}, local=False)
        self.assertEqual(code, 403)
        # liste des lots : publique (page « État des lots »), sans les cles ; un badge envoye doit etre valide
        code, lots = self.call('POST', '/api/lots/summary/', {'user': self.badge(self.user)}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(lots[0]['id'], self.lot.id)
        self.assertNotIn('verif_key', lots[0])
        code, lots = self.call('POST', '/api/lots/summary/', {}, local=False)
        self.assertEqual((code, lots[0]['id']), (200, self.lot.id))
        self.assertNotIn('verif_key', lots[0])
        code, _ = self.call('POST', '/api/lots/summary/', {'user': {'matricule': 'M001', 'key': 'faux'}}, local=False)
        self.assertEqual(code, 403)
        # reglages des notifications web : seulement pour qui peut en recevoir
        code, body = self.call('POST', '/api/auth/', self.badge(gestion), local=False)
        self.assertTrue(body['notifications'])
        code, body = self.call('POST', '/api/auth/', self.badge(self.user), local=False)
        self.assertFalse(body['notifications'])
        # changement de role, et ancien champ privileged = admin
        code, body = self.call('PATCH', '/api/users/M001/', {'role': 'admin'})
        self.assertEqual(body['role'], 'admin')
        code, body = self.call('PATCH', '/api/users/M001/', {'privileged': False})
        self.assertEqual(body['role'], 'normal')


class SealTests(ApiTestCase):
    def fill_lot(self):
        items = self.create(self.compresses, self.today + timedelta(days=60), 2)
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid for item in items], 'user': 'M001'})
        return items

    def test_seal_requires_complete_lot(self):
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001'})
        self.assertEqual(code, 400)
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001', 'force': True})
        self.assertEqual(code, 200)
        self.assertTrue(body['is_sealed'])

    def test_seal_qr_and_break(self):
        self.fill_lot()
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001', 'seal_number': 'S123'})
        self.assertEqual(code, 200)
        self.assertEqual(body['seal_number'], 'S123')
        self.assertEqual(body['valid_until'], (self.today + timedelta(days=60)).isoformat())
        seal_code = body['seal_url'].split('s=')[1]
        # QR du scelle valide sur l'API publique, sans cle
        code, public = self.call('GET', f'/api/lots/{self.lot.id}/?seal={seal_code}', local=False)
        self.assertEqual(public['seal_check'], 'valid')
        self.assertNotIn('seal_url', public)
        code, public = self.call('GET', f'/api/lots/{self.lot.id}/?seal=ancien', local=False)
        self.assertEqual(public['seal_check'], 'wrong')
        # ouverture publique : cle du lot obligatoire
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/unseal/', {'name': 'x'}, local=False)
        self.assertEqual(code, 403)
        # une verif brise le scelle
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [], 'user': 'M001'})
        self.assertTrue(report['unsealed'])
        code, public = self.call('GET', f'/api/lots/{self.lot.id}/?seal={seal_code}', local=False)
        self.assertEqual(public['seal_check'], 'unsealed')
        self.assertIsNotNone(public['unsealed'])

    def test_open_label_opens_seal_without_login(self):
        self.fill_lot()
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001'})
        open_code = body['seal_open_url'].split('c=')[1]
        self.assertIn('/unseal?lot=', body['seal_open_url'])
        # ni l'etiquette du scelle ni un code faux n'ouvrent le lot
        seal_code = body['seal_url'].split('s=')[1]
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/seal-open/', {'code': seal_code}, local=False)
        self.assertEqual(code, 404)
        # scan sans personne de connecte : le scelle est ouvert, anonymement
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal-open/', {'code': open_code}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(body['result'], 'opened')
        self.assertFalse(body['identified'])
        self.assertFalse(body['lot']['is_sealed'])
        self.assertNotIn('seal_open_url', body['lot'])
        self.assertEqual(body['lot']['unsealed_by'], 'Anonyme (étiquette intérieure)')
        # ecran d'ouverture : le scelle etait valide, contenu valide jusqu'a la premiere peremption
        self.assertTrue(body['seal']['valid'])
        self.assertTrue(body['seal']['sealed'])
        self.assertEqual(body['seal']['valid_until'], body['lot']['valid_until'])
        self.assertIsNotNone(body['seal']['opened'])
        code, public = self.call('GET', f'/api/lots/{self.lot.id}/?seal={seal_code}', local=False)
        self.assertEqual(public['seal_check'], 'unsealed')
        # l'utilisateur se connecte juste apres : l'ouverture est signee
        user = {'matricule': 'M001', 'key': self.user.new_key}
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal-open/', {'code': open_code, 'user': user},
                               local=False)
        self.assertEqual(body['result'], 'signed')
        self.assertEqual(body['lot']['unsealed_by'], 'Jeanne')  # API publique : prenom seul
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal-open/', {'code': open_code, 'user': user},
                               local=False)
        self.assertEqual(body['result'], 'already')
        # nouveau scellage : l'ancienne etiquette d'ouverture n'ouvre plus le lot
        self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001', 'force': True})
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal-open/', {'code': open_code}, local=False)
        self.assertEqual(code, 404)
        self.lot.refresh_from_db()
        self.assertTrue(self.lot.is_sealed)

    def test_open_label_recommends_verif(self):
        self.fill_lot()
        sub_lot = Lots(lot_type=self.lot_type, name='Sac O2', name_short='O2', created_by='test', parent=self.lot)
        sub_lot.save()
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001', 'force': True})
        open_code = body['seal_open_url'].split('c=')[1]
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal-open/', {'code': open_code}, local=False)
        self.assertTrue(body['lot']['verif_recommended'])
        self.assertEqual(body['lot']['restocked_count'], 0)
        self.assertEqual(ser.lot_state(body['lot'])['code'], 'opened')
        self.assertEqual(ser.lot_state(body['lot'])['kind'], 'warn')
        sub_lot.refresh_from_db()
        self.assertTrue(sub_lot.verif_recommended)
        # un nouveau scan ne change rien, une vérif complète remet le lot au vert
        self.call('POST', f'/api/lots/{self.lot.id}/seal-open/', {'code': open_code}, local=False)
        items = self.create(self.compresses, self.today + timedelta(days=60), 2)
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid for item in items], 'user': 'M001'})
        self.lot.refresh_from_db()
        self.assertFalse(self.lot.verif_recommended)

    def test_seal_required_lot_stays_red_until_resealed(self):
        code, body = self.call('PATCH', f'/api/lot-types/{self.lot_type.type}/', {'seal_required': True})
        self.assertEqual(code, 200)
        self.assertTrue(body['seal_required'])
        self.fill_lot()
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/')
        self.assertTrue(body['seal_required'])
        self.assertEqual(body['state']['code'], 'must_seal')  # verifie mais jamais scelle
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001'})
        self.assertEqual(ser.lot_state(body)['code'], 'sealed')
        open_code = body['seal_open_url'].split('c=')[1]
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal-open/', {'code': open_code}, local=False)
        # ouvert : rouge, pas orange
        self.assertEqual(ser.lot_state(body['lot'])['code'], 'seal_opened')
        self.assertEqual(ser.lot_state(body['lot'])['kind'], 'bad')
        # une verif complete le marque « doit etre scelle », toujours rouge
        items = list(Items.objects.filter(location=self.lot, status=ItemStatus.ACTIVE))
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid for item in items], 'user': 'M001'})
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/')
        self.assertEqual(body['state']['code'], 'must_seal')
        self.assertEqual(body['state']['kind'], 'bad')
        # un nouveau scelle le remet au vert
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001'})
        self.assertEqual(ser.lot_state(body)['kind'], 'ok')

    def test_open_label_on_station_uses_logged_user(self):
        self.fill_lot()
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001'})
        open_code = body['seal_open_url'].split('c=')[1]
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal-open/', {'code': open_code})
        self.assertEqual(body['result'], 'opened')
        self.assertTrue(body['identified'])
        self.assertEqual(body['lot']['unsealed_by'], 'Admin Poste')

    def test_adding_items_breaks_seal(self):
        self.fill_lot()
        self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001'})
        extra = self.create(self.garrot, None, 1)[0]
        self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [extra.iid], 'user': 'M001'})
        self.lot.refresh_from_db()
        self.assertFalse(self.lot.is_sealed)

    def test_requirement_location(self):
        code, body = self.call('PUT', f'/api/lot-types/{self.lot_type.type}/requirements/', {
            'requirements': [{'type': 'compre', 'quantity': 2, 'location': 'Pochette bleue'}]
        })
        self.assertEqual(body['requirements'][0]['location'], 'Pochette bleue')
        code, lot = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertEqual(lot['requirements'][0]['location'], 'Pochette bleue')


class SubLotTests(ApiTestCase):
    """Lot global (B+) compose de sous-lots (sac de soin, sac d'O2), et rangements du stock."""

    def setUp(self):
        super().setUp()
        self.o2 = ItemType.objects.create(type='bouo2x', name='Bouteille O2', perissable=False)
        self.global_type = LotType.objects.create(type='bplusx', name='B+')
        self.o2_type = LotType.objects.create(type='saco2x', name='Sac O2')
        LotRequirements.objects.create(lot_type=self.o2_type, item_type=self.o2, quantity=1)
        code, self.bplus = self.call('POST', '/api/lots/', {'lot_type': 'bplusx', 'name': 'B+ 1', 'user': 'M001'})
        code, self.soin = self.call('POST', '/api/lots/', {'lot_type': 'sacpse', 'name': 'Sac de soin',
                                                           'parent': self.bplus['id'], 'user': 'M001'})
        code, self.sac_o2 = self.call('POST', '/api/lots/', {'lot_type': 'saco2x', 'name': 'Sac O2',
                                                             'parent': self.bplus['id'], 'user': 'M001'})
        self.assertEqual(code, 201)
        self.keys = {lot.id: lot.verif_key for lot in Lots.objects.all()}
        self.badge = {'matricule': 'M001', 'key': self.user.new_key}

    def fresh(self, count=2):
        return [item.iid for item in self.create(self.compresses, self.today + timedelta(days=90), count)]

    def bottle(self):
        return self.create(self.o2, None, 1)[0].iid

    def test_tree_and_global_state(self):
        code, lot = self.call('GET', f"/api/lots/{self.sac_o2['id']}/", local=False)
        self.assertEqual(lot['parent'], self.bplus['id'])
        self.assertEqual(lot['path'], [{'id': self.bplus['id'], 'name': 'B+ 1'}])
        self.assertEqual(lot['global']['id'], self.bplus['id'])
        self.assertEqual([row['name'] for row in lot['global']['lots']], ['B+ 1', 'Sac de soin', 'Sac O2'])
        self.assertEqual(lot['global']['kind'], 'bad')
        # le B+ ne contient rien lui-meme : il ne compte pas dans l'etat global
        self.assertFalse(lot['global']['lots'][0]['counted'])
        # verif separee de chaque sous-lot
        self.call('POST', f"/api/lots/{self.soin['id']}/verif/", {'items': self.fresh(), 'user': 'M001'})
        code, lot = self.call('GET', f"/api/lots/{self.bplus['id']}/", local=False)
        self.assertEqual(lot['global']['kind'], 'bad')
        self.assertIsNone(lot['global']['last_verif'])  # le sac O2 n'a jamais ete verifie
        self.call('POST', f"/api/lots/{self.sac_o2['id']}/verif/", {'items': [self.bottle()], 'user': 'M001'})
        code, lot = self.call('GET', f"/api/lots/{self.soin['id']}/", local=False)
        self.assertEqual(lot['global']['kind'], 'ok')
        soin = Lots.objects.get(id=self.soin['id'])
        self.assertEqual(lot['global']['last_verif'], timezone.localtime(soin.last_verif).isoformat(timespec='seconds'))
        # liste : ordre de l'arborescence, profondeur et etat global
        code, lots = self.call('GET', '/api/lots/')
        names = [row['name'] for row in lots]
        self.assertEqual(names[names.index('B+ 1'):names.index('B+ 1') + 3], ['B+ 1', 'Sac de soin', 'Sac O2'])
        self.assertEqual([row['depth'] for row in lots if row['root'] == self.bplus['id']], [0, 1, 1])
        self.assertTrue(all(row['global']['kind'] == 'ok' for row in lots if row['root'] == self.bplus['id']))

    def test_global_verif_covers_sub_lots(self):
        bottle = self.bottle()
        code, report = self.call('POST', f"/api/lots/{self.bplus['id']}/verif/",
                                 {'items': self.fresh() + [bottle], 'key': self.keys[self.bplus['id']], 'user': self.badge},
                                 local=False)
        self.assertEqual(code, 200)
        self.assertTrue(report['complete'])
        self.assertEqual(Items.objects.get(iid=bottle).location_id, self.sac_o2['id'])
        self.assertEqual(Items.objects.filter(location_id=self.soin['id']).count(), 2)
        self.assertEqual({row['id'] for row in report['lots'] if row['verified']},
                         {self.bplus['id'], self.soin['id'], self.sac_o2['id']})

    def test_two_private_labels_add_up(self):
        bottle = self.bottle()
        payload = {'lots': [{'id': self.soin['id'], 'key': self.keys[self.soin['id']]},
                            {'id': self.sac_o2['id'], 'key': self.keys[self.sac_o2['id']]}],
                   'items': self.fresh() + [bottle], 'user': self.badge}
        code, report = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 200)
        self.assertTrue(report['complete'])
        self.assertEqual(len(report['lots']), 2)
        self.assertEqual(Items.objects.get(iid=bottle).location_id, self.sac_o2['id'])
        # aucune cle valide
        payload['lots'][0]['key'] = 'faux'
        payload['lots'][1]['key'] = 'faux'
        code, body = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 403)
        # une seule cle valide suffit pour toute la verif
        payload['lots'][1]['key'] = self.keys[self.sac_o2['id']]
        code, body = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 200)
        # la cle du lot global couvre ses sous-lots
        payload['lots'] = [{'id': self.sac_o2['id']}, {'id': self.bplus['id'], 'key': self.keys[self.bplus['id']]}]
        payload['items'] = [bottle]
        code, body = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 200)
        # et la cle d'un seul sous-lot suffit pour la verif du lot global
        payload['lots'] = [{'id': self.bplus['id']}, {'id': self.sac_o2['id'], 'key': self.keys[self.sac_o2['id']]}]
        code, report = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 200)
        self.assertEqual({row['id'] for row in report['lots']}, {self.bplus['id'], self.soin['id'], self.sac_o2['id']})
        payload['lots'] = [{'id': self.bplus['id']}]
        code, body = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 403)

    def test_sealed_sub_lot_scanned_counts_as_verified(self):
        # sac O2 rempli et scelle
        self.call('POST', f"/api/lots/{self.sac_o2['id']}/verif/", {'items': [self.bottle()], 'user': 'M001'})
        code, sealed = self.call('POST', f"/api/lots/{self.sac_o2['id']}/seal/", {'user': 'M001'})
        seal_code = sealed['seal_url'].split('s=')[1]
        # vérif du B+ : le sac de soin est scanne, l'etiquette du scelle du sac O2 aussi
        payload = {'lots': [{'id': self.bplus['id'], 'key': self.keys[self.bplus['id']]}], 'items': self.fresh(),
                   'user': self.badge, 'sealed': [{'id': self.sac_o2['id'], 'code': seal_code}]}
        code, report = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 200)
        self.assertTrue(report['complete'])
        self.assertFalse(report['partial'])
        self.assertEqual(report['missing'], [])  # la bouteille du sac scelle n'est pas attendue
        rows = {row['id']: row for row in report['lots']}
        self.assertTrue(rows[self.sac_o2['id']]['sealed'])
        self.assertEqual(report['sealed_lots'], ['Sac O2'])
        self.assertTrue(Lots.objects.get(id=self.sac_o2['id']).is_sealed)  # scelle garde
        # verif d'un seul sous-lot, le lot scelle n'en fait pas partie : il est compte quand meme
        payload['lots'] = [{'id': self.soin['id'], 'key': self.keys[self.soin['id']]}]
        code, report = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 200)
        self.assertIn(self.sac_o2['id'], {row['id'] for row in report['lots'] if row.get('sealed')})
        # etiquette d'un ancien scelle refusee
        payload['sealed'] = [{'id': self.sac_o2['id'], 'code': 'wrong'}]
        code, body = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 400)

    def test_lots_must_share_global_lot(self):
        payload = {'lots': [{'id': self.soin['id']}, {'id': self.lot.id}], 'items': [], 'user': 'M001'}
        code, body = self.call('POST', '/api/verifs/', payload)
        self.assertEqual(code, 400)

    def test_partial_verif_only_complete_lots(self):
        # contenu actuel : sac de soin rempli
        self.call('POST', f"/api/lots/{self.soin['id']}/verif/", {'items': self.fresh(), 'user': 'M001'})
        before = Lots.objects.get(id=self.soin['id']).last_verif
        # vérif du B+ : seule la bouteille d'O2 est scannee (sac O2 complet), plus un item de reassort du sac de soin
        bottle = self.bottle()
        extra = self.fresh(1)
        code, report = self.call('POST', f"/api/lots/{self.bplus['id']}/verif/",
                                 {'items': [bottle] + extra, 'user': 'M001', 'partial': True})
        self.assertEqual(code, 200)
        self.assertTrue(report['partial'])
        verified = {row['id'] for row in report['lots'] if row['verified']}
        self.assertIn(self.sac_o2['id'], verified)
        self.assertNotIn(self.soin['id'], verified)
        self.assertEqual(report['missing'], [])  # le contenu du sac de soin n'est pas signale manquant
        soin = Lots.objects.get(id=self.soin['id'])
        self.assertEqual(soin.last_verif, before)
        self.assertTrue(soin.verif_recommended)  # l'item ajoute est un reassort
        self.assertEqual(report['restocked'], extra)
        self.assertEqual(Items.objects.get(iid=extra[0]).location_id, self.soin['id'])
        self.assertIsNotNone(Lots.objects.get(id=self.sac_o2['id']).last_verif)

    def test_parent_rules(self):
        code, body = self.call('PATCH', f"/api/lots/{self.bplus['id']}/update/", {'parent': self.soin['id']})
        self.assertEqual(code, 400)  # boucle
        code, body = self.call('PATCH', f"/api/lots/{self.bplus['id']}/update/", {'active': False})
        self.assertEqual(code, 400)  # sous-lots actifs
        code, body = self.call('PATCH', f"/api/lots/{self.soin['id']}/update/", {'parent': ''})
        self.assertEqual(code, 200)
        self.assertIsNone(body['parent'])
        self.assertIsNone(body['global'])  # lot independant : pas de lot global

    def test_sealed_global_lot(self):
        bottle = self.bottle()
        self.call('POST', f"/api/lots/{self.bplus['id']}/verif/", {'items': self.fresh() + [bottle], 'user': 'M001'})
        code, body = self.call('POST', f"/api/lots/{self.bplus['id']}/seal/", {'user': 'M001'})
        self.assertEqual(code, 200)
        code, lot = self.call('GET', f"/api/lots/{self.sac_o2['id']}/", local=False)
        self.assertEqual(lot['global']['kind'], 'ok')
        # ouvrir le sac O2 pour le verifier brise le scelle du B+
        code, report = self.call('POST', f"/api/lots/{self.sac_o2['id']}/verif/", {'items': [bottle], 'user': 'M001'})
        self.assertTrue(report['unsealed'])
        self.assertFalse(Lots.objects.get(id=self.bplus['id']).is_sealed)

    def test_seal_requires_complete_sub_lots(self):
        self.call('POST', f"/api/lots/{self.soin['id']}/verif/", {'items': self.fresh(), 'user': 'M001'})
        code, body = self.call('POST', f"/api/lots/{self.bplus['id']}/seal/", {'user': 'M001'})
        self.assertEqual(code, 400)
        self.assertIn('Sac O2', body['error'])

    def test_storage_counts_as_stock(self):
        # un rangement est un lot unique : chaque armoire, chaque tiroir a son type, cree avec son lot
        code, body = self.call('POST', '/api/lot-types/', {'type': 'armoi1', 'name': 'Armoire 1', 'storage': True})
        self.assertEqual(code, 201)
        armoire = body['created_lot']
        code, body = self.call('POST', '/api/lot-types/', {'type': 'tiroi3', 'name': 'Tiroir 3', 'storage': True,
                                                           'parent': armoire['id']})
        tiroir = body['created_lot']
        stored = self.fresh(3)
        loose = self.fresh(1)
        code, report = self.call('POST', f"/api/lots/{tiroir['id']}/verif/", {'items': stored, 'user': 'M001'})
        self.assertTrue(report['complete'])
        code, stock = self.call('GET', '/api/stock/')
        row = next(row for row in stock if row['type'] == 'compre')
        self.assertEqual(row['stock_fresh'], 4)
        self.assertEqual(row['lots_fresh'], 0)
        code, body = self.call('GET', f'/api/items/{stored[0]}/', local=False)
        self.assertTrue(body['in_stock'])
        self.assertTrue(body['url'].endswith(f'item?id={stored[0]}'))
        code, items = self.call('GET', '/api/items/?location=stock')
        self.assertEqual(len(items), 4)
        # verif d'un seul tiroir : le reste du stock n'est pas touche
        code, report = self.call('POST', f"/api/lots/{tiroir['id']}/verif/", {'items': stored[:2], 'user': 'M001'})
        self.assertEqual(report['missing'], [stored[2]])
        self.assertEqual(Items.objects.get(iid=loose[0]).missed_verifs, 0)
        # la verif du stock non range laisse les items du tiroir a leur place
        code, report = self.call('POST', '/api/stock/verif/', {'items': loose + stored[:1], 'user': 'M001'})
        self.assertEqual(Items.objects.get(iid=stored[0]).location_id, tiroir['id'])
        self.assertEqual(Items.objects.get(iid=loose[0]).location_id, None)
        # ranger dans un rangement n'est pas un reassort
        self.call('POST', f"/api/lots/{tiroir['id']}/add/", {'items': loose, 'user': 'M001'})
        self.assertFalse(Lots.objects.get(id=tiroir['id']).verif_recommended)

    def test_storage_is_unique_without_requirements_nor_expiry(self):
        code, body = self.call('POST', '/api/lot-types/', {'type': 'armoi1', 'name': 'Armoire 1', 'storage': True})
        self.assertTrue(body['unique'])
        armoire = Lots.objects.get(id=body['lot'])
        self.assertIsNone(armoire.verif_key_expires)
        self.assertIsNone(body['created_lot']['verif_key_expires'])
        code, _ = self.call('POST', '/api/lots/', {'lot_type': 'armoi1', 'name': 'Armoire 2'})
        self.assertEqual(code, 409)
        # pas de contenu attendu
        code, _ = self.call('PUT', '/api/lot-types/armoi1/requirements/', {'requirements': [{'type': 'compre'}]})
        self.assertEqual(code, 400)
        # rester unique
        code, body = self.call('PATCH', '/api/lot-types/armoi1/', {'unique': False})
        self.assertTrue(body['unique'])
        # l'etiquette privee n'expire jamais, meme renouvelee, et n'est jamais signalee
        self.assertTrue(armoire.check_key(armoire.verif_key))
        self.call('POST', f'/api/lots/{armoire.id}/rotate-key/')
        armoire.refresh_from_db()
        self.assertIsNone(armoire.verif_key_expires)
        from . import notifications
        report = notifications.check_key_expirations(self.today + timedelta(days=10000))
        self.assertNotIn(armoire.name, ' '.join(report['lot_key_expiring']))

    def test_type_becomes_storage(self):
        trousse_type = LotType.objects.create(type='trousx', name='Trousse')
        LotRequirements.objects.create(lot_type=trousse_type, item_type=self.compresses, quantity=2)
        self.call('POST', '/api/lots/', {'lot_type': 'trousx', 'name': 'Trousse 1'})
        self.call('POST', '/api/lots/', {'lot_type': 'trousx', 'name': 'Trousse 2'})
        # plusieurs lots : ne peut pas devenir un rangement (lot unique)
        code, _ = self.call('PATCH', '/api/lot-types/trousx/', {'storage': True})
        self.assertEqual(code, 400)
        Lots.objects.filter(name='Trousse 2').delete()
        code, body = self.call('PATCH', '/api/lot-types/trousx/', {'storage': True})
        self.assertEqual(code, 200)
        self.assertTrue(body['unique'])
        self.assertEqual(body['requirements'], [])
        trousse = Lots.objects.get(name='Trousse 1')
        self.assertIsNone(trousse.verif_key_expires)
        self.assertTrue(trousse.parent.is_stock)  # un rangement fait partie du stock
        # n'est plus un rangement : l'etiquette expire de nouveau, et le lot sort du stock
        self.call('PATCH', '/api/lot-types/trousx/', {'storage': False})
        trousse.refresh_from_db()
        self.assertIsNotNone(trousse.verif_key_expires)
        self.assertIsNone(trousse.parent)

    def test_stock_groups_storage_lots(self):
        """Tous les rangements sont dans le stock (lot global, existe toujours) : on les verifie a la suite."""
        stock = Lots.stock()
        self.assertEqual(Lots.objects.filter(lot_type_id=STOCK_LOT_TYPE).count(), 1)
        self.assertIsNone(stock.verif_key_expires)
        code, body = self.call('POST', '/api/lot-types/', {'type': 'armoi1', 'name': 'Armoire 1', 'storage': True})
        armoire = body['created_lot']
        self.assertEqual(armoire['parent'], stock.id)
        code, body = self.call('POST', '/api/lot-types/', {'type': 'armoi2', 'name': 'Armoire 2', 'storage': True})
        armoire2 = body['created_lot']
        # le stock ne se cree pas, ne s'archive pas et reste un rangement
        code, _ = self.call('POST', '/api/lot-types/', {'type': STOCK_LOT_TYPE, 'name': 'Autre stock', 'storage': True})
        self.assertEqual(code, 409)
        code, _ = self.call('POST', '/api/lots/', {'lot_type': STOCK_LOT_TYPE, 'name': 'Stock 2'})
        self.assertEqual(code, 409)
        code, _ = self.call('PATCH', f'/api/lots/{stock.id}/update/', {'active': False})
        self.assertEqual(code, 400)
        code, _ = self.call('PATCH', f'/api/lot-types/{STOCK_LOT_TYPE}/', {'storage': False})
        self.assertEqual(code, 400)
        code, _ = self.call('PATCH', f'/api/lots/{stock.id}/update/', {'parent': armoire['id']})
        self.assertEqual(code, 400)
        # un rangement reste dans le stock, un lot ordinaire n'y entre pas
        code, body = self.call('PATCH', f"/api/lots/{armoire2['id']}/update/", {'parent': ''})
        self.assertEqual(body['parent'], stock.id)
        code, _ = self.call('PATCH', f"/api/lots/{armoire2['id']}/update/", {'parent': self.bplus['id']})
        self.assertEqual(code, 400)
        code, _ = self.call('PATCH', f"/api/lots/{self.bplus['id']}/update/", {'parent': armoire['id']})
        self.assertEqual(code, 400)
        # verif de deux rangements a la suite, chacun avec son etiquette
        first, second = self.fresh(2), self.fresh(1)
        locked = {'lots': [{'id': armoire['id'], 'key': armoire['verif_key']},
                           {'id': armoire2['id'], 'key': armoire2['verif_key']}]}
        code, report = self.call('POST', '/api/verifs/', {**locked, 'items': first + second, 'user': 'M001',
                                                          'places': {second[0]: armoire2['id']}})
        self.assertEqual(code, 200, report)
        self.assertEqual(Items.objects.get(iid=first[0]).location_id, armoire['id'])
        self.assertEqual(Items.objects.get(iid=second[0]).location_id, armoire2['id'])
        # verif partielle : un rangement n'est complet que si tous ses items sont scannes (ce sont ses attendus)
        code, report = self.call('POST', '/api/verifs/', {**locked, 'items': first[:1] + second, 'user': 'M001',
                                                          'partial': True})
        self.assertEqual({row['id'] for row in report['lots'] if row['verified']}, {armoire2['id']})
        self.assertEqual(report['missing'], [])
        self.assertEqual(Items.objects.get(iid=first[1]).missed_verifs, 0)


@override_settings(QRPROTEC={**settings.QRPROTEC, 'SMS_SYNC': True})
class SmsTests(ApiTestCase):
    def setUp(self):
        super().setUp()
        from . import notifications
        self.sent = []
        self._original = notifications.send_free_sms
        notifications.send_free_sms = lambda user, password, message: (self.sent.append((user, message)) or (True, 'Envoyé'))
        self.addCleanup(setattr, notifications, 'send_free_sms', self._original)
        self.call('POST', '/api/notifications/recipients/', {'name': 'A', 'user': 'u1', 'password': 'p1'})
        self.call('POST', '/api/notifications/recipients/', {'name': 'B', 'user': 'u2', 'password': 'p2'})

    def test_recipients_hide_password(self):
        code, body = self.call('GET', '/api/notifications/')
        self.assertEqual(len(body['recipients']), 2)
        self.assertNotIn('password', body['recipients'][0])
        self.assertTrue(body['recipients'][0]['has_password'])
        code, _ = self.call('GET', '/api/notifications/', local=False)
        self.assertEqual(code, 404)

    def test_disabled_sends_nothing(self):
        with self.captureOnCommitCallbacks(execute=True):
            self.create(self.compresses, self.today + timedelta(days=60), 3)
            self.call('GET', '/api/stock/')
        self.assertEqual(self.sent, [])

    def test_stock_low_once_per_crossing(self):
        self.call('PATCH', '/api/notifications/', {'enabled': True})
        with self.captureOnCommitCallbacks(execute=True):
            self.call('GET', '/api/stock/')
        # compresses 0/10 et garrot 0/2, un SMS par destinataire
        self.assertEqual(len(self.sent), 2)
        self.assertEqual({user for user, _ in self.sent}, {'u1', 'u2'})
        self.assertIn('Compresses 0/10', self.sent[0][1])
        self.sent.clear()
        with self.captureOnCommitCallbacks(execute=True):
            self.call('GET', '/api/stock/')
        self.assertEqual(self.sent, [])
        # retour au-dessus du minimum puis nouveau passage dessous
        with self.captureOnCommitCallbacks(execute=True):
            self.call('POST', '/api/items/batch/', {'type': 'garrot', 'count': 2, 'user': 'M001'})
        garrots = [item.iid for item in Items.objects.filter(pack__item_type=self.garrot)]
        with self.captureOnCommitCallbacks(execute=True):
            self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': garrots[:1], 'user': 'M001'})
        self.assertEqual(len(self.sent), 2)
        self.assertIn('Garrot 1/2', self.sent[0][1])

    def test_verif_problem_and_seal_broken(self):
        self.call('PATCH', '/api/notifications/', {'enabled': True, 'events': {'stock_low': False}})
        self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001', 'force': True})
        with self.captureOnCommitCallbacks(execute=True):
            self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [], 'user': 'M001'})
        messages = [message for user, message in self.sent if user == 'u1']
        self.assertEqual(len(messages), 2)
        self.assertTrue(any('scellé' in message for message in messages))
        self.assertTrue(any('manque 2 Compresses' in message for message in messages))

    def test_test_sms(self):
        with self.captureOnCommitCallbacks(execute=True):
            code, body = self.call('POST', '/api/notifications/test/', {'user': 'M001'})
        self.assertEqual(body['sent'], 2)
        code, settings_body = self.call('GET', '/api/notifications/')
        self.assertEqual(settings_body['recipients'][0]['last_status'], 'Envoyé')


    def test_pin_blocked_notified_once_per_block(self):
        self.call('PATCH', '/api/notifications/', {'enabled': True})
        self.call('PATCH', '/api/users/M001/', {'pin': '123456', 'pin_contact': 'P001'})
        self.user.refresh_from_db()
        badge = {'matricule': 'M001', 'key': self.user.new_key}
        with self.captureOnCommitCallbacks(execute=True):
            for _ in range(5):  # demandes repetees : une seule notification
                code, body = self.call('POST', '/api/pin-forgot/', badge, local=False)
                self.call('POST', '/api/auth/', {**badge, 'pin': '123456'}, local=False)
        self.assertTrue(body['pin_reset']['notified'])
        self.assertEqual(len(self.sent), 2)  # un SMS par destinataire
        self.assertIn('Jeanne Dupont (M001) a oublié son code PIN (contact : Admin Poste)', self.sent[0][1])
        self.assertIn('/pinreset?m=M001&t=', self.sent[0][1])
        # evenement desactivable
        self.call('PATCH', '/api/users/M001/', {'pin': '123456'})  # nouveau PIN : debloque
        self.call('PATCH', '/api/notifications/', {'events': {'pin_blocked': False}})
        self.sent.clear()
        with self.captureOnCommitCallbacks(execute=True):
            code, body = self.call('POST', '/api/pin-forgot/', badge, local=False)
        self.assertEqual(self.sent, [])
        self.assertFalse(body['pin_reset']['notified'])
        # active ensuite : la tentative suivante previent les admins, une fois
        self.call('PATCH', '/api/notifications/', {'events': {'pin_blocked': True}})
        with self.captureOnCommitCallbacks(execute=True):
            for _ in range(3):
                self.call('POST', '/api/auth/', {**badge, 'pin': '123456'}, local=False)
        self.assertEqual(len(self.sent), 2)

    def test_pin_blocked_after_failures_notifies(self):
        self.call('PATCH', '/api/notifications/', {'enabled': True})
        self.call('PATCH', '/api/users/M001/', {'pin': '123456'})
        Secouristes.objects.filter(pk=self.user.pk).update(pin_failures_total=49)
        with self.captureOnCommitCallbacks(execute=True):
            code, body = self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.new_key, 'pin': '0000'},
                                   local=False)
        self.assertTrue(body['pin_blocked'])
        self.assertFalse(body['pin_reset']['forgotten'])
        self.assertEqual(len(self.sent), 2)
        self.assertIn("bloqué après trop d'essais", self.sent[0][1])

    def test_key_renewals(self):
        self.call('PATCH', '/api/notifications/', {'enabled': True, 'events': {'stock_low': False}})
        with self.captureOnCommitCallbacks(execute=True):
            self.call('POST', f'/api/lots/{self.lot.id}/rotate-key/')
            self.call('POST', '/api/users/M001/renew-key/')
        self.assertEqual(self.sent, [])  # desactives par defaut
        self.call('PATCH', '/api/notifications/', {'events': {'lot_key_renewed': True, 'badge_renewed': True}})
        with self.captureOnCommitCallbacks(execute=True):
            self.assertEqual(self.call('POST', f'/api/lots/{self.lot.id}/rotate-key/')[0], 200)
            self.assertEqual(self.call('POST', '/api/users/M001/renew-key/')[0], 200)
        messages = [message for user, message in self.sent if user == 'u1']
        self.assertEqual(messages, ['QRProtec : étiquette clé du lot Sac A renouvelée par Admin Poste',
                                    'QRProtec : badge de Jeanne Dupont (M001) renouvelé par Admin Poste'])

    def test_key_expirations_once_per_stage(self):
        from django.core.management import call_command
        self.call('PATCH', '/api/notifications/', {
            'enabled': True, 'events': {'stock_low': False, 'lot_key_expiring': True, 'badge_expiring': True}})
        Lots.objects.filter(pk=self.lot.pk).update(verif_key_expires=self.today + timedelta(days=10))
        Secouristes.objects.filter(pk=self.user.pk).update(key_expires=self.today - timedelta(days=1))
        with self.captureOnCommitCallbacks(execute=True):
            call_command('check_alerts', stdout=StringIO())
        messages = [message for user, message in self.sent if user == 'u1']
        day = (self.today + timedelta(days=10)).strftime('%d/%m/%Y')
        self.assertEqual(len(messages), 2)
        self.assertIn(f'étiquettes clés de lot qui expirent bientôt : Sac A ({day})', messages[0])
        self.assertIn('badges expirés : Jeanne Dupont (M001)', messages[1])
        # deja annonce : rien de plus
        self.sent.clear()
        with self.captureOnCommitCallbacks(execute=True):
            call_command('check_alerts', stdout=StringIO())
        self.assertEqual(self.sent, [])
        # l'etiquette expire : nouvelle etape, une alerte
        Lots.objects.filter(pk=self.lot.pk).update(verif_key_expires=self.today - timedelta(days=1))
        with self.captureOnCommitCallbacks(execute=True):
            call_command('check_alerts', stdout=StringIO())
        self.assertEqual(len(self.sent), 2)
        self.assertIn('étiquettes clés de lot expirées : Sac A', self.sent[0][1])
        # renouvellement : l'alerte reviendra a la prochaine expiration
        self.call('POST', f'/api/lots/{self.lot.id}/rotate-key/')
        self.lot.refresh_from_db()
        self.assertEqual(self.lot.key_expiry_stage, 0)

    def test_order_due_once_per_order(self):
        from django.core.management import call_command
        self.call('PATCH', '/api/notifications/', {'enabled': True, 'events': {'stock_low': False, 'order_due': True}})
        self.compresses.min_quantity = 5
        self.compresses.save()
        # 30 par mois consommes, 12 en stock : sous le minimum dans moins de 15 jours (delai de commande)
        item = self.create(self.compresses, self.today + timedelta(days=300), 1)[0]
        Items.objects.filter(pk=item.pk).update(status=ItemStatus.DELETED)
        ItemMovement.objects.bulk_create([
            ItemMovement(item=item, item_type=self.compresses, kind=MovementKind.USED,
                         at=timezone.now() - timedelta(hours=1 + index)) for index in range(30)])
        Verifs.objects.create(lot=self.lot, datetime=timezone.now() - timedelta(days=30), by='test')
        self.create(self.compresses, self.today + timedelta(days=300), 11)

        def run():
            self.sent.clear()
            with self.captureOnCommitCallbacks(execute=True):
                call_command('check_alerts', stdout=StringIO())
            return [message for user, message in self.sent if user == 'u1']

        messages = run()
        self.assertEqual(len(messages), 1)
        self.assertIn('commande à passer : Compresses : ', messages[0])
        self.assertTrue(ItemType.objects.get(pk='compre').order_notified)
        self.assertEqual(run(), [])  # deja annoncee
        # stock recu : plus de commande due, l'alerte reviendra a la prochaine commande
        self.create(self.compresses, self.today + timedelta(days=300), 200)
        self.assertEqual(run(), [])
        self.assertFalse(ItemType.objects.get(pk='compre').order_notified)

    def test_key_expiry_warning_days_setting(self):
        from django.core.management import call_command
        code, body = self.call('GET', '/api/notifications/')
        self.assertEqual(body['key_expiry_warning_days'], 30)
        self.assertEqual(self.call('PATCH', '/api/notifications/', {'key_expiry_warning_days': 0})[0], 400)
        code, body = self.call('PATCH', '/api/notifications/', {
            'enabled': True, 'key_expiry_warning_days': 5, 'events': {'stock_low': False, 'lot_key_expiring': True}})
        self.assertEqual(body['key_expiry_warning_days'], 5)
        Lots.objects.filter(pk=self.lot.pk).update(verif_key_expires=self.today + timedelta(days=10))
        with self.captureOnCommitCallbacks(execute=True):
            call_command('check_alerts', stdout=StringIO())
        self.assertEqual(self.sent, [])  # 10 jours > 5 : pas encore
        self.call('PATCH', '/api/notifications/', {'key_expiry_warning_days': 15})
        with self.captureOnCommitCallbacks(execute=True):
            call_command('check_alerts', stdout=StringIO())
        self.assertEqual(len(self.sent), 2)
        # vide : retour a la valeur de qrprotec.conf
        code, body = self.call('PATCH', '/api/notifications/', {'key_expiry_warning_days': None})
        self.assertEqual(body['key_expiry_warning_days'], 30)

@override_settings(QRPROTEC={**settings.QRPROTEC, 'SMS_SYNC': True})
class WebPushTests(ApiTestCase):
    """Notifications web : chiffrement RFC 8291, VAPID, abonnement des admins et alertes de stock."""

    def setUp(self):
        super().setUp()
        from cryptography.hazmat.primitives.asymmetric import ec

        from . import views, webpush
        self.webpush = webpush
        self.sent = []   # (endpoint, corps chiffre, en-tetes)
        self.reply = 201
        original = webpush.post_push
        webpush.post_push = lambda endpoint, body, headers: (self.sent.append((endpoint, body, headers)) or self.reply)
        self.addCleanup(setattr, webpush, 'post_push', original)
        self.admin = Secouristes(matricule='A001', nom='Ad', prenom='Min', role='admin')
        self.admin.renew_key()
        self.admin.set_pin('4821')
        self.admin.save()
        self.badge = {'matricule': 'A001', 'key': self.admin.new_key, 'session': views.session_token(self.admin)}
        self.browser_key = ec.generate_private_key(ec.SECP256R1())
        self.browser_auth = webpush.b64url(b'0123456789abcdef')

    def subscription(self, endpoint='https://push.example.net/abc'):
        return {'endpoint': endpoint, 'keys': {
            'p256dh': self.webpush.b64url(self.webpush._public_point(self.browser_key)), 'auth': self.browser_auth}}

    def subscribe(self, **prefs):
        return self.call('POST', '/api/push/subscription/',
                         {'user': self.badge, 'subscription': self.subscription(), **prefs}, local=False)

    def decrypt(self, body):
        """Dechiffrement cote navigateur (RFC 8291) pour verifier les messages envoyes."""
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
        hkdf = self.webpush._hkdf
        salt, key_length = body[:16], body[20]
        sender_point = body[21:21 + key_length]
        sender = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), sender_point)
        shared = self.browser_key.exchange(ec.ECDH(), sender)
        receiver_point = self.webpush._public_point(self.browser_key)
        ikm = hkdf(self.webpush.b64url_decode(self.browser_auth), shared,
                   b'WebPush: info\x00' + receiver_point + sender_point, 32)
        key = hkdf(salt, ikm, b'Content-Encoding: aes128gcm\x00', 16)
        nonce = hkdf(salt, ikm, b'Content-Encoding: nonce\x00', 12)
        plain = AESGCM(key).decrypt(nonce, body[21 + key_length:], None)
        self.assertEqual(plain[-1:], b'\x02')
        return json.loads(plain[:-1])

    def messages(self):
        return [self.decrypt(body) for _, body, _ in self.sent]

    def test_rfc8291_example(self):
        from cryptography.hazmat.primitives.asymmetric import ec
        webpush = self.webpush

        def private(text):
            return ec.derive_private_key(int.from_bytes(webpush.b64url_decode(text), 'big'), ec.SECP256R1())
        receiver = private('q1dXpw3UpT5VOmu_cf_v6ih07Aems3njxI-JWgLcM94')
        body = webpush.encrypt(b'When I grow up, I want to be a watermelon',
                               webpush.b64url(webpush._public_point(receiver)), 'BTBZMqHH6r4Tts7J_aSIgg',
                               salt=webpush.b64url_decode('DGv6ra1nlYgDCS1FRnbzlw'),
                               sender_key=private('yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw'))
        self.assertEqual(webpush.b64url(body),
                         'DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6'
                         'TlzAC8wEqKK6PBru3jl7A_yl95bQpu6cVPTpK4Mqgkf1CXztLVBSt2Ks3oZwbuwXPXLWyouBWLVWGNWQexSgSxsj_Qul'
                         'cy4a-fN')

    def test_vapid_token_is_signed_by_the_published_key(self):
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
        webpush = self.webpush
        code, body = self.call('GET', '/api/push/key/', local=False)
        self.assertEqual(code, 200)
        self.assertEqual(self.call('GET', '/api/push/key/', local=False)[1], body)  # cle stable
        authorization = webpush.vapid_authorization('https://push.example.net/abc/def', now=1000)
        token, key = authorization.removeprefix('vapid t=').split(', k=')
        self.assertEqual(key, body['public_key'])
        header, claims, signature = token.split('.')
        self.assertEqual(json.loads(webpush.b64url_decode(claims)),
                         {'aud': 'https://push.example.net', 'exp': 1000 + webpush.JWT_VALIDITY, 'sub': webpush.subject()})
        public = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), webpush.b64url_decode(key))
        raw = webpush.b64url_decode(signature)
        der = encode_dss_signature(int.from_bytes(raw[:32], 'big'), int.from_bytes(raw[32:], 'big'))
        public.verify(der, f'{header}.{claims}'.encode(), ec.ECDSA(hashes.SHA256()))  # leve si invalide

    def test_gestion_and_admins_subscribe(self):
        code, body = self.call('POST', '/api/push/subscription/',
                               {'user': {'matricule': 'M001', 'key': self.user.new_key}, 'subscription': self.subscription()},
                               local=False)
        self.assertEqual((code, body['error']), (403, 'Réservé aux rôles gestion et admin'))
        # sans jeton de session (PIN), refuse aussi
        code, _ = self.call('POST', '/api/push/subscription/',
                            {'user': {'matricule': 'A001', 'key': self.admin.new_key}, 'subscription': self.subscription()},
                            local=False)
        self.assertEqual(code, 403)
        code, _ = self.call('POST', '/api/push/subscription/',
                            {'user': self.badge, 'subscription': self.subscription('http://insecure/')}, local=False)
        self.assertEqual(code, 400)

    def test_subscribe_status_and_unsubscribe(self):
        code, body = self.subscribe(stock_low=False, stock_empty=True)
        self.assertEqual(code, 200)
        self.assertEqual((body['subscribed'], body['stock_low'], body['stock_empty']), (True, False, True))
        endpoint = self.subscription()['endpoint']
        code, body = self.call('POST', '/api/push/subscription/', {'user': self.badge, 'endpoint': endpoint}, local=False)
        self.assertEqual((body['subscribed'], body['stock_low']), (True, False))
        code, body = self.subscribe(stock_low=True, stock_empty=True)  # mise a jour, pas de doublon
        self.assertEqual(self.webpush.PushSubscription.objects.count(), 1)
        code, body = self.call('POST', '/api/push/unsubscribe/', {'endpoint': endpoint}, local=False)
        self.assertTrue(body['deleted'])
        code, body = self.call('POST', '/api/push/subscription/', {'user': self.badge, 'endpoint': endpoint}, local=False)
        self.assertFalse(body['subscribed'])
        self.assertEqual([row['type'] for row in body['types']][:3], ['stock_low', 'stock_empty', 'pin_blocked'])

    def test_test_notification(self):
        self.subscribe()
        with self.captureOnCommitCallbacks(execute=True):
            code, _ = self.call('POST', '/api/push/test/', {'user': self.badge, 'endpoint': self.subscription()['endpoint']},
                                local=False)
        self.assertEqual(code, 200)
        endpoint, _, headers = self.sent[0]
        self.assertEqual(endpoint, 'https://push.example.net/abc')
        self.assertEqual(headers['Content-Encoding'], 'aes128gcm')
        self.assertTrue(headers['Authorization'].startswith('vapid t='))
        self.assertEqual(self.messages()[0]['title'], 'QRProtec : test')

    def test_stock_low_and_empty_once_per_crossing(self):
        self.subscribe()
        self.create(self.garrot, None, 1)  # garrot 1/2 : bas mais pas vide ; compresses 0/10 : vide
        with self.captureOnCommitCallbacks(execute=True):
            self.call('GET', '/api/stock/')
        [message] = self.messages()
        self.assertEqual(message['tag'], 'stock-empty')
        self.assertEqual(message['body'], 'Stock vide : Compresses\nStock bas : Garrot 1/2')
        self.sent.clear()
        with self.captureOnCommitCallbacks(execute=True):
            self.call('GET', '/api/stock/')
        self.assertEqual(self.sent, [])
        # le dernier garrot part dans un lot : stock vide (deja signale bas)
        garrot = Items.objects.get(pack__item_type=self.garrot)
        with self.captureOnCommitCallbacks(execute=True):
            self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [garrot.iid], 'user': 'M001'})
        self.assertEqual([m['body'] for m in self.messages()], ['Stock vide : Garrot'])

    def test_preferences_and_expired_subscription(self):
        self.subscribe(stock_low=True, stock_empty=False)
        with self.captureOnCommitCallbacks(execute=True):
            self.call('GET', '/api/stock/')
        self.assertEqual([m['body'] for m in self.messages()], ['Stock bas : Compresses 0/10, Garrot 0/2'])
        # abonnement annule par le navigateur : supprime au premier envoi refuse
        self.sent.clear()
        self.reply = 410
        ItemType.objects.update(low_notified=False)
        with self.captureOnCommitCallbacks(execute=True):
            self.call('GET', '/api/stock/')
        self.assertEqual(len(self.sent), 1)
        self.assertFalse(self.webpush.PushSubscription.objects.exists())

    def test_demoted_admin_receives_nothing(self):
        self.subscribe()
        Secouristes.objects.filter(matricule='A001').update(role='normal')
        with self.captureOnCommitCallbacks(execute=True):
            self.call('GET', '/api/stock/')
        self.assertEqual(self.sent, [])

    def test_gestion_receives_only_its_types(self):
        self.subscribe(badge_renewed=True, lot_key_renewed=True)
        Secouristes.objects.filter(matricule='A001').update(role='gestion')
        with self.captureOnCommitCallbacks(execute=True):
            self.call('POST', '/api/users/M001/renew-key/')  # reserve aux admins
            self.call('POST', f'/api/lots/{self.lot.id}/rotate-key/')
        self.assertEqual([message['title'] for message in self.messages()], ['QRProtec : étiquette de lot renouvelée'])

    def test_device_manager(self):
        self.subscribe()
        endpoint = self.subscription()['endpoint']
        other = self.webpush.PushSubscription.objects.create(
            user=self.admin, endpoint='https://push.example.net/other', p256dh='x', auth='y', device='Firefox · Linux')
        code, body = self.call('POST', '/api/push/devices/', {'user': self.badge, 'endpoint': endpoint}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual([(row['device'], row['current']) for row in body['devices']],
                         [('Firefox · Linux', False), ('Navigateur', True)])
        self.assertEqual(len(body['types']), 9)
        # alertes d'un autre appareil, puis desabonnement a distance
        code, body = self.call('POST', f'/api/push/devices/{other.id}/', {'user': self.badge, 'stock_low': False},
                               local=False)
        self.assertFalse(body['devices'][0]['stock_low'])
        code, body = self.call('POST', f'/api/push/devices/{other.id}/', {'user': self.badge, 'delete': True},
                               local=False)
        self.assertEqual(len(body['devices']), 1)
        # l'appareil d'un autre utilisateur est introuvable
        mine = self.webpush.PushSubscription.objects.get(endpoint=endpoint)
        gestion = Secouristes(matricule='G001', nom='Ges', prenom='Tion', role='gestion')
        gestion.renew_key()
        gestion.save()
        badge = {'matricule': 'G001', 'key': gestion.new_key, 'session': views.session_token(gestion)}
        code, _ = self.call('POST', f'/api/push/devices/{mine.id}/', {'user': badge, 'delete': True}, local=False)
        self.assertEqual(code, 404)
        code, body = self.call('POST', '/api/push/devices/', {'user': badge}, local=False)
        self.assertEqual((body['devices'], len(body['types'])), ([], 5))

    def test_admin_disables_types_per_user(self):
        self.subscribe()
        code, users = self.call('GET', '/api/users/')
        row = next(user for user in users if user['matricule'] == 'A001')
        self.assertEqual(row['push_devices'], 1)
        self.assertTrue(all(entry['enabled'] for entry in row['push_types']))
        self.assertEqual(self.call('PATCH', '/api/users/A001/', {'push_disabled': ['inconnu']})[0], 400)
        code, row = self.call('PATCH', '/api/users/A001/', {'push_disabled': ['stock_low', 'stock_empty']})
        self.assertEqual([entry['type'] for entry in row['push_types'] if not entry['enabled']], ['stock_low', 'stock_empty'])
        with self.captureOnCommitCallbacks(execute=True):
            self.call('GET', '/api/stock/')
        self.assertEqual(self.sent, [])
        # l'utilisateur ne voit plus ces types dans son gestionnaire
        code, body = self.call('POST', '/api/push/devices/', {'user': self.badge}, local=False)
        self.assertNotIn('stock_low', [entry['type'] for entry in body['types']])
        self.call('PATCH', '/api/users/A001/', {'push_disabled': []})
        ItemType.objects.update(low_notified=False, empty_notified=False)  # nouveau passage sous le seuil
        with self.captureOnCommitCallbacks(execute=True):
            self.call('GET', '/api/stock/')
        self.assertEqual(len(self.sent), 1)

    def test_device_name(self):
        name = self.webpush.device_name
        self.assertEqual(name('Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 Chrome/129.0 Mobile Safari/537.36'),
                         'Chrome · Android')
        self.assertEqual(name('Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) AppleWebKit/605.1.15 Version/17.5 '
                              'Mobile/15E148 Safari/604.1'), 'Safari · iPhone')
        self.assertEqual(name(''), 'Navigateur')

    def test_pin_blocked(self):
        code, body = self.subscribe(stock_low=False, stock_empty=False)
        self.assertTrue(body['pin_blocked'])
        self.call('PATCH', '/api/users/M001/', {'pin': '123456'})
        self.user.refresh_from_db()
        badge = {'matricule': 'M001', 'key': self.user.new_key}
        with self.captureOnCommitCallbacks(execute=True):
            code, body = self.call('POST', '/api/pin-forgot/', badge, local=False)
            self.call('POST', '/api/pin-forgot/', badge, local=False)
        self.assertTrue(body['pin_reset']['notified'])
        [message] = self.messages()
        self.assertEqual(message['title'], 'QRProtec : PIN bloqué')
        self.assertEqual((message['url'], message['tab']), (body['pin_reset']['url'], 'pinreset'))
        self.assertIn('Jeanne Dupont (M001) a oublié son code PIN', message['body'])
        # desactive pour ce navigateur
        self.call('PATCH', '/api/users/M001/', {'pin': '123456'})  # nouveau PIN : debloque
        self.subscribe(stock_low=True, pin_blocked=False)
        self.sent.clear()
        with self.captureOnCommitCallbacks(execute=True):
            self.call('POST', '/api/pin-forgot/', badge, local=False)
        self.assertEqual(self.sent, [])

    def test_key_renewed(self):
        code, body = self.subscribe()
        self.assertFalse(body['lot_key_renewed'])
        with self.captureOnCommitCallbacks(execute=True):
            self.call('POST', f'/api/lots/{self.lot.id}/rotate-key/')
        self.assertEqual(self.sent, [])
        code, body = self.subscribe(stock_low=False, stock_empty=False, pin_blocked=False, lot_key_renewed=True)
        self.assertTrue(body['lot_key_renewed'])
        with self.captureOnCommitCallbacks(execute=True):
            self.call('POST', f'/api/lots/{self.lot.id}/rotate-key/')
        [message] = self.messages()
        self.assertEqual(message['title'], 'QRProtec : étiquette de lot renouvelée')
        self.assertIn('Sac A', message['body'])


class RemoteScannerTests(TestCase):
    """Telephone-douchette : vrai serveur `serve` (deux ports) et clients WebSocket bruts."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        import threading
        from django.core.servers.basehttp import WSGIServer, get_internal_wsgi_application
        from .management.commands.serve import run_server
        from .middleware import RoleWSGIHandler
        from .remote_scanner import hub
        cls.hub = hub
        hub.enabled = True
        application = get_internal_wsgi_application()
        cls.ports = {}
        for role in ('public', 'local'):
            port = cls._free_port()
            cls.ports[role] = port
            threading.Thread(target=run_server, daemon=True,
                             args=('127.0.0.1', port, RoleWSGIHandler(application, role), WSGIServer, role)).start()
        import time
        time.sleep(0.3)

    @staticmethod
    def _free_port():
        import socket
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            return sock.getsockname()[1]

    def http(self, role, method, path, body=None):
        import http.client
        connection = http.client.HTTPConnection('127.0.0.1', self.ports[role], timeout=5)
        connection.request(method, path, json.dumps(body) if body is not None else None,
                           {'Content-Type': 'application/json'})
        response = connection.getresponse()
        data = response.read()
        is_json = response.getheader('Content-Type', '').startswith('application/json')
        return response.status, (json.loads(data) if is_json and data else None)

    def ws(self, role, path):
        return RawWebSocket('127.0.0.1', self.ports[role], path)

    def create_session(self, minutes=5):
        code, body = self.http('local', 'POST', '/api/remote-scanner/', {'timeout_minutes': minutes})
        self.assertEqual(code, 201)
        from urllib.parse import parse_qs, urlsplit
        query = parse_qs(urlsplit(body['url']).query)
        return body, query['s'][0], query['k'][0]

    def test_relay_between_phone_and_front(self):
        body, session_id, key = self.create_session()
        self.assertTrue(body['url'].startswith('https://example.com/scanner?'))
        front = self.ws('local', f'/ws/scanner/front?s={session_id}')
        self.assertEqual(front.receive()['type'], 'hello')
        phone = self.ws('public', f'/ws/scanner/phone?s={session_id}&k={key}')
        hello = phone.receive()
        self.assertEqual(hello, {**hello, 'type': 'hello', 'front_connected': True})
        self.assertEqual(front.receive(), {**front.last, 'type': 'phone', 'connected': True})
        phone.send({'type': 'scan', 'code': 'compre20271231000000A1', 'id': 7})
        self.assertEqual(front.receive(), {'type': 'scan', 'code': 'compre20271231000000A1', 'id': 7})
        self.assertEqual(phone.receive()['ok'], True)
        front.send({'type': 'feedback', 'result': 'error', 'message': 'PÉRIMÉ'})
        self.assertEqual(phone.receive(), {'type': 'feedback', 'result': 'error', 'message': 'PÉRIMÉ'})
        # fermeture depuis le poste : le telephone est prevenu, la cle ne marche plus
        front.send({'type': 'close'})
        self.assertEqual(phone.receive()['type'], 'closed')
        code, _ = self.http('public', 'GET', f'/api/remote-scanner/check/?s={session_id}&k={key}')
        self.assertEqual(code, 404)

    def test_access_rules(self):
        body, session_id, key = self.create_session()
        # mauvaise cle, route du poste sur l'API publique, creation sur l'API publique
        self.assertEqual(self.ws('public', f'/ws/scanner/phone?s={session_id}&k=faux').status, 404)
        self.assertEqual(self.ws('public', f'/ws/scanner/front?s={session_id}').status, 404)
        code, _ = self.http('public', 'POST', '/api/remote-scanner/', {})
        self.assertEqual(code, 404)
        code, check = self.http('public', 'GET', f'/api/remote-scanner/check/?s={session_id}&k={key}')
        self.assertEqual(code, 200)
        # les requetes HTTP ordinaires passent toujours
        code, health = self.http('public', 'GET', '/api/health/')
        self.assertTrue(health['remote_scanner'])

    def test_session_expires_after_disconnection(self):
        import time
        body, session_id, key = self.create_session(minutes=1)
        front = self.ws('local', f'/ws/scanner/front?s={session_id}')
        front.receive()
        phone = self.ws('public', f'/ws/scanner/phone?s={session_id}&k={key}')
        phone.receive()
        front.receive()
        phone.close()
        self.assertEqual(front.receive(), {**front.last, 'type': 'phone', 'connected': False})
        session = self.hub.get(session_id)
        self.assertEqual(self.hub.reap(time.monotonic() + 30), [])       # encore dans le delai
        self.assertEqual(self.hub.reap(session.phone_lost + 61), [session_id])
        self.assertEqual(front.receive()['type'], 'closed')
        self.assertEqual(self.ws('public', f'/ws/scanner/phone?s={session_id}&k={key}').status, 404)


class VerifPlacesTests(ApiTestCase):
    """Verif de sous-lots : un item venu du stock va dans le lot dont l'etiquette a ete scannee en dernier."""

    def setUp(self):
        super().setUp()
        vps_type = LotType.objects.create(type='vpsvps', name='VPS')
        self.vps = Lots(lot_type=vps_type, name='VPS', name_short='V', created_by='test')
        self.vps.save()
        self.armoire1 = Lots(lot_type=self.lot_type, name='Armoire 1', name_short='A1', created_by='test', parent=self.vps)
        self.armoire1.save()
        self.armoire2 = Lots(lot_type=self.lot_type, name='Armoire 2', name_short='A2', created_by='test', parent=self.vps)
        self.armoire2.save()

    def location(self, item):
        return Items.objects.get(iid=item.iid).location_id

    def test_stock_item_goes_to_last_scanned_lot(self):
        stock = self.create(self.compresses, self.today + timedelta(days=100), 2)
        code, _ = self.call('POST', '/api/verifs/', {
            'lots': [{'id': self.vps.id}], 'items': [item.iid for item in stock],
            'places': {stock[0].iid: self.armoire2.id, stock[1].iid: self.vps.id}})
        self.assertEqual(code, 200)
        # armoire 1 attendait aussi des compresses, mais c'est l'armoire 2 qui a ete scannee
        self.assertEqual(self.location(stock[0]), self.armoire2.id)
        # etiquette du VPS : un de ses sous-lots qui en attend encore
        self.assertEqual(self.location(stock[1]), self.armoire1.id)

    def test_item_of_a_sibling_lot_moves_unless_its_label_was_scanned(self):
        found = self.create(self.compresses, self.today + timedelta(days=100), 2)
        self.call('POST', f'/api/lots/{self.armoire2.id}/add/', {'items': [item.iid for item in found]})
        # vérif de l'armoire 1 seule : l'item de l'armoire 2 y est deplace
        self.call('POST', '/api/verifs/', {'lots': [{'id': self.armoire1.id}], 'items': [found[0].iid],
                                           'places': {found[0].iid: self.armoire1.id}})
        self.assertEqual(self.location(found[0]), self.armoire1.id)
        # armoire 2 ajoutee a la vérif (son etiquette scannee) : son item y a bien ete trouve
        self.call('POST', '/api/verifs/', {'lots': [{'id': self.armoire1.id}, {'id': self.armoire2.id}],
                                           'items': [found[1].iid], 'places': {found[1].iid: self.armoire1.id}})
        self.assertEqual(self.location(found[1]), self.armoire2.id)


class RecentVerifsTests(ApiTestCase):
    def test_recent_verifs_need_a_badge_and_group_lots(self):
        sub = Lots(lot_type=self.lot_type, name='Sac A1', name_short='A1', created_by='test', parent=self.lot)
        sub.save()
        items = self.create(self.compresses, self.today + timedelta(days=200), 4)
        self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [item.iid for item in items[:2]]})
        self.call('POST', f'/api/lots/{sub.id}/add/', {'items': [item.iid for item in items[2:]]})
        self.call('POST', '/api/verifs/', {'lots': [{'id': self.lot.id}], 'items': [item.iid for item in items]})
        self.call('POST', f'/api/lots/{sub.id}/verif/', {'items': [items[2].iid]})
        code, _ = self.call('POST', '/api/verifs/recent/', {}, local=False)
        self.assertEqual(code, 403)
        badge = {'matricule': self.user.matricule, 'key': self.user.new_key}
        code, rows = self.call('POST', '/api/verifs/recent/', {'user': badge}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(len(rows), 2)
        latest, grouped = rows
        self.assertEqual([lot['id'] for lot in latest['lots']], [sub.id])
        self.assertFalse(latest['complete'])
        # verif groupee (lot et sous-lot au meme instant) : une seule ligne
        self.assertEqual({lot['id'] for lot in grouped['lots']}, {self.lot.id, sub.id})
        self.assertEqual(grouped['present'], 4)
        self.assertEqual(grouped['by'], 'Admin')  # nom court de l'API publique (prenom seul, pas d'homonyme)

    def test_live_names_are_anonymized(self):
        from .live import public_name
        Secouristes.objects.create(matricule='M002', nom='Durand', prenom='Jeanne')
        self.assertEqual(public_name('M:M001'), 'Jeanne Dup.')
        self.assertEqual(public_name('D:Paul Martin'), 'Paul M.')


class FakeSocket:
    """WebSocket factice pour LiveHub : messages a recevoir (file), messages envoyes (liste)."""

    def __init__(self, *incoming):
        import queue
        self.incoming = queue.Queue()
        for message in incoming:
            self.push(message)
        self.sent = []
        self.closed = False

    def push(self, message):
        self.incoming.put(json.dumps(message) if message is not None else None)

    def receive(self):
        from .remote_scanner import WebSocketClosed
        message = self.incoming.get(timeout=5)
        if message is None:
            raise WebSocketClosed('fin')
        return message

    def send_json(self, message):
        self.sent.append(message)
        return True

    def close(self, code=1000):
        self.closed = True


class LiveHubTests(ApiTestCase):
    """Suivi des vérifs en direct : publication par la page de scan, diffusion aux pages de suivi."""

    def setUp(self):
        super().setUp()
        from unittest import mock
        from . import live
        self.live = live
        self.hub = live.LiveHub()
        # les connexions tournent dans des threads : le badge est verifie sans la base de test
        self.users = users = {self.user.matricule: self.user.new_key}
        patcher = mock.patch.object(live, 'badge_identity', side_effect=lambda credentials: (
            f'M:{credentials["matricule"]}' if isinstance(credentials, dict)
            and users.get(credentials.get('matricule')) == credentials.get('key') else None))
        patcher.start()
        self.addCleanup(patcher.stop)
        names = mock.patch.object(live, 'public_name', side_effect=lambda identity: {'M:M001': 'Jeanne', 'M:M002': 'Paul'}
                                  .get(identity, identity.partition(':')[2]))
        names.start()
        self.addCleanup(names.stop)
        self.badge = {'matricule': 'M001', 'key': self.user.new_key}

    def run_socket(self, target, socket):
        import threading
        thread = threading.Thread(target=target, args=(socket,), daemon=True)
        thread.start()
        return thread

    def wait(self, condition):
        import time
        for _ in range(200):
            if condition():
                return
            time.sleep(0.01)
        self.fail('condition jamais remplie')

    def test_watcher_needs_a_badge(self):
        socket = FakeSocket({'type': 'hello', 'user': {'matricule': 'M001', 'key': 'faux'}})
        self.hub.run_watcher(socket)
        self.assertEqual(socket.sent[0]['type'], 'error')
        self.assertTrue(socket.sent[0]['login_required'])
        self.assertFalse(self.hub.watchers)

    def test_scans_are_relayed_live_then_done(self):
        watcher = FakeSocket({'type': 'hello', 'user': self.badge})
        watching = self.run_socket(self.hub.run_watcher, watcher)
        self.wait(lambda: watcher.sent)
        self.assertEqual(watcher.sent[0], {'type': 'snapshot', 'sessions': []})
        scanner = FakeSocket({'type': 'hello', 'user': self.badge})
        scanning = self.run_socket(self.hub.run_scanner, scanner)
        self.wait(lambda: scanner.sent)
        hello = scanner.sent[0]
        self.assertEqual(hello['type'], 'hello')
        scanner.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'lots': [],
                      'scanned': [{'name': 'Compresses', 'expired': True}], 'count': 1, 'remaining': 1})
        self.wait(lambda: any(m['type'] == 'update' for m in watcher.sent))
        update = next(m for m in watcher.sent if m['type'] == 'update')['session']
        self.assertEqual(update['who'], 'Jeanne')
        self.assertTrue(update['verified'])
        self.assertTrue(update['connected'])
        self.assertEqual(update['lot'], {'id': self.lot.id, 'name': 'Sac A'})
        self.assertEqual(update['scanned'], [{'name': 'Compresses', 'expired': True, 'error': False, 'extra': False}])
        self.assertNotIn('token', update)
        scanner.push({'type': 'done', 'complete': True, 'present': 2})
        self.wait(lambda: watcher.sent[-1]['type'] == 'update' and watcher.sent[-1]['session']['done'])
        self.assertEqual(watcher.sent[-1]['session']['done']['present'], 2)
        # page de scan fermee : la verif terminee reste affichee DONE_KEEP, puis disparait
        scanner.push(None)
        scanning.join(5)
        session = self.hub.sessions[hello['id']]
        self.assertEqual(self.hub.reap(session.lost + 60), [])
        self.assertEqual(self.hub.reap(session.lost + self.live.DONE_KEEP + 1), [hello['id']])
        self.wait(lambda: watcher.sent[-1] == {'type': 'remove', 'id': hello['id']})
        watcher.push(None)
        watching.join(5)
        self.assertFalse(self.hub.watchers)

    def test_reload_resumes_the_session(self):
        first = FakeSocket({'type': 'hello', 'name': 'Paul Martin'})
        thread = self.run_socket(self.hub.run_scanner, first)
        self.wait(lambda: first.sent)
        hello = first.sent[0]
        first.push(None)
        thread.join(5)
        second = FakeSocket({'type': 'hello', 'id': hello['id'], 'pid': hello['pid'], 'token': hello['token'],
                             'name': 'Paul Martin'})
        thread = self.run_socket(self.hub.run_scanner, second)
        self.wait(lambda: second.sent)
        self.assertEqual((second.sent[0]['id'], second.sent[0]['pid']), (hello['id'], hello['pid']))
        participant = self.hub.sessions[hello['id']].participants[hello['pid']]
        self.assertEqual((participant.who, participant.verified, participant.identity),
                         ('Paul Martin', False, 'D:Paul Martin'))
        # mauvais jeton : nouvelle session
        third = FakeSocket({'type': 'hello', 'id': hello['id'], 'pid': hello['pid'], 'token': 'faux'})
        other = self.run_socket(self.hub.run_scanner, third)
        self.wait(lambda: third.sent)
        self.assertNotEqual(third.sent[0]['id'], hello['id'])
        for socket, running in ((second, thread), (third, other)):
            socket.push(None)
            running.join(5)

    def join_team(self):
        """Jeanne commence la verif du lot, Paul (badge) la rejoint depuis la page de suivi."""
        from .models import Secouristes
        paul = Secouristes(matricule='M002', nom='Martin', prenom='Paul')
        paul.renew_key()
        paul.save()
        self.users['M002'] = paul.new_key
        self.paul = {'matricule': 'M002', 'key': paul.new_key}
        items = self.create(self.compresses, self.today + timedelta(days=200), 2)
        first = FakeSocket({'type': 'hello', 'user': self.badge})
        self.run_socket(self.hub.run_scanner, first)
        self.wait(lambda: first.sent)
        hello = first.sent[0]
        entry = {'code': items[0].iid, 'kind': 'item', 'iid': items[0].iid, 'info': {'type_name': 'Compresses'}}
        first.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'root': self.lot.id,
                    'scanned': [{'name': 'Compresses'}], 'count': 1, 'remaining': 1, 'entries': [entry],
                    'keys': [{'id': self.lot.id, 'key': self.lot.verif_key}]})
        self.wait(lambda: self.hub.sessions[hello['id']].state)
        # nom declare : on ne rejoint pas une verif
        stranger = FakeSocket({'type': 'hello', 'join': hello['id'], 'name': 'Inconnu'})
        self.hub.run_scanner(stranger)
        self.assertEqual(stranger.sent[0]['type'], 'error')
        second = FakeSocket({'type': 'hello', 'join': hello['id'], 'user': self.paul})
        self.run_socket(self.hub.run_scanner, second)
        self.wait(lambda: any(m['type'] == 'team' for m in second.sent))
        joined = second.sent[0]
        self.assertEqual(joined['id'], hello['id'])
        team = next(m for m in second.sent if m['type'] == 'team')
        self.assertEqual(team['lots'], [self.lot.id])
        self.assertEqual(team['keyed'], [self.lot.id])
        self.assertNotIn(self.lot.verif_key, json.dumps(second.sent))  # la cle reste sur le serveur
        self.assertEqual([e['iid'] for e in team['entries']], [items[0].iid])
        self.assertEqual(team['entries'][0]['who'], 'Jeanne')
        self.assertEqual([p['who'] for p in team['participants']], ['Jeanne', 'Paul'])
        second.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'root': self.lot.id,
                     'entries': [{'code': items[1].iid, 'kind': 'item', 'iid': items[1].iid}], 'count': 2})
        self.wait(lambda: any(m['type'] == 'team' and m['entries'] for m in first.sent))
        self.wait(lambda: any(m['type'] == 'team' and len(m['participants']) == 2 and m['entries']
                              and m['entries'][-1]['iid'] == items[1].iid for m in first.sent))
        return first, second, hello, joined, items

    def test_team_check_is_sent_by_the_last_participant(self):
        from unittest import mock
        first, second, hello, joined, items = self.join_team()
        public = self.hub.visible()[0]
        self.assertEqual([p['who'] for p in public['participants']], ['Jeanne', 'Paul'])
        # Jeanne valide sa partie : Paul est encore connecte, rien n'est envoye
        first.push({'type': 'validate'})
        self.wait(lambda: any(m['type'] == 'validate' for m in first.sent))
        answer = next(m for m in first.sent if m['type'] == 'validate')
        self.assertEqual(answer, {'type': 'validate', 'submit': False, 'waiting': ['Paul']})
        # Paul valide en dernier : c'est lui qui envoie la verif, sans la cle du lot
        second.push({'type': 'validate'})
        self.wait(lambda: any(m['type'] == 'validate' for m in second.sent))
        self.assertTrue(next(m for m in second.sent if m['type'] == 'validate')['submit'])
        with mock.patch.object(self.live, 'hub', self.hub):
            code, report = self.call('POST', '/api/verifs/', {
                'lots': [{'id': self.lot.id}], 'items': [item.iid for item in items], 'user': self.paul,
                'live': {'id': joined['id'], 'pid': joined['pid'], 'token': joined['token']}}, local=False)
        self.assertEqual(code, 200, report)
        self.assertTrue(report['complete'])
        self.assertEqual(report['participants'], ['Paul', 'Jeanne'])
        verif = Verifs.objects.get(lot=self.lot)
        self.assertEqual((verif.by, verif.participants), ('M:M002', ['M:M001']))
        # le compte rendu arrive chez Jeanne, la verif est terminee pour le suivi
        self.wait(lambda: any(m['type'] == 'report' for m in first.sent))
        self.assertTrue(next(m for m in first.sent if m['type'] == 'report')['report']['complete'])
        self.assertFalse(any(m['type'] == 'report' for m in second.sent))
        self.assertTrue(self.hub.sessions[hello['id']].done['complete'])
        code, rows = self.call('POST', '/api/verifs/recent/', {'user': self.badge}, local=False)
        self.assertEqual((rows[0]['by'], rows[0]['participants']), ('Paul', ['Jeanne']))
        for socket in (first, second):
            socket.push(None)

    def test_validated_participant_sends_once_the_others_left(self):
        first, second, hello, joined, items = self.join_team()
        first.push({'type': 'validate', 'partial': True})
        self.wait(lambda: any(m['type'] == 'validate' for m in first.sent))
        second.push(None)  # Paul ferme la page sans valider
        session = self.hub.sessions[hello['id']]
        paul = session.participants[joined['pid']]
        self.wait(lambda: paul.socket is None)
        self.hub.reap(paul.lost + 1)  # encore dans le delai
        self.assertFalse(any(m['type'] == 'submit' for m in first.sent))
        self.hub.reap(paul.lost + self.live.GRACE + 1)
        self.assertEqual(first.sent[-1], {'type': 'submit', 'partial': True})
        self.assertEqual(session.submitting, hello['pid'])
        self.hub.reap(paul.lost + self.live.GRACE + 2)  # une seule demande
        self.assertEqual(sum(m['type'] == 'submit' for m in first.sent), 1)
        first.push(None)

    def test_checks_of_the_same_global_lot_are_merged(self):
        sub_a = Lots(lot_type=self.lot_type, name='Armoire 1', name_short='A1', created_by='test', parent=self.lot)
        sub_a.save()
        sub_b = Lots(lot_type=self.lot_type, name='Armoire 2', name_short='A2', created_by='test', parent=self.lot)
        sub_b.save()

        def start(hello, lot, key):
            socket = FakeSocket(hello)
            self.run_socket(self.hub.run_scanner, socket)
            self.wait(lambda: socket.sent)
            socket.push({'type': 'state', 'lot': {'id': lot.id, 'name': lot.name}, 'root': self.lot.id,
                         'keys': [{'id': lot.id, 'key': key}]})
            return socket

        first = start({'type': 'hello', 'user': self.badge}, sub_a, 'cle-a')
        self.wait(lambda: self.hub.visible())
        # quelqu'un commence la vérif d'un autre sous-lot du meme lot : il rejoint la vérif commune
        second = start({'type': 'hello', 'name': 'Paul Martin'}, sub_b, 'cle-b')
        self.wait(lambda: any(m['type'] == 'hello' and m.get('merged') for m in second.sent))
        merged = next(m for m in second.sent if m.get('merged'))
        self.assertEqual(merged['id'], first.sent[0]['id'])
        self.assertEqual(merged['who'], 'Jeanne')
        self.assertEqual(len(self.hub.sessions), 1)
        session = self.hub.sessions[merged['id']]
        self.assertEqual(list(session.lots), [sub_a.id, sub_b.id])
        self.wait(lambda: any(m['type'] == 'team' and len(m['participants']) == 2 for m in first.sent))
        # la connexion de Paul suit la vérif commune
        second.push({'type': 'validate'})
        self.wait(lambda: any(m['type'] == 'validate' for m in second.sent))
        self.assertTrue(session.participants[merged['pid']].validated)
        # « Quitter » : nouvelle vérif seule, jamais fusionnee
        third = start({'type': 'hello', 'name': 'Luc', 'solo': True}, sub_b, '')
        self.wait(lambda: any(other.state for other in list(self.hub.sessions.values()) if other is not session))
        self.assertEqual(len(self.hub.sessions), 2)
        self.assertFalse(any(m.get('merged') for m in third.sent))
        for socket in (first, second, third):
            socket.push(None)

    def lonely_check(self, validate, remaining):
        """Verif d'un seul participant qui ferme sa page ; renvoie la session apres le delai d'abandon."""
        items = self.create(self.compresses, self.today + timedelta(days=200), 2)
        scanner = FakeSocket({'type': 'hello', 'user': self.badge})
        thread = self.run_socket(self.hub.run_scanner, scanner)
        self.wait(lambda: scanner.sent)
        hello = scanner.sent[0]
        scanner.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'count': 2, 'remaining': remaining,
                      'entries': [{'code': item.iid, 'kind': 'item', 'iid': item.iid} for item in items],
                      'keys': [{'id': self.lot.id, 'key': self.lot.verif_key}]})
        if validate:
            scanner.push({'type': 'validate'})
        scanner.push(None)
        thread.join(5)
        session = self.hub.sessions[hello['id']]
        self.assertEqual(self.hub.reap(session.lost + 60), [])  # rechargement de la page possible
        return session, self.hub.reap(session.lost + self.live.LOST_KEEP + 1)

    def test_abandoned_check_is_dropped(self):
        session, expired = self.lonely_check(validate=False, remaining=3)
        self.assertEqual(expired, [session.id])
        self.assertFalse(Verifs.objects.exists())

    def test_validated_check_without_participants_is_saved(self):
        session, expired = self.lonely_check(validate=True, remaining=3)
        self.assertEqual(expired, [])
        verif = Verifs.objects.get(lot=self.lot)
        self.assertEqual((verif.by, verif.present_count), ('M:M001', 2))
        self.assertIsNotNone(session.done)
        # affichee comme terminee DONE_KEEP a partir de son enregistrement
        self.assertEqual(self.hub.reap(session.finished + 60), [])
        self.assertEqual(self.hub.reap(session.finished + self.live.DONE_KEEP + 1), [session.id])

    def test_complete_check_without_participants_is_saved(self):
        session, expired = self.lonely_check(validate=False, remaining=0)
        self.assertEqual(expired, [])
        self.assertTrue(Verifs.objects.get(lot=self.lot).complete)

    def test_state_is_bounded(self):
        state = self.live.clean_state({'lot': {'id': 'x' * 50, 'name': 'n' * 500}, 'count': -3, 'remaining': 'a',
                                       'scanned': [{'name': 'i'}] * 1000, 'lots': ['a'] * 100})
        self.assertEqual(len(state['lot']['id']), 16)
        self.assertEqual(len(state['lot']['name']), 64)
        self.assertEqual((state['count'], state['remaining']), (0, 0))
        self.assertEqual(len(state['scanned']), self.live.MAX_SCANNED)
        self.assertEqual(len(state['lots']), 20)
        self.assertIsNone(self.live.clean_state({'lot': None}))


class RawWebSocket:
    """Client WebSocket minimal pour les tests (trames texte masquees)."""

    def __init__(self, host, port, path):
        import base64
        import os
        import socket
        self.sock = socket.create_connection((host, port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f'GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n'
                           f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        head = b''
        while b'\r\n\r\n' not in head:
            chunk = self.sock.recv(1)
            if not chunk:
                break
            head += chunk
        self.status = int(head.split(b' ')[1]) if head else 0
        self.last = None

    def send(self, message):
        import os
        payload = json.dumps(message).encode()
        mask = os.urandom(4)
        header = bytes([0x81, 0x80 | len(payload)]) if len(payload) < 126 else \
            bytes([0x81, 0x80 | 126]) + len(payload).to_bytes(2, 'big')
        self.sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def _exact(self, size):
        data = b''
        while len(data) < size:
            chunk = self.sock.recv(size - len(data))
            if not chunk:
                raise ConnectionError('fermee')
            data += chunk
        return data

    def receive(self):
        while True:
            first, second = self._exact(2)
            length = second & 0x7F
            if length == 126:
                length = int.from_bytes(self._exact(2), 'big')
            payload = self._exact(length)
            if first & 0x0F == 0x1:
                self.last = json.loads(payload)
                return self.last
            if first & 0x0F == 0x8:
                raise ConnectionError('fermee par le serveur')

    def close(self):
        self.sock.sendall(bytes([0x88, 0x80]) + b'\0\0\0\0')
        self.sock.close()


class MovementTests(ApiTestCase):
    """Journal des mouvements : absences comptees comme utilisations, annulees si l'item est retrouve."""

    def setUp(self):
        super().setUp()
        self.items = self.create(self.compresses, self.today + timedelta(days=200), 3)
        self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [item.iid for item in self.items]})

    def verif(self, items):
        return self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid for item in items]})

    def absences(self, item):
        return ItemMovement.objects.filter(item=item, kind__in=[MovementKind.USED, MovementKind.DISCARDED])

    def test_restock_is_logged(self):
        moves = ItemMovement.objects.filter(kind=MovementKind.MOVE, to_lot=self.lot)
        self.assertEqual(moves.count(), 3)
        self.assertIsNone(moves.first().from_lot)

    def test_absence_counted_once_and_cancelled_when_found(self):
        lost = self.items[2]
        self.verif(self.items[:2])
        self.verif(self.items[:2])
        self.assertEqual(self.absences(lost).count(), 1)  # une seule utilisation, meme apres plusieurs verifs
        absence = self.absences(lost).get()
        self.assertEqual(absence.kind, MovementKind.USED)
        self.assertEqual(absence.from_lot, self.lot)
        self.assertIsNone(absence.cancelled)
        self.assertEqual(forecast.forecast()['types'][0]['per_month'] > 0, True)
        # verif mal faite : l'item etait bien dans le lot
        self.verif(self.items)
        absence.refresh_from_db()
        self.assertIsNotNone(absence.cancelled)
        compresses = next(row for row in forecast.forecast()['types'] if row['type'] == 'compre')
        self.assertEqual(compresses['per_month'], 0)

    def test_found_after_marked_missing_elsewhere(self):
        lost = self.items[2]
        for _ in range(3):
            self.verif(self.items[:2])
        lost.refresh_from_db()
        self.assertEqual(lost.status, ItemStatus.MISSING)
        # retrouve dans le stock : l'absence est annulee et le retour en stock est un deplacement
        self.call('POST', '/api/stock/verif/', {'items': [lost.iid]})
        self.assertIsNotNone(self.absences(lost).get().cancelled)
        move = ItemMovement.objects.filter(item=lost, kind=MovementKind.MOVE).last()
        self.assertEqual((move.from_lot_id, move.to_lot_id), (self.lot.id, None))
        # une nouvelle absence plus tard compte de nouveau
        self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [lost.iid]})
        self.verif(self.items[:2])
        self.assertEqual(self.absences(lost).filter(cancelled__isnull=True).count(), 1)

    def test_expired_absence_is_discarded_and_restore_cancels(self):
        old = self.create(self.compresses, self.today - timedelta(days=2), 1)[0]
        self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [old.iid]})
        self.verif(self.items)
        self.assertEqual(self.absences(old).get().kind, MovementKind.DISCARDED)
        code, _ = self.call('POST', f'/api/items/{old.iid}/delete/', {'reason': 'jeté'})
        self.assertEqual(ItemMovement.objects.filter(item=old, kind=MovementKind.DELETED).count(), 1)
        self.call('POST', f'/api/items/{old.iid}/restore/', {})
        self.assertIsNotNone(self.absences(old).get().cancelled)
        self.assertEqual(ItemMovement.objects.filter(item=old, kind=MovementKind.RESTORED).count(), 1)

    def test_history_reconstruction(self):
        lost, found = self.items[1], self.items[2]
        self.verif(self.items[:1])          # deux absents
        self.verif(self.items[:1] + [found])  # l'un d'eux est retrouve
        ItemMovement.objects.all().delete()
        module = importlib.import_module('inventory.migrations.0011_item_movements')
        module.reconstruct(django_apps, None)
        self.assertEqual(ItemMovement.objects.filter(reconstructed=True, kind=MovementKind.USED).count(), 2)
        self.assertIsNone(self.absences(lost).get().cancelled)
        self.assertIsNotNone(self.absences(found).get().cancelled)


class ForecastTests(ApiTestCase):
    def setUp(self):
        super().setUp()
        self.now = timezone.now()
        # historique de 6 mois : la consommation se mesure sur toute la periode
        Verifs.objects.create(lot=self.lot, datetime=self.now - timedelta(days=200), by='test')

    def used(self, item_type, count, lot=None):
        item = self.create(item_type, self.today + timedelta(days=400) if item_type.perissable else None, 1)[0]
        ItemMovement.objects.bulk_create([
            ItemMovement(item=item, item_type=item_type, kind=MovementKind.USED, from_lot=lot,
                         at=self.now - timedelta(days=1 + index))
            for index in range(count)
        ])
        item.status = ItemStatus.DELETED
        item.save()

    def row(self, data, type_id='compre'):
        return next(row for row in data['types'] if row['type'] == type_id)

    def test_simulate_first_expired_first_used(self):
        fates, shortages = forecast.simulate([5.0, 10.0, 40.0, math.inf], [6.0, 12.0, 18.0, 24.0, 30.0], 60)
        # l'item qui expire au jour 5 est perdu, les suivants sont utilises dans l'ordre de leurs dates
        self.assertEqual(fates, [('lost', 5.0), ('used', 6.0), ('used', 12.0), ('used', 18.0)])
        self.assertEqual(shortages, [24.0, 30.0])

    def test_latest_arrival(self):
        # utilisations aux jours 10, 20 et 30 ; l'item perime au jour 25
        self.assertEqual(forecast.latest_arrival([], [10.0, 20.0, 30.0], 25.0), 20)
        # un item plus ancien deja sur place passe avant lui le jour 10
        self.assertEqual(forecast.latest_arrival([12.0], [10.0, 20.0, 30.0], 25.0), 20)
        self.assertIsNone(forecast.latest_arrival([], [30.0], 25.0))

    def test_projection_order_and_below_min(self):
        self.compresses.min_quantity = 10
        self.compresses.default_pack_size = 5
        self.compresses.save()
        self.create(self.compresses, self.today + timedelta(days=20), 6)   # perimeront avant d'etre utilises
        self.create(self.compresses, self.today + timedelta(days=300), 14)
        self.used(self.compresses, 6 * 6)  # 6 par mois sur 6 mois (stock)
        data = forecast.forecast(months=3, lead_days=10)
        row = self.row(data)
        self.assertAlmostEqual(row['per_month'], 6, delta=0.5)
        self.assertEqual(row['stock_now'], 20)
        points = row['points']
        self.assertEqual(points[0]['stock'], 20)
        # a M+1 : environ 6 utilises (les plus vieux d'abord), le reste de ce lot perime
        self.assertEqual(points[1]['stock_static'], 14)
        self.assertLess(points[1]['stock'], 15)
        self.assertGreater(points[1]['expiring'], 0)
        self.assertGreaterEqual(points[1]['lost'], 1)
        self.assertLessEqual(row['below_min'], self.today + timedelta(days=60))
        order = row['order']
        self.assertEqual(order['quantity'] % 5, 0)
        self.assertEqual(order['before'], max(self.today, row['below_min'] - timedelta(days=10)))
        months = row['calendar']
        self.assertEqual(len(months), 3)
        self.assertEqual(sum(month['expiring']['stock'] for month in months), 6)

    def test_lot_consumption_and_sealed_transfer(self):
        sealed = Lots(lot_type=self.lot_type, name='Scellé', name_short='S', created_by='test', is_sealed=True)
        sealed.save()
        soon = self.create(self.compresses, self.today + timedelta(days=45), 2)
        late = self.create(self.compresses, self.today + timedelta(days=400), 4)
        Items.objects.filter(iid__in=[item.iid for item in soon]).update(location=sealed)
        Items.objects.filter(iid__in=[item.iid for item in late]).update(location=self.lot)
        self.used(self.compresses, 30, lot=self.lot)  # le sac A utilise 5 compresses par mois
        data = forecast.forecast(months=6)
        row = self.row(data)
        self.assertAlmostEqual(row['lots_consumption'][0]['per_month'], 5, delta=0.5)
        self.assertEqual(row['lots_consumption'][0]['id'], self.lot.id)
        self.assertEqual(row['calendar'][0]['expiring']['sealed'] + row['calendar'][1]['expiring']['sealed'], 2)
        transfers = data['transfers']
        self.assertEqual(len(transfers), 1)
        group = transfers[0]
        self.assertEqual(group['source']['id'], sealed.id)
        self.assertTrue(group['source']['sealed'])
        self.assertEqual(group['count'], 2)
        move = group['moves'][0]
        self.assertEqual(move['target']['id'], self.lot.id)
        self.assertEqual(sorted(move['take']), sorted(item.iid for item in soon))
        self.assertTrue(set(move['back']) <= {item.iid for item in late})
        self.assertLess(group['before'], self.today + timedelta(days=45))
        # une fois l'echange fait, plus rien a proposer
        Items.objects.filter(iid__in=move['take']).update(location=self.lot)
        Items.objects.filter(iid__in=move['back']).update(location=sealed)
        self.assertEqual(forecast.forecast(months=6)['transfers'], [])

    def test_unconfirmed_items_do_not_count(self):
        items = self.create(self.compresses, self.today + timedelta(days=100), 3)
        Items.objects.filter(iid=items[0].iid).update(missed_verifs=1)
        row = self.row(forecast.forecast())
        self.assertEqual(row['stock_now'], 2)
        self.assertEqual(row['unconfirmed'], 1)

    def test_routes(self):
        code, body = self.call('GET', '/api/stock/forecast/?months=3&lead_days=7')
        self.assertEqual(code, 200)
        self.assertEqual((body['months'], body['lead_days']), (3, 7))
        self.assertEqual(len(self.row(body)['points']), 4)
        code, _ = self.call('GET', '/api/stock/forecast/?months=99')
        self.assertEqual(code, 400)
        code, _ = self.call('GET', '/api/stock/forecast/', local=False)
        self.assertEqual(code, 404)
        gestion = Secouristes(matricule='G002', nom='G', prenom='G', role='gestion')
        gestion.renew_key()
        gestion.save()
        badge = {'matricule': 'G002', 'key': gestion.new_key}
        code, body = self.call('POST', '/api/stock/forecast/summary/', {'user': badge, 'months': 3}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(body['months'], 3)
        code, _ = self.call('POST', '/api/stock/forecast/summary/',
                            {'user': {'matricule': 'M001', 'key': self.user.new_key}}, local=False)
        self.assertEqual(code, 403)


class TypeRulesTests(ApiTestCase):
    """Noms en double, types archives, lots uniques."""

    def test_duplicate_names_refused(self):
        code, body = self.call('POST', '/api/item-types/', {'type': 'compr2', 'name': ' compresses '})
        self.assertEqual(code, 409)
        self.assertIn('Compresses', body['error'])
        code, _ = self.call('POST', '/api/lot-types/', {'type': 'sacps2', 'name': 'SAC PSÉ'})
        self.assertEqual(code, 409)  # sans accents ni casse
        code, _ = self.call('POST', '/api/lots/', {'lot_type': 'sacpse', 'name': 'sac a'})
        self.assertEqual(code, 409)
        code, _ = self.call('PATCH', '/api/item-types/garrot/', {'name': 'Compresses'})
        self.assertEqual(code, 409)
        code, _ = self.call('PATCH', '/api/item-types/garrot/', {'name': 'Garrot tourniquet'})
        self.assertEqual(code, 200)
        # un lot archive libere son nom
        code, other = self.call('POST', '/api/lots/', {'lot_type': 'sacpse', 'name': 'Sac B'})
        self.call('PATCH', f"/api/lots/{other['id']}/update/", {'active': False})
        code, _ = self.call('POST', '/api/lots/', {'lot_type': 'sacpse', 'name': 'Sac B'})
        self.assertEqual(code, 201)
        code, _ = self.call('PATCH', f"/api/lots/{other['id']}/update/", {'active': True})
        self.assertEqual(code, 409)  # le nom est repris par le nouveau lot

    def test_archive_item_and_lot_types(self):
        code, body = self.call('PATCH', '/api/item-types/compre/', {'archived': True})
        self.assertEqual(code, 400)  # encore attendu dans le Sac PSE
        code, body = self.call('PATCH', '/api/item-types/garrot/', {'archived': True})
        self.assertTrue(body['archived'])
        code, types = self.call('GET', '/api/item-types/')
        self.assertTrue(next(row for row in types if row['type'] == 'garrot')['archived'])
        code, stock = self.call('GET', '/api/stock/')
        self.assertNotIn('garrot', [row['type'] for row in stock])
        code, body = self.call('PATCH', '/api/item-types/garrot/', {'archived': False})
        self.assertFalse(body['archived'])

        code, _ = self.call('PATCH', '/api/lot-types/sacpse/', {'archived': True})
        self.assertEqual(code, 400)  # le Sac A est actif
        self.call('PATCH', f'/api/lots/{self.lot.id}/update/', {'active': False})
        code, body = self.call('PATCH', '/api/lot-types/sacpse/', {'archived': True})
        self.assertTrue(body['archived'])
        code, _ = self.call('POST', '/api/lots/', {'lot_type': 'sacpse', 'name': 'Sac C'})
        self.assertEqual(code, 400)
        code, _ = self.call('PATCH', f'/api/lots/{self.lot.id}/update/', {'active': True})
        self.assertEqual(code, 400)  # type archive
        self.call('PATCH', '/api/lot-types/sacpse/', {'archived': False})
        code, _ = self.call('PATCH', f'/api/lots/{self.lot.id}/update/', {'active': True})
        self.assertEqual(code, 200)

    def test_unique_lot_type(self):
        code, vps = self.call('POST', '/api/lot-types/', {'type': 'vpsxxx', 'name': 'VPS', 'unique': True,
                                                          'name_short': 'autre', 'user': 'M001'})
        self.assertEqual(code, 201)
        self.assertTrue(vps['unique'])
        lot = vps['created_lot']
        self.assertEqual((lot['name'], lot['name_short'], lot['lot_type']), ('VPS', 'VPS', 'vpsxxx'))
        self.assertEqual(vps['lot'], lot['id'])
        code, armoire = self.call('POST', '/api/lot-types/', {'type': 'armvp1', 'name': 'Armoire 1 VPS', 'unique': True,
                                                              'parent': lot['id']})
        self.assertEqual(armoire['created_lot']['parent'], lot['id'])
        code, _ = self.call('POST', '/api/lots/', {'lot_type': 'vpsxxx', 'name': 'VPS 2'})
        self.assertEqual(code, 409)
        # le lot suit le nom du type
        code, body = self.call('PATCH', '/api/lot-types/vpsxxx/', {'name': 'VPS Paris'})
        self.assertEqual(Lots.objects.get(id=lot['id']).name, 'VPS Paris')
        self.assertEqual(Lots.objects.get(id=lot['id']).name_short, 'VPS Paris')
        # archive avec son type (apres ses sous-lots)
        code, _ = self.call('PATCH', '/api/lot-types/vpsxxx/', {'archived': True})
        self.assertEqual(code, 400)
        self.call('PATCH', '/api/lot-types/armvp1/', {'archived': True})
        code, body = self.call('PATCH', '/api/lot-types/vpsxxx/', {'archived': True})
        self.assertEqual(code, 200)
        self.assertFalse(Lots.objects.get(id=lot['id']).active)
        self.call('PATCH', '/api/lot-types/vpsxxx/', {'archived': False})
        self.assertTrue(Lots.objects.get(id=lot['id']).active)
        # un type qui a deja plusieurs lots ne peut pas devenir unique
        self.call('POST', '/api/lots/', {'lot_type': 'sacpse', 'name': 'Sac B'})
        code, _ = self.call('PATCH', '/api/lot-types/sacpse/', {'unique': True})
        self.assertEqual(code, 400)


class TearOffTests(ApiTestCase):
    """Sachet de serums phy etiquete une fois : l'etiquette est dechiree a l'ouverture."""

    def setUp(self):
        super().setUp()
        self.serum = ItemType.objects.create(type='serphy', name='Sachet sérum phy', tear_off=True)
        LotRequirements.objects.create(lot_type=self.lot_type, item_type=self.serum, quantity=1)

    def test_unscanned_tear_off_item_is_used_at_once(self):
        fresh = [item.iid for item in self.create(self.compresses, self.today + timedelta(days=90), 2)]
        sachet = self.create(self.serum, None, 1)[0]
        Items.objects.filter(iid__in=fresh + [sachet.iid]).update(location=self.lot)
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/')
        self.assertEqual({item['iid'] for item in body['items'] if item['tear_off']}, {sachet.iid})
        # sachet entame : etiquette arrachee, non scannee
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': fresh, 'user': 'M001'})
        self.assertEqual(report['torn'], [sachet.iid])
        self.assertEqual(Items.objects.get(iid=sachet.iid).status, ItemStatus.MISSING)  # sans attendre 3 verifs
        self.assertFalse(report['complete'])
        self.assertEqual(ItemMovement.objects.filter(item_id=sachet.iid, kind=MovementKind.USED).count(), 1)
        # un sachet neuf complete le lot
        new = self.create(self.serum, None, 1)[0]
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': fresh + [new.iid]})
        self.assertTrue(report['complete'])

    def test_partial_verif_ignores_torn_items(self):
        """Un lot qui ne contient qu'un sachet dechire peut etre valide par une verif partielle sans scan."""
        storage_type = LotType.objects.create(type='trousx', name='Trousse')
        trousse = Lots(lot_type=storage_type, name='Trousse', name_short='T', created_by='test', parent=self.lot)
        trousse.save()
        sachet = self.create(self.serum, None, 1)[0]
        Items.objects.filter(iid=sachet.iid).update(location=trousse)
        fresh = [item.iid for item in self.create(self.compresses, self.today + timedelta(days=90), 2)]
        serum = self.create(self.serum, None, 1)[0].iid
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/',
                                 {'items': fresh + [serum], 'user': 'M001', 'partial': True})
        self.assertEqual(code, 200)
        self.assertEqual({row['id'] for row in report['lots'] if row['verified']}, {self.lot.id, trousse.id})
        self.assertEqual(report['torn'], [sachet.iid])


class NonConsumableTests(ApiTestCase):
    """Lecteur de glycemie : non consommable, absent a la verif de son lot il passe hors lot au lieu d'etre perdu."""

    def setUp(self):
        super().setUp()
        code, body = self.call('POST', '/api/item-types/', {'type': 'glycem', 'name': 'Lecteur de glycémie',
                                                             'consumable': False})
        self.assertEqual((code, body['consumable']), (201, False))
        self.glycemie = ItemType.objects.get(type='glycem')

    def test_flag_editable(self):
        code, body = self.call('PATCH', '/api/item-types/glycem/', {'consumable': True})
        self.assertTrue(body['consumable'])
        self.assertTrue(ItemType.objects.get(type='glycem').consumable)
        self.assertTrue(ItemType.objects.get(type='garrot').consumable)  # consommable par defaut

    def test_missing_from_lot_is_loose(self):
        fresh = [item.iid for item in self.create(self.compresses, self.today + timedelta(days=90), 2)]
        lecteur = self.create(self.glycemie, None, 1)[0]
        Items.objects.filter(iid__in=fresh + [lecteur.iid]).update(location=self.lot)
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/')
        self.assertEqual({item['iid'] for item in body['items'] if not item['consumable']}, {lecteur.iid})
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': fresh, 'user': 'M001'})
        self.assertEqual(code, 200)
        self.assertEqual(report['loose'], [lecteur.iid])
        self.assertEqual(report['missing'], [])
        lecteur.refresh_from_db()
        self.assertEqual((lecteur.status, lecteur.location_id, lecteur.missed_verifs), (ItemStatus.LOOSE, None, 0))
        # ni utilise ni jete : un deplacement vers le stock
        self.assertFalse(ItemMovement.objects.filter(item=lecteur, kind__in=ABSENCE_KINDS).exists())
        self.assertTrue(ItemMovement.objects.filter(item=lecteur, kind=MovementKind.MOVE, from_lot=self.lot,
                                                    to_lot=None).exists())
        self.assertTrue(Operation.objects.filter(kind='remove', lot=self.lot).exists())
        # hors lot : ni dans les lots ni dans le stock
        code, info = self.call('GET', f'/api/items/{lecteur.iid}/', local=False)
        self.assertFalse(info['in_stock'])
        row = next(row for row in services.stock_status() if row['type'] == 'glycem')
        self.assertEqual((row['stock_fresh'], row['lots_fresh'], row['loose']), (0, 0, 1))
        # une verif du stock qui ne le voit pas le laisse hors lot (il n'etait pas en stock)
        code, report = self.call('POST', '/api/stock/verif/', {'items': []})
        self.assertNotIn(lecteur.iid, report['missing'])
        self.assertEqual(Items.objects.get(iid=lecteur.iid).status, ItemStatus.LOOSE)
        # scanne a la verif du stock : il y entre
        code, report = self.call('POST', '/api/stock/verif/', {'items': [lecteur.iid]})
        lecteur.refresh_from_db()
        self.assertEqual((lecteur.status, lecteur.location_id), (ItemStatus.ACTIVE, None))

    def test_manual_return_to_stock(self):
        lecteur = self.create(self.glycemie, None, 1)[0]
        Items.objects.filter(iid=lecteur.iid).update(location=self.lot)
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [], 'user': 'M001'})
        self.assertEqual(Items.objects.get(iid=lecteur.iid).status, ItemStatus.LOOSE)
        code, body = self.call('POST', '/api/items/to-stock/', {'items': [lecteur.iid]})
        self.assertEqual((code, body['moved']), (200, [lecteur.iid]))
        lecteur.refresh_from_db()
        self.assertEqual((lecteur.status, lecteur.location_id), (ItemStatus.ACTIVE, None))

    def test_partial_verif_not_held_by_non_consumable(self):
        fresh = [item.iid for item in self.create(self.compresses, self.today + timedelta(days=90), 2)]
        lecteur = self.create(self.glycemie, None, 1)[0]
        Items.objects.filter(iid=lecteur.iid).update(location=self.lot)
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/',
                                 {'items': fresh, 'user': 'M001', 'partial': True})
        self.assertEqual(code, 200)
        self.assertIn(self.lot.id, {row['id'] for row in report['lots'] if row['verified']})
        self.assertEqual(report['loose'], [lecteur.iid])

    def test_missing_from_storage_is_missing(self):
        storage_type = LotType.objects.create(type='armoir', name='Armoire', storage=True)
        armoire = Lots(lot_type=storage_type, name='Armoire', name_short='A', created_by='test')
        armoire.save()
        lecteur = self.create(self.glycemie, None, 1)[0]
        Items.objects.filter(iid=lecteur.iid).update(location=armoire)
        code, report = self.call('POST', f'/api/lots/{armoire.id}/verif/', {'items': [], 'user': 'M001'})
        self.assertEqual(code, 200)
        self.assertEqual(report['loose'], [])
        self.assertEqual(report['missing'], [lecteur.iid])
        self.assertEqual(Items.objects.get(iid=lecteur.iid).location_id, armoire.id)

    def test_out_is_never_counted_used(self):
        lecteur = self.create(self.glycemie, None, 1)[0]
        code, body = self.call('POST', '/api/items/out/', {'items': [lecteur.iid]})
        self.assertEqual((code, body['until']), (200, None))
        code, info = self.call('GET', f'/api/items/{lecteur.iid}/', local=False)
        self.assertIsNone(info['out_until'])
        self.assertEqual(services.expire_out_items(timezone.now() + timedelta(days=400)), [])
        self.assertEqual(Items.objects.get(iid=lecteur.iid).status, ItemStatus.OUT)


class DefaultContactTests(ApiTestCase):
    def test_default_admin_contact(self):
        admin = Secouristes(matricule='A001', nom='Ad', prenom='Min', role='admin')
        admin.renew_key()
        admin.save()
        code, body = self.call('PATCH', '/api/users/M001/', {'default_contact': True})
        self.assertFalse(body['default_contact'])  # reserve aux administrateurs
        code, body = self.call('PATCH', '/api/users/A001/', {'default_contact': True})
        self.assertTrue(body['default_contact'])
        self.user.refresh_from_db()
        self.assertEqual(self.user.contact_admin(), admin)
        self.assertIsNone(admin.contact_admin())  # pas lui-meme
        # un seul admin par defaut
        self.call('PATCH', '/api/users/P001/', {'default_contact': True})
        admin.refresh_from_db()
        self.assertFalse(admin.default_contact)
        self.assertEqual(self.user.contact_admin(), self.operator)
        # le contact choisi pour l'utilisateur passe avant
        self.call('PATCH', '/api/users/M001/', {'pin_contact': 'A001'})
        self.user.refresh_from_db()
        self.assertEqual(self.user.contact_admin(), admin)
        # plus admin : plus contact par defaut
        code, body = self.call('PATCH', '/api/users/P001/', {'role': 'gestion'})
        self.assertFalse(body['default_contact'])


class LotSheetTests(ApiTestCase):
    def sub_lots(self):
        """B+ (regroupement) compose d'un sac de soin et d'un sac O2, trie par emplacement."""
        global_type = LotType.objects.create(type='bplus0', name='B+')
        o2_type = LotType.objects.create(type='sacoxy', name='Sac O2')
        LotRequirements.objects.create(lot_type=self.lot_type, item_type=self.garrot, quantity=1, location='Poche avant')
        LotRequirements.objects.create(lot_type=o2_type, item_type=self.garrot, quantity=3, location='Écran')
        root = Lots(lot_type=global_type, name='B+ 1', name_short='B1', created_by='test')
        root.save()
        self.lot.parent = root
        self.lot.save()
        o2 = Lots(lot_type=o2_type, name='Sac O2 1', name_short='O1', created_by='test', parent=root)
        o2.save()
        return root, o2

    def pages(self, response):
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response['Content-Type'], 'application/pdf')
        pdf = response.content
        self.assertTrue(pdf.startswith(b'%PDF-1.4') and pdf.rstrip().endswith(b'%%EOF'))
        return pdf.count(b'/Type /Page ')

    def page_texts(self, pdf):
        import re
        import zlib
        streams = re.findall(rb'stream\n(.*?)\nendstream', pdf, re.S)
        return [zlib.decompress(stream).decode('cp1252') for stream in streams]

    def test_one_page_per_sub_lot(self):
        root, o2 = self.sub_lots()
        response = self.client.get(f'/api/lots/{root.id}/sheet.pdf')
        self.assertEqual(self.pages(response), 2)  # Sac A, Sac O2 : le B+ sans item attendu n'a pas de page
        texts = self.page_texts(response.content)
        self.assertIn('(Sac A)', texts[0])
        # sac de soin : « Poche avant » avant « Sans emplacement »
        self.assertLess(texts[0].index('(Poche avant)'), texts[0].index('(Sans emplacement)'))
        self.assertIn('(Compresses)', texts[0])
        self.assertIn('dans B+ 1', texts[1])
        self.assertIn('(\xc9cran)', texts[1])
        self.assertNotIn('Signature', texts[0])  # liste de controle, pas de trace de verification
        # un sous-lot seul : sa page uniquement ; le B+ seul sans sous-lot actif : une page « aucun item »
        self.assertEqual(self.pages(self.client.get(f'/api/lots/{o2.id}/sheet.pdf')), 1)
        Lots.objects.filter(parent=root).update(active=False)
        response = self.client.get(f'/api/lots/{root.id}/sheet.pdf')
        self.assertEqual(self.pages(response), 1)
        self.assertIn('Aucun item attendu', self.page_texts(response.content)[0])

    def test_long_lot_fits_one_page_then_continues(self):
        for index in range(70):
            item_type = ItemType.objects.create(type=f'it{index:04d}', name=f'Item {index}')
            LotRequirements.objects.create(lot_type=self.lot_type, item_type=item_type, location=f'Poche {index // 10}')
        response = self.client.get(f'/api/lots/{self.lot.id}/sheet.pdf')
        self.assertEqual(self.pages(response), 1)  # deux colonnes, texte reduit
        for index in range(70, 200):
            item_type = ItemType.objects.create(type=f'it{index:04d}', name=f'Item {index}')
            LotRequirements.objects.create(lot_type=self.lot_type, item_type=item_type, location=f'Poche {index // 10}')
        response = self.client.get(f'/api/lots/{self.lot.id}/sheet.pdf')
        self.assertGreaterEqual(self.pages(response), 2)
        texts = self.page_texts(response.content)
        self.assertIn('Sac A \\(suite\\)', texts[1])

    def test_short_list_stays_on_one_column(self):
        """Un seul item sans emplacement : une colonne, le bandeau avec son item, pas de « suite »."""
        lot_type = LotType.objects.create(type='armoir', name='Armoire')
        LotRequirements.objects.create(lot_type=lot_type, item_type=self.garrot, quantity=1)
        lot = Lots(lot_type=lot_type, name='Armoire 1', name_short='AR1', created_by='test')
        lot.save()
        for count in (1, 3):
            response = self.client.get(f'/api/lots/{lot.id}/sheet.pdf')
            text = self.page_texts(response.content)[0]
            self.assertEqual(text.count('(Sans emplacement'), 1)
            self.assertNotIn('suite', text)
            self.assertNotIn('0.7 G 0.4 w', text)  # pas de filet entre deux colonnes
            for index in range(count, 3):
                item_type = ItemType.objects.create(type=f'ar{index:04d}', name=f'Item {index}')
                LotRequirements.objects.create(lot_type=lot_type, item_type=item_type)

    def test_public_qr_and_key_frame(self):
        """Premiere page du lot : QR code de l'etiquette publique et cadre de l'etiquette cle ; pas sur la suite."""
        for index in range(200):
            item_type = ItemType.objects.create(type=f'it{index:04d}', name=f'Item {index}')
            LotRequirements.objects.create(lot_type=self.lot_type, item_type=item_type, location=f'Poche {index // 10}')
        texts = self.page_texts(self.client.get(f'/api/lots/{self.lot.id}/sheet.pdf').content)
        self.assertIn('(\xc9tiquette cl\xe9)', texts[0])
        self.assertIn('[3 2] 0 d', texts[0])  # cadre en pointilles
        self.assertNotIn('(\xc9tiquette cl\xe9)', texts[1])

    def test_qr_matrix(self):
        from .qr import qr_matrix
        modules = qr_matrix('https://example.com/verif?lot=sacpse00000001')
        self.assertEqual(len(modules), 33)  # version 4
        finder = [[max(abs(x - 3), abs(y - 3)) not in (2, 4) for x in range(7)] for y in range(7)]
        self.assertEqual([row[:7] for row in modules[:7]], finder)
        self.assertEqual([row[-7:] for row in modules[:7]], finder)
        self.assertEqual([row[:7] for row in modules[-7:]], finder)
        self.assertEqual(len(qr_matrix('x' * 150)), 49)  # version 8, avec ses informations de version

    def test_archived_lot_hidden_from_public(self):
        self.lot.active = False
        self.lot.save()
        self.assertEqual(self.client.get(f'/api/lots/{self.lot.id}/sheet.pdf').status_code, 404)
        self.assertEqual(self.client.get(f'/api/lots/{self.lot.id}/sheet.pdf', **LOCAL).status_code, 200)
        self.assertEqual(self.client.get('/api/lots/inconnu/sheet.pdf').status_code, 404)


class OperationJournalTests(ApiTestCase):
    """Journal des operations : qui fait quoi et quand, filtres de la fenetre Journal (gestion et admin)."""

    def journal(self, query='', session=None):
        code, body = self.call('GET', '/api/operations/' + query, session=session)
        self.assertEqual(code, 200, body)
        return body

    def test_operations_are_recorded_and_filtered(self):
        items = self.create(self.compresses, self.today + timedelta(days=60), 2)
        user = {'matricule': 'M001', 'key': self.user.new_key}
        # reassort par l'etiquette privee (Jeanne), verif, scellage et ouverture par l'etiquette interieure
        self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [items[0].iid], 'key': self.lot.verif_key,
                                                           'user': user}, local=False)
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid for item in items]})
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'seal_number': 'S9'})
        open_code = body['seal_open_url'].split('c=')[1]
        self.call('POST', f'/api/lots/{self.lot.id}/seal-open/', {'code': open_code}, local=False)
        self.call('POST', '/api/items/to-stock/', {'items': [items[1].iid]})

        body = self.journal('?facets=1')
        kinds = [row['kind'] for row in body['operations']]
        self.assertEqual(kinds, ['remove', 'unseal', 'seal', 'verif', 'restock'])  # plus recent d'abord
        restock = body['operations'][-1]
        self.assertEqual(restock['by_name'], 'Jeanne Dupont')
        self.assertEqual(restock['lot_name'], 'Sac A')
        self.assertEqual(restock['summary'], '1 × Compresses')
        self.assertEqual(restock['details']['items'], [items[0].iid])
        verif = body['operations'][3]
        self.assertEqual(verif['summary'], 'complète : 2 présents')
        self.assertEqual(verif['by_name'], 'Admin Poste')
        self.assertEqual(body['operations'][2]['summary'], 'scellé n°S9')
        unseal = body['operations'][1]
        self.assertEqual(unseal['by_name'], 'Anonyme (étiquette intérieure)')
        self.assertIn("étiquette d'ouverture", unseal['summary'])
        self.assertEqual({person['name'] for person in body['people']},
                         {'Jeanne Dupont', 'Admin Poste', 'Anonyme (étiquette intérieure)'})
        self.assertIn({'kind': 'verif', 'label': 'Vérif'}, body['kinds'])

        # ouverture anonyme signee juste apres : la ligne du journal est attribuee
        self.call('POST', f'/api/lots/{self.lot.id}/seal-open/', {'code': open_code, 'user': user}, local=False)
        unseal = self.journal('?kind=unseal')['operations'][0]
        self.assertEqual(unseal['by_name'], 'Jeanne Dupont')
        self.assertTrue(unseal['details']['signed_after_scan'])

        # filtres : qui, quoi, lot, texte, dates, pages
        self.assertEqual([row['kind'] for row in self.journal('?by=M:M001')['operations']], ['unseal', 'restock'])
        self.assertEqual(len(self.journal('?kind=seal,unseal')['operations']), 2)
        self.assertEqual(len(self.journal('?q=dupont')['operations']), 2)
        self.assertEqual(len(self.journal('?q=S9')['operations']), 2)
        self.assertEqual(len(self.journal(f'?lot={self.lot.id}')['operations']), 5)
        tomorrow = (self.today + timedelta(days=1)).isoformat()
        self.assertEqual(self.journal(f'?since={tomorrow}')['operations'], [])
        self.assertEqual(len(self.journal(f'?until={self.today.isoformat()}')['operations']), 5)
        first = self.journal('?limit=2')
        self.assertTrue(first['more'])
        rest = self.journal(f"?limit=10&before={first['operations'][-1]['id']}")
        self.assertFalse(rest['more'])
        self.assertEqual(len(rest['operations']), 3)
        code, body = self.call('GET', '/api/operations/?kind=inconnu')
        self.assertEqual(code, 400)

    def test_sub_lots_and_management_operations(self):
        bag = Lots(lot_type=self.lot_type, name='Sac B', name_short='B', created_by='test', parent=self.lot)
        bag.save()
        item = self.create(self.garrot, None, 1)[0]
        self.call('POST', f'/api/lots/{bag.id}/add/', {'items': [item.iid]})
        # le journal d'un lot comprend ses sous-lots (sub=0 : le lot seul)
        self.assertEqual(len(self.journal(f'?lot={self.lot.id}')['operations']), 1)
        self.assertEqual(self.journal(f'?lot={self.lot.id}&sub=0')['operations'], [])
        # operations de gestion : lot archive, badge renouvele, reception
        self.call('PATCH', f'/api/lots/{bag.id}/update/', {'parent': ''})
        self.call('POST', '/api/users/M001/renew-key/')
        self.call('POST', '/api/items/batch/', {'type': 'garrot', 'count': 3})
        rows = self.journal()['operations']
        self.assertEqual([row['kind'] for row in rows[:3]], ['reception', 'user', 'lot'])
        self.assertEqual(rows[0]['summary'], '3 × Garrot')
        self.assertIn('Jeanne Dupont', rows[1]['summary'])
        self.assertEqual(rows[2]['summary'], 'lot modifié : lot indépendant')

    def test_reserved_to_management_roles(self):
        code, _ = self.call('GET', '/api/operations/', session=False)
        self.assertEqual(code, 403)
        code, _ = self.call('GET', '/api/operations/', session=views.session_token(self.user))
        self.assertEqual(code, 403)
        self.user.role = 'gestion'
        self.user.save()
        code, _ = self.call('GET', '/api/operations/', session=views.session_token(self.user))
        self.assertEqual(code, 200)
        code, _ = self.call('GET', '/api/operations/', local=False)
        self.assertEqual(code, 404)

    def test_reconstruct_history(self):
        items = self.create(self.compresses, self.today + timedelta(days=60), 2)
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [items[0].iid]})
        self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [items[1].iid]})
        self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'force': True})
        Operation.objects.all().delete()
        module = importlib.import_module('inventory.migrations.0016_operations')
        module.reconstruct(django_apps, None)
        rows = Operation.objects.order_by('at', 'id')
        self.assertTrue(all(row.reconstructed for row in rows))
        self.assertEqual({row.kind for row in rows}, {'reception', 'lot', 'verif', 'restock', 'seal', 'catalog'})
        self.assertEqual(rows.filter(kind='verif').count(), 1)
        # l'item range dans le lot pendant la verif ne compte pas comme un reassort
        self.assertEqual(rows.get(kind='restock').details['items'], [items[1].iid])
        self.assertEqual(rows.get(kind='seal').lot_id, self.lot.id)


class ItemOutTests(ApiTestCase):
    """Materiel sorti du stock sans lot : statut « sorti », compte comme utilise s'il ne revient pas."""

    def test_out_then_back_in_a_verif(self):
        item = self.create(self.garrot, None, 1)[0]
        code, body = self.call('POST', '/api/items/out/', {'items': [item.iid, 'inconnu']})
        self.assertEqual(code, 200)
        self.assertEqual((body['out'], body['unknown']), ([item.iid], ['inconnu']))
        item.refresh_from_db()
        self.assertEqual((item.status, item.location_id), (ItemStatus.OUT, None))
        self.assertTrue(ItemMovement.objects.filter(item=item, kind=MovementKind.OUT).exists())
        self.assertTrue(Operation.objects.filter(kind='item_out').exists())
        code, info = self.call('GET', f'/api/items/{item.iid}/', local=False)
        self.assertFalse(info['in_stock'])
        self.assertEqual(info['out_until'], (self.today + timedelta(days=30)).isoformat())
        # hors du stock, et une verif du stock ne le signale pas absent
        row = next(row for row in services.stock_status() if row['type'] == 'garrot')
        self.assertEqual((row['stock_fresh'], row['out']), (0, 1))
        code, report = self.call('POST', '/api/stock/verif/', {'items': []})
        self.assertEqual(report['missing'], [])
        # il revient dans un lot : actif, et rien n'est compte comme utilise
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid]})
        item.refresh_from_db()
        self.assertEqual((item.status, item.location_id), (ItemStatus.ACTIVE, self.lot.id))
        self.assertIn(item.iid, report['reactivated'])
        self.assertEqual(services.expire_out_items(timezone.now() + timedelta(days=60)), [])

    def local_noon(self, days):
        # midi heure locale, `days` jours apres aujourd'hui : now + timedelta(days=N) tombe la veille pres de minuit
        # quand l'intervalle traverse un changement d'heure (le delai est compte en jours calendaires)
        day = datetime.combine(self.today + timedelta(days=days), datetime.min.time())
        return timezone.make_aware(day + timedelta(hours=12))

    def test_not_seen_counts_as_used(self):
        fresh = self.create(self.garrot, None, 1)[0]
        soon = self.create(self.compresses, self.today + timedelta(days=5), 1)[0]
        self.call('POST', '/api/items/out/', {'items': [fresh.iid, soon.iid]})
        self.assertEqual(services.expire_out_items(), [])
        # peremption avant le mois : compte utilise des sa peremption
        self.assertEqual(services.expire_out_items(self.local_noon(5)), [soon.iid])
        self.assertEqual(Items.objects.get(iid=soon.iid).status, ItemStatus.MISSING)
        self.assertTrue(ItemMovement.objects.filter(item=soon, kind=MovementKind.USED).exists())
        # non perissable : utilise au bout d'un mois
        self.assertEqual(services.expire_out_items(self.local_noon(30)), [fresh.iid])
        used = ItemMovement.objects.get(item=fresh, kind=MovementKind.USED)
        self.assertIsNone(used.cancelled)
        self.assertTrue(Operation.objects.filter(kind='item_out_used').exists())
        # retrouve plus tard (verif du stock) : l'utilisation est annulee
        self.call('POST', '/api/stock/verif/', {'items': [fresh.iid]})
        used.refresh_from_db()
        self.assertIsNotNone(used.cancelled)
        self.assertEqual(Items.objects.get(iid=fresh.iid).status, ItemStatus.ACTIVE)

    def test_only_stock_items_go_out(self):
        in_lot, in_stock = self.create(self.garrot, None, 2)
        Items.objects.filter(iid=in_lot.iid).update(location=self.lot)
        code, body = self.call('POST', '/api/items/out/', {'items': [in_lot.iid, in_stock.iid]})
        self.assertEqual(body['out'], [in_stock.iid])
        self.assertEqual([row['iid'] for row in body['refused']], [in_lot.iid])
        self.assertEqual(Items.objects.get(iid=in_lot.iid).location_id, self.lot.id)
        code, body = self.call('POST', '/api/items/out/', {'items': [in_lot.iid]})
        self.assertEqual(code, 400)
        # identite obligatoire, comme pour une verif
        code, body = self.call('POST', '/api/items/out/', {'items': [in_stock.iid]}, session=False)
        self.assertEqual(code, 403)

    def test_check_alerts_expires_out_items(self):
        item = self.create(self.garrot, None, 1)[0]
        self.call('POST', '/api/items/out/', {'items': [item.iid]})
        Items.objects.filter(iid=item.iid).update(last_seen=timezone.now() - timedelta(days=31))
        call_command('check_alerts', stdout=StringIO())
        self.assertEqual(Items.objects.get(iid=item.iid).status, ItemStatus.MISSING)


@override_settings(QRPROTEC={**settings.QRPROTEC, 'SMS_SYNC': True})
class ProblemReportTests(ApiTestCase):
    def setUp(self):
        super().setUp()
        from . import notifications
        from .models import NotificationSettings, SmsRecipient
        self.sent = []
        original = notifications.send_free_sms
        notifications.send_free_sms = lambda user, password, message: (self.sent.append(message) or (True, 'Envoyé'))
        self.addCleanup(setattr, notifications, 'send_free_sms', original)
        NotificationSettings.objects.update_or_create(pk=1, defaults={'enabled': True})
        SmsRecipient.objects.create(name='A', user='u', password='p')
        self.badge = {'matricule': 'M001', 'key': self.user.new_key}
        self.items = self.create(self.compresses, self.today + timedelta(days=30), 2)
        Items.objects.filter(iid__in=[item.iid for item in self.items]).update(location=self.lot)

    def report(self, data, local=False):
        payload = {'user': self.badge, **data} if not local else data
        return self.call('POST', '/api/reports/', payload, local=local)

    def test_login_required(self):
        code, _ = self.call('POST', '/api/reports/', {'item': self.items[0].iid, 'usable': True}, local=False)
        self.assertEqual(code, 403)
        code, _ = self.call('POST', '/api/reports/', {'item': self.items[0].iid, 'usable': True}, session=False)
        self.assertEqual(code, 403)
        code, body = self.report({'item': self.items[0].iid})  # la question « utilisable ? » est obligatoire
        self.assertEqual(code, 400)

    def test_report_notifies_admins_and_lists(self):
        with self.captureOnCommitCallbacks(execute=True):
            code, body = self.report({'item': self.items[0].iid, 'comment': 'Emballage déchiré', 'usable': True})
        self.assertEqual(code, 201)
        self.assertEqual(body['report']['color'], 'red')
        self.assertEqual(body['report']['lot'], self.lot.id)
        self.assertEqual(body['report']['type'], 'compre')
        self.assertTrue(body['item']['reported'])
        self.assertFalse(body['item']['expired'])
        self.assertEqual(len(self.sent), 1)
        self.assertIn('Emballage déchiré', self.sent[0])
        # suivi en lecture seule : tout utilisateur connecte
        code, body = self.call('POST', '/api/reports/list/', {'user': self.badge}, local=False)
        self.assertEqual(code, 200)
        self.assertFalse(body['can_manage'])
        self.assertEqual(body['counts']['new'], 1)
        self.assertEqual(body['reports'][0]['comment'], 'Emballage déchiré')
        code, _ = self.call('POST', '/api/reports/list/', {}, local=False)
        self.assertEqual(code, 403)
        # signalement d'un type d'item d'un lot, depuis le poste
        code, body = self.report({'lot': self.lot.id, 'type': 'compre', 'usable': False}, local=True)
        self.assertEqual(code, 201)
        self.assertEqual(body['report']['target'], 'Compresses (Sac A)')
        self.assertTrue(Operation.objects.filter(kind='report').exists())

    def test_admin_update(self):
        _, body = self.report({'item': self.items[0].iid, 'usable': True})
        report_id = body['report']['id']
        # un secouriste ne gere pas les signalements
        code, _ = self.call('POST', f'/api/reports/{report_id}/', {'user': self.badge, 'status': 'progress'}, local=False)
        self.assertEqual(code, 403)
        code, body = self.call('POST', f'/api/reports/{report_id}/', {'status': 'progress', 'usable': False,
                                                                      'note': 'Commandé'})
        self.assertEqual(code, 200)
        self.assertEqual((body['status'], body['color'], body['note']), ('progress', 'orange', 'Commandé'))
        self.assertTrue(Items.objects.get(iid=self.items[0].iid).unusable)
        code, body = self.call('POST', f'/api/reports/{report_id}/', {'usable': True})
        self.assertEqual(body['color'], 'green')
        code, body = self.call('POST', f'/api/reports/{report_id}/', {'status': 'resolved'})
        self.assertEqual(body['color'], 'green')
        item = Items.objects.get(iid=self.items[0].iid)
        self.assertFalse(item.reported or item.unusable)
        code, body = self.call('POST', '/api/reports/list/', {})
        self.assertTrue(body['can_manage'])
        self.assertEqual(body['reports'], [])
        code, body = self.call('POST', '/api/reports/list/', {'show': 'all'})
        self.assertEqual(len(body['reports']), 1)

    def test_unusable_item_must_be_replaced(self):
        broken, other = self.items
        self.report({'item': broken.iid, 'usable': False})
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/')
        self.assertFalse(body['complete'])
        self.assertEqual(body['requirements'][0]['present'], 1)
        # verif : l'item inutilisable scanne reste a remplacer
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [broken.iid, other.iid]})
        self.assertFalse(report['complete'])
        self.assertEqual(report['unusable'], [broken.iid])
        # remplacant scanne : l'item inutilisable sort du lot
        new = self.create(self.compresses, self.today + timedelta(days=300), 1)[0]
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [broken.iid, other.iid, new.iid]})
        self.assertTrue(report['complete'])
        self.assertEqual(report['replaced'], [broken.iid])

    def test_reported_item_never_missing(self):
        reported, other = self.items
        self.report({'item': reported.iid, 'usable': True})
        for _ in range(3):
            code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [other.iid]})
            self.assertNotIn(reported.iid, report['missing'])
        item = Items.objects.get(iid=reported.iid)
        self.assertEqual((item.status, item.missed_verifs), (ItemStatus.ACTIVE, 0))

    def test_resolve_non_compliant_item(self):
        disabled, removed = self.items
        _, body = self.report({'item': disabled.iid, 'usable': False, 'comment': 'Date effacée'})
        first = body['report']['id']
        _, body = self.report({'item': removed.iid, 'usable': False})
        second = body['report']['id']
        # l'issue se donne en resolvant
        code, _ = self.call('POST', f'/api/reports/{first}/', {'status': 'progress', 'resolution': 'disabled'})
        self.assertEqual(code, 400)
        code, body = self.call('POST', f'/api/reports/{first}/', {'status': 'resolved', 'resolution': 'disabled'})
        self.assertEqual(code, 200)
        self.assertEqual((body['resolution'], body['resolution_label']), ('disabled', 'Désactivé'))
        item = Items.objects.get(iid=disabled.iid)
        self.assertTrue(item.disabled and not item.unusable and not item.reported)
        self.assertIn('Date effacée', item.disabled_reason)
        self.assertTrue(item.needs_replacement())
        code, body = self.call('POST', f'/api/reports/{second}/', {'status': 'resolved', 'resolution': 'removed'})
        self.assertEqual(code, 200)
        self.assertEqual(Items.objects.get(iid=removed.iid).status, ItemStatus.DELETED)
        # rouvert : l'issue est effacee, l'item reste desactive (reactivation a la main)
        code, body = self.call('POST', f'/api/reports/{first}/', {'status': 'progress'})
        self.assertEqual(body['resolution'], '')
        self.assertTrue(Items.objects.get(iid=disabled.iid).disabled)
        # signalement d'un lot : pas d'item a desactiver
        _, body = self.report({'lot': self.lot.id, 'usable': True})
        code, _ = self.call('POST', f"/api/reports/{body['report']['id']}/", {'status': 'resolved',
                                                                              'resolution': 'disabled'})
        self.assertEqual(code, 400)


class ItemDisableTests(ApiTestCase):
    def setUp(self):
        super().setUp()
        self.items = self.create(self.compresses, self.today + timedelta(days=30), 3)
        Items.objects.filter(iid__in=[item.iid for item in self.items[:2]]).update(location=self.lot)

    def disable(self, data, **kwargs):
        return self.call('POST', '/api/items/disable/', data, **kwargs)

    def test_disable_one_item(self):
        target, other, _ = self.items
        code, _ = self.disable({'items': [target.iid]})
        self.assertEqual(code, 400)  # raison obligatoire
        code, _ = self.disable({'items': [target.iid], 'reason': 'Rappel'}, local=False)
        self.assertIn(code, (403, 405))  # au poste seulement (route absente de l'API publique)
        gestion = Secouristes(matricule='G001', nom='Gestion', prenom='Gil', role='gestion')
        gestion.renew_key()
        gestion.save()
        code, _ = self.disable({'items': [target.iid], 'reason': 'Rappel'}, session=views.session_token(gestion))
        self.assertEqual(code, 403)  # administrateurs seulement
        code, body = self.disable({'items': [target.iid], 'reason': 'Rappel produit'})
        self.assertEqual((code, body['count']), (200, 1))
        _, item = self.call('GET', f'/api/items/{target.iid}/')
        self.assertTrue(item['disabled'] and item['expired'] and not item['outdated'])
        self.assertEqual(item['disabled_reason'], 'Rappel produit')
        self.assertTrue(Operation.objects.filter(kind='item_disable').exists())
        # comme un perime : le lot est incomplet, le remplacant sort l'item desactive du lot
        code, lot = self.call('GET', f'/api/lots/{self.lot.id}/')
        self.assertFalse(lot['complete'])
        new = self.create(self.compresses, self.today + timedelta(days=300), 1)[0]
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [target.iid, other.iid, new.iid]})
        self.assertTrue(report['complete'])
        self.assertEqual(report['replaced'], [target.iid])
        # reactivation
        code, body = self.disable({'items': [target.iid], 'disabled': False})
        self.assertEqual(body['count'], 1)
        self.assertFalse(Items.objects.get(iid=target.iid).disabled)

    def test_receptions_and_whole_reception(self):
        later = self.create(self.compresses, self.today + timedelta(days=30), 2)  # autre reception, meme paquet
        code, rows = self.call('GET', '/api/receptions/?type=compre')
        self.assertEqual(code, 200)
        self.assertEqual([row['count'] for row in rows], [2, 3])
        code, rows = self.call('GET', f'/api/receptions/?item={self.items[1].iid}')
        self.assertEqual((len(rows), rows[0]['item'], rows[0]['count']), (1, self.items[0].iid, 3))
        code, body = self.disable({'reception': self.items[1].iid, 'reason': 'Rappel sérum phy'})
        self.assertEqual(body['count'], 3)
        self.assertFalse(Items.objects.get(iid=later[0].iid).disabled)
        code, rows = self.call('GET', '/api/receptions/?type=compre')
        self.assertEqual(rows[1]['disabled'], 3)
        code, stock = self.call('GET', '/api/stock/')
        row = next(row for row in stock if row['type'] == 'compre')
        self.assertEqual((row['stock_fresh'], row['stock_expired']), (2, 1))
        self.assertEqual((row['lots_fresh'], row['lots_expired']), (0, 2))

    def test_sealed_pack_report_and_disable(self):
        date = (self.today + timedelta(days=200)).isoformat()
        _, body = self.call('POST', '/api/items/batch/',
                            {'type': 'compre', 'peremption': date, 'count': 3, 'sealed_pack': True, 'user': 'M001'})
        pack_id = body['sealed_pack']['id']
        badge = {'matricule': 'M001', 'key': self.user.new_key}
        code, body = self.call('POST', '/api/reports/', {'user': badge, 'pack': pack_id, 'usable': True,
                                                         'comment': 'Boîte écrasée'}, local=False)
        self.assertEqual(code, 201)
        self.assertEqual(body['report']['pack'], pack_id)
        self.assertEqual(body['report']['target'], f'Paquet de Compresses ({pack_id})')
        report_id = body['report']['id']
        # tous les items du paquet sont signales : popup au scan, avec le commentaire
        iid = Items.objects.filter(sealed_pack_id=pack_id).first().iid
        _, item = self.call('GET', f'/api/items/{iid}/', local=False)
        self.assertTrue(item['reported'])
        self.assertEqual(item['reports'][0]['comment'], 'Boîte écrasée')
        _, pack = self.call('GET', f'/api/packs/{pack_id}/', local=False)
        self.assertEqual((pack['reported'], pack['disabled']), (1, 0))
        # resolu : paquet desactive
        code, body = self.call('POST', f'/api/reports/{report_id}/', {'status': 'resolved', 'resolution': 'disabled'})
        self.assertEqual(code, 200)
        self.assertEqual(Items.objects.filter(sealed_pack_id=pack_id, disabled=True, reported=False).count(), 3)
        # desactivation directe d'un paquet (reactivation ici)
        code, body = self.disable({'pack': pack_id, 'disabled': False})
        self.assertEqual(body['count'], 3)
