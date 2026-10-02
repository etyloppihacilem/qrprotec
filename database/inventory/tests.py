import importlib
import json
import math
from datetime import timedelta
from io import StringIO

from django.apps import apps as django_apps

from django.conf import settings
from django.test import TestCase, override_settings
from django.utils import timezone

from . import views
from .base62 import decode_base62
from . import forecast
from .models import (
    FrontKey, ItemMovement, ItemStatus, Items, ItemsPacks, ItemType, LotRequirements, Lots, LotType, MovementKind,
    Secouristes, Verifs,
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
        for url in ('/', '/verif', f'/verif?lot={self.lot.id}&key=x', '/badge?m=M001&key=x', '/pack?id=1'):
            response = self.client.get(url)
            self.assertEqual(response.status_code, 200, url)
            self.assertIn(b'web/app.js', b''.join(response.streaming_content))
            self.assertEqual(response['Referrer-Policy'], 'no-referrer')

    def test_assets_whitelist(self):
        self.assertEqual(self.client.get('/web/app.js').status_code, 200)
        self.assertEqual(self.client.get('/web/vendor/jsQR.js').status_code, 200)
        self.assertEqual(self.client.get('/web/../views.py').status_code, 404)
        self.assertEqual(self.client.get('/web/index.html').status_code, 404)

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
                               {'items': [item.iid for item in new], 'key': self.lot.verif_key, 'name': 'x'}, local=False)
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
        # liste des lots : tout badge valide, sans les cles
        code, lots = self.call('POST', '/api/lots/summary/', {'user': self.badge(self.user)}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(lots[0]['id'], self.lot.id)
        self.assertNotIn('verif_key', lots[0])
        code, _ = self.call('POST', '/api/lots/summary/', {'user': {'matricule': 'M001', 'key': 'faux'}}, local=False)
        self.assertEqual(code, 403)
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
                                 {'items': self.fresh() + [bottle], 'key': self.keys[self.bplus['id']], 'name': 'x'},
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
                   'items': self.fresh() + [bottle], 'name': 'x'}
        code, report = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 200)
        self.assertTrue(report['complete'])
        self.assertEqual(len(report['lots']), 2)
        self.assertEqual(Items.objects.get(iid=bottle).location_id, self.sac_o2['id'])
        # cle manquante pour un des lots
        payload['lots'][1]['key'] = 'faux'
        code, body = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 403)
        # la cle du lot global couvre ses sous-lots
        payload['lots'] = [{'id': self.sac_o2['id']}, {'id': self.bplus['id'], 'key': self.keys[self.bplus['id']]}]
        payload['items'] = [bottle]
        code, body = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 200)

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
        code, _ = self.call('POST', '/api/lot-types/', {'type': 'tiroir', 'name': 'Tiroir', 'storage': True})
        self.assertEqual(code, 201)
        code, armoire = self.call('POST', '/api/lots/', {'lot_type': 'tiroir', 'name': 'Armoire 1', 'user': 'M001'})
        code, tiroir = self.call('POST', '/api/lots/', {'lot_type': 'tiroir', 'name': 'Tiroir 3',
                                                         'parent': armoire['id'], 'user': 'M001'})
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
        self.assertEqual(messages, ['QRProtec : étiquette privée du lot Sac A renouvelée par Admin Poste',
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
        self.assertIn(f'étiquettes privées de lot qui expirent bientôt : Sac A ({day})', messages[0])
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
        self.assertIn('étiquettes privées de lot expirées : Sac A', self.sent[0][1])
        # renouvellement : l'alerte reviendra a la prochaine expiration
        self.call('POST', f'/api/lots/{self.lot.id}/rotate-key/')
        self.lot.refresh_from_db()
        self.assertEqual(self.lot.key_expiry_stage, 0)

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
        self.assertEqual(len(body['types']), 7)
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
        self.assertEqual((body['devices'], len(body['types'])), ([], 4))

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
