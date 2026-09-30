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
        self.assertEqual(admin.role, 'admin')
        old_key = admin.key
        call_command('createadmin', 'R001', stdout=StringIO())
        self.assertNotEqual(Secouristes.objects.get(matricule='R001').key, old_key)


class PinTests(ApiTestCase):
    def auth(self, user, **extra):
        return self.call('POST', '/api/auth/', {'matricule': user.matricule, 'key': user.key, **extra}, local=False)

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
        badge = {'matricule': 'A001', 'key': admin.key}
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
        return {'matricule': user.matricule, 'key': user.key}

    def test_roles_and_public_summaries(self):
        code, body = self.call('POST', '/api/users/', {'matricule': 'G001', 'nom': 'Gest', 'prenom': 'Ion',
                                                       'role': 'gestion'})
        self.assertEqual(code, 201)
        self.assertEqual(body['role'], 'gestion')
        self.assertTrue(body['privileged'])
        code, _ = self.call('POST', '/api/users/', {'matricule': 'X001', 'nom': 'a', 'prenom': 'b', 'role': 'chef'})
        self.assertEqual(code, 400)
        gestion = Secouristes.objects.get(matricule='G001')
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
