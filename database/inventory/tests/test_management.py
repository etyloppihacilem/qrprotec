"""Tests du back : gestion (API locale)."""

from __future__ import annotations

from typing import TYPE_CHECKING

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)

if TYPE_CHECKING:
    from inventory.models import Secouristes


class ManagementTests(ApiTestCase):
    def test_batch_with_sealed_pack_and_stock(self) -> None:
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

    def test_batch_several_sealed_packs(self) -> None:
        date = (self.today + timedelta(days=200)).isoformat()
        code, body = self.call('POST', '/api/items/batch/', {'type': 'compre', 'peremption': date, 'count': 4,
                                                             'sealed_pack': True, 'packs': 3, 'user': 'M001'})
        self.assertEqual(code, 201)
        self.assertEqual(len(body['items']), 12)
        self.assertEqual(len(body['sealed_packs']), 3)
        self.assertEqual(body['sealed_pack']['id'], body['sealed_packs'][0]['id'])
        for pack in body['sealed_packs']:
            self.assertEqual(pack['count'], 4)
            self.assertEqual(Items.objects.filter(sealed_pack_id=pack['id']).count(), 4)
        # une seule reception dans les receptions passees, avec ses 3 paquets
        code, receptions = self.call('GET', f"/api/receptions/?item={body['items'][0]['iid']}")
        self.assertEqual(len(receptions), 1)
        self.assertEqual(receptions[0]['count'], 12)
        self.assertEqual(receptions[0]['sealed_packs'], 3)
        # sans paquet ferme, le nombre de paquets est ignore
        code, body = self.call('POST', '/api/items/batch/', {'type': 'compre', 'peremption': date, 'count': 2,
                                                             'packs': 5})
        self.assertEqual(len(body['items']), 2)
        self.assertNotIn('sealed_packs', body)
        code, _ = self.call('POST', '/api/items/batch/', {'type': 'compre', 'peremption': date, 'count': 100,
                                                          'sealed_pack': True, 'packs': 101})
        self.assertEqual(code, 400)

    def test_batch_into_storage(self) -> None:
        storage_type = LotType.objects.create(type='armoir', name='Armoire', storage=True)
        armoire = Lots(lot_type=storage_type, name='Armoire', name_short='A', created_by='test', parent=Lots.stock())
        armoire.save()
        date = (self.today + timedelta(days=200)).isoformat()
        code, body = self.call('POST', '/api/items/batch/',
                               {'type': 'compre', 'peremption': date, 'count': 50, 'location': armoire.id, 'user': 'M001'})
        self.assertEqual(code, 201)
        self.assertEqual(Items.objects.filter(location=armoire).count(), 50)
        code, stock = self.call('GET', '/api/stock/')
        self.assertEqual(next(row for row in stock if row['type'] == 'compre')['stock_fresh'], 50)
        # le lot du stock vaut le stock en vrac, un lot normal est refuse
        code, body = self.call('POST', '/api/items/batch/',
                               {'type': 'compre', 'peremption': date, 'count': 2, 'location': Lots.stock().id})
        self.assertEqual(code, 201)
        self.assertIsNone(Items.objects.get(iid=body['items'][0]['iid']).location_id)
        code, _ = self.call('POST', '/api/items/batch/',
                            {'type': 'compre', 'peremption': date, 'count': 2, 'location': self.lot.id})
        self.assertEqual(code, 400)
        self.assertEqual(Items.objects.filter(location=self.lot).count(), 0)

    def test_store_reception(self) -> None:
        storage_type = LotType.objects.create(type='armoir', name='Armoire', storage=True)
        armoire = Lots(lot_type=storage_type, name='Armoire', name_short='A', created_by='test', parent=Lots.stock())
        armoire.save()
        date = (self.today + timedelta(days=200)).isoformat()
        code, body = self.call('POST', '/api/items/batch/', {'type': 'compre', 'peremption': date, 'count': 5})
        iids = [item['iid'] for item in body['items']]
        Items.objects.filter(iid=iids[0]).update(location=self.lot)  # deja mis dans un lot : il y reste
        code, receptions = self.call('GET', f'/api/receptions/?item={iids[1]}')
        self.assertEqual((receptions[0]['count'], receptions[0]['in_stock']), (5, 4))
        code, body = self.call('POST', '/api/receptions/store/',
                               {'reception': iids[2], 'location': armoire.id, 'user': 'M001'})
        self.assertEqual(code, 200)
        self.assertEqual((body['count'], body['total']), (4, 5))
        self.assertEqual(sorted(Items.objects.filter(location=armoire).values_list('iid', flat=True)), iids[1:])
        self.assertEqual(Items.objects.get(iid=iids[0]).location_id, self.lot.id)
        armoire.refresh_from_db()
        self.assertFalse(armoire.verif_recommended)
        # retour en vrac dans le stock
        code, body = self.call('POST', '/api/receptions/store/', {'reception': iids[2], 'location': ''})
        self.assertEqual(body['count'], 4)
        self.assertEqual(Items.objects.filter(location__isnull=True).count(), 4)
        code, _ = self.call('POST', '/api/receptions/store/', {'reception': iids[2], 'location': self.lot.id})
        self.assertEqual(code, 400)

    def test_reception_storage_suggestion(self) -> None:
        storage_type = LotType.objects.create(type='armoir', name='Armoire', storage=True)
        armoire = Lots(lot_type=storage_type, name='Armoire', name_short='A', created_by='test', parent=Lots.stock())
        armoire.save()
        tiroir_type = LotType.objects.create(type='tiroir', name='Tiroir', storage=True)
        tiroir = Lots(lot_type=tiroir_type, name='Tiroir', name_short='T', created_by='test', parent=armoire)
        tiroir.save()
        date = (self.today + timedelta(days=200)).isoformat()
        code, body = self.call('GET', '/api/receptions/storage/?type=compre')
        self.assertEqual((code, body['location']), (200, None))
        self.call('POST', '/api/items/batch/', {'type': 'compre', 'peremption': date, 'count': 3, 'location': armoire.id})
        code, body = self.call('GET', '/api/receptions/storage/?type=compre')
        self.assertEqual(body['location'], armoire.id)
        # une reception en vrac rangee ensuite dans le tiroir : le tiroir devient la proposition
        code, body = self.call('POST', '/api/items/batch/', {'type': 'compre', 'peremption': date, 'count': 2})
        self.call('POST', '/api/receptions/store/', {'reception': body['items'][0]['iid'], 'location': tiroir.id})
        code, body = self.call('GET', '/api/receptions/storage/?type=compre')
        self.assertEqual(body['location'], tiroir.id)
        code, body = self.call('GET', '/api/receptions/storage/?type=garrot')
        self.assertIsNone(body['location'])

    def test_create_entities(self) -> None:
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

    def test_requirement_locations_order(self) -> None:
        """Emplacements du contenu attendu : ordre choisi par la TECH, nouveaux a la suite, meme emplacement malgre la
        casse ou les accents, inutilises oublies ; exigences rendues dans cet ordre, sans emplacement en dernier."""
        ItemType.objects.create(type='serphy', name='Sérum phy')
        ItemType.objects.create(type='ciseau', name='Ciseaux')
        url = '/api/lot-types/sacpse/requirements/'
        code, body = self.call('PUT', url, {'requirements': [
            {'type': 'compre', 'location': ' Pochette  rouge '}, {'type': 'garrot'},
            {'type': 'serphy', 'location': 'Pochette bleue'}, {'type': 'ciseau', 'location': 'pochette ROUGE'}]})
        self.assertEqual(code, 200)
        self.assertEqual(body['locations'], ['Pochette rouge', 'Pochette bleue'])
        self.assertEqual([(row['type'], row['location']) for row in body['requirements']], [
            ('ciseau', 'Pochette rouge'), ('compre', 'Pochette rouge'), ('serphy', 'Pochette bleue'), ('garrot', '')])
        rows = [{'type': row['type'], 'location': row['location']} for row in body['requirements']]
        code, body = self.call('PUT', url, {'requirements': rows, 'locations': ['pochette bleue', 'Inutilisée', 'Pochette rouge']})
        self.assertEqual(body['locations'], ['Pochette bleue', 'Pochette rouge'])
        self.assertEqual([row['type'] for row in body['requirements']], ['serphy', 'ciseau', 'compre', 'garrot'])
        # sans locations : l'ordre enregistre est garde
        code, body = self.call('PUT', url, {'requirements': rows})
        self.assertEqual(body['locations'], ['Pochette bleue', 'Pochette rouge'])
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/')
        self.assertEqual([row['type'] for row in body['requirements']], ['serphy', 'ciseau', 'compre', 'garrot'])
        self.assertEqual([location for location, _ in lot_sheet.requirement_groups(self.lot)],
                         ['Pochette bleue', 'Pochette rouge', lot_sheet.NO_LOCATION])

    def test_lot_own_content(self) -> None:
        """Contenu propre d'un lot : items en plus du type (ajoutes a sa quantite ou seuls), emplacements et ordre de
        ce lot ; le type reste la base des autres lots, et un lot qui reprend l'ordre du type le suit."""
        ItemType.objects.create(type='serphy', name='Sérum phy')
        self.call('PUT', '/api/lot-types/sacpse/requirements/', {'requirements': [
            {'type': 'compre', 'quantity': 2, 'location': 'Pochette rouge'},
            {'type': 'serphy', 'quantity': 1, 'location': 'Pochette bleue'}]})
        other = Lots(lot_type=self.lot_type, name='Sac B', name_short='B', created_by='test')
        other.save()
        url = f'/api/lots/{self.lot.id}/content/'
        code, body = self.call('PUT', url, {'content': [
            {'type': 'compre', 'extra': 3}, {'type': 'garrot', 'extra': 1, 'location': 'poche AVANT'},
            {'type': 'serphy', 'location': 'Poche avant'}], 'locations': ['Poche avant', 'Pochette rouge']})
        self.assertEqual(code, 200)
        self.assertEqual([(row['type'], row['required'], row['extra'], row['location']) for row in body['requirements']], [
            ('garrot', 1, 1, 'Poche avant'), ('serphy', 1, 0, 'Poche avant'), ('compre', 5, 3, 'Pochette rouge')])
        self.assertEqual(body['own_content']['locations'], ['Poche avant', 'Pochette rouge'])
        self.assertFalse(body['complete'])
        # les autres lots du type ne changent pas
        code, body = self.call('GET', f'/api/lots/{other.id}/')
        self.assertEqual([(row['type'], row['required']) for row in body['requirements']], [('compre', 2), ('serphy', 1)])
        self.assertEqual(services._required_by_lot([self.lot])[0][self.lot.id], {'garrot': 1, 'serphy': 1, 'compre': 5})
        self.assertEqual([location for location, _ in lot_sheet.requirement_groups(self.lot)], ['Poche avant', 'Pochette rouge'])
        # meme emplacement que le type : le suit ; meme ordre que le type : le lot n'a pas d'ordre a lui
        code, body = self.call('PUT', url, {'content': [{'type': 'compre', 'extra': 0, 'location': 'pochette rouge'},
                                                        {'type': 'garrot', 'extra': 1, 'location': 'Pochette bleue'}],
                                            'locations': ['pochette rouge', 'Pochette bleue']})
        self.assertEqual(body['own_content'], {'locations': [], 'rows': [
            {'type': 'garrot', 'type_name': 'Garrot', 'extra': 1, 'removed': 0, 'location': 'Pochette bleue'}]})
        self.assertEqual([row['type'] for row in body['requirements']], ['compre', 'garrot', 'serphy'])
        # seulement ajouter, un type en double refuse, un item en plus empeche d'archiver son type
        code, _ = self.call('PUT', url, {'content': [{'type': 'garrot', 'extra': -1}]})
        self.assertEqual(code, 400)
        code, _ = self.call('PUT', url, {'content': [{'type': 'garrot', 'extra': 1}, {'type': 'garrot', 'extra': 2}]})
        self.assertEqual(code, 400)
        code, body = self.call('PATCH', '/api/item-types/garrot/', {'archived': True})
        self.assertEqual(code, 400)
        self.assertIn('Sac A', body['error'])

    def test_content_versions(self) -> None:
        """Une nouvelle version du contenu (type de lot, contenu propre d'un lot) n'est creee que si le contenu change
        et qu'une verif a deja ete faite sur la version en cours ; sinon elle est corrigee en place."""
        url = '/api/lot-types/sacpse/requirements/'
        version = self.lot_type.version
        rows = [{'type': 'compre', 'quantity': 2, 'location': 'Pochette rouge'}]
        code, body = self.call('PUT', url, {'requirements': rows})
        self.assertEqual((code, body['version'], body['new_version']), (200, version, False))  # aucune verif
        code, body = self.call('PUT', url, {'requirements': rows})
        self.assertEqual((body['version'], body['new_version']), (version, False))  # rien de change
        items = self.create(self.compresses, self.today + timedelta(days=200), 2)
        code, body = self.call('POST', '/api/verifs/', {'lots': [self.lot.id], 'items': [item.iid for item in items]})
        self.assertEqual(code, 200, body)
        self.assertEqual(Verifs.objects.get(lot=self.lot).lot_type_version, version)
        code, body = self.call('PUT', url, {'requirements': rows})
        self.assertEqual((body['version'], body['new_version']), (version, False))  # toujours rien de change
        rows[0]['quantity'] = 3
        code, body = self.call('PUT', url, {'requirements': rows})
        self.assertEqual((body['version'], body['new_version']), (version + 1, True))
        rows[0]['quantity'] = 4  # aucune verif sur la nouvelle version : corrigee en place
        code, body = self.call('PUT', url, {'requirements': rows})
        self.assertEqual((body['version'], body['new_version']), (version + 1, False))
        # contenu propre du lot : meme regle, sur la version du lot
        self.lot.refresh_from_db()
        lot_version = self.lot.version
        content = f'/api/lots/{self.lot.id}/content/'
        self.call('PUT', content, {'content': [{'type': 'garrot', 'extra': 1}]})  # verif faite sur cette version
        self.lot.refresh_from_db()
        self.assertEqual(self.lot.version, lot_version + 1)
        self.call('PUT', content, {'content': [{'type': 'garrot', 'extra': 1}]})  # rien de change
        self.call('PUT', content, {'content': [{'type': 'garrot', 'extra': 2}]})  # pas de verif dessus : en place
        self.lot.refresh_from_db()
        self.assertEqual(self.lot.version, lot_version + 1)

    def test_user_fonction(self) -> None:
        """Role dans l'equipe : texte libre, Secouriste par defaut, sans effet sur les droits."""
        code, body = self.call('POST', '/api/users/', {'matricule': 'M002', 'nom': 'Martin', 'prenom': 'Paul'})
        self.assertEqual((code, body['fonction'], body['role']), (201, 'Secouriste', 'normal'))
        code, body = self.call('POST', '/api/users/', {'matricule': 'M003', 'nom': 'Durand', 'prenom': 'Léa',
                                                       'fonction': "  l'OPÉ  "})
        self.assertEqual((code, body['fonction'], body['role']), (201, "l'OPÉ", 'normal'))
        code, body = self.call('PATCH', '/api/users/M002/', {'fonction': 'le   TECH'})
        self.assertEqual((code, body['fonction'], body['privileged']), (200, 'le TECH', False))
        code, body = self.call('PATCH', '/api/users/M002/', {'fonction': '  '})
        self.assertEqual(body['fonction'], 'Secouriste')  # vide : role par defaut
        code, body = self.call('PATCH', '/api/users/M002/', {'fonction': 'x' * 40})
        self.assertEqual(len(body['fonction']), 32)

    def test_item_type_tasks(self) -> None:
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
        self.assertTrue(body['task_url'].endswith(f'task?id={compact_iid(item.iid)}'))
        code, body = self.call('PATCH', '/api/item-types/defibr/', {'tasks': []})
        self.assertEqual(body['tasks'], [])
        code, body = self.call('GET', f'/api/items/{self.create(self.garrot, None, 1)[0].iid}/', local=False)
        self.assertEqual(body['tasks'], [])

    def test_delete_requires_reason(self) -> None:
        item = self.create(self.garrot, None, 1)[0]
        code, _ = self.call('POST', f'/api/items/{item.iid}/delete/', {})
        self.assertEqual(code, 400)
        code, body = self.call('POST', f'/api/items/{item.iid}/delete/', {'reason': 'casse'})
        self.assertEqual(body['status'], 'deleted')
        code, body = self.call('POST', f'/api/items/{item.iid}/restore/', {})
        self.assertEqual(body['status'], 'active')


class RoleTests(ApiTestCase):
    def badge(self, user: Secouristes) -> dict[str, str]:
        return {'matricule': user.matricule, 'key': user.new_key}

    def test_roles_and_public_summaries(self) -> None:
        code, body = self.call('POST', '/api/users/', {'matricule': 'G001', 'nom': 'Gest', 'prenom': 'Ion',
                                                       'role': 'gestion'})
        self.assertEqual(code, 201)
        self.assertEqual(body['role'], 'gestion')
        self.assertTrue(body['privileged'])
        gestion_key = body['key']  # cle du badge : seulement dans la reponse de creation
        code, _ = self.call('POST', '/api/users/', {'matricule': 'X001', 'nom': 'a', 'prenom': 'b', 'role': 'chief'})
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
