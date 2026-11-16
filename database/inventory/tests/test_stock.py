"""Tests du back : stock, reassort, mouvements, types, consommables."""

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)


class LostObjectTests(ApiTestCase):
    def test_lost_object_fiche(self):
        laptop = ItemType.objects.create(type='ordina', name='Ordinateur', consumable=False)
        item = self.create(laptop, None, 1)[0]
        Items.objects.filter(iid=item.iid).update(location=self.lot)
        # type ordinaire : dernier emplacement, pas de message
        code, body = self.call('GET', f'/api/items/{item.iid}/', local=False)
        self.assertFalse(body['lost_object'])
        self.assertNotIn('lost_message', body)
        self.assertEqual(body['last_location']['id'], self.lot.id)
        # case cochee sur le type (gestion) : message commun, pas de lot
        code, body = self.call('PATCH', '/api/item-types/ordina/', {'lost_object': True})
        self.assertEqual(code, 200, body)
        self.assertTrue(body['lost_object'])
        code, body = self.call('GET', f'/api/items/{item.iid}/', local=False)
        self.assertTrue(body['lost_object'])
        self.assertIsNone(body['last_location'])
        self.assertIn('secouristes', body['lost_message'])  # message par defaut
        # message modifie (le meme pour tous les objets)
        code, body = self.call('PATCH', '/api/lost-object-message/', {'message': '  Appelez le 01 23 45 67 89  '})
        self.assertEqual(code, 200, body)
        self.assertEqual(body['message'], 'Appelez le 01 23 45 67 89')
        self.assertEqual(self.call('GET', '/api/lost-object-message/')[1]['message'], 'Appelez le 01 23 45 67 89')
        code, body = self.call('GET', f'/api/items/{item.iid}/', local=False)
        self.assertEqual(body['lost_message'], 'Appelez le 01 23 45 67 89')
        self.assertEqual(self.call('PATCH', '/api/lost-object-message/', {'message': 'x' * 2001})[0], 400)
        # modification reservee a la gestion
        self.assertEqual(self.call('PATCH', '/api/lost-object-message/', {'message': 'a'}, session=False)[0], 403)
        # creation directe avec la case
        code, body = self.call('POST', '/api/item-types/', {'type': 'tablet', 'name': 'Tablette', 'lost_object': True})
        self.assertEqual(code, 201, body)
        self.assertTrue(body['lost_object'])


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

    def test_restock_setting(self):
        # reglage du serveur coche par defaut : tout reassort recommande la verif (test ci-dessus)
        code, body = self.call('GET', '/api/server-settings/')
        self.assertTrue(body['restock_verif_recommended'])
        self.assertEqual((body['restock_credit_outings'], body['restock_credit_days']), (3, 15))
        self.assertTrue(self.call('GET', '/api/health/')[1]['restock_verif_recommended'])
        self.assertEqual(self.call('PATCH', '/api/server-settings/', {'restock_credit_days': 'x'})[0], 400)
        # decoche : credit de confiance (3 sorties / 15 jours depuis la derniere verif complete)
        code, body = self.call('PATCH', '/api/server-settings/', {'restock_verif_recommended': False})
        self.assertEqual(code, 200, body)
        self.assertFalse(body['restock_verif_recommended'])
        Lots.objects.filter(id=self.lot.id).update(last_verif=timezone.now(), verif_recommended=False, outings=0)

        def lot():
            return self.call('GET', f'/api/lots/{self.lot.id}/', local=False)[1]

        def used_then_restock():
            code, body = self.call('POST', f'/api/lots/{self.lot.id}/used/', {'user': 'M001'})
            self.assertEqual(code, 200, body)
            self.assertTrue(lot()['verif_recommended'])  # rentre sans reassort : orange
            new = self.create(self.compresses, self.today + timedelta(days=90), 1)
            code, body = self.call('POST', f'/api/lots/{self.lot.id}/add/',
                                   {'items': [item.iid for item in new], 'user': 'M001'})
            self.assertEqual(code, 200, body)
            return lot()

        self.assertEqual(lot()['restock_credit']['outings_left'], 3)
        for left in (2, 1, 0):
            row = used_then_restock()
            self.assertFalse(row['verif_recommended'])  # reassorti sous le credit : vert
            self.assertEqual(row['restock_credit']['outings_left'], left)
        row = used_then_restock()  # 4e sortie : credit epuise
        self.assertTrue(row['verif_recommended'])
        self.assertEqual(row['restocked_count'], 1)
        self.assertIsNone(row['restock_credit'])
        # une verif complete remet le credit a zero sortie
        items = [item.iid for item in Items.objects.filter(location=self.lot, status=ItemStatus.ACTIVE)]
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': items, 'user': 'M001'})
        self.assertEqual(Lots.objects.get(id=self.lot.id).outings, 0)
        # plus de 15 jours depuis la derniere verif : verif recommandee meme sans sortie
        Lots.objects.filter(id=self.lot.id).update(last_verif=timezone.now() - timedelta(days=16),
                                                   verif_recommended=False)
        new = self.create(self.compresses, self.today + timedelta(days=90), 1)
        self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [item.iid for item in new], 'user': 'M001'})
        self.assertTrue(lot()['verif_recommended'])


class MoveItemsRulesTests(ApiTestCase):
    """Reassort (move_items) : items supprimes, remplaces, perimes ou d'un autre lot refuses (audit, point 7), sauf
    options de debug d'un admin au poste."""

    def add(self, items, lot=None, local=True, **payload):
        lot = lot or self.lot
        return self.call('POST', f'/api/lots/{lot.id}/add/', {'items': [item.iid for item in items], **payload},
                         local=local)

    def test_deleted_replaced_and_expired_refused(self):
        fresh, deleted, replaced = self.create(self.compresses, self.today + timedelta(days=90), 3)
        Items.objects.filter(iid=deleted.iid).update(status=ItemStatus.DELETED)
        Items.objects.filter(iid=replaced.iid).update(status=ItemStatus.REPLACED)
        old = self.create(self.compresses, self.today - timedelta(days=1), 1)[0]
        code, body = self.add([fresh, deleted, replaced, old])
        self.assertEqual(code, 200)
        self.assertEqual(body['moved'], [fresh.iid])
        self.assertEqual({row['iid'] for row in body['refused']}, {deleted.iid, replaced.iid, old.iid})
        self.assertEqual(Items.objects.get(iid=deleted.iid).status, ItemStatus.DELETED)
        self.assertIsNone(Items.objects.get(iid=old.iid).location_id)
        # rien a deplacer : refus en entier, avec les raisons
        code, body = self.add([deleted])
        self.assertEqual(code, 400)
        self.assertIn('supprimé', body['error'])
        # un perime retourne en stock sans probleme (il y attend d'etre jete), pas un supprime
        Items.objects.filter(iid=old.iid).update(location=self.lot)
        code, body = self.call('POST', '/api/items/to-stock/', {'items': [old.iid, deleted.iid]})
        self.assertEqual((code, body['moved']), (200, [old.iid]))

    def test_declared_name_only_from_stock_or_own_lot(self):
        self.call('PATCH', '/api/server-settings/', {'declared_identity': True})
        other = Lots(lot_type=self.lot_type, name='Sac B', name_short='B', created_by='test')
        other.save()
        in_stock, elsewhere, here = self.create(self.compresses, self.today + timedelta(days=90), 3)
        Items.objects.filter(iid=elsewhere.iid).update(location=other)
        Items.objects.filter(iid=here.iid).update(location=self.lot)
        code, body = self.add([in_stock, elsewhere, here], local=False, key=self.lot.verif_key, name='Jean')
        self.assertEqual(code, 200)
        self.assertEqual(sorted(body['moved']), sorted([in_stock.iid, here.iid]))
        self.assertEqual([row['iid'] for row in body['refused']], [elsewhere.iid])
        self.assertEqual(Items.objects.get(iid=elsewhere.iid).location_id, other.id)
        # avec un badge, l'item d'un autre lot peut etre deplace
        code, body = self.add([elsewhere], local=False, key=self.lot.verif_key,
                              user={'matricule': 'M001', 'key': self.user.new_key})
        self.assertEqual((code, body['moved']), (200, [elsewhere.iid]))

    def test_debug_options_for_admins_on_front(self):
        old = self.create(self.compresses, self.today - timedelta(days=1), 1)[0]
        deleted = self.create(self.compresses, self.today + timedelta(days=90), 1)[0]
        Items.objects.filter(iid=deleted.iid).update(status=ItemStatus.DELETED)
        # pas un admin (gestion), ou pas au poste : refuse, pas ignore
        manager = Secouristes(matricule='G001', nom='Gest', prenom='Ion', role='gestion')
        manager.renew_key()
        manager.save()
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [old.iid], 'debug': True},
                            session=views.session_token(manager))
        self.assertEqual(code, 403)
        code, _ = self.add([old], local=False, debug=True, key=self.lot.verif_key,
                           user={'matricule': 'M001', 'key': self.user.new_key})
        self.assertEqual(code, 403)
        # admin au poste : lot inacceptable en vrai, construit pour tester
        code, body = self.add([old, deleted], debug=True)
        self.assertEqual(code, 200)
        self.assertEqual(sorted(body['moved']), sorted([old.iid, deleted.iid]))
        self.assertEqual(body['refused'], [])
        self.assertEqual(Items.objects.get(iid=old.iid).location_id, self.lot.id)


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
        # perime dans le lot (il l'est devenu sur place : un reassort refuse un item perime)
        Items.objects.filter(iid=old.iid).update(location=self.lot)
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
        # J+45 tombe dans le 2e ou le 3e mois du calendrier selon le jour du mois courant
        self.assertEqual(sum(month['expiring']['sealed'] for month in row['calendar']), 2)
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
        # en clair, dans l'ordre : sortir celui qui perime, ranger la ou il servira, reprendre un plus recent
        self.assertEqual(len(move['steps']), 2)
        self.assertTrue(move['steps'][0].startswith(f"Sortir de « {sealed.name} » 2 "))
        self.assertIn(f"à ranger dans « {self.lot.name} »", move['steps'][0])
        self.assertTrue(move['steps'][1].startswith(f"Prendre de « {self.lot.name} » 2 "))
        self.assertIn(f"à mettre dans « {sealed.name} » à la place", move['steps'][1])
        self.assertIn('ces items périmeraient sans servir', move['reason'])
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


class ConsumableTests(ApiTestCase):
    """Consommables d'etiquettes : references, receptions, inventaires, stock estime d'apres les impressions."""

    def setUp(self):
        super().setUp()
        self.vellum = self.ref({'kind': 'labels', 'name': 'Vélin 40x30', 'supplier': 'Fournisseur', 'width_mm': 40,
                                'height_mm': 30, 'support': 'direct', 'per_roll': 2000, 'per_box': 12,
                                'box_price': '57.00'})
        self.ribbon = self.ref({'kind': 'ribbon', 'name': 'Cire-résine 57 mm', 'length_m': 74, 'per_box': 25,
                                'box_price': '62.50'})

    def ref(self, data):
        code, body = self.call('POST', '/api/consumables/refs/', data)
        self.assertEqual(code, 201, body)
        return next(ref for ref in body['refs'] if ref['name'] == data['name'])

    def move(self, ref, kind, boxes=0, rolls=0, loose=0, days_ago=0):
        code, body = self.call('POST', '/api/consumables/moves/', {'ref': ref['id'], 'kind': kind, 'boxes': boxes,
                                                                   'rolls': rolls, 'loose': loose})
        self.assertEqual(code, 201, body)
        if days_ago:
            ConsumableMove.objects.filter(id=body['moves'][0]['id']).update(
                created=timezone.now() - timedelta(days=days_ago))
        return body

    def printed(self, days_ago, count, width=40, height=30, support='direct'):
        PrintCount.objects.create(day=self.today - timedelta(days=days_ago), width_mm=width, height_mm=height,
                                  support=support, count=count)

    def pool(self, key):
        return next(pool for pool in self.call('GET', '/api/consumables/')[1]['pools'] if pool['key'] == key)

    def test_stock_from_reception_minus_prints(self):
        self.assertEqual(self.pool('40x30-direct')['level'], 'unknown')
        self.move(self.vellum, 'reception', boxes=1, days_ago=30)  # 24 000 etiquettes
        self.printed(31, 500)  # avant la reception : pas comptee
        self.printed(30, 100)  # le jour de la reception : pas comptee non plus (a partir du lendemain)
        self.printed(10, 3000)
        self.printed(1, 1500, 60, 40, 'transfer')  # autre categorie
        pool = self.pool('40x30-direct')
        self.assertEqual(pool['stock'], 21000)
        self.assertEqual(pool['level'], 'ok')
        self.assertIn('21 000 étiquettes', pool['stock_text'])
        # 3 600 etiquettes sur 32 jours de compteur (depuis le premier jour imprime)
        self.assertEqual(pool['monthly'], int(Decimal(3600) / 32 * Decimal('30.44')))
        self.assertEqual(pool['autonomy_days'], int(21000 / (Decimal(3600) / 32)))
        self.assertAlmostEqual(pool['monthly_cost'], round(float(Decimal(3600) / 32 * Decimal('30.44')) * 57 / 24000, 2),
                               places=2)

    def test_inventory_resets_stock_and_alert_task(self):
        self.move(self.vellum, 'reception', boxes=2, days_ago=60)
        self.move(self.vellum, 'inventory', rolls=1, loose=500, days_ago=5)  # 2 500 etiquettes comptees
        self.printed(3, 900)
        pool = self.pool('40x30-direct')
        self.assertEqual(pool['stock'], 1600)
        self.assertEqual(pool['level'], 'warn')  # 1 600 / (900 / 4 jours) = 7 jours < 60
        poste = self.call('GET', '/api/tasks/')[1]
        titles = [task['title'] for task in poste['tasks']]
        self.assertIn('Commander : 40 × 30 mm, thermique direct', titles)
        task = next(task for task in poste['tasks'] if task['title'].startswith('Commander :'))
        self.assertEqual((task['category'], task['link']), ('orders', 'consumables'))
        # pas sur le web (Gestion de la TECH du telephone)
        self.assertNotIn('Commander : 40 × 30 mm, thermique direct',
                         [task['title'] for task in views.tasks.todo()['tasks']])
        # seuil abaisse : plus d'alerte
        self.assertEqual(self.call('PATCH', '/api/consumables/', {'alert_days': 5})[0], 200)
        self.assertEqual(self.pool('40x30-direct')['level'], 'ok')

    def test_ribbon_used_by_transfer_labels_length(self):
        self.move(self.ribbon, 'reception', rolls=2, days_ago=20)  # 148 m
        self.printed(5, 1000, 60, 40, 'transfer')  # 1 000 x (40 + 3) mm = 43 m
        self.printed(5, 5000)  # thermique direct : pas de ruban
        pool = self.pool('ribbon')
        self.assertEqual(pool['stock'], 148000 - 43000)
        self.assertIn('105 m', pool['stock_text'])

    def test_refs_moves_and_errors(self):
        code, _ = self.call('POST', '/api/consumables/refs/', {'kind': 'labels', 'name': 'X', 'width_mm': 40})
        self.assertEqual(code, 400)
        code, body = self.call('PATCH', f"/api/consumables/refs/{self.vellum['id']}/", {'box_price': '', 'per_box': 6})
        self.assertEqual(code, 200)
        ref = next(ref for ref in body['refs'] if ref['id'] == self.vellum['id'])
        self.assertEqual((ref['box_price'], ref['per_box']), (None, 6))
        self.assertEqual(self.call('POST', '/api/consumables/moves/', {'ref': self.vellum['id'],
                                                                       'kind': 'reception'})[0], 400)
        body = self.move(self.vellum, 'reception', boxes=1)
        self.assertEqual(body['moves'][0]['what'], '1 boîte')
        self.assertEqual(body['moves'][0]['units'], 12000)
        code, body = self.call('POST', f"/api/consumables/moves/{body['moves'][0]['id']}/delete/")
        self.assertEqual((code, body['moves']), (200, []))
        self.call('PATCH', f"/api/consumables/refs/{self.vellum['id']}/", {'archived': True})
        self.assertEqual(self.call('POST', '/api/consumables/moves/', {'ref': self.vellum['id'], 'kind': 'reception',
                                                                       'boxes': 1})[0], 400)

    def test_access(self):
        self.assertEqual(self.call('GET', '/api/consumables/', local=False)[0], 404)
        self.assertEqual(self.call('GET', '/api/consumables/', session=False)[0], 403)
        self.assertEqual(self.call('GET', '/api/consumables/', session=views.session_token(self.user))[0], 403)


class UnitGroupTests(ApiTestCase):
    """Attentes en quantite (« 200 mL de sérum phy ») : un type a unites rempli par des sous-types interchangeables
    (sachet de 5 dosettes de 5 mL : 25 mL, flacon : 50 mL), avec un minimum d'un sous-type compris dedans."""

    def setUp(self):
        super().setUp()
        code, body = self.call('POST', '/api/item-types/', {'type': 'serphy', 'name': 'Sérum phy', 'unit': 'mL'})
        self.assertEqual(code, 201)
        self.assertEqual(body['unit'], 'mL')
        code, body = self.call('POST', '/api/item-types/', {'type': 'sersac', 'name': 'Sachet 5 dosettes sérum',
                                                            'perissable': True, 'tear_off': True,
                                                            'group': 'serphy', 'fills': 25})
        self.assertEqual(code, 201)
        self.assertEqual((body['group'], body['fills']), ('serphy', 25))
        self.call('POST', '/api/item-types/', {'type': 'serfla', 'name': 'Flacon sérum 50 mL', 'perissable': True,
                                               'group': 'serphy', 'fills': 50})
        code, body = self.call('PATCH', '/api/item-types/serphy/', {'default_subtype': 'sersac'})
        self.assertEqual(body['default_subtype'], 'sersac')
        self.serphy = ItemType.objects.get(type='serphy')
        self.sachet = ItemType.objects.get(type='sersac')
        self.flacon = ItemType.objects.get(type='serfla')
        self.later = self.today + timedelta(days=200)

    def require(self, rows):
        code, body = self.call('PUT', '/api/lot-types/sacpse/requirements/', {'requirements': rows})
        self.assertEqual(code, 200, body)

    def row(self, body, type_id):
        return next(row for row in body['requirements'] if row['type'] == type_id)

    def test_rules(self):
        # un type a unites n'a pas d'etiquette : ni reception, ni peremption, ni sous-type d'un autre
        with self.assertRaises(ValueError):
            self.create(self.serphy, None, 1)
        self.assertEqual(self.call('PATCH', '/api/item-types/serphy/', {'perissable': True})[0], 400)
        self.assertEqual(self.call('PATCH', '/api/item-types/serphy/', {'group': 'serphy'})[0], 400)
        self.assertEqual(self.call('POST', '/api/item-types/', {'type': 'autre1', 'name': 'Autre', 'unit': 'mL',
                                                                 'group': 'serphy'})[0], 400)
        self.assertFalse(ItemType.objects.filter(type='autre1').exists())
        # sous-type d'un type sans unite, ou sous-type par defaut qui n'en est pas un : refuses
        self.assertEqual(self.call('PATCH', '/api/item-types/garrot/', {'group': 'compre'})[0], 400)
        self.assertEqual(self.call('PATCH', '/api/item-types/serphy/', {'default_subtype': 'garrot'})[0], 400)
        # un type qui a des sous-types garde son unite ; un type deja etiquete ne devient pas un type a unites
        self.assertEqual(self.call('PATCH', '/api/item-types/serphy/', {'unit': ''})[0], 400)
        self.create(self.garrot, None, 1)
        self.assertEqual(self.call('PATCH', '/api/item-types/garrot/', {'unit': 'u'})[0], 400)

    def test_requirement_in_units(self):
        """200 mL dont 2 sachets : les sachets comptent dans les 200 mL, un flacon ne remplit que le reste."""
        self.require([{'type': 'compre', 'quantity': 2}, {'type': 'serphy', 'quantity': 200},
                      {'type': 'sersac', 'quantity': 2}])
        compresses = self.create(self.compresses, self.later, 2)
        sachets = self.create(self.sachet, self.later, 2)
        flacons = self.create(self.flacon, self.later, 3)
        url = f'/api/lots/{self.lot.id}/'
        code, body = self.call('GET', url)
        row = self.row(body, 'serphy')
        self.assertEqual((row['unit'], row['required'], row['present']), ('mL', 200, 0))
        self.assertEqual(row['fills'], {'sersac': 25, 'serfla': 50})
        self.assertEqual(row['suggest'], {'type': 'sersac', 'type_name': 'Sachet 5 dosettes sérum', 'count': 8})
        self.assertEqual(self.row(body, 'sersac')['within'], 'serphy')
        # 4 flacons de 50 mL : 200 mL, mais pas les 2 sachets exiges
        flacons += self.create(self.flacon, self.later, 1)
        report = self.call('POST', f'{url}verif/', {'items': [i.iid for i in compresses + flacons], 'user': 'M001'})[1]
        self.assertFalse(report['complete'])
        self.assertEqual(self.row(report, 'serphy')['present'], 200)
        self.assertEqual(self.row(report, 'sersac')['present'], 0)
        # 2 sachets (50 mL) + 3 flacons (150 mL) : complet ; le 4e flacon est en trop (300 mL en tout)
        report = self.call('POST', f'{url}verif/', {'items': [i.iid for i in compresses + sachets + flacons[:3]],
                                                    'user': 'M001'})[1]
        self.assertTrue(report['complete'])
        self.assertEqual(self.row(report, 'serphy')['present'], 200)
        self.assertNotIn('suggest', self.row(report, 'serphy'))

    def test_split_sachet_into_dosettes(self):
        """Un sachet de 5 dosettes (25 mL) se decoupe en 5 dosettes (5 mL) etiquetees une par une : meme peremption,
        meme lot ; le sachet est remplace et ne revient plus."""
        code, body = self.call('POST', '/api/item-types/', {'type': 'serdos', 'name': 'Dosette sérum 5 mL',
                                                            'perissable': True, 'group': 'serphy', 'fills': 5,
                                                            'label_style': 'sheet'})
        self.assertEqual((code, body['label_style']), (201, 'sheet'))
        sachet = self.create(self.sachet, self.later, 1)[0]
        Items.objects.filter(iid=sachet.iid).update(location=self.lot)
        url = f'/api/items/{sachet.iid}/split/'
        code, body = self.call('GET', url)
        self.assertEqual(body['targets'], [{'type': 'serdos', 'name': 'Dosette sérum 5 mL', 'count': 5}])
        # le flacon (50 mL) n'est pas une part du sachet ; un type hors du groupe non plus
        self.assertEqual(self.call('POST', url, {'type': 'serfla'})[0], 400)
        self.assertEqual(self.call('POST', url, {'type': 'compre'})[0], 400)
        code, body = self.call('POST', url, {'type': 'serdos'})
        self.assertEqual(code, 200, body)
        self.assertEqual(len(body['items']), 5)
        self.assertEqual({item['type'] for item in body['items']}, {'serdos'})
        self.assertEqual({item['peremption'] for item in body['items']}, {self.later.isoformat()})
        self.assertEqual(Items.objects.filter(pack__item_type=self.dosette(), location=self.lot,
                                              status=ItemStatus.ACTIVE).count(), 5)
        old = Items.objects.get(iid=sachet.iid)
        self.assertEqual((old.status, old.location_id), (ItemStatus.REPLACED, None))
        self.assertEqual(old.deleted_reason, 'Découpé en 5 × Dosette sérum 5 mL')
        self.assertTrue(Operation.objects.filter(kind=OperationKind.ITEM_SPLIT, details__source=sachet.iid).exists())
        # deja decoupe : plus rien a decouper ; une dosette ne se decoupe pas
        self.assertEqual(self.call('POST', url, {'type': 'serdos'})[0], 400)
        self.assertEqual(self.call('GET', f"/api/items/{body['items'][0]['iid']}/split/")[1]['targets'], [])

    def dosette(self):
        return ItemType.objects.get(type='serdos')

    def test_newcomers_fill_units(self):
        """Verif groupee de deux lots : les items venus du stock vont la ou il manque des unites."""
        self.require([{'type': 'compre', 'quantity': 2}, {'type': 'serphy', 'quantity': 50}])
        other = Lots(lot_type=self.lot_type, name='Sac B', name_short='B', created_by='test')
        other.save()
        flacons = self.create(self.flacon, self.later, 1)
        sachets = self.create(self.sachet, self.later, 2)
        target = services.assign_items([self.lot, other], list(Items.objects.filter(
            iid__in=[i.iid for i in flacons + sachets]).select_related('pack__item_type')), self.today)
        self.assertEqual(target[flacons[0].iid], self.lot.id)
        self.assertEqual({target[i.iid] for i in sachets}, {other.id})

    def test_expired_replaced_by_units(self):
        """Un sachet perime est remplace (jete) des que le lot a ses mL frais, meme en flacons."""
        self.require([{'type': 'compre', 'quantity': 2}, {'type': 'serphy', 'quantity': 50}])
        compresses = self.create(self.compresses, self.later, 2)
        old = self.create(self.sachet, self.today - timedelta(days=1), 1)
        Items.objects.filter(iid__in=[i.iid for i in compresses + old]).update(location=self.lot)
        flacon = self.create(self.flacon, self.later, 1)
        report = self.call('POST', f'/api/lots/{self.lot.id}/verif/',
                           {'items': [i.iid for i in compresses + old + flacon], 'user': 'M001'})[1]
        self.assertEqual(report['replaced'], [old[0].iid])
        self.assertTrue(report['complete'])

    def test_stock_in_units(self):
        """Etat des stocks : le type a unites additionne les mL de ses sous-types ; son minimum (en mL) et celui de
        chaque sous-type (en items) alertent chacun ; la commande proposee porte sur le sous-type par defaut."""
        ItemType.objects.filter(type='serphy').update(min_quantity=200)
        ItemType.objects.filter(type='sersac').update(min_quantity=1, default_pack_size=4)
        self.create(self.sachet, self.later, 2)
        self.create(self.flacon, self.later, 1)
        rows = {row['type']: row for row in services.stock_status()}
        self.assertEqual((rows['serphy']['stock_fresh'], rows['serphy']['unit']), (100, 'mL'))
        self.assertEqual(rows['sersac']['stock_fresh'], 2)
        from .. import notifications
        low = notifications.check_stock_levels(['serfla'])
        self.assertEqual(low, ['Sérum phy 100/200 mL'])
        row = next(row for row in forecast.forecast(months=6)['types'] if row['type'] == 'serphy')
        self.assertEqual(row['stock_now'], 100)
        self.assertEqual(row['order']['subtype'], 'sersac')
        self.assertEqual(row['order']['quantity'] % 4, 0)
        self.assertGreaterEqual(row['order']['quantity'] * 25, row['order']['units'])

    def test_checklist_line_in_units(self):
        """Liste de matériel « 100 mL de sérum phy » : n'importe quel sous-type, compté en mL."""
        code, body = self.call('POST', '/api/checklists/', {'user': 'M001', 'name': 'Poste', 'lines': [
            {'type': 'serphy', 'quantity': 100}]})
        self.assertEqual(code, 201, body)
        url = f"/api/checklists/{body['id']}/"
        flacon = self.create(self.flacon, self.later, 1)[0]
        sachets = self.create(self.sachet, self.later, 3)
        code, body = self.call('POST', f'{url}scan/', {'user': 'M001', 'item': flacon.iid})
        self.assertEqual(code, 200, body)
        self.assertIn('50/100 mL', body['message'])
        self.call('POST', f'{url}scan/', {'user': 'M001', 'item': sachets[0].iid})
        self.call('POST', f'{url}scan/', {'user': 'M001', 'item': sachets[1].iid})
        code, body = self.call('POST', f'{url}scan/', {'user': 'M001', 'item': sachets[2].iid})
        self.assertEqual(code, 400)
        body = self.call('POST', url, {'user': 'M001'})[1]
        line = body['lines'][0]
        self.assertEqual((line['scanned'], line['quantity'], line['unit']), (100, 100, 'mL'))
        self.assertTrue(body['complete'])

    def test_coverage_in_units(self):
        """Couverture d'un referentiel en mL : 8 sachets ranges dans un sac couvrent 200 mL."""
        referentiel = LotType.objects.create(type='refbpl', name='B+', referentiel=True)
        LotRequirements.objects.create(lot_type=referentiel, item_type=self.serphy, quantity=200)
        LotType.objects.filter(type='sacpse').update(part_of=referentiel)
        LotRequirements.objects.filter(lot_type=self.lot_type).delete()
        LotRequirements.objects.create(lot_type=self.lot_type, item_type=self.sachet, quantity=8)
        line = services.lot_coverage(self.lot, referentiel)
        self.assertTrue(line['complete'])
        self.assertEqual(line['lines'][0]['placed'], 200)
        self.assertEqual(line['extra'], [])

    def test_distribution_in_units(self):
        """Repartition d'un referentiel en mL : 20 mL de serum phy dans la liste, 4 dosettes de 5 mL attendues dans
        un sac valident la ligne ; 3 seulement (15 mL) la laissent dans aucun sac."""
        dosette = ItemType.objects.create(type='serdos', name='Dosette sérum 5 mL', perissable=True, group=self.serphy,
                                          fills=5)
        referentiel = LotType.objects.create(type='refbpl', name='B+', referentiel=True)
        LotRequirements.objects.create(lot_type=referentiel, item_type=self.serphy, quantity=20)
        LotType.objects.filter(type='sacpse').update(part_of=referentiel)
        LotRequirements.objects.filter(lot_type=self.lot_type).delete()
        LotRequirements.objects.create(lot_type=self.lot_type, item_type=dosette, quantity=4, location='Pochette')
        bplus = Lots.objects.create(lot_type=referentiel, name='B+ 1', name_short='B+ 1', created_by='M001')
        Lots.objects.filter(id=self.lot.id).update(parent=bplus)
        bplus.refresh_from_db()
        group = services.lot_distribution(bplus)[0]
        line = group['lines'][0]
        self.assertEqual((line['required'], line['placed'], line['unit']), (20, 20, 'mL'))
        place = line['places'][0]
        self.assertEqual((place['quantity'], place['units']), (0, 20))
        self.assertEqual(place['subtypes'], [{'type': 'serdos', 'type_name': 'Dosette sérum 5 mL', 'quantity': 4,
                                              'fills': 5}])
        self.assertEqual(services.unplaced_lines(bplus), [])
        code, body = self.call('GET', f'/api/lots/{bplus.id}/')
        self.assertEqual(body['unplaced'], [])
        self.assertNotEqual(body['state']['code'], 'unplaced')
        # 3 dosettes : 15 mL sur 20, il manque 5 mL
        LotRequirements.objects.filter(lot_type=self.lot_type, item_type=dosette).update(quantity=3)
        self.assertEqual(services.unplaced_lines(bplus), [{'type': 'serphy', 'type_name': 'Sérum phy', 'missing': 5}])
        # le sac attend aussi 20 mL du type a unites : les dosettes sont comprises dedans, pas ajoutees
        LotRequirements.objects.create(lot_type=self.lot_type, item_type=self.serphy, quantity=20)
        self.assertEqual(services.lot_distribution(bplus)[0]['lines'][0]['placed'], 20)
