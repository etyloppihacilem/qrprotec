import json
from datetime import timedelta

from django.conf import settings
from django.test import TestCase, override_settings
from django.utils import timezone

from .base62 import decode_base62
from .models import ItemStatus, Items, ItemsPacks, ItemType, LotRequirements, Lots, LotType, Secouristes

LOCAL = {'qrprotec.role': 'local'}


class ApiTestCase(TestCase):
    def call(self, method, path, data=None, local=True):
        extra = LOCAL if local else {}
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
        payload['user'] = {'matricule': 'M001', 'key': self.user.key}
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
        code, body = self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.key}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(body['prenom'], 'Jeanne')
        code, _ = self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': 'x'}, local=False)
        self.assertEqual(code, 403)
        self.user.key_expires = self.today - timedelta(days=1)
        self.user.save()
        code, _ = self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.key}, local=False)
        self.assertEqual(code, 403)

    @override_settings(QRPROTEC={**__import__('django.conf').conf.settings.QRPROTEC, 'LOCAL_API_TOKEN': 'secret'})
    def test_local_token(self):
        code, _ = self.call('GET', '/api/stock/')
        self.assertEqual(code, 403)
        response = self.client.get('/api/stock/', HTTP_X_QRPROTEC_TOKEN='secret', **LOCAL)
        self.assertEqual(response.status_code, 200)


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


class SetupTests(ApiTestCase):
    def test_setup_and_createadmin(self):
        from io import StringIO
        from django.core.management import call_command
        code, body = self.call('GET', '/api/setup/')
        self.assertTrue(body['needs_admin'])
        self.assertEqual(self.call('GET', '/api/setup/', local=False)[0], 404)
        out = StringIO()
        call_command('createadmin', 'R001', 'Melica', 'Hippolyte', stdout=out)
        self.assertIn('badge?m=R001&key=', out.getvalue())
        code, body = self.call('GET', '/api/setup/')
        self.assertFalse(body['needs_admin'])
        admin = Secouristes.objects.get(matricule='R001')
        self.assertTrue(admin.privileged)
        old_key = admin.key
        call_command('createadmin', 'R001', stdout=StringIO())
        self.assertNotEqual(Secouristes.objects.get(matricule='R001').key, old_key)


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
