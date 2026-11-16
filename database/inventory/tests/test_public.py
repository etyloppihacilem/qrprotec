"""Tests du back : API publique, identite declaree, reglages web, sessions du poste."""

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)


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

    def test_badge_verif_needs_a_label_not_the_key(self):
        """Avec un badge, plus d'etiquette cle : l'etiquette (publique ou cle) d'un lot de la verif suffit."""
        items = self.create(self.compresses, self.today + timedelta(days=10), 2)
        payload = {'items': [item.iid for item in items], 'user': {'matricule': 'M001', 'key': self.user.new_key}}
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/verif/', payload, local=False)
        self.assertEqual(code, 403)
        self.assertTrue(body['label_required'])
        self.assertIn("Scannez l'étiquette", body['error'])
        self.assertIn(views.de_name(self.lot.name), body['error'])  # le nom du lot, pas « lot ou sous-lot »
        self.assertNotIn('sous-lot', body['error'])
        other = Lots.objects.create(lot_type=self.lot_type, name="Autre sac", created_by="test")
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/verif/', dict(payload, labels=[other.id]), local=False)
        self.assertEqual(code, 403)
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/verif/', dict(payload, labels=[self.lot.id]),
                               local=False)
        self.assertEqual(code, 200, body)
        self.assertTrue(body['complete'])
        # l'etiquette cle scannee compte comme etiquette du lot
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/verif/', dict(payload, key=self.lot.verif_key),
                            local=False)
        self.assertEqual(code, 200)

    def test_badge_restock_needs_a_label_not_the_key(self):
        items = self.create(self.compresses, self.today + timedelta(days=10), 1)
        payload = {'items': [items[0].iid], 'user': {'matricule': 'M001', 'key': self.user.new_key}}
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/add/', payload, local=False)
        self.assertEqual(code, 403)
        self.assertTrue(body['label_required'])
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/add/', dict(payload, labels=[self.lot.id]), local=False)
        self.assertEqual(code, 200, body)

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
        # badge obligatoire (defaut) : l'etiquette cle n'autorise rien de plus que la publique, elle n'expire plus
        code, body = self.call('GET', path + self.lot.verif_key, local=False)
        self.assertEqual(body['key_check'], 'valid')
        self.assertNotIn('key_error', body)
        # nom declare autorise : la cle est de nouveau une autorisation, sa date s'applique
        models.ServerSettings.objects.update_or_create(pk=1, defaults={'declared_identity': True})
        code, body = self.call('GET', path + self.lot.verif_key, local=False)
        self.assertEqual(body['key_check'], 'expired')
        self.assertIn('a expiré', body['key_error'])

    def test_lot_key_expiry_ignored_while_badge_required(self):
        from .. import notifications, tasks
        Lots.objects.filter(pk=self.lot.pk).update(verif_key_expires=self.today - timedelta(days=1))
        titles = lambda: [task['title'] for task in tasks._periodic_tasks(self.today)]
        self.assertEqual(notifications.check_key_expirations(self.today).get('lot_key_expiring', []), [])
        self.assertNotIn(f'Étiquette clé : {self.lot.name}', titles())
        models.ServerSettings.objects.update_or_create(pk=1, defaults={'declared_identity': True})
        self.assertEqual(len(notifications.check_key_expirations(self.today)['lot_key_expiring']), 1)
        self.assertIn(f'Étiquette clé : {self.lot.name}', titles())

    def test_missing_key_message_explains_label(self):
        # l'etiquette cle n'est plus exigee que sans badge, sous un nom declare (option du serveur)
        models.ServerSettings.objects.update_or_create(pk=1, defaults={'declared_identity': True})
        Lots.objects.filter(pk=self.lot.pk).update(key_location='poche intérieure du couvercle')
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [], 'name': 'Jean',
                                                                           'labels': [self.lot.id]}, local=False)
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

    def test_remote_front_maintenance(self):
        import tempfile
        from pathlib import Path

        remote = {'qrprotec.role': 'remote', 'REMOTE_ADDR': '127.0.0.1', 'HTTP_X_QRPROTEC_SESSION': self.operator_session}
        _, accueil = FrontKey.create('accueil')
        _, borne = FrontKey.create('borne')
        with tempfile.TemporaryDirectory() as folder, override_settings(
                QRPROTEC={**settings.QRPROTEC, 'MAINTENANCE_DIR': folder}):
            # hors maintenance (pas de page) : le fichier des fronts admis ne change rien
            (Path(folder) / 'fronts').write_text('borne\n')
            self.assertEqual(self.client.get('/api/stock/', HTTP_X_QRPROTEC_KEY=accueil, **remote).status_code, 200)
            (Path(folder) / 'index.html').write_text('maintenance')
            with self.assertLogs('django.request', 'ERROR'):  # 503 journalise par Django
                response = self.client.get('/api/stock/', HTTP_X_QRPROTEC_KEY=accueil, **remote)
            self.assertEqual(response.status_code, 503)
            self.assertTrue(response.json()['maintenance'])
            self.assertEqual(self.client.get('/api/stock/', HTTP_X_QRPROTEC_KEY=borne, **remote).status_code, 200)
            # une cle invalide reste refusee comme avant, maintenance ou pas
            self.assertEqual(self.client.get('/api/stock/', HTTP_X_QRPROTEC_KEY='qrpf_faux', **remote).status_code, 401)
            (Path(folder) / 'fronts').write_text('*\n')
            self.assertEqual(self.client.get('/api/stock/', HTTP_X_QRPROTEC_KEY=accueil, **remote).status_code, 200)
            (Path(folder) / 'fronts').unlink()
            with self.assertLogs('django.request', 'ERROR'):
                self.assertEqual(self.client.get('/api/stock/', HTTP_X_QRPROTEC_KEY=borne, **remote).status_code, 503)
            # l'API locale du poste ne passe jamais par la maintenance
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

    def test_front_config_from_poste(self):
        """Poste, admin : cle + fichier chiffre telechargeable 5 minutes par le lien du QR code."""
        from datetime import timedelta
        from unittest import mock

        from inventory import front_config
        from inventory.models import FrontConfigDownload

        with mock.patch.object(front_config, 'server_ca_pem', return_value='-----BEGIN CERTIFICATE-----\nX\n'):
            code, body = self.call('POST', '/api/front-configs/', {'name': 'accueil'})
        self.assertEqual(code, 201, body)
        password = body['password']  # genere : 3 groupes de 4
        self.assertRegex(password, r'^[a-z2-9]{4}-[a-z2-9]{4}-[a-z2-9]{4}$')
        self.assertTrue(body['ca'])
        path = body['url'][body['url'].index('/front-config/'):]
        # telechargement par l'API publique, sans connexion
        response = self.client.get(path)
        self.assertEqual(response.status_code, 200)
        self.assertIn('accueil.qrprotec', response['Content-Disposition'])
        payload = front_config.decrypt(response.content.decode(), password)
        self.assertEqual(payload['api_ca_pem'], '-----BEGIN CERTIFICATE-----\nX\n')
        self.assertIsNotNone(FrontKey.authenticate(payload['api_key']))
        self.assertEqual(self.client.get(path).status_code, 200)  # plusieurs fois pendant 5 minutes
        # nom deja pris : 409 sans replace ; mot de passe choisi, pas renvoye
        code, body = self.call('POST', '/api/front-configs/', {'name': 'accueil', 'password': 'mot-de-passe'})
        self.assertEqual((code, body.get('exists')), (409, True))
        with mock.patch.object(front_config, 'server_ca_pem', return_value=''):
            code, body = self.call('POST', '/api/front-configs/',
                                   {'name': 'accueil', 'password': 'mot-de-passe', 'replace': True})
        self.assertEqual((code, body['password'], body['ca'], body['replaced']), (201, '', False, True))
        self.assertIsNone(FrontKey.authenticate(payload['api_key']))  # ancienne cle remplacee
        self.assertEqual(self.client.get(path).status_code, 404)  # ancien lien supprime avec l'ancienne cle
        # expire apres 5 minutes
        new_path = body['url'][body['url'].index('/front-config/'):]
        FrontConfigDownload.objects.update(expires=timezone.now() - timedelta(seconds=1))
        self.assertEqual(self.client.get(new_path).status_code, 404)
        self.assertFalse(FrontConfigDownload.objects.exists())
        # liste ; refus : pas admin, personne de connecte, nom invalide, front distant
        code, body = self.call('GET', '/api/front-configs/')
        self.assertEqual([front['name'] for front in body['fronts']], ['accueil'])
        self.assertEqual(self.call('POST', '/api/front-configs/', {'name': 'b'}, session=False)[0], 403)
        self.assertEqual(self.call('POST', '/api/front-configs/', {'name': 'avec espace'})[0], 400)
        self.assertEqual(self.call('POST', '/api/front-configs/', {'name': 'b', 'password': 'court'})[0], 400)
        _, key = FrontKey.create('distant')
        response = self.client.post('/api/front-configs/', json.dumps({'name': 'b'}), content_type='application/json',
                                    HTTP_X_QRPROTEC_KEY=key, HTTP_X_QRPROTEC_SESSION=self.operator_session,
                                    **{'qrprotec.role': 'remote', 'REMOTE_ADDR': '127.0.0.1'})
        self.assertEqual(response.status_code, 403)
        self.assertFalse(FrontKey.objects.filter(name='b').exists())
        self.assertEqual(self.client.post('/api/front-configs/', {'name': 'b'}).status_code, 404)  # API publique

    def test_front_config_revoke_restore_delete(self):
        """Onglet Ordinateurs connectes : revoquer (lien en cours coupe), reactiver, supprimer, journal."""
        from unittest import mock

        from inventory import front_config
        from inventory.models import FrontConfigDownload, Operation

        with mock.patch.object(front_config, 'server_ca_pem', return_value=''):
            code, body = self.call('POST', '/api/front-configs/', {'name': 'accueil'})
        path = body['url'][body['url'].index('/front-config/'):]
        key = front_config.decrypt(self.client.get(path).content.decode(), body['password'])['api_key']
        code, listing = self.call('GET', '/api/front-configs/')
        self.assertTrue(listing['fronts'][0]['download_pending'])
        self.assertEqual(self.call('POST', '/api/front-configs/accueil/', {'action': 'revoke'})[0], 200)
        self.assertIsNone(FrontKey.authenticate(key))
        self.assertFalse(FrontConfigDownload.objects.exists())
        self.assertEqual(self.client.get(path).status_code, 404)
        code, listing = self.call('GET', '/api/front-configs/')
        self.assertEqual((listing['fronts'][0]['revoked'], listing['fronts'][0]['download_pending']), (True, False))
        self.assertEqual(self.call('POST', '/api/front-configs/accueil/', {'action': 'restore'})[0], 200)
        self.assertIsNotNone(FrontKey.authenticate(key))
        self.assertEqual(self.call('POST', '/api/front-configs/accueil/', {'action': 'x'})[0], 400)
        self.assertEqual(self.call('POST', '/api/front-configs/accueil/', {'action': 'delete'}, session=False)[0], 403)
        self.assertEqual(self.call('POST', '/api/front-configs/accueil/', {'action': 'delete'})[0], 200)
        self.assertFalse(FrontKey.objects.exists())
        self.assertEqual(self.call('POST', '/api/front-configs/accueil/', {'action': 'delete'})[0], 404)
        summaries = list(Operation.objects.filter(kind='settings').values_list('summary', flat=True))
        self.assertIn('ordinateur « accueil » révoqué', summaries)
        self.assertIn('ordinateur « accueil » supprimé', summaries)

    def remote_call(self, method, path, key, data=None):
        """Requete d'un front distant (API distante, cle de front), l'admin du test connecte."""
        extra = {'HTTP_X_QRPROTEC_KEY': key, 'HTTP_X_QRPROTEC_SESSION': self.operator_session,
                 'qrprotec.role': 'remote', 'REMOTE_ADDR': '127.0.0.1'}
        if method == 'GET':
            response = self.client.get(path, **extra)
        else:
            response = self.client.post(path, json.dumps(data or {}), content_type='application/json', **extra)
        return response.status_code, response.json()

    def test_main_poste_key(self):
        """Back sans kiosk : la cle marquee « poste principal » a les droits du front de l'API locale (ordinateurs
        connectes, impressions en attente), une simple cle non ; le poste ne se coupe pas lui-meme l'acces."""
        from io import StringIO
        from unittest import mock

        from django.core.management import call_command

        from inventory import front_config

        _, plain = FrontKey.create('bureau')
        out = StringIO()
        call_command('frontkey', 'add', 'borne', '--poste', stdout=out)
        poste = next(word for word in out.getvalue().split() if word.startswith('qrpf_'))
        self.assertTrue(FrontKey.objects.get(name='borne').poste)
        self.assertTrue(self.call('GET', '/api/health/')[1]['poste'])  # API locale : toujours poste principal
        self.assertTrue(self.remote_call('GET', '/api/health/', poste)[1]['poste'])
        self.assertFalse(self.remote_call('GET', '/api/health/', plain)[1]['poste'])
        self.assertEqual(self.remote_call('GET', '/api/front-configs/', plain)[0], 403)
        code, listing = self.remote_call('GET', '/api/front-configs/', poste)
        self.assertEqual(code, 200)
        rows = {front['name']: front for front in listing['fronts']}
        self.assertEqual((rows['borne']['poste'], rows['borne']['self'], rows['bureau']['poste']), (True, True, False))
        # creation depuis le poste distant, d'un autre poste principal
        with mock.patch.object(front_config, 'server_ca_pem', return_value=''):
            code, body = self.remote_call('POST', '/api/front-configs/', poste, {'name': 'secours', 'poste': True})
        self.assertEqual((code, body['poste']), (201, True))
        self.assertTrue(FrontKey.objects.get(name='secours').poste)
        # droits donnes et retires ; le poste ne se revoque, ne se supprime ni ne se retire ses droits
        self.assertEqual(self.remote_call('POST', '/api/front-configs/bureau/', poste, {'action': 'poste'})[0], 200)
        self.assertTrue(FrontKey.objects.get(name='bureau').poste)
        self.assertEqual(self.remote_call('POST', '/api/front-configs/bureau/', poste, {'action': 'unposte'})[0], 200)
        self.assertFalse(FrontKey.objects.get(name='bureau').poste)
        for action in ('revoke', 'delete', 'unposte'):
            self.assertEqual(self.remote_call('POST', '/api/front-configs/borne/', poste, {'action': action})[0], 409)
        self.assertEqual(self.remote_call('POST', '/api/front-configs/borne/', plain, {'action': 'delete'})[0], 403)
        # en ligne de commande
        call_command('frontkey', 'poste', 'borne', '--off', stdout=StringIO())
        self.assertFalse(FrontKey.objects.get(name='borne').poste)
        self.assertEqual(self.remote_call('GET', '/api/front-configs/', poste)[0], 403)
        call_command('frontkey', 'poste', 'borne', stdout=StringIO())
        out = StringIO()
        call_command('frontkey', 'list', stdout=out)
        self.assertIn('poste principal', out.getvalue())

    def test_server_ca_pem(self):
        import tempfile
        from pathlib import Path

        from inventory import front_config

        self.assertEqual(front_config.server_ca_pem('https://x.org', 'admin@x.org'), '')  # Let's Encrypt
        with tempfile.TemporaryDirectory() as directory:
            ca = Path(directory) / 'ca.pem'
            ca.write_text('-----BEGIN CERTIFICATE-----\nY\n')
            self.assertEqual(front_config.server_ca_pem('https://x.org', '', str(ca)), ca.read_text())

    def test_frontkey_config_file(self):
        """frontkey add --config : URL, cle et certificat chiffres en AES, relus avec le mot de passe."""
        import json
        import os
        import tempfile
        from io import StringIO
        from pathlib import Path
        from unittest import mock

        from django.core.management import CommandError, call_command

        from inventory import front_config

        pem = '-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n'
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.dict(os.environ, {'QRPROTEC_FRONT_CONFIG_PASSWORD': 'secret-antenne'}):
            ca = Path(directory) / 'root.crt'
            ca.write_text(pem)
            path = Path(directory) / 'accueil.qrprotec'
            out = StringIO()
            call_command('frontkey', 'add', 'accueil', '--config', str(path), '--url', 'https://inv.example.org/',
                         '--ca-file', str(ca), stdout=out)
            self.assertNotIn('qrpf_', out.getvalue())  # la cle n'est que dans le fichier chiffre
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            text = path.read_text()
            self.assertNotIn('qrpf_', text)
            self.assertNotIn('inv.example.org', text)
            document = json.loads(text)
            self.assertEqual((document['qrprotec'], document['cipher']), ('front-config', 'AES-256-GCM'))
            payload = front_config.decrypt(text, 'secret-antenne')
            self.assertEqual(payload['name'], 'accueil')
            self.assertEqual(payload['api_url'], 'https://inv.example.org')
            self.assertEqual(payload['api_ca_pem'], pem)
            self.assertIsNotNone(FrontKey.authenticate(payload['api_key']))
            with self.assertRaisesMessage(front_config.FrontConfigError, 'Mot de passe incorrect'):
                front_config.decrypt(text, 'mauvais-mot-de-passe')
            tampered = dict(document, iterations=document['iterations'] + 1)
            with self.assertRaises(front_config.FrontConfigError):
                front_config.decrypt(json.dumps(tampered), 'secret-antenne')
            # fichier deja la : refuse sans --replace, et l'ancienne cle reste valable
            with self.assertRaises(CommandError):
                call_command('frontkey', 'add', 'accueil', '--config', str(path), stdout=StringIO())
            self.assertIsNotNone(FrontKey.authenticate(payload['api_key']))
            # sortie standard : seulement le fichier, les explications sur l'erreur standard
            out = StringIO()
            call_command('frontkey', 'add', 'accueil', '--replace', '--config', '-', '--url', 'https://inv.example.org',
                         stdout=out, stderr=StringIO())
            new_payload = front_config.decrypt(out.getvalue(), 'secret-antenne')
            self.assertNotIn('api_ca_pem', new_payload)
            self.assertIsNone(FrontKey.authenticate(payload['api_key']))
            self.assertIsNotNone(FrontKey.authenticate(new_payload['api_key']))
            # front distant en HTTPS seulement ; mot de passe trop court
            with self.assertRaises(CommandError):
                call_command('frontkey', 'add', 'b', '--config', '-', '--url', 'http://inv.example.org',
                             stdout=StringIO())
        with mock.patch.dict(os.environ, {'QRPROTEC_FRONT_CONFIG_PASSWORD': 'court'}), \
                self.assertRaises(CommandError):
            call_command('frontkey', 'add', 'b', '--config', '-', '--url', 'https://x.org', stdout=StringIO())
        self.assertFalse(FrontKey.objects.filter(name='b').exists())


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


class WebPrefsTests(ApiTestCase):
    """Reglages du front web (explication de la vérif) : par l'utilisateur et par un admin au poste."""

    def my_prefs(self, prefs, user=None):
        user = user or self.user
        credentials = {'matricule': user.matricule, 'key': user.new_key, 'session': views.session_token(user)}
        return self.call('POST', '/api/me/prefs/', {'user': credentials, 'web_prefs': prefs}, local=False)

    def test_defaults_user_and_admin(self):
        code, body = self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.new_key}, local=False)
        self.assertEqual(body['web_prefs'], {'verif_help_hidden': False, 'scan_bubble': False})
        # l'utilisateur coche « ne plus afficher ce message » : les autres reglages restent tels quels
        code, body = self.my_prefs({'verif_help_hidden': True})
        self.assertEqual((code, body['web_prefs']), (200, {'verif_help_hidden': True, 'scan_bubble': False}))
        code, body = self.my_prefs({'scan_bubble': True})
        self.assertEqual(body['web_prefs'], {'verif_help_hidden': True, 'scan_bubble': True})
        # l'ancien reglage « ligne d'info pendant la vérif » n'existe plus : refuse, et ignore s'il est deja en base
        self.assertEqual(self.my_prefs({'verif_info_line': True})[0], 400)
        # l'ancien reglage « Bulle de connexion » non plus (la grande fenetre de connexion est toujours utilisee)
        self.assertEqual(self.my_prefs({'login_bubble': True})[0], 400)
        self.user.refresh_from_db()
        self.user.web_prefs = {**self.user.web_prefs, 'verif_info_line': True}
        self.user.save(update_fields=['web_prefs'])
        # reglage inconnu ou valeur non booleenne : refuse
        self.assertEqual(self.my_prefs({'inconnu': True})[0], 400)
        self.assertEqual(self.my_prefs({'scan_bubble': 'oui'})[0], 400)
        self.assertEqual(self.call('POST', '/api/me/prefs/', {'web_prefs': {'scan_bubble': True}}, local=False)[0], 403)
        # un admin reaffiche l'explication depuis le poste (fenetre Utilisateurs), journalise
        code, body = self.call('PATCH', '/api/users/M001/', {'web_prefs': {'verif_help_hidden': False}})
        self.assertEqual((code, body['web_prefs']), (200, {'verif_help_hidden': False, 'scan_bubble': True}))
        self.assertEqual(self.call('PATCH', '/api/users/M001/', {'web_prefs': ['x']})[0], 400)


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

    def test_admin_sets_user_volume(self):
        m = self.user.matricule
        row = next(user for user in self.call('GET', '/api/users/')[1] if user['matricule'] == m)
        self.assertIsNone(row['scanner_volume'])  # defaut du serveur
        code, row = self.call('PATCH', f'/api/users/{m}/', {'scanner_volume': 2})
        self.assertEqual((code, row['scanner_volume']), (200, 2))
        self.assertEqual(self.login(self.user)['scanner_volume'], 2)
        self.assertEqual(self.call('PATCH', f'/api/users/{m}/', {'scanner_volume': 5})[0], 400)
        session = views.session_token(self.user)
        self.assertEqual(self.call('PATCH', f'/api/users/{m}/', {'scanner_volume': 0}, session=session)[0], 403)
        code, row = self.call('PATCH', f'/api/users/{m}/', {'scanner_volume': None})
        self.assertIsNone(row['scanner_volume'])
        self.assertEqual(self.login(self.user)['scanner_volume'], 4)


class PublicNamesTests(ApiTestCase):
    """API publique : prenom seul, plus le debut du nom qui distingue les homonymes de meme prenom."""

    def test_short_names(self):
        people = [(1, 'Michel', 'Dubois'), (2, 'Julien', 'Dubois'), (3, 'Julien', 'Saxe')]
        self.assertEqual(idendity.short_names(people), {1: 'Michel', 2: 'Julien D.', 3: 'Julien S.'})
        people.append((4, 'julien', 'Deraine'))
        self.assertEqual(idendity.short_names(people),
                         {1: 'Michel', 2: 'Julien Du.', 3: 'Julien S.', 4: 'julien De.'})
        # accents et casse ignores ; homonymes complets et noms contenus dans un autre : jamais le nom entier
        self.assertEqual(idendity.short_names([(1, 'Émile', 'Roux'), (2, 'Emile', 'rousseau'),
                                               (3, 'Ana', 'Lima'), (4, 'Ana', 'Lima')]),
                         {1: 'Émile Rou.', 2: 'Emile rous.', 3: 'Ana Lim.', 4: 'Ana Lim.'})
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
