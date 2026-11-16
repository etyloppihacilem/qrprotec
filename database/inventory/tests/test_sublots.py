"""Tests du back : sous-lots, scelles, contenu facultatif, cache de vérif."""

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)


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


class SubLotCase(ApiTestCase):
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


class SubLotTests(SubLotCase):
    """Lot global (B+) compose de sous-lots (sac de soin, sac d'O2), et rangements du stock."""

    def test_lots_batch(self):
        """Creer N lots d'un coup : noms numerotes apres l'existant, sous-lots proposes d'apres les lots deja
        crees (et les rangements du referentiel), creation tout ou rien."""
        vps_type = LotType.objects.create(type='vpsxxx', name='VPS', unique=True)
        vps_bag_type = LotType.objects.create(type='sacvps', name='Sac de soin VPS', part_of=self.global_type)
        self.global_type.referentiel = True
        self.global_type.save()
        code, vps = self.call('POST', '/api/lots/', {'lot_type': 'vpsxxx', 'name': 'VPS', 'user': 'M001'})
        code, _ = self.call('POST', '/api/lots/', {'lot_type': 'sacvps', 'name': 'Sac VPS', 'parent': vps['id'],
                                                   'user': 'M001'})
        self.assertEqual(code, 201)
        code, body = self.call('POST', '/api/lots/batch/', {'lot_type': 'bplusx', 'count': 2, 'preview': True})
        self.assertEqual(code, 200)
        counts = {row['lot_type']: row['count'] for row in body['sub_types']}
        self.assertEqual(counts, {'sacpse': 1, 'saco2x': 1, 'sacvps': 0})  # le sac du VPS n'est pas dans un B+
        self.assertEqual([lot['name'] for lot in body['lots']], ['B+ 2', 'B+ 3'])  # B+ 1 existe deja
        self.assertEqual([sub['name'] for sub in body['lots'][0]['sub_lots']], ['Sac O2 B+ 2', 'Sac PSE B+ 2'])
        code, body = self.call('POST', '/api/lots/batch/', {'lot_type': 'bplusx', 'count': 1, 'preview': True,
                                                            'sub_lots': [{'lot_type': 'sacpse', 'count': 2}]})
        self.assertEqual([sub['name'] for sub in body['lots'][0]['sub_lots']],
                         ['Sac PSE 1 B+ 2', 'Sac PSE 2 B+ 2'])
        code, body = self.call('POST', '/api/lots/batch/', {'lot_type': 'vpsxxx', 'count': 1, 'preview': True})
        self.assertEqual(code, 400)  # lot unique
        code, plan = self.call('POST', '/api/lots/batch/', {'lot_type': 'bplusx', 'count': 2, 'preview': True})
        plan['lots'][1]['sub_lots'][0]['name'] = 'B+ 1'  # deja pris : rien n'est cree
        code, body = self.call('POST', '/api/lots/batch/', {'lot_type': 'bplusx', 'lots': plan['lots'], 'user': 'M001'})
        self.assertEqual(code, 409)
        self.assertFalse(Lots.objects.filter(name='B+ 2').exists())
        plan['lots'][1]['sub_lots'][0]['name'] = 'Sac O2 B+ 3'
        code, body = self.call('POST', '/api/lots/batch/', {'lot_type': 'bplusx', 'lots': plan['lots'], 'user': 'M001'})
        self.assertEqual(code, 201)
        self.assertEqual(len(body['lots']), 6)
        bplus3 = Lots.objects.get(name='B+ 3')
        self.assertEqual(sorted(child.name for child in bplus3.children.all()), ['Sac O2 B+ 3', 'Sac PSE B+ 3'])

    def test_rangement_needs_parent_or_spare(self):
        """Un lot d'un rangement (sac de soin du B+) se range dans un lot, sauf un sac de rechange ; un sac sorti
        d'un lot (remplace, libere) devient un sac de rechange."""
        self.global_type.referentiel = True
        self.global_type.save()
        LotType.objects.filter(type='sacpse').update(part_of=self.global_type)
        code, body = self.call('POST', '/api/lots/', {'lot_type': 'sacpse', 'name': 'Sac seul', 'user': 'M001'})
        self.assertEqual(code, 400)
        self.assertIn('Sac de rechange', body['error'])
        code, spare = self.call('POST', '/api/lots/', {'lot_type': 'sacpse', 'name': 'Sac réserve', 'spare': True,
                                                       'user': 'M001'})
        self.assertEqual(code, 201)
        self.assertTrue(spare['spare'])
        code, body = self.call('POST', '/api/lots/batch/', {'lot_type': 'sacpse', 'user': 'M001',
                                                            'lots': [{'name': 'Sac PSE 9'}]})
        self.assertEqual(code, 400)
        code, body = self.call('POST', '/api/lots/batch/', {'lot_type': 'sacpse', 'user': 'M001', 'spare': True,
                                                            'lots': [{'name': 'Sac PSE 9'}]})
        self.assertEqual(code, 201)
        # sortir le sac de soin de son B+ : refuse, sauf en le marquant sac de rechange
        code, body = self.call('PATCH', f"/api/lots/{self.soin['id']}/update/", {'parent': ''})
        self.assertEqual(code, 400)
        code, body = self.call('PATCH', f"/api/lots/{self.soin['id']}/update/", {'parent': '', 'spare': True})
        self.assertEqual(code, 200)
        self.assertTrue(body['spare'])
        # un lot ordinaire n'est jamais un sac de rechange
        code, body = self.call('PATCH', f"/api/lots/{self.sac_o2['id']}/update/", {'spare': True})
        self.assertFalse(body['spare'])

    def test_distribution_per_lot(self):
        """Repartition d'un referentiel entre les sacs d'un lot global, lot par lot : les types des sacs donnent le
        defaut, ce lot met la cardio-pompe dans son sac d'O2, un autre B+ garde le defaut."""
        cardio = ItemType.objects.create(type='cardio', name='Cardio-pompe', perissable=False)
        self.global_type.referentiel = True
        self.global_type.save()
        LotRequirements.objects.create(lot_type=self.global_type, item_type=cardio, quantity=1)
        LotRequirements.objects.create(lot_type=self.global_type, item_type=self.o2, quantity=1)
        sac_type = LotType.objects.get(type='sacpse')
        LotRequirements.objects.create(lot_type=sac_type, item_type=cardio, quantity=1, location='Poche avant')
        LotType.objects.filter(type__in=['sacpse', 'saco2x']).update(part_of=self.global_type)
        code, lot = self.call('GET', f"/api/lots/{self.bplus['id']}/")
        (dist,) = lot['distribution']
        self.assertEqual(dist['referentiel'], 'bplusx')
        line = next(line for line in dist['lines'] if line['type'] == 'cardio')
        self.assertEqual({p['lot']: p['quantity'] for p in line['places']}, {self.soin['id']: 1, self.sac_o2['id']: 0})
        code, body = self.call('PUT', f"/api/lots/{self.bplus['id']}/distribution/", {'referentiel': 'bplusx', 'rows': [
            {'lot': self.soin['id'], 'type': 'cardio', 'quantity': 0},
            {'lot': self.sac_o2['id'], 'type': 'cardio', 'quantity': 1, 'location': 'Poche latérale'}]})
        self.assertEqual(code, 200)
        line = next(line for line in body['distribution'][0]['lines'] if line['type'] == 'cardio')
        self.assertEqual(line['placed'], 1)
        self.assertEqual({p['lot']: (p['quantity'], p['location']) for p in line['places']},
                         {self.soin['id']: (0, 'Poche avant'), self.sac_o2['id']: (1, 'Poche latérale')})
        code, soin = self.call('GET', f"/api/lots/{self.soin['id']}/")
        self.assertNotIn('cardio', [row['type'] for row in soin['requirements']])
        code, o2 = self.call('GET', f"/api/lots/{self.sac_o2['id']}/")
        self.assertIn('cardio', [row['type'] for row in o2['requirements']])
        # le contenu propre du sac de soin (items en plus) garde la repartition
        code, body = self.call('PUT', f"/api/lots/{self.soin['id']}/content/", {'content': []})
        self.assertNotIn('cardio', [row['type'] for row in body['requirements']])
        # un autre B+ garde la repartition par defaut (celle des types)
        code, bplus2 = self.call('POST', '/api/lots/', {'lot_type': 'bplusx', 'name': 'B+ 2', 'user': 'M001'})
        code, soin2 = self.call('POST', '/api/lots/', {'lot_type': 'sacpse', 'name': 'Sac de soin 2',
                                                       'parent': bplus2['id'], 'user': 'M001'})
        self.assertIn('cardio', [row['type'] for row in soin2['requirements']])
        # une ligne hors du referentiel est refusee
        code, body = self.call('PUT', f"/api/lots/{self.bplus['id']}/distribution/", {'referentiel': 'bplusx', 'rows': [
            {'lot': self.soin['id'], 'type': 'compre', 'quantity': 1}]})
        self.assertEqual(code, 400)

    def test_lot_type_slots(self):
        """Lots exiges : place fixe (vide = lot incomplet, remplacement par un lot non fixe puis retour de
        l'original) et place a l'assemblage (n'importe quel lot libre, jamais un lot fixe)."""
        code, body = self.call('PUT', '/api/lot-types/bplusx/slots/', {'slots': [
            {'accepted': ['sacpse'], 'mode': 'fixed'}, {'name': 'Sac O2', 'accepted': ['saco2x']}]})
        self.assertEqual(code, 200)
        self.assertEqual([slot['name'] for slot in body['slots']], ['Sac PSE', 'Sac O2'])
        code, lot = self.call('GET', f"/api/lots/{self.bplus['id']}/")
        self.assertEqual(lot['slots_missing'], [])
        self.assertEqual([row['lots'][0]['id'] for row in lot['slots']], [self.soin['id'], self.sac_o2['id']])
        code, bplus2 = self.call('POST', '/api/lots/', {'lot_type': 'bplusx', 'name': 'B+ 2', 'user': 'M001'})
        code, o2_2 = self.call('POST', '/api/lots/', {'lot_type': 'saco2x', 'name': 'Sac O2 2',
                                                      'parent': bplus2['id'], 'user': 'M001'})
        code, lot = self.call('GET', f"/api/lots/{bplus2['id']}/")
        self.assertEqual(lot['slots_missing'], ['Sac PSE'])
        self.assertFalse(lot['complete'])
        self.assertTrue(lot['global']['lots'][0]['counted'])  # le B+ vide de sac compte dans son lot global
        # remplacement : un sac libre prend la place du sac de soin du B+ 1 ; le sac O2 du B+ 2 (fixe) n'est pas propose
        code, spare = self.call('POST', '/api/lots/', {'lot_type': 'sacpse', 'name': 'Sac réserve', 'user': 'M001'})
        code, lot = self.call('GET', f"/api/lots/{self.bplus['id']}/")
        soin_slot, o2_slot = lot['slots']
        self.assertIn(spare['id'], [row['id'] for row in soin_slot['candidates']])  # lots libres (le sac du setUp aussi)
        self.assertEqual(o2_slot['candidates'], [])
        code, body = self.call('POST', f"/api/lots/{self.bplus['id']}/slots/replace/",
                               {'slot': o2_slot['id'], 'original': self.sac_o2['id'], 'lot': o2_2['id'], 'user': 'M001'})
        self.assertEqual(code, 400)  # on ne prend pas un lot fixe
        code, body = self.call('POST', f"/api/lots/{self.bplus['id']}/slots/replace/",
                               {'slot': soin_slot['id'], 'original': self.soin['id'], 'lot': spare['id'], 'user': 'M001'})
        self.assertEqual(code, 200)
        self.assertEqual(body['slots'][0]['lots'][0]['replaces_name'], 'Sac de soin')
        self.assertIsNone(Lots.objects.get(id=self.soin['id']).parent_id)
        code, body = self.call('POST', f"/api/lots/{self.bplus['id']}/slots/restore/",
                               {'lot': spare['id'], 'user': 'M001'})
        self.assertEqual(code, 200)
        self.assertEqual(Lots.objects.get(id=self.soin['id']).parent_id, self.bplus['id'])
        self.assertIsNone(Lots.objects.get(id=spare['id']).parent_id)
        # assemblage : un C prend n'importe quel B+ libre
        LotType.objects.create(type='lotcxx', name='Lot C')
        code, body = self.call('PUT', '/api/lot-types/lotcxx/slots/', {'slots': [
            {'name': 'B+', 'accepted': ['bplusx'], 'mode': 'assembly'}]})
        code, lot_c = self.call('POST', '/api/lots/', {'lot_type': 'lotcxx', 'name': 'C 1', 'user': 'M001'})
        code, lot = self.call('GET', f"/api/lots/{lot_c['id']}/")
        self.assertEqual(lot['slots_missing'], [])  # une place a l'assemblage vide n'est pas un manque
        slot = lot['slots'][0]
        self.assertEqual({row['id'] for row in slot['available']}, {self.bplus['id'], bplus2['id']})
        code, body = self.call('POST', f"/api/lots/{lot_c['id']}/slots/assemble/",
                               {'slot': slot['id'], 'lot': self.bplus['id'], 'user': 'M001'})
        self.assertEqual(code, 200)
        self.assertTrue(Lots.objects.get(id=self.bplus['id']).assembled)
        code, body = self.call('POST', f"/api/lots/{lot_c['id']}/slots/assemble/",
                               {'slot': slot['id'], 'lot': bplus2['id'], 'user': 'M001'})
        self.assertEqual(code, 400)  # place deja occupee
        code, body = self.call('POST', f"/api/lots/{lot_c['id']}/slots/release/",
                               {'lot': self.bplus['id'], 'user': 'M001'})
        self.assertEqual(code, 200)
        self.assertIsNone(Lots.objects.get(id=self.bplus['id']).parent_id)

    def test_referentiel_coverage(self):
        """Referentiel : liste obligatoire sans emplacements, repartie entre des rangements (les sacs) ; le lot du
        referentiel n'attend rien lui-meme, la couverture compare la somme de ses sacs a la liste."""
        code, body = self.call('GET', '/api/lot-types/saco2x/')
        self.assertEqual(body['suggested_part_of'], 'bplusx')  # tous les sacs O2 sont dans des B+
        code, body = self.call('PATCH', '/api/lot-types/saco2x/', {'part_of': 'bplusx'})
        self.assertEqual(code, 400)  # le B+ n'est pas (encore) un referentiel
        code, body = self.call('PATCH', '/api/lot-types/bplusx/', {'referentiel': True})
        self.assertEqual(code, 200)
        self.assertTrue(body['referentiel'])
        code, body = self.call('PUT', '/api/lot-types/bplusx/requirements/', {'requirements': [
            {'type': 'compre', 'quantity': 3, 'location': 'Pochette rouge'}, {'type': 'bouo2x', 'quantity': 1},
            {'type': 'garrot', 'quantity': 1}]})
        self.assertEqual(code, 200)
        self.assertEqual(body['locations'], [])
        self.assertEqual({row['location'] for row in body['requirements']}, {''})
        # sans rangement, un lot du referentiel attend sa liste lui-meme (un kit d'un seul sac)
        bplus = Lots.objects.get(id=self.bplus['id'])
        self.assertEqual(len(services.lot_requirements(bplus)), 3)
        for code_type in ('sacpse', 'saco2x'):
            code, body = self.call('PATCH', f'/api/lot-types/{code_type}/', {'part_of': 'bplusx'})
            self.assertEqual(code, 200)
            self.assertEqual(body['part_of_name'], 'B+')
        code, body = self.call('GET', '/api/lot-types/bplusx/')
        self.assertEqual([row['name'] for row in body['rangements']], ['Sac O2', 'Sac PSE'])
        self.assertEqual(services.lot_requirements(bplus), [])
        code, body = self.call('GET', f"/api/lots/{self.bplus['id']}/", local=False)
        self.assertEqual(body['requirements'], [])
        code, body = self.call('GET', '/api/lot-types/bplusx/coverage/')
        self.assertEqual(code, 200)
        # le Sac A (rangement du B+ hors de tout B+) doit couvrir la liste a lui seul
        self.assertEqual([lot['name'] for lot in body['lots']], ['B+ 1', 'Sac A'])
        lot = body['lots'][0]
        self.assertEqual([(line['type'], line['required'], line['placed']) for line in lot['lines']],
                         [('bouo2x', 1, 1), ('compre', 3, 2), ('garrot', 1, 0)])
        self.assertEqual(lot['lines'][1]['places'], [
            {'lot': self.soin['id'], 'lot_name': 'Sac de soin', 'location': '', 'quantity': 2}])
        self.assertFalse(lot['complete'])
        self.assertEqual(lot['missing'], 2)
        # du contenu propre comble un manque ; un type absent de la liste est en plus
        self.call('PUT', f"/api/lots/{self.soin['id']}/content/", {'content': [
            {'type': 'compre', 'extra': 1}, {'type': 'garrot', 'extra': 2, 'location': 'Poche avant'}]})
        ItemType.objects.create(type='ciseau', name='Ciseaux')
        self.call('PUT', f"/api/lots/{self.sac_o2['id']}/content/", {'content': [{'type': 'ciseau', 'extra': 1}]})
        code, body = self.call('GET', '/api/lot-types/bplusx/coverage/')
        lot = body['lots'][0]
        self.assertTrue(lot['complete'])
        self.assertEqual([(line['type'], line['placed']) for line in lot['lines']],
                         [('bouo2x', 1), ('compre', 3), ('garrot', 2)])
        self.assertEqual([(line['type'], line['placed']) for line in lot['extra']], [('ciseau', 1)])
        # un referentiel reparti ne redevient pas un type ordinaire, un referentiel n'est le rangement de personne
        code, body = self.call('PATCH', '/api/lot-types/bplusx/', {'referentiel': False})
        self.assertEqual(code, 400)
        self.assertIn('Sac O2', body['error'])
        code, _ = self.call('PATCH', '/api/lot-types/sacpse/', {'referentiel': True})
        self.assertEqual(code, 400)
        code, _ = self.call('GET', '/api/lot-types/sacpse/coverage/')
        self.assertEqual(code, 400)
        code, body = self.call('PATCH', '/api/lot-types/sacpse/', {'part_of': ''})
        self.assertEqual(body['part_of'], '')

    def test_tree_and_global_state(self):
        code, lot = self.call('GET', f"/api/lots/{self.sac_o2['id']}/", local=False)
        self.assertEqual(lot['parent'], self.bplus['id'])
        self.assertEqual(lot['path'], [{'id': self.bplus['id'], 'name': 'B+ 1', 'hidden': False}])
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

    def test_lots_to_handle_first(self):
        # seul le sac de soin est verifie : le sac O2 (jamais verifie) passe devant lui dans l'arborescence du B+
        self.call('POST', f"/api/lots/{self.soin['id']}/verif/", {'items': self.fresh(), 'user': 'M001'})
        code, lot = self.call('GET', f"/api/lots/{self.soin['id']}/", local=False)
        self.assertEqual([row['name'] for row in lot['global']['lots']], ['B+ 1', 'Sac O2', 'Sac de soin'])
        code, lots = self.call('POST', '/api/lots/summary/', {}, local=False)
        self.assertEqual([row['name'] for row in lots][:3], ['B+ 1', 'Sac O2', 'Sac de soin'])
        # B+ entierement valide : le sac A (jamais verifie) remonte au-dessus, le B+ garde ses sous-lots dessous
        self.call('POST', f"/api/lots/{self.sac_o2['id']}/verif/", {'items': [self.bottle()], 'user': 'M001'})
        code, lots = self.call('POST', '/api/lots/summary/', {}, local=False)
        names = [row['name'] for row in lots if not row['storage']]
        self.assertEqual(names, ['Sac A', 'B+ 1', 'Sac de soin', 'Sac O2'])
        # liste du poste : ordre de l'arborescence par nom, avec la place de chaque lot quand on trie par attention
        code, lots = self.call('GET', '/api/lots/')
        self.assertEqual([row['name'] for row in sorted(lots, key=lambda row: row['attention_order'])
                          if not row['storage']], names)

    def test_custom_lot_order(self):
        """Ordre choisi (ecran « Ordre des lots ») : departage les lots de meme couleur, apres rouge > orange > vert,
        a chaque niveau (lots globaux entre eux, sous-lots d'un lot entre eux)."""
        names = lambda lots: [row['name'] for row in lots if not row['storage']]
        code, lots = self.call('POST', '/api/lots/summary/', {}, local=False)
        self.assertEqual(names(lots), ['B+ 1', 'Sac de soin', 'Sac O2', 'Sac A'])  # tous rouges : type puis nom
        # B+ apres le sac A, sac O2 avant le sac de soin
        code, lots = self.call('POST', '/api/lots/order/', {'lots': [self.lot.id, self.bplus['id']]})
        self.assertEqual(code, 200)
        self.assertEqual(names(lots), ['Sac A', 'B+ 1', 'Sac de soin', 'Sac O2'])
        code, lots = self.call('POST', '/api/lots/order/', {'parent': self.bplus['id'], 'lots': [self.sac_o2['id']]})
        self.assertEqual(names(lots), ['Sac A', 'B+ 1', 'Sac O2', 'Sac de soin'])
        code, lots = self.call('POST', '/api/lots/summary/', {}, local=False)
        self.assertEqual(names(lots), ['Sac A', 'B+ 1', 'Sac O2', 'Sac de soin'])
        self.assertEqual(Lots.objects.get(id=self.soin['id']).sort_order, 2)  # absent de la liste : range apres
        code, lots = self.call('GET', '/api/lots/')
        self.assertEqual(names(lots), ['Sac A', 'B+ 1', 'Sac O2', 'Sac de soin'])
        self.assertEqual([row['tree_order'] for row in lots], list(range(len(lots))))
        code, lot = self.call('GET', f"/api/lots/{self.bplus['id']}/", local=False)
        self.assertEqual([row['name'] for row in lot['global']['lots']], ['B+ 1', 'Sac O2', 'Sac de soin'])
        # la couleur passe avant : le B+ verifie (vert) descend sous le sac A (rouge) malgre l'ordre choisi
        self.call('POST', '/api/lots/order/', {'lots': [self.bplus['id']]})
        code, lots = self.call('POST', '/api/lots/summary/', {}, local=False)
        self.assertEqual(names(lots)[:2], ['B+ 1', 'Sac O2'])
        self.call('POST', f"/api/lots/{self.soin['id']}/verif/", {'items': self.fresh(), 'user': 'M001'})
        self.call('POST', f"/api/lots/{self.sac_o2['id']}/verif/", {'items': [self.bottle()], 'user': 'M001'})
        code, lots = self.call('POST', '/api/lots/summary/', {}, local=False)
        self.assertEqual(names(lots), ['Sac A', 'B+ 1', 'Sac O2', 'Sac de soin'])
        # sous-lot propose a un autre niveau : refuse ; remise a l'ordre d'origine
        code, body = self.call('POST', '/api/lots/order/', {'lots': [self.soin['id']]})
        self.assertEqual(code, 400)
        code, lots = self.call('POST', '/api/lots/order/', {'parent': self.bplus['id'], 'reset': True})
        self.assertEqual(names(lots), ['B+ 1', 'Sac de soin', 'Sac O2', 'Sac A'])  # ordre de l'arbre, B+ range en tete
        self.assertIsNone(Lots.objects.get(id=self.soin['id']).sort_order)
        # au poste seulement (gestion des lots) : refuse depuis le web, meme en gestion
        self.user.role = 'gestion'
        self.user.save()
        code, _ = self.call('POST', '/api/lots/order/', {'user': self.badge, 'lots': [self.bplus['id']]}, local=False)
        self.assertEqual(code, 403)

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

    def test_verif_of_a_sealed_lot_by_its_seal_label(self):
        """Vérif lancee sur le lot scelle lui-meme : l'etiquette du scelle seule suffit, sans rien scanner dedans."""
        self.call('POST', f"/api/lots/{self.sac_o2['id']}/verif/", {'items': [self.bottle()], 'user': 'M001'})
        code, sealed = self.call('POST', f"/api/lots/{self.sac_o2['id']}/seal/", {'user': 'M001'})
        seal_code = sealed['seal_url'].split('s=')[1]
        payload = {'lots': [{'id': self.sac_o2['id']}], 'items': [], 'user': self.badge,
                   'labels': [self.sac_o2['id']], 'sealed': [{'id': self.sac_o2['id'], 'code': seal_code}]}
        for step in (False, True):  # vérif simple, puis validation d'un lot de la vérif en cascade
            code, report = self.call('POST', '/api/verifs/', dict(payload, step=step), local=False)
            self.assertEqual(code, 200, report)
            self.assertTrue(report['complete'])
            self.assertEqual(report['missing'], [])
            self.assertEqual(report['sealed_lots'], ['Sac O2'])
            self.assertTrue(Lots.objects.get(id=self.sac_o2['id']).is_sealed)  # scelle garde

    def test_badge_verif_with_a_sub_lot_label(self):
        """Avec un badge, l'etiquette d'un seul sous-lot (ou du lot qui le contient) suffit, pas celle d'un autre lot
        du meme lot global."""
        payload = {'lots': [{'id': self.bplus['id']}], 'items': self.fresh() + [self.bottle()], 'user': self.badge}
        code, body = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 403)
        self.assertTrue(body['label_required'])
        code, body = self.call('POST', '/api/verifs/', dict(payload, labels=[self.sac_o2['id']]), local=False)
        self.assertEqual(code, 200, body)
        # sac O2 seul : l'etiquette du B+ (lot qui le contient) compte, celle du sac de soin non
        payload = {'lots': [{'id': self.sac_o2['id']}], 'items': [self.bottle()], 'user': self.badge, 'step': True}
        code, _ = self.call('POST', '/api/verifs/', dict(payload, labels=[self.soin['id']]), local=False)
        self.assertEqual(code, 403)
        code, body = self.call('POST', '/api/verifs/', dict(payload, labels=[self.bplus['id']]), local=False)
        self.assertEqual(code, 200, body)

    def test_cascade_steps(self):
        """Vérif en cascade du B+ : chaque sac est valide a part, avec la cle du B+ scannee au debut."""
        bplus_key = [{'id': self.bplus['id'], 'key': self.keys[self.bplus['id']]}]
        bottle = self.bottle()
        # sac O2 seul, la cle du B+ (lot non verifie) valide l'etape
        payload = {'lots': [{'id': self.sac_o2['id']}], 'keys': bplus_key, 'items': [bottle], 'user': self.badge,
                   'step': True}
        code, report = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 200, report)
        self.assertEqual([row['id'] for row in report['lots']], [self.sac_o2['id']])
        self.assertTrue(report['complete'])
        # sans cle : refuse ; la cle d'un lot d'un autre lot global ne compte pas
        code, body = self.call('POST', '/api/verifs/', dict(payload, keys=[]), local=False)
        self.assertEqual(code, 403)
        code, body = self.call('POST', '/api/verifs/', dict(payload, keys=[{'id': self.lot.id, 'key': self.lot.verif_key}]),
                               local=False)
        self.assertEqual(code, 403)
        # le B+ ensuite : le sac O2, deja valide, est laisse de cote ; le sac de soin, pas fait, aussi
        before = Lots.objects.get(id=self.soin['id']).last_verif
        code, report = self.call('POST', '/api/verifs/', {
            'lots': bplus_key, 'items': [], 'user': self.badge,
            'exclude': [self.sac_o2['id'], self.soin['id']]}, local=False)
        self.assertEqual(code, 200, report)
        self.assertEqual([row['id'] for row in report['lots']], [self.bplus['id']])
        self.assertEqual(report['missing'], [])
        self.assertEqual(Items.objects.get(iid=bottle).status, ItemStatus.ACTIVE)
        self.assertEqual(Lots.objects.get(id=self.soin['id']).last_verif, before)
        # exclure un lot verifie, ou un lot hors de la verif : refuse
        code, body = self.call('POST', '/api/verifs/', {'lots': bplus_key, 'items': [], 'user': self.badge,
                                                         'exclude': [self.bplus['id']]}, local=False)
        self.assertEqual(code, 400)
        code, body = self.call('POST', '/api/verifs/', {'lots': [{'id': self.soin['id']}], 'keys': bplus_key,
                                                         'items': [], 'user': self.badge,
                                                         'exclude': [self.sac_o2['id']]}, local=False)
        self.assertEqual(code, 400)

    def test_debug_codes_reference_lot_has_no_label(self):
        """Outil de debug des codes : un lot de referentiel (B+, agregat de sacs) n'a ni etiquette publique ni cle ;
        un lot cache dans la cascade est signale pour que la suite de la vérif le saute."""
        from .. import debug_codes
        self.global_type.referentiel = True
        self.global_type.save()
        rows = {row['id']: row for row in debug_codes.catalog()['lots']}
        self.assertTrue(rows[self.bplus['id']]['virtual'])
        self.assertEqual(rows[self.bplus['id']]['codes'], [])
        self.assertFalse(rows[self.soin['id']]['virtual'])
        self.assertEqual([code['kind'] for code in rows[self.soin['id']]['codes']], ['public', 'key'])

    def test_unplaced_referentiel_line(self):
        """Ligne du referentiel dans aucun sac : le B+ est rouge (et son lot global), meme si ses sacs sont valides,
        et il reste affiche meme cache ; placee dans un sac par la repartition, le B+ redevient complet."""
        cardio = ItemType.objects.create(type='cardio', name='Cardio-pompe', perissable=False)
        self.global_type.referentiel = True
        self.global_type.save()
        LotRequirements.objects.create(lot_type=self.global_type, item_type=cardio, quantity=1)
        LotType.objects.filter(type__in=['sacpse', 'saco2x']).update(part_of=self.global_type)
        Lots.objects.filter(id=self.bplus['id']).update(hidden_in_verif=True)
        code, lot = self.call('GET', f"/api/lots/{self.bplus['id']}/")
        self.assertEqual([row['type'] for row in lot['unplaced']], ['cardio'])
        self.assertFalse(lot['complete'])
        self.assertEqual(lot['state']['code'], 'unplaced')
        self.assertFalse(lot['global']['complete'])
        code, rows = self.call('GET', '/api/lots/')
        self.assertFalse(next(row for row in rows if row['id'] == self.bplus['id'])['hidden'])
        code, body = self.call('PUT', f"/api/lots/{self.bplus['id']}/distribution/", {'referentiel': 'bplusx', 'rows': [
            {'lot': self.sac_o2['id'], 'type': 'cardio', 'quantity': 1}]})
        self.assertEqual(code, 200, body)
        code, lot = self.call('GET', f"/api/lots/{self.bplus['id']}/")
        self.assertEqual(lot['unplaced'], [])
        self.assertNotEqual(lot['state']['code'], 'unplaced')

    def test_hidden_lot_in_cascade(self):
        """B+ du VPS cache dans la vérif en cascade : ses sacs remontent dans le VPS (descendants 'hidden'), le B+
        (referentiel, sans etiquette) est valide par ses sacs, jamais par une etape a lui."""
        vps_type = LotType.objects.create(type='vpsxxx', name='VPS', unique=True)
        self.global_type.referentiel = True
        self.global_type.save()
        code, vps = self.call('POST', '/api/lots/', {'lot_type': 'vpsxxx', 'name': 'VPS', 'user': 'M001'})
        code, body = self.call('PATCH', f"/api/lots/{self.bplus['id']}/update/", {'parent': vps['id']})
        self.assertEqual(code, 200, body)
        code, body = self.call('PATCH', f"/api/lots/{self.bplus['id']}/update/", {'hidden_in_verif': True})
        self.assertEqual(code, 200, body)
        self.assertTrue(body['hidden_in_verif'])
        self.assertTrue(body['virtual'])
        code, lot = self.call('GET', f"/api/lots/{vps['id']}/")
        rows = {row['id']: row for row in lot['descendants']}
        self.assertTrue(rows[self.bplus['id']]['hidden'])
        self.assertFalse(rows[self.soin['id']]['hidden'])
        self.assertEqual(rows[self.soin['id']]['parent'], self.bplus['id'])  # vraie hierarchie, remontee par les fronts
        # un lot qui attend des items a lui reste affiche
        LotRequirements.objects.create(lot_type=self.global_type, item_type=self.compresses, quantity=1)
        code, lot = self.call('GET', f"/api/lots/{vps['id']}/")
        self.assertFalse({row['id']: row for row in lot['descendants']}[self.bplus['id']]['hidden'])
        LotRequirements.objects.filter(lot_type=self.global_type).delete()
        vps_key = [{'id': vps['id'], 'key': Lots.objects.get(id=vps['id']).verif_key}]
        # premier sac : le B+ n'est pas encore valide
        code, report = self.call('POST', '/api/verifs/', {'lots': [{'id': self.sac_o2['id']}], 'keys': vps_key,
                                                         'items': [self.bottle()], 'user': self.badge, 'step': True},
                                 local=False)
        self.assertEqual(code, 200, report)
        self.assertIsNone(Lots.objects.get(id=self.bplus['id']).last_verif)
        # second sac : le B+ est valide avec lui
        code, report = self.call('POST', '/api/verifs/', {'lots': [{'id': self.soin['id']}], 'keys': vps_key,
                                                         'items': [], 'user': self.badge, 'step': True}, local=False)
        self.assertEqual(code, 200, report)
        self.assertIn(self.bplus['id'], [row['id'] for row in report['lots'] if row['verified']])
        self.assertIsNotNone(Lots.objects.get(id=self.bplus['id']).last_verif)
        # le VPS valide a la fin : le B+, sans vérif a lui, n'est pas revalide (ses sacs sont laisses de cote)
        stamp = Lots.objects.get(id=self.bplus['id']).last_verif
        code, report = self.call('POST', '/api/verifs/', {'lots': vps_key, 'items': [], 'user': self.badge,
                                                         'exclude': [self.soin['id'], self.sac_o2['id']]}, local=False)
        self.assertEqual(code, 200, report)
        self.assertNotIn(self.bplus['id'], [row['id'] for row in report['lots']])
        self.assertEqual(Lots.objects.get(id=self.bplus['id']).last_verif, stamp)

    def test_cascade_item_counted_where_found(self):
        """Vérif en cascade : un item range dans un sous-lot, scanne pendant qu'on etait dans un autre, y est deplace."""
        content = self.fresh()
        self.call('POST', f"/api/lots/{self.soin['id']}/verif/", {'items': content, 'user': 'M001'})
        bplus_key = [{'id': self.bplus['id'], 'key': self.keys[self.bplus['id']]}]
        bottle = self.bottle()
        places = {bottle: self.sac_o2['id'], content[0]: self.sac_o2['id']}
        code, report = self.call('POST', '/api/verifs/', {
            'lots': bplus_key, 'items': content + [bottle], 'places': places, 'user': self.badge}, local=False)
        self.assertEqual(code, 200, report)
        self.assertEqual(Items.objects.get(iid=content[0]).location_id, self.sac_o2['id'])
        self.assertEqual(Items.objects.get(iid=content[1]).location_id, self.soin['id'])
        # scanne au niveau du B+ (lot qui contient son sac) : il reste dans son sac
        code, report = self.call('POST', '/api/verifs/', {
            'lots': bplus_key, 'items': content + [bottle], 'places': {content[0]: self.bplus['id']},
            'user': self.badge}, local=False)
        self.assertEqual(code, 200, report)
        self.assertEqual(Items.objects.get(iid=content[0]).location_id, self.sac_o2['id'])

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
        self.assertTrue(body['url'].endswith(f'item?id={compact_iid(stored[0])}'))
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

    def test_storage_hierarchy_and_kind(self):
        # piece > armoire > tiroir, chacun avec son genre (« rangement » par defaut)
        code, body = self.call('POST', '/api/lot-types/', {'type': 'piece1', 'name': 'La TECH', 'storage': True,
                                                           'storage_kind': '  pièce '})
        self.assertEqual(code, 201)
        piece = body['created_lot']
        self.assertEqual(piece['storage_kind'], 'pièce')
        self.assertEqual(piece['parent'], Lots.stock().id)
        code, body = self.call('POST', '/api/lot-types/', {'type': 'armoi1', 'name': 'Armoire 1', 'storage': True,
                                                           'storage_kind': 'armoire', 'parent': piece['id']})
        armoire = body['created_lot']
        code, body = self.call('POST', '/api/lot-types/', {'type': 'tiroi1', 'name': 'Tiroir 1', 'storage': True,
                                                           'parent': armoire['id']})
        tiroir = body['created_lot']
        self.assertEqual(tiroir['storage_kind'], 'rangement')
        self.assertEqual([p['name'] for p in tiroir['path']], [Lots.stock().name, 'La TECH', 'Armoire 1'])
        code, body = self.call('PATCH', f"/api/lots/{tiroir['id']}/update/", {'storage_kind': 'tiroir'})
        self.assertEqual(body['storage_kind'], 'tiroir')
        code, body = self.call('PATCH', f"/api/lots/{tiroir['id']}/update/", {'storage_kind': ''})
        self.assertEqual(body['storage_kind'], 'rangement')
        # un lot ordinaire n'a pas de genre, le stock non plus
        self.assertEqual(self.call('GET', f"/api/lots/{self.soin['id']}/")[1]['storage_kind'], '')
        self.assertEqual(self.call('GET', f'/api/lots/{Lots.stock().id}/')[1]['storage_kind'], '')
        # arbre de la piece : genre de chaque rangement
        code, body = self.call('GET', f"/api/lots/{piece['id']}/")
        kinds = {row['name']: row['storage_kind'] for row in body['global']['lots']}
        self.assertEqual(kinds['Armoire 1'], 'armoire')

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
        from .. import notifications
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


class OptionalContentTests(ApiTestCase):
    """Materiel facultatif a la verif (casques du VPS) : le lot se valide sans, mais on peut le scanner pour savoir ou
    il est ; non scanne, il reste ou il etait. Seulement du non consommable sans taches."""

    def setUp(self):
        super().setUp()
        self.casque = ItemType.objects.create(type='casque', name='Casque', consumable=False)
        self.vps_type = LotType.objects.create(type='vpsxxx', name='VPS')
        code, self.vps = self.call('POST', '/api/lots/', {'lot_type': 'vpsxxx', 'name': 'VPS 1', 'user': 'M001'})
        self.assertEqual(code, 201)

    def fresh(self, count=2):
        return [item.iid for item in self.create(self.compresses, self.today + timedelta(days=90), count)]

    def set_requirements(self, rows):
        return self.call('PUT', '/api/lot-types/vpsxxx/requirements/', {'requirements': rows})

    def test_only_non_consumable_without_tasks(self):
        code, body = self.set_requirements([{'type': 'compre', 'quantity': 2, 'optional': True}])
        self.assertEqual(code, 400)
        self.assertIn('ne peut pas être facultatif', body['error'] if 'error' in body else str(body))
        self.casque.tasks = ['Jugulaire']
        self.casque.save()
        code, body = self.set_requirements([{'type': 'casque', 'quantity': 4, 'optional': True}])
        self.assertEqual(code, 400)
        self.casque.tasks = []
        self.casque.save()
        code, body = self.set_requirements([{'type': 'casque', 'quantity': 4, 'optional': True}])
        self.assertEqual(code, 200)
        self.assertEqual(body['requirements'][0]['optional'], True)
        # devenu consommable : le reglage ne vaut plus rien, l'attendu redevient obligatoire
        self.casque.consumable = True
        self.casque.save()
        code, lot = self.call('GET', f"/api/lots/{self.vps['id']}/")
        self.assertEqual((lot['requirements'][0]['optional'], lot['requirements'][0]['required']), (False, 4))

    def test_lot_valid_without_optional_and_items_stay(self):
        self.set_requirements([{'type': 'compre', 'quantity': 2}, {'type': 'casque', 'quantity': 2, 'optional': True}])
        casques = [item.iid for item in self.create(self.casque, None, 2)]
        Items.objects.filter(iid__in=casques).update(location_id=self.vps['id'])
        code, lot = self.call('GET', f"/api/lots/{self.vps['id']}/")
        row = next(row for row in lot['requirements'] if row['type'] == 'casque')
        self.assertEqual((row['required'], row['expected'], row['optional']), (0, 2, True))
        self.assertTrue(all(item['optional'] for item in lot['items'] if item['type'] == 'casque'))
        # verif sans les casques : complete, les casques restent dans le VPS (ni manquants, ni hors lot)
        code, report = self.call('POST', f"/api/lots/{self.vps['id']}/verif/", {'items': self.fresh(), 'user': 'M001'})
        self.assertEqual(code, 200)
        self.assertTrue(report['complete'])
        self.assertEqual((report['missing'], report['loose']), ([], []))
        for item in Items.objects.filter(iid__in=casques):
            self.assertEqual((item.status, item.location_id, item.missed_verifs), (ItemStatus.ACTIVE, self.vps['id'], 0))
        code, lot = self.call('GET', f"/api/lots/{self.vps['id']}/")
        self.assertEqual(lot['state']['code'], 'verified')
        # un casque scanne ailleurs (dans le sac A) y est range : on sait ou il est
        code, report = self.call('POST', f"/api/lots/{self.lot.id}/verif/",
                                 {'items': self.fresh() + [casques[0]], 'user': 'M001'})
        self.assertEqual(Items.objects.get(iid=casques[0]).location_id, self.lot.id)
        # a la vérif suivante du sac A, il n'y est pas facultatif : absent, il passe hors lot comme avant
        code, report = self.call('POST', f"/api/lots/{self.lot.id}/verif/", {'items': self.fresh(), 'user': 'M001'})
        self.assertEqual(report['loose'], [casques[0]])

    def test_partial_verif_not_held_by_optional(self):
        self.set_requirements([{'type': 'compre', 'quantity': 2}, {'type': 'casque', 'quantity': 2, 'optional': True}])
        casque = self.create(self.casque, None, 1)[0].iid
        Items.objects.filter(iid=casque).update(location_id=self.vps['id'])
        verif = services.perform_verif(Lots.objects.get(id=self.vps['id']), self.fresh(), 'M001', partial=True)
        self.assertEqual([row['verified'] for row in verif['lots']], [True])
        self.assertEqual(Items.objects.get(iid=casque).location_id, self.vps['id'])

    def test_optional_sub_lot(self):
        rack_type = LotType.objects.create(type='rackxx', name='Râtelier casques')
        LotRequirements.objects.create(lot_type=rack_type, item_type=self.casque, quantity=4)
        code, rack = self.call('POST', '/api/lots/', {'lot_type': 'rackxx', 'name': 'Casques', 'user': 'M001'})
        # pas de parent : refuse
        code, body = self.call('PATCH', f"/api/lots/{rack['id']}/update/", {'optional': True})
        self.assertEqual(code, 400)
        code, rack = self.call('PATCH', f"/api/lots/{rack['id']}/update/", {'parent': self.vps['id'], 'optional': True})
        self.assertEqual(code, 200)
        self.assertTrue(rack['optional'])
        self.assertEqual(rack['requirements'][0]['required'], 0)
        # jamais verifie : facultatif, le VPS est valide sans lui
        self.assertEqual(rack['state']['code'], 'optional')
        code, sac = self.call('POST', '/api/lots/', {'lot_type': 'sacpse', 'name': 'Sac VPS', 'parent': self.vps['id'],
                                                     'user': 'M001'})
        self.call('POST', f"/api/lots/{sac['id']}/verif/", {'items': self.fresh(), 'user': 'M001'})
        code, lot = self.call('GET', f"/api/lots/{self.vps['id']}/", local=False)
        self.assertEqual(lot['global']['kind'], 'ok')
        self.assertFalse(next(row for row in lot['global']['lots'] if row['id'] == rack['id'])['counted'])
        # verif du VPS entier : les casques non scannes du sous-lot restent dedans
        casques = [item.iid for item in self.create(self.casque, None, 2)]
        Items.objects.filter(iid__in=casques).update(location_id=rack['id'])
        report = services.perform_verif(Lots.objects.get(id=self.vps['id']), self.fresh(), 'M001')
        self.assertTrue(report['complete'])
        self.assertEqual(report['loose'], [])
        self.assertEqual(set(Items.objects.filter(iid__in=casques).values_list('location_id', flat=True)), {rack['id']})

    def test_whole_sub_lot_optional_even_consumables(self):
        code, sac = self.call('POST', '/api/lots/', {'lot_type': 'sacpse', 'name': 'Sac de réserve',
                                                     'parent': self.vps['id'], 'user': 'M001'})
        code, sac = self.call('PATCH', f"/api/lots/{sac['id']}/update/", {'optional': True})
        self.assertEqual(code, 200)
        self.assertEqual((sac['requirements'][0]['required'], sac['requirements'][0]['optional']), (0, True))
        compresses = self.fresh()
        Items.objects.filter(iid__in=compresses).update(location_id=sac['id'])
        report = services.perform_verif(Lots.objects.get(id=self.vps['id']), [], 'M001')
        self.assertTrue(report['complete'])
        self.assertEqual(report['missing'], [])
        self.assertEqual(set(Items.objects.filter(iid__in=compresses).values_list('location_id', flat=True)), {sac['id']})

    def test_full_verif_makes_everything_required(self):
        self.set_requirements([{'type': 'compre', 'quantity': 2}, {'type': 'casque', 'quantity': 2, 'optional': True}])
        casques = [item.iid for item in self.create(self.casque, None, 2)]
        Items.objects.filter(iid__in=casques).update(location_id=self.vps['id'])
        code, report = self.call('POST', '/api/verifs/', {'lots': [{'id': self.vps['id']}], 'items': self.fresh(),
                                                         'user': 'M001', 'full': True})
        self.assertEqual(code, 200)
        self.assertFalse(report['complete'])
        self.assertEqual(sorted(report['loose']), sorted(casques))  # non consommables absents : hors lot
        row = next(row for row in report['requirements'] if row['type'] == 'casque')
        self.assertEqual((row['required'], row['optional']), (2, False))


class VerifCacheTests(SubLotCase):
    """Cache de vérif des fronts (verif_cache) : tout charge au debut de la vérif, puis seulement ce qui a change."""

    def setUp(self):
        super().setUp()  # lot global B+ avec un sac de soin et un sac O2
        from .. import verif_cache
        self.cache = verif_cache
        verif_cache.forget()

    def load(self, known=None, local=False):
        self.cache.forget()
        code, body = self.call('POST', '/api/verifs/cache/', {'buckets': known or {}}, local=local)
        self.assertEqual(code, 200)
        return body

    @staticmethod
    def records(body):
        return {key: value for bucket in body['buckets'].values() for key, value in bucket['records'].items()}

    def test_full_then_only_changes(self):
        soin = Lots.objects.get(id=self.soin['id'])
        iids = self.fresh(2)
        services.perform_verif(soin, iids, 'M:M001')
        stock = self.fresh(1)[0]
        body = self.load()
        records = self.records(body)
        self.assertEqual(body['count'], len(records))
        # types d'items (meme hors des lots), items en lot et en stock, lot global avec ses sous-lots
        self.assertIn('t:garrot', records)
        self.assertIn(f'i:{stock}', records)
        self.assertEqual(records[f'i:{iids[0]}']['location'], soin.id)
        root = records[f"l:{self.bplus['id']}"]
        self.assertNotIn(f"l:{self.soin['id']}", records)  # un sous-lot est dans l'arbre de son lot global
        bag = next(row for row in root['descendants'] if row['id'] == soin.id)
        self.assertEqual(sorted(bag['items']), sorted(iids))  # items par iid seulement
        # jamais de cle ni de code de scelle
        self.assertNotIn('verif_key', json.dumps(body))
        known = {index: bucket['hash'] for index, bucket in body['buckets'].items()}
        again = self.load(known)
        self.assertEqual(again['buckets'], {})
        self.assertEqual(again['version'], body['version'])
        # nouveau signalement sur un item : son seau (et celui du lot dont l'etat change) revient, pas les autres
        code, _ = self.call('POST', '/api/reports/', {'user': self.badge, 'item': iids[0], 'comment': 'Emballage percé',
                                                      'usable': False}, local=False)
        self.assertEqual(code, 201)
        changed = self.load(known)
        self.assertNotEqual(changed['version'], body['version'])
        self.assertLess(len(changed['buckets']), 10)
        item = self.records(changed)[f'i:{iids[0]}']
        self.assertTrue(item['reported'])
        self.assertTrue(item['unusable'])

    def test_badge_checked_and_full_names_on_poste(self):
        code, _ = self.call('POST', '/api/verifs/cache/', {'user': {'matricule': 'M001', 'key': 'faux'}}, local=False)
        self.assertNotEqual(code, 200)
        code, body = self.call('POST', '/api/verifs/cache/', {'user': self.badge}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(self.load(local=True)['count'], body['count'])
