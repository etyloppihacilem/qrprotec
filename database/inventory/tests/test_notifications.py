"""Tests du back : alertes et notifications web."""

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)


class AlertTests(ApiTestCase):
    """Alertes des admins (notifications web, l'API SMS n'est plus maintenue) : une alerte par blocage, par etape
    d'expiration et par commande. Les envois de webpush sont remplaces : self.sent garde (evenement, texte)."""

    def setUp(self):
        super().setUp()
        from .. import webpush
        self.sent = []
        self.reached = 1  # abonnements atteints par chaque envoi
        for name, record in (
                ('notify_event', lambda field, title, body, **web: self.record(field, body)),
                ('notify_pin_blocked', lambda text, url: self.record('pin_blocked', f'{text} {url}'))):
            self.addCleanup(setattr, webpush, name, getattr(webpush, name))
            setattr(webpush, name, record)

    def record(self, event, text):
        if self.reached:
            self.sent.append((event, text))
        return self.reached

    def test_settings_only_expose_warning_days(self):
        code, body = self.call('GET', '/api/notifications/')
        self.assertEqual(body, {'key_expiry_warning_days': 30})
        code, _ = self.call('GET', '/api/notifications/', local=False)
        self.assertEqual(code, 404)
        # API SMS plus maintenue : destinataires et SMS de test retires
        self.assertEqual(self.call('POST', '/api/notifications/recipients/',
                                   {'name': 'A', 'user': 'u1', 'password': 'p1'})[0], 404)
        self.assertEqual(self.call('POST', '/api/notifications/test/', {'user': 'M001'})[0], 404)

    def test_pin_blocked_notified_once_per_block(self):
        self.call('PATCH', '/api/users/M001/', {'pin': '123456', 'pin_contact': 'P001'})
        self.user.refresh_from_db()
        badge = {'matricule': 'M001', 'key': self.user.new_key}
        for _ in range(5):  # demandes repetees : une seule notification
            code, body = self.call('POST', '/api/pin-forgot/', badge, local=False)
            self.call('POST', '/api/auth/', {**badge, 'pin': '123456'}, local=False)
        self.assertTrue(body['pin_reset']['notified'])
        self.assertEqual(len(self.sent), 1)
        self.assertIn('Jeanne a oublié son code PIN', self.sent[0][1])
        self.assertIn('/pinreset?m=M001&t=', self.sent[0][1])
        # personne a prevenir : la tentative suivante reessaiera
        self.call('PATCH', '/api/users/M001/', {'pin': '123456'})  # nouveau PIN : debloque
        self.reached = 0
        self.sent.clear()
        code, body = self.call('POST', '/api/pin-forgot/', badge, local=False)
        self.assertEqual(self.sent, [])
        self.assertFalse(body['pin_reset']['notified'])
        self.reached = 1
        for _ in range(3):
            self.call('POST', '/api/auth/', {**badge, 'pin': '123456'}, local=False)
        self.assertEqual(len(self.sent), 1)

    def test_pin_blocked_after_failures_notifies(self):
        Secouristes.objects.create(matricule='M002', nom='Durand', prenom='Jeanne')  # homonyme : debut du nom
        self.call('PATCH', '/api/users/M001/', {'pin': '123456'})
        Secouristes.objects.filter(pk=self.user.pk).update(pin_failures_total=49)
        code, body = self.call('POST', '/api/auth/', {'matricule': 'M001', 'key': self.user.new_key, 'pin': '000000'},
                               local=False)
        self.assertTrue(body['pin_blocked'])
        self.assertFalse(body['pin_reset']['forgotten'])
        self.assertEqual(len(self.sent), 1)
        self.assertIn("Jeanne Dup. a son code PIN bloqué après trop d'essais", self.sent[0][1])
        self.assertNotIn('M001)', self.sent[0][1])

    def test_key_renewals(self):
        self.assertEqual(self.call('POST', f'/api/lots/{self.lot.id}/rotate-key/')[0], 200)
        self.assertEqual(self.call('POST', '/api/users/M001/renew-key/')[0], 200)
        self.assertEqual(self.sent, [
            ('lot_key_renewed', 'étiquette clé du lot Sac A renouvelée par Admin'),
            ('badge_renewed', 'badge de Jeanne renouvelé par Admin')])

    def test_key_expirations_once_per_stage(self):
        from django.core.management import call_command
        models.ServerSettings.objects.update_or_create(pk=1, defaults={'declared_identity': True})  # cles qui expirent
        Lots.objects.filter(pk=self.lot.pk).update(verif_key_expires=self.today + timedelta(days=10))
        Secouristes.objects.filter(pk=self.user.pk).update(key_expires=self.today - timedelta(days=1))
        call_command('check_alerts', stdout=StringIO())
        messages = [text for event, text in self.sent if event.endswith('_expiring')]
        day = (self.today + timedelta(days=10)).strftime('%d/%m/%Y')
        self.assertEqual(len(messages), 2)
        self.assertIn(f'étiquettes clés de lot qui expirent bientôt : Sac A ({day})', messages[0])
        self.assertIn('badges expirés : Jeanne (', messages[1])
        # deja annonce : rien de plus
        self.sent.clear()
        call_command('check_alerts', stdout=StringIO())
        self.assertEqual([event for event, _ in self.sent if event.endswith('_expiring')], [])
        # l'etiquette expire : nouvelle etape, une alerte
        Lots.objects.filter(pk=self.lot.pk).update(verif_key_expires=self.today - timedelta(days=1))
        call_command('check_alerts', stdout=StringIO())
        messages = [text for event, text in self.sent if event.endswith('_expiring')]
        self.assertEqual(len(messages), 1)
        self.assertIn('étiquettes clés de lot expirées : Sac A', messages[0])
        # renouvellement : l'alerte reviendra a la prochaine expiration
        self.call('POST', f'/api/lots/{self.lot.id}/rotate-key/')
        self.lot.refresh_from_db()
        self.assertEqual(self.lot.key_expiry_stage, 0)

    def test_order_due_once_per_order(self):
        from django.core.management import call_command
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
            call_command('check_alerts', stdout=StringIO())
            return [text for event, text in self.sent if event == 'order_due']

        messages = run()
        self.assertEqual(len(messages), 1)
        self.assertIn('commande à passer : Compresses : ', messages[0])
        self.assertTrue(ItemType.objects.get(pk='compre').order_notified)
        self.assertEqual(run(), [])  # deja annoncee
        # stock recu : plus de commande due, l'alerte reviendra a la prochaine commande
        self.create(self.compresses, self.today + timedelta(days=300), 200)
        self.assertEqual(run(), [])
        self.assertFalse(ItemType.objects.get(pk='compre').order_notified)

    def test_verif_problem(self):
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [], 'user': 'M001'})
        messages = [text for event, text in self.sent if event == 'verif_problem']
        self.assertEqual(len(messages), 1)
        self.assertIn('vérif du lot Sac A incomplète', messages[0])
        self.assertIn('manque 2 Compresses', messages[0])

    def test_expired_daily_summary(self):
        from django.core.management import call_command
        expired = self.create(self.compresses, self.today - timedelta(days=1), 2)
        Items.objects.filter(iid__in=[item.iid for item in expired]).update(location=self.lot)
        call_command('check_alerts', stdout=StringIO())
        self.assertIn(('expired_daily', 'lots contenant des périmés : Sac A (2)'), self.sent)

    def test_key_expiry_warning_days_setting(self):
        from django.core.management import call_command
        self.assertEqual(self.call('PATCH', '/api/notifications/', {'key_expiry_warning_days': 0})[0], 400)
        code, body = self.call('PATCH', '/api/notifications/', {'key_expiry_warning_days': 5})
        self.assertEqual(body['key_expiry_warning_days'], 5)
        models.ServerSettings.objects.update_or_create(pk=1, defaults={'declared_identity': True})  # cles qui expirent
        Lots.objects.filter(pk=self.lot.pk).update(verif_key_expires=self.today + timedelta(days=10))

        def lot_alerts():
            self.sent.clear()
            call_command('check_alerts', stdout=StringIO())
            return [text for event, text in self.sent if event == 'lot_key_expiring']

        self.assertEqual(lot_alerts(), [])  # 10 jours > 5 : pas encore
        self.call('PATCH', '/api/notifications/', {'key_expiry_warning_days': 15})
        self.assertEqual(len(lot_alerts()), 1)
        # vide : retour a la valeur de qrprotec.conf
        code, body = self.call('PATCH', '/api/notifications/', {'key_expiry_warning_days': None})
        self.assertEqual(body['key_expiry_warning_days'], 30)


@override_settings(QRPROTEC={**settings.QRPROTEC, 'NOTIFY_SYNC': True})
class WebPushTests(ApiTestCase):
    """Notifications web : chiffrement RFC 8291, VAPID, abonnement des admins et alertes de stock."""

    def setUp(self):
        super().setUp()
        from cryptography.hazmat.primitives.asymmetric import ec

        from .. import views, webpush
        self.webpush = webpush
        self.sent = []   # (endpoint, corps chiffre, en-tetes)
        self.reply = 201
        original = webpush.post_push
        webpush.post_push = lambda endpoint, body, headers: (self.sent.append((endpoint, body, headers)) or self.reply)
        self.addCleanup(setattr, webpush, 'post_push', original)
        self.admin = Secouristes(matricule='A001', nom='Ad', prenom='Min', role='admin')
        self.admin.renew_key()
        self.admin.set_pin('482100')
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

    def now(self, function, *args, **kwargs):
        """Appel dont les notifications partent tout de suite (envoyees a la validation de la transaction)."""
        with self.captureOnCommitCallbacks(execute=True):
            return function(*args, **kwargs)

    def test_server_problem_off_by_default_admins_only(self):
        from .. import notifications
        self.subscribe()
        self.assertFalse(models.PushSubscription.objects.get().server_problem)  # desactivee par defaut
        self.now(notifications.server_problem, 'backup', 'base introuvable')
        self.assertEqual(self.messages(), [])
        self.assertIn('server_problem', self.admin.push_role_types())
        self.assertNotIn('server_problem', Secouristes(role='gestion').push_role_types())

    def test_server_problem_throttled_then_resolved(self):
        from django.core.management import call_command

        from .. import notifications
        problem = lambda *args: self.now(notifications.server_problem, *args)
        self.subscribe(server_problem=True)
        self.now(call_command, 'server_problem', 'backup', 'base introuvable : /var/lib/qrprotec/db.sqlite3',
                 stdout=StringIO())
        [message] = self.messages()
        self.assertEqual(message['title'], 'QRProtec : problème de serveur, sauvegarde en échec')
        self.assertIn('base introuvable', message['body'])
        self.assertIn('journalctl -u qrprotec-backup', message['body'])
        # meme probleme qui dure : pas de nouvelle notification avant le rappel
        self.assertFalse(problem('backup', 'encore'))
        self.assertEqual(len(self.sent), 1)
        self.assertEqual(models.ServerProblem.objects.get(pk='backup').count, 2)
        # autre source : annoncee
        self.assertTrue(problem('sync', 'ssh: connect to host nas port 22: No route to host'))
        # sauvegarde reussie, puis nouvel echec : annonce tout de suite
        call_command('server_problem', 'backup', '--resolved', stdout=StringIO())
        self.assertTrue(problem('backup', 'disque plein'))
        self.assertEqual(len(self.sent), 3)
        # rappel apres le delai
        models.ServerProblem.objects.filter(pk='sync').update(
            notified=timezone.now() - notifications.SERVER_PROBLEM_REMINDER)
        self.assertTrue(problem('sync', 'toujours'))
        self.assertIn('2 erreurs depuis le', self.messages()[-1]['body'])

    def test_server_errors_in_the_log_notify(self):
        import logging
        self.subscribe(server_problem=True)
        # erreurs provoquees : pas de traces dans la sortie des tests, le handler server_problem reste branche
        with quiet_stderr(), self.captureOnCommitCallbacks(execute=True):
            try:
                raise RuntimeError('database is locked')
            except RuntimeError:
                logging.getLogger('django.request').error('Internal Server Error: /api/verifs/', exc_info=True)
            logging.getLogger('inventory.live').warning('simple avertissement')       # pas une erreur
            logging.getLogger('django.security.DisallowedHost').error('Invalid HTTP_HOST header')  # robots
        [message] = self.messages()
        self.assertEqual(message['title'], 'QRProtec : problème de serveur, erreur du serveur')
        self.assertIn('Internal Server Error: /api/verifs/ (RuntimeError : database is locked)', message['body'])
        self.assertIn('journalctl -u qrprotec', message['body'])

    def test_remote_front_reports_backup_failure(self):
        self.subscribe(server_problem=True)
        code, _ = self.call('POST', '/api/server-problem/', {'source': 'autre'})
        self.assertEqual(code, 400)
        code, body = self.now(self.call, 'POST', '/api/server-problem/', {'source': 'sync', 'message': 'rsync code 255'})
        self.assertEqual((code, body['sent']), (200, True))
        [message] = self.messages()
        self.assertIn('envoi des sauvegardes en échec', message['title'])
        self.assertIn('sur ce poste', message['body'])
        self.assertEqual(self.call('POST', '/api/server-problem/', {'source': 'sync'})[1]['sent'], False)
        self.call('POST', '/api/server-problem/', {'source': 'sync', 'resolved': True})
        self.assertFalse(models.ServerProblem.objects.exists())
        # API publique : refuse
        self.assertNotEqual(self.call('POST', '/api/server-problem/', {'source': 'sync'}, local=False)[0], 200)

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
        # envoi a la validation (sortie de captureOnCommitCallbacks) : assertLogs l'englobe
        with self.assertLogs('inventory.webpush', 'WARNING') as logs, self.captureOnCommitCallbacks(execute=True):
            self.call('GET', '/api/stock/')
        self.assertIn('Abonnement expiré', logs.output[0])
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

    def test_seal_opening_notified(self):
        self.subscribe()
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001', 'force': True})
        open_code = body['seal_open_url'].split('c=')[1]
        # etiquette d'ouverture scannee sans connexion : notification anonyme qui ouvre la fiche du lot
        with self.captureOnCommitCallbacks(execute=True):
            self.call('POST', f'/api/lots/{self.lot.id}/seal-open/', {'code': open_code}, local=False)
        [message] = self.messages()
        self.assertEqual(message['title'], 'QRProtec : scellé ouvert')
        self.assertIn(self.lot.name, message['body'])
        self.assertIn('Anonyme', message['body'])
        self.assertEqual((message['tag'], message['tab'], message['url']),
                         (f'seal-{self.lot.id}', 'lot', f'../verif?lot={self.lot.id}'))
        # connexion juste apres : la notification (meme tag) est remplacee par la version signee
        self.sent.clear()
        user = {'matricule': 'M001', 'key': self.user.new_key}
        with self.captureOnCommitCallbacks(execute=True):
            self.call('POST', f'/api/lots/{self.lot.id}/seal-open/', {'code': open_code, 'user': user}, local=False)
        [message] = self.messages()
        self.assertEqual(message['tag'], f'seal-{self.lot.id}')
        self.assertIn('Jeanne', message['body'])
        self.assertNotIn('Anonyme', message['body'])
        # type coupe sur l'appareil : plus rien
        self.webpush.PushSubscription.objects.update(seal_broken=False)
        self.sent.clear()
        self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001', 'force': True})
        with self.captureOnCommitCallbacks(execute=True):
            self.call('POST', f'/api/lots/{self.lot.id}/unseal/', {'user': 'M001'})
        self.assertEqual(self.sent, [])

    def test_device_manager(self):
        self.subscribe()
        endpoint = self.subscription()['endpoint']
        other = self.webpush.PushSubscription.objects.create(
            user=self.admin, endpoint='https://push.example.net/other', p256dh='x', auth='y', device='Firefox · Linux')
        code, body = self.call('POST', '/api/push/devices/', {'user': self.badge, 'endpoint': endpoint}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual([(row['device'], row['current']) for row in body['devices']],
                         [('Firefox · Linux', False), ('Navigateur', True)])
        self.assertEqual(len(body['types']), 14)
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
        self.assertEqual((body['devices'], len(body['types'])), ([], 9))

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
        self.assertIn('Jeanne a oublié son code PIN', message['body'])
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
