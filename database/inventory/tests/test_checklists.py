"""Tests du back : listes de materiel et taches du TECH."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)

if TYPE_CHECKING:
    from inventory.models import Items, Lots, Secouristes


class ChecklistTests(ApiTestCase):
    """Listes de materiel : lignes, verif en direct, validation (sortie du stock), retour par la liste."""

    def setUp(self) -> None:
        super().setUp()
        self.chief = self.user
        self.chief.role = 'chef'
        self.chief.save(update_fields=['role'])
        self.other = Secouristes(matricule='M002', nom='Martin', prenom='Paul')  # secouriste
        self.other.renew_key()
        self.other.save()
        self.fresh = self.create(self.compresses, self.today + timedelta(days=200), 3)
        self.garrot.consumable = False  # materiel : reste sorti s'il ne rentre pas
        self.garrot.save()
        self.garrots = self.create(self.garrot, None, 2)

    def as_(self, user: Secouristes, path: str, data: Any = None) -> Any:
        return self.call('POST', path, {**(data or {}), 'user': {'matricule': user.matricule, 'key': user.new_key}},
                         local=False)

    def new_list(self, user: Secouristes | None = None, **extra: Any) -> dict[str, Any]:
        code, body = self.as_(user or self.chief, '/api/checklists/', {
            'name': 'Poste de secours', 'lines': [{'lot': self.lot.id}, {'type': 'compre', 'quantity': 2},
                                                  {'type': 'garrot', 'quantity': 1}], **extra})
        self.assertEqual(code, 201, body)
        return body

    def test_options_with_descriptions(self) -> None:
        """Options de l'editeur : descriptions des types d'items et de lots (cherchees apres les noms), celle du type
        pour un lot."""
        self.compresses.description = 'Marque Hartmann, gazes'
        self.compresses.save(update_fields=['description'])
        self.lot.lot_type.description = 'Sac rouge de premiers secours'
        self.lot.lot_type.save(update_fields=['description'])
        Lots.objects.create(lot_type=self.lot.lot_type, name='Sac B', name_short='Sac B', created_by='M001')
        code, body = self.as_(self.chief, '/api/checklists/options/')
        self.assertEqual(code, 200, body)
        self.assertEqual({row['type']: row['description'] for row in body['types']}['compre'], 'Marque Hartmann, gazes')
        self.assertEqual({row['id']: row['description'] for row in body['lots']}[self.lot.id], 'Sac rouge de premiers secours')
        self.assertEqual({row['type']: row['description'] for row in body['lot_types']}[self.lot.lot_type.type],
                         'Sac rouge de premiers secours')

    def test_options_single_lot_not_a_choice(self) -> None:
        """Un type de lot unique (un VPS, meme referentiel) ou qui n'a qu'un lot n'est pas propose en « lot au choix » :
        seulement son lot (Hippolyte, 2026-11-11)."""
        vps_type = LotType.objects.create(type='vps', name='VPS', unique=True, referentiel=True)
        vps = Lots.objects.create(lot_type=vps_type, name='VPS', name_short='VPS', created_by='M001')
        code, body = self.as_(self.chief, '/api/checklists/options/')
        self.assertEqual(code, 200, body)
        choices = {row['type'] for row in body['lot_types']}
        self.assertNotIn('vps', choices)
        self.assertNotIn(self.lot.lot_type.type, choices)  # un seul sac de ce type
        self.assertIn(vps.id, {row['id'] for row in body['lots']})
        Lots.objects.create(lot_type=self.lot.lot_type, name='Sac B', name_short='Sac B', created_by='M001')
        code, body = self.as_(self.chief, '/api/checklists/options/')
        self.assertIn(self.lot.lot_type.type, {row['type'] for row in body['lot_types']})

    def test_scan_lot_not_in_list_asks_to_add(self) -> None:
        """Etiquette d'un lot hors liste a la preparation : qui peut modifier la liste se voit proposer de l'ajouter
        (ask_add) ; un lot du meme type pas encore coche est remplace, sinon le lot s'ajoute. Un secouriste qui ne peut
        pas la modifier garde l'erreur."""
        spare = Lots.objects.create(lot_type=self.lot.lot_type, name='Sac B', name_short='Sac B', created_by='M001')
        other_type = LotType.objects.create(type='kitacc', name='Kit accouchement')
        kit = Lots.objects.create(lot_type=other_type, name='Kit 1', name_short='Kit 1', created_by='M001')
        checklist = self.new_list()
        path = f"/api/checklists/{checklist['id']}/"
        code, body = self.as_(self.chief, path + 'scan/', {'lot': spare.id})
        self.assertEqual(code, 200, body)
        self.assertEqual(body['ask_add']['replaces'], {'lot': self.lot.id, 'lot_name': self.lot.name})
        self.assertEqual([line.get('lot') for line in body['lines'] if line['kind'] == 'lot'], [self.lot.id])  # rien change
        self.as_(self.chief, path + 'members/', {'add': [self.other.matricule]})
        code, body = self.as_(self.other, path + 'scan/', {'lot': spare.id})
        self.assertEqual(code, 400)  # le secouriste ne modifie pas la liste du chef
        self.assertIn('pas demandé', body['error'])
        code, body = self.as_(self.chief, path + 'add-lot/', {'lot': spare.id})
        self.assertEqual(code, 200, body)
        self.assertIn('Sac B remplace', body['message'])
        lots = [line for line in body['lines'] if line['kind'] == 'lot']
        self.assertEqual([(line['lot'], line['scanned']) for line in lots], [(spare.id, 1)])
        code, body = self.as_(self.chief, path + 'scan/', {'lot': self.lot.id})
        self.assertEqual(body['ask_add']['replaces'], None)  # Sac B deja coche : rien a remplacer
        code, body = self.as_(self.chief, path + 'scan/', {'lot': kit.id})
        self.assertIsNone(body['ask_add']['replaces'])
        code, body = self.as_(self.chief, path + 'add-lot/', {'lot': kit.id})
        self.assertEqual(code, 200, body)
        self.assertIn('ajouté à la liste', body['message'])
        lots = [line for line in body['lines'] if line['kind'] == 'lot']
        self.assertEqual([(line['lot'], line['scanned']) for line in lots], [(spare.id, 1), (kit.id, 1)])

    def test_departure(self) -> None:
        """« Partir avec » : liste generee depuis un type de lot, ligne « n'importe quel lot de ce type » (jamais un
        lot fixe, ni un lot deja pris par une autre liste), reglage « vérif complète à chaque départ », retour."""
        from ..models import LotTypeSlot, ServerSettings
        bplus_type = LotType.objects.create(type='bplusx', name='B+')
        vps_type = LotType.objects.create(type='vpsxxx', name='VPS')
        LotTypeSlot.objects.create(lot_type=vps_type, name='B+', accepted=['bplusx'])
        c_type = LotType.objects.create(type='lotcxx', name='Lot C')
        LotTypeSlot.objects.create(lot_type=c_type, name='B+', accepted=['bplusx'], mode='assembly')
        LotRequirements.objects.create(lot_type=c_type, item_type=self.compresses, quantity=2)
        b1 = Lots.objects.create(lot_type=bplus_type, name='B+ 1', name_short='B+ 1', created_by='M001')
        b2 = Lots.objects.create(lot_type=bplus_type, name='B+ 2', name_short='B+ 2', created_by='M001')
        vps = Lots.objects.create(lot_type=vps_type, name='VPS', name_short='VPS', created_by='M001')
        b_vps = Lots.objects.create(lot_type=bplus_type, name='B+ VPS', name_short='B+ VPS', created_by='M001',
                                    parent=vps)
        code, body = self.as_(self.other, '/api/checklists/departure/')  # un secouriste peut partir
        self.assertEqual(code, 200, body)
        rows = {row['type']: row for row in body['lot_types']}
        self.assertEqual(rows['lotcxx']['lines'], 2)
        self.assertEqual(rows['bplusx']['available'], 2)  # le B+ du VPS est fixe
        code, depart = self.as_(self.other, '/api/checklists/departure/', {'lot_type': 'lotcxx'})
        self.assertEqual(code, 201, depart)
        self.assertEqual([line['kind'] for line in depart['lines']], ['lot_type', 'type'])
        self.assertEqual({row['id'] for row in depart['lines'][0]['available']}, {b1.id, b2.id})
        path = f"/api/checklists/{depart['id']}/"
        code, body = self.as_(self.other, path + 'scan/', {'lot': b_vps.id})
        self.assertEqual(code, 400)  # on ne part pas avec le B+ du VPS
        self.assertIn('pour de bon', body['error'])
        code, body = self.as_(self.other, path + 'scan/', {'lot': b1.id})
        self.assertEqual(code, 200, body)
        code, body = self.as_(self.other, path + 'scan/', {'lot': b2.id})
        self.assertEqual(code, 400)  # 1/1 deja
        other = self.new_list(lines=[{'lot_types': ['bplusx'], 'quantity': 1}])
        code, body = self.as_(self.chief, f"/api/checklists/{other['id']}/scan/", {'lot': b1.id})
        self.assertEqual(code, 400)  # deja pris pour le depart
        self.assertEqual([row['id'] for row in other['lines'][0]['available']], [b2.id])
        settings_row = ServerSettings.get()
        settings_row.departure_full_verif = True
        settings_row.save()
        code, body = self.as_(self.other, path + 'validate/', {'partial': True})
        self.assertEqual(code, 400)
        self.assertIn('B+ 1', body['error'])
        settings_row.departure_full_verif = False
        settings_row.save()
        code, body = self.as_(self.other, path + 'validate/', {'partial': True})
        self.assertEqual(code, 200, body)
        self.assertFalse(Lots.objects.get(id=b1.id).verif_recommended)  # en poste : pas encore
        self.assertEqual(Lots.objects.get(id=b1.id).outings, 1)  # une sortie (credit de confiance du reassort)
        code, body = self.as_(self.chief, f"/api/checklists/{other['id']}/scan/", {'lot': b1.id})
        self.assertEqual(code, 400)  # en poste avec la liste sortie
        code, body = self.as_(self.other, path + 'return-scan/', {'lot': b1.id})
        self.assertEqual(code, 200, body)
        self.assertTrue(Lots.objects.get(id=b1.id).verif_recommended)  # rentre : vérif recommandee
        code, body = self.as_(self.chief, f"/api/checklists/{other['id']}/scan/", {'lot': b1.id})
        self.assertEqual(code, 200)  # rentre : de nouveau libre

    def test_rights_and_templates(self) -> None:
        mine = self.new_list()
        self.assertTrue(mine['can_edit'])
        code, body = self.as_(self.other, f"/api/checklists/{mine['id']}/update/", {'name': 'Autre'})
        self.assertEqual(code, 403)
        code, body = self.as_(self.other, f"/api/checklists/{mine['id']}/")
        self.assertEqual((code, body['can_edit'], body['can_duplicate']), (200, False, False))  # lien : lisible
        self.assertEqual(body['owner_name'], 'Jeanne')  # front web : prenom seul
        code, body = self.call('POST', f"/api/checklists/{mine['id']}/")
        self.assertEqual(body['owner_name'], 'Jeanne Dupont')  # poste : nom complet
        code, body = self.call('POST', f"/api/checklists/{mine['id']}/update/", {'name': 'Par la gestion'})
        self.assertEqual((code, body['name']), (200, 'Par la gestion'))  # admin du poste
        template = self.new_list(template=True)
        code, _ = self.as_(self.chief, f"/api/checklists/{template['id']}/scan/", {'lot': self.lot.id})
        self.assertEqual(code, 400)  # un modele ne se verifie pas
        # un secouriste ne cree pas de liste, ne duplique pas, ne voit pas les modeles
        code, _ = self.as_(self.other, '/api/checklists/', {'name': 'Interdit'})
        self.assertEqual(code, 403)
        code, _ = self.as_(self.other, f"/api/checklists/{template['id']}/duplicate/")
        self.assertEqual(code, 403)
        code, _ = self.as_(self.other, f"/api/checklists/{template['id']}/")
        self.assertEqual(code, 403)
        code, _ = self.as_(self.other, '/api/checklists/options/')
        self.assertEqual(code, 403)
        code, body = self.as_(self.other, '/api/checklists/list/', {'show': 'templates'})
        self.assertEqual((body['lists'], body['can_create'], body['can_see_all']), ([], False, False))
        code, body = self.as_(self.other, '/api/checklists/list/', {'show': 'all'})
        self.assertEqual(body['lists'], [])  # pas « Toutes » : seulement les siennes
        # un autre chef : toutes les listes, tous les modeles (et il les modifie), pas les listes des autres
        chef = Secouristes(matricule='C002', nom='Leroy', prenom='Chloé', role='chef')
        chef.renew_key()
        chef.save()
        code, body = self.as_(chef, '/api/checklists/list/', {'show': 'all'})
        self.assertEqual(({row['id'] for row in body['lists']}, body['can_create']), ({mine['id']}, True))
        code, body = self.as_(chef, '/api/checklists/list/', {'show': 'templates'})
        self.assertEqual({row['id'] for row in body['lists']}, {template['id']})
        code, body = self.as_(chef, f"/api/checklists/{template['id']}/update/", {'name': 'Modèle partagé'})
        self.assertEqual(code, 200)
        code, body = self.as_(chef, f"/api/checklists/{mine['id']}/update/", {'name': 'Pas à lui'})
        self.assertEqual(code, 403)
        code, copy = self.as_(chef, f"/api/checklists/{template['id']}/duplicate/")
        self.assertEqual((code, copy['owner'], copy['template'], copy['line_count']), (201, 'C002', False, 3))
        code, body = self.as_(chef, '/api/checklists/list/')
        self.assertEqual({row['id'] for row in body['lists']}, {copy['id']})  # pas le modele : onglet Modeles
        code, body = self.call('POST', '/api/checklists/list/', {'show': 'all'})
        self.assertEqual({row['id'] for row in body['lists']}, {mine['id'], copy['id']})

    def test_members(self) -> None:
        mine = self.new_list()
        path = f"/api/checklists/{mine['id']}/"
        code, body = self.as_(self.other, '/api/checklists/list/')
        self.assertEqual(body['lists'], [])  # pas attribuee : invisible
        code, body = self.as_(self.chief, path + 'members/', {'add': ['M002']})
        self.assertEqual((code, body['members']), (200, [{'matricule': 'M002', 'name': 'Paul'}]))
        code, body = self.as_(self.other, '/api/checklists/list/')
        self.assertEqual([row['id'] for row in body['lists']], [mine['id']])
        code, _ = self.as_(self.other, path + 'members/', {'remove': ['M002']})
        self.assertEqual(code, 403)  # seul qui modifie la liste choisit ses secouristes
        code, body = self.as_(self.chief, path + 'members/', {'remove': ['M002']})
        self.assertEqual(body['members'], [])
        # participer sans etre attribue : scan possible, la liste ne s'ajoute pas tant qu'il ne l'ouvre pas par le lien
        code, body = self.as_(self.other, path + 'scan/', {'lot': self.lot.id})
        self.assertEqual(code, 200, body)
        code, body = self.as_(self.other, '/api/checklists/list/')
        self.assertEqual(body['lists'], [])
        code, body = self.as_(self.other, path, {'open': True})  # lien ou QR code
        self.assertEqual([row['matricule'] for row in body['members']], ['M002'])
        code, body = self.as_(self.other, '/api/checklists/list/')
        self.assertEqual([row['id'] for row in body['lists']], [mine['id']])
        # pas de secouristes sur un modele, utilisateur inconnu refuse
        template = self.new_list(template=True)
        code, _ = self.as_(self.chief, f"/api/checklists/{template['id']}/members/", {'add': ['M002']})
        self.assertEqual(code, 400)
        code, _ = self.as_(self.chief, path + 'members/', {'add': ['X999']})
        self.assertEqual(code, 400)

    def test_departure_checked_lots(self) -> None:
        """Page « Départ » : liste des lots coches (sous-lot coche avec son lot ignore), lots pris ou fixes refuses et
        signales comme non cochables."""
        from ..models import LotTypeSlot
        bplus_type = LotType.objects.create(type='bplusx', name='B+')
        sac_type = LotType.objects.create(type='sacsoi', name='Sac de soin')
        vps_type = LotType.objects.create(type='vpsxxx', name='VPS')
        LotTypeSlot.objects.create(lot_type=vps_type, name='Sac', accepted=['sacsoi'])
        b1 = Lots.objects.create(lot_type=bplus_type, name='B+ 1', name_short='B+ 1', created_by='M001')
        sac = Lots.objects.create(lot_type=sac_type, name='Sac B+ 1', name_short='S1', created_by='M001', parent=b1)
        vps = Lots.objects.create(lot_type=vps_type, name='VPS', name_short='VPS', created_by='M001')
        sac_vps = Lots.objects.create(lot_type=sac_type, name='Sac VPS', name_short='SV', created_by='M001', parent=vps)
        code, body = self.as_(self.other, '/api/checklists/departure/', {'blocked': True})
        self.assertEqual((code, body['blocked']), (200, {sac_vps.id: 'Part avec VPS'}))
        fiche = lambda lot: self.client.get(f'/api/lots/{lot.id}/').json()['departure']  # bouton de la fiche
        self.assertEqual([fiche(lot)['blocked'] for lot in (sac_vps, b1, sac)], ['Part avec VPS', '', ''])
        code, body = self.as_(self.other, '/api/checklists/departure/', {'lots': [sac_vps.id]})
        self.assertEqual(code, 400)
        self.assertIn('pour de bon', body['error'])
        code, depart = self.as_(self.other, '/api/checklists/departure/', {'lots': [b1.id, sac.id, self.lot.id]})
        self.assertEqual(code, 201, depart)
        self.assertEqual([line['lot'] for line in depart['lines']], [b1.id, self.lot.id])  # le sac part avec B+ 1
        self.assertEqual([row['matricule'] for row in depart['members']], ['M002'])
        self.assertTrue(depart['name'].startswith('Départ du '))
        code, body = self.as_(self.other, f"/api/checklists/{depart['id']}/scan/", {'lot': b1.id})
        self.assertEqual(code, 200, body)
        code, body = self.as_(self.other, '/api/checklists/departure/', {'blocked': True})
        self.assertEqual(body['blocked'][b1.id], f"Pris pour « {depart['name']} »")
        self.assertEqual(body['blocked'][sac.id], f"Pris pour « {depart['name']} »")  # avec son lot
        taken = {'blocked': f"Pris pour « {depart['name']} »", 'list': {'id': depart['id'], 'name': depart['name'], 'out': False}}
        self.assertEqual((fiche(b1), fiche(sac)), (taken, taken))  # bouton « Continuer le départ »
        code, body = self.as_(self.chief, '/api/checklists/departure/', {'lots': [sac.id]})
        self.assertEqual(code, 400)
        self.assertIn('déjà pris', body['error'])
        code, body = self.as_(self.other, '/api/checklists/departure/', {'lots': []})
        self.assertEqual(code, 400)
        code, body = self.as_(self.other, f"/api/checklists/{depart['id']}/validate/", {'partial': True})
        self.assertEqual(code, 200, body)
        gone = fiche(sac)['list']  # en poste : bouton « Retour », aussi sur le sac parti avec B+ 1
        self.assertEqual((gone['out'], gone['gone']['id']), (True, b1.id))
        self.assertFalse(Lots.objects.get(id=b1.id).verif_recommended)  # pas pendant le poste
        code, rows = self.call('POST', '/api/lots/summary/', {}, local=False)  # État des lots : en poste
        out = {row['id']: row['out_with'] for row in rows}
        self.assertEqual((out[b1.id], out[sac.id], out[self.lot.id]), (depart['name'], depart['name'], ''))
        code, body = self.as_(self.other, f'/api/lots/{sac.id}/return/')
        # tout ce qui est sorti est rentre : le retour de la liste est clos d'office
        self.assertEqual((code, body['message']), (200, 'B+ 1 : rentré, vérif recommandée. Tout est rentré : liste close'))
        self.assertTrue(Lots.objects.get(id=b1.id).verif_recommended)
        code, body = self.as_(self.other, f"/api/checklists/{depart['id']}/")
        self.assertEqual(body['status'], 'returned')
        self.assertEqual((fiche(b1), fiche(sac)), ({'blocked': '', 'list': None},) * 2)  # de nouveau « Partir »
        code, body = self.as_(self.other, f'/api/lots/{sac.id}/return/')
        self.assertEqual(code, 400)

    def reference_bplus(self, name: str) -> tuple[Lots, Lots, Lots]:
        """Lot de referentiel sans etiquette (B+ = sac de soin + sac O2), ses deux sacs avec leur etiquette."""
        bplus_type = LotType.objects.get_or_create(type='bplusx', defaults={'name': 'B+', 'referentiel': True})[0]
        soin_type = LotType.objects.get_or_create(type='sacsoi', defaults={'name': 'Sac de soin', 'part_of': bplus_type})[0]
        o2_type = LotType.objects.get_or_create(type='saco2x', defaults={'name': 'Sac O2', 'part_of': bplus_type})[0]
        bplus = Lots.objects.create(lot_type=bplus_type, name=name, name_short=name, created_by='M001')
        soin = Lots.objects.create(lot_type=soin_type, name=f'Sac de soin {name}', name_short='S', created_by='M001',
                                   parent=bplus)
        o2 = Lots.objects.create(lot_type=o2_type, name=f'Sac O2 {name}', name_short='O', created_by='M001',
                                 parent=bplus)
        self.assertTrue(services.virtual_lot(bplus))
        return bplus, soin, o2

    def test_reference_lot_bag_by_bag(self) -> None:
        """Lot B+ 1 (referentiel, sans etiquette) choisi d'un bloc : dans la liste, une ligne par sac, chacun coche par
        son etiquette ; pris, en poste et rentre par ses sacs, aussi depuis la fiche du B+ (Hippolyte, 2026-11-10)."""
        b1, soin, o2 = self.reference_bplus('Lot B+ 1')
        code, depart = self.as_(self.other, '/api/checklists/departure/', {'lots': [b1.id]})
        self.assertEqual(code, 201, depart)
        self.assertEqual(list(Checklist.objects.get(id=depart['id']).lines.values_list('lot_id', flat=True)), [b1.id])
        self.assertEqual([(line['lot'], line['part_of']) for line in depart['lines']], [(o2.id, b1.id), (soin.id, b1.id)])  # par nom
        self.assertEqual((depart['line_count'], depart['done_count']), (2, 0))
        path = f"/api/checklists/{depart['id']}/"
        code, body = self.as_(self.other, path + 'scan/', {'lot': o2.id})
        self.assertEqual((code, body['message']), (200, 'Sac O2 Lot B+ 1 : coché (Lot B+ 1 : reste Sac de soin Lot B+ 1) (⚠ Jamais vérifié)'))
        self.assertEqual((body['done_count'], body['complete']), (1, False))
        fiche = lambda lot: self.client.get(f'/api/lots/{lot.id}/').json()['departure']
        self.assertEqual(fiche(b1)['list']['id'], depart['id'])  # le B+ est pris des qu'un sac l'est
        code, body = self.as_(self.chief, '/api/checklists/departure/', {'blocked': True})
        self.assertEqual({lot_id for lot_id in body['blocked']} >= {b1.id, soin.id, o2.id}, True)
        code, body = self.as_(self.other, path + 'unscan/', {'lot': o2.id})
        self.assertEqual(code, 200, body)
        for lot in (soin, o2):
            code, body = self.as_(self.other, path + 'scan/', {'lot': lot.id})
            self.assertEqual(code, 200, body)
        self.assertTrue(body['complete'])
        self.assertIn('(Lot B+ 1 : complet)', body['message'])
        code, body = self.as_(self.other, path + 'validate/')
        self.assertEqual((code, body['status']), (200, 'validated'))
        code, rows = self.call('POST', '/api/lots/summary/', {}, local=False)  # État des lots : en poste
        out = {row['id']: row['out_with'] for row in rows}
        self.assertEqual((out[b1.id], out[soin.id], out[o2.id]), (depart['name'],) * 3)
        code, body = self.as_(self.other, f'/api/lots/{b1.id}/return/')  # « Retour de ce lot » sur la fiche du B+
        self.assertEqual((code, body['message']),
                         (200, 'Lot B+ 1 : rentré, vérif recommandée. Tout est rentré : liste close'))
        self.assertTrue(Lots.objects.get(id=soin.id).verif_recommended)

    def test_reference_lot_picked_by_a_bag_and_split(self) -> None:
        """Un sac du B+ choisi (Départ, editeur) : le B+ entier ; la croix d'un sac dans la liste separe le B+, ce sac
        sort de la liste, l'autre reste seul sur sa ligne (Hippolyte, 2026-11-10)."""
        b1, soin, o2 = self.reference_bplus('Lot B+ 1')
        code, options = self.as_(self.chief, '/api/checklists/options/')
        rows = {row['id']: row for row in options['lots']}
        self.assertEqual(rows[o2.id]['unit']['id'], b1.id)
        self.assertNotIn('unit', rows[b1.id])
        code, rows = self.call('POST', '/api/lots/summary/', {}, local=False)
        self.assertTrue({row['id']: row for row in rows}[b1.id]['virtual'])
        code, depart = self.as_(self.other, '/api/checklists/departure/', {'lots': [o2.id]})
        self.assertEqual(code, 201, depart)
        self.assertIn('Lot B+ 1', depart['name'])
        self.assertEqual([line['part_of'] for line in depart['lines']], [b1.id, b1.id])
        path = f"/api/checklists/{depart['id']}/"
        code, body = self.as_(self.other, path + 'scan/', {'lot': soin.id})
        code, body = self.as_(self.chief, path + 'remove-part/', {'lot': soin.id})
        self.assertEqual(code, 403)  # pas l'auteur de la liste
        code, body = self.as_(self.other, path + 'remove-part/', {'lot': soin.id})
        self.assertEqual((code, body['message']), (200, 'Sac de soin Lot B+ 1 retiré de la liste : Sac O2 Lot B+ 1 part seul'))
        self.assertEqual([(line['lot'], line.get('part_of')) for line in body['lines']], [(o2.id, None)])
        self.assertEqual(body['done_count'], 0)  # le scan du sac retire est parti avec lui
        code, body = self.as_(self.other, path + 'scan/', {'lot': o2.id})
        self.assertEqual((code, body['complete']), (200, True))
        # la croix d'un lot simple le retire aussi, meme coche (on decoche en touchant sa case)
        code, body = self.as_(self.other, path + 'remove-part/', {'lot': o2.id})
        self.assertEqual((code, body['message'], body['lines']), (200, 'Sac O2 Lot B+ 1 retiré de la liste', []))
        code, body = self.as_(self.other, path + 'remove-part/', {'lot': o2.id})
        self.assertEqual(code, 400)  # plus dans la liste

    def test_any_reference_lot_bag_by_bag(self) -> None:
        """Ligne « n'importe quel B+ » : l'etiquette d'un sac prend son B+ (sans etiquette), complet avec ses deux
        sacs ; pris sans scanner, tous ses sacs ; ✕ retire le B+ entier."""
        b1, soin1, o21 = self.reference_bplus('Lot B+ 1')
        b2, soin2, o22 = self.reference_bplus('Lot B+ 2')
        code, created = self.as_(self.chief, '/api/checklists/', {
            'name': 'Poste', 'lines': [{'lot_types': ['bplusx'], 'quantity': 1}]})
        self.assertEqual(code, 201, created)
        self.assertEqual({row['id'] for row in created['lines'][0]['available']}, {b1.id, b2.id})
        path = f"/api/checklists/{created['id']}/"
        code, body = self.as_(self.chief, path + 'scan/', {'lot': soin1.id})
        self.assertEqual((code, body['message']),
                         (200, 'Sac de soin Lot B+ 1 : pris pour Lot B+ 1, reste à scanner : Sac O2 Lot B+ 1'))
        line = body['lines'][0]
        self.assertEqual((line['scanned'], line['scans'][0]['lot'], line['scans'][0]['missing']),
                         (0, b1.id, ['Sac O2 Lot B+ 1']))
        code, body = self.as_(self.chief, path + 'scan/', {'lot': soin2.id})
        self.assertEqual(code, 400)  # 1/1 : B+ 1 commence tient la place
        code, body = self.as_(self.chief, path + 'scan/', {'lot': o21.id})
        self.assertEqual((code, body['message'], body['complete']), (200, 'Lot B+ 1 : 1/1 (⚠ Jamais vérifié)', True))
        code, body = self.as_(self.chief, path + 'unscan/', {'lot': b1.id})
        self.assertEqual((code, body['lines'][0]['scans']), (200, []))
        code, body = self.as_(self.chief, path + 'scan/', {'lot': b2.id})  # pris sans scanner
        self.assertEqual((code, body['message'], body['complete']), (200, 'Lot B+ 2 : 1/1 (⚠ Jamais vérifié)', True))
        self.assertEqual(set(models.ChecklistScan.objects.filter(checklist_id=created['id']).values_list('lot_id', flat=True)),
                         {soin2.id, o22.id})

    def test_verif_recommended_lots(self) -> None:
        """Lots en vérif recommandée dans une liste à préparer (lot fixe pris ou non, lot au choix pris) : signalés pour
        le bouton « Lancer la vérif », jamais un lot scellé."""
        sac_type = LotType.objects.create(type='sacsoi', name='Sac de soin')
        sac = Lots.objects.create(lot_type=sac_type, name='Sac 2', name_short='S2', created_by='M001',
                                  verif_recommended=True)
        Lots.objects.filter(id=self.lot.id).update(verif_recommended=True)
        code, created = self.as_(self.chief, '/api/checklists/', {
            'name': 'Poste', 'lines': [{'lot': self.lot.id}, {'lot_types': ['sacsoi'], 'quantity': 1}]})
        self.assertEqual(code, 201, created)
        lines = {line['kind']: line for line in created['lines']}
        self.assertTrue(lines['lot']['verif_recommended'])  # pas encore pris : bouton quand meme
        code, body = self.as_(self.chief, f"/api/checklists/{created['id']}/scan/", {'lot': sac.id})
        self.assertEqual(code, 200, body)
        Lots.objects.filter(id=self.lot.id).update(is_sealed=True)
        code, body = self.as_(self.chief, f"/api/checklists/{created['id']}/")
        lines = {line['kind']: line for line in body['lines']}
        self.assertFalse(lines['lot']['verif_recommended'])  # scelle : le scelle intact suffit
        self.assertEqual([(row['lot'], row['verif_recommended']) for row in lines['lot_type']['scans']], [(sac.id, True)])
        Lots.objects.filter(id=sac.id).update(verif_recommended=False)
        code, body = self.as_(self.chief, f"/api/checklists/{created['id']}/")
        self.assertFalse({line['kind']: line for line in body['lines']}['lot_type']['scans'][0]['verif_recommended'])

    def test_sub_lot_verifs_on_top(self) -> None:
        """Départ avec un VPS dont le sac de soin et le sac O2 sont marqués utilisés : une case « Vérif » par sac en
        haut (avant de prendre le VPS), cochée quand le sac redevient vert ; VPS lui-même à vérifier : une seule case
        « Vérif VPS » (Hippolyte, 2026-11-16)."""
        now = timezone.now()
        vps_type = LotType.objects.create(type='vpsxxx', name='VPS')
        soin_type = LotType.objects.create(type='sacsoi', name='Sac de soin')
        o2_type = LotType.objects.create(type='saco2x', name='Sac O2')
        made = dict(created_by='M001', last_verif=now, last_verif_by='M001')
        vps = Lots.objects.create(lot_type=vps_type, name='VPS', name_short='VPS', **made)
        soin = Lots.objects.create(lot_type=soin_type, name='Sac de soin VPS', name_short='SS', parent=vps, **made)
        o2 = Lots.objects.create(lot_type=o2_type, name='Sac O2 VPS', name_short='O2', parent=vps, **made)
        Lots.objects.filter(id__in=[soin.id, o2.id]).update(verif_recommended=True, marked_used=now)
        code, created = self.as_(self.other, '/api/checklists/departure/', {'lots': [vps.id]})
        self.assertEqual(code, 201, created)
        rows = lambda body: [(row['lot'], row['done']) for row in body['verifs']]
        self.assertEqual(rows(created), [(soin.id, False), (o2.id, False)])
        self.assertEqual({(row['root'], row['taken']) for row in created['verifs']}, {(vps.id, False)})
        # vérif du sac de soin faite : sa case se coche, et reste là
        Lots.objects.filter(id=soin.id).update(verif_recommended=False, marked_used=None)
        code, body = self.as_(self.other, f"/api/checklists/{created['id']}/")
        self.assertEqual(rows(body), [(soin.id, True), (o2.id, False)])
        # le VPS entier marqué utilisé : une seule vérif complète, celle du VPS
        Lots.objects.filter(id__in=[vps.id, soin.id, o2.id]).update(verif_recommended=True, marked_used=now)
        code, body = self.as_(self.other, f"/api/checklists/{created['id']}/")
        self.assertEqual(rows(body), [(vps.id, False)])
        Lots.objects.filter(id__in=[vps.id, soin.id, o2.id]).update(verif_recommended=False, marked_used=None)
        code, body = self.as_(self.other, f"/api/checklists/{created['id']}/scan/", {'lot': vps.id})
        self.assertEqual(code, 200, body)
        self.assertEqual([(row['lot'], row['done'], row['taken']) for row in body['verifs']],
                         [(vps.id, True, True)])  # la vérif du VPS couvre ses sacs
        # un sac de nouveau à vérifier sous le VPS vérifié : sa case revient
        Lots.objects.filter(id=soin.id).update(verif_recommended=True, marked_used=now)
        code, body = self.as_(self.other, f"/api/checklists/{created['id']}/")
        self.assertEqual(rows(body), [(vps.id, True), (soin.id, False)])
        Lots.objects.filter(id=soin.id).update(verif_recommended=False, marked_used=None)
        # sortie : plus de cases de vérif
        code, body = self.as_(self.other, f"/api/checklists/{created['id']}/validate/", {'partial': False})
        self.assertEqual((code, body['verifs']), (200, []), body)

    def test_list_date_and_order(self) -> None:
        """Date d'une liste (aujourd'hui par défaut, modifiable) ; les listes rangées la date la plus proche en haut."""
        today = timezone.localdate()
        first = self.new_list()
        self.assertEqual(first['date'], today.isoformat())
        far = self.new_list(name='Loin', date=(today + timedelta(days=10)).isoformat())
        near = self.new_list(name='Demain', date=(today + timedelta(days=1)).isoformat())
        past = self.new_list(name='Avant-hier', date=(today - timedelta(days=2)).isoformat())
        code, body = self.as_(self.chief, f"/api/checklists/{first['id']}/update/",
                              {'date': (today - timedelta(days=1)).isoformat()})
        self.assertEqual((code, body['date']), (200, (today - timedelta(days=1)).isoformat()), body)
        code, body = self.as_(self.chief, '/api/checklists/list/', {'show': 'mine'})
        self.assertEqual(code, 200, body)
        self.assertEqual([row['id'] for row in body['lists']], [near['id'], first['id'], past['id'], far['id']])
        code, body = self.as_(self.chief, '/api/checklists/', {'name': 'X', 'date': 'demain'})
        self.assertEqual(code, 400, body)

    def test_assigned_for_departure(self) -> None:
        """Page « Départ » : listes attribuees (members) et pas encore rentrees, pas celles de l'auteur seul."""
        mine = self.new_list()
        other = self.new_list()
        code, body = self.as_(self.chief, '/api/checklists/list/', {'show': 'assigned'})
        self.assertEqual((code, body['lists']), (200, []))  # auteur, pas attribuee
        for checklist in (mine, other):
            self.as_(self.chief, f"/api/checklists/{checklist['id']}/members/", {'add': ['M002']})
        code, body = self.as_(self.other, '/api/checklists/list/', {'show': 'assigned'})
        self.assertEqual({row['id'] for row in body['lists']}, {mine['id'], other['id']})
        Checklist.objects.filter(id=other['id']).update(status=ChecklistStatus.RETURNED)
        code, body = self.as_(self.other, '/api/checklists/list/', {'show': 'assigned'})
        self.assertEqual([row['id'] for row in body['lists']], [mine['id']])

    def test_user_search(self) -> None:
        for matricule, prenom, nom in (('M003', 'Paul', 'Muller'), ('M004', 'Émilie', 'Roux'),
                                       ('M005', 'Jean-Paul', 'Petit')):
            Secouristes.objects.create(matricule=matricule, nom=nom, prenom=prenom)
        Secouristes.objects.create(matricule='M006', nom='Paulin', prenom='Marc')  # nom, pas prenom
        Secouristes.objects.create(matricule='M007', nom='Vieux', prenom='Pauline', active=False)
        code, body = self.as_(self.chief, '/api/checklists/users/', {'q': 'pau'})
        self.assertEqual(code, 200, body)
        # prenoms seulement, noms courts sur le web (jamais le nom entier)
        self.assertEqual([row['name'] for row in body['users']], ['Jean-Paul', 'Paul Ma.', 'Paul Mu.'])
        code, body = self.as_(self.chief, '/api/checklists/users/', {'q': 'emi'})
        self.assertEqual([row['matricule'] for row in body['users']], ['M004'])
        code, body = self.call('POST', '/api/checklists/users/', {'q': 'emi'})
        self.assertEqual([row['name'] for row in body['users']], ['Émilie Roux'])  # poste : nom complet
        code, _ = self.as_(self.other, '/api/checklists/users/', {'q': 'pau'})
        self.assertEqual(code, 403)  # secouriste : pas de recherche

    def test_free_tasks(self) -> None:
        code, checklist = self.as_(self.chief, '/api/checklists/', {
            'name': 'Local', 'lines': [{'task': "Prendre des bonbons"}, {'type': 'compre', 'quantity': 1}]})
        self.assertEqual(code, 201, checklist)
        task = checklist['lines'][0]
        self.assertEqual((task['kind'], task['scanned']), ('task', 0))
        path = f"/api/checklists/{checklist['id']}/"
        code, body = self.as_(self.other, path + 'check/', {'line': task['id']})
        self.assertEqual((code, body['lines'][0]['scanned'], body['lines'][0]['done_by']), (200, 1, 'Paul'))
        # modifier la liste garde la tache cochee
        code, body = self.as_(self.chief, path + 'update/', {'lines': [
            {'task': "Prendre des bonbons"}, {'type': 'compre', 'quantity': 1}, {'task': 'Fermer'}]})
        self.assertEqual([line['scanned'] for line in body['lines']], [1, 0, 0])
        code, body = self.as_(self.chief, path + 'validate/', {'partial': True})
        # une tache cochee suffit a valider ; rien n'est sorti du stock : pas de retour a faire
        self.assertEqual((body['status'], body['status_label']), ('returned', 'Terminée'))
        code, copy = self.as_(self.chief, path + 'duplicate/')
        self.assertEqual([line['scanned'] for line in copy['lines']], [0, 0, 0])

    def test_check_validate_and_return(self) -> None:
        checklist = self.new_list()
        path = f"/api/checklists/{checklist['id']}/"
        code, body = self.as_(self.other, path + 'scan/', {'item': self.fresh[0].iid})
        self.assertEqual((code, body['message']), (200, 'Compresses : 1/2'))
        code, body = self.as_(self.chief, path + 'scan/', {'item': self.fresh[1].iid})
        code, body = self.as_(self.chief, path + 'scan/', {'item': self.fresh[2].iid})
        self.assertEqual(code, 400)  # deja 2/2
        code, body = self.as_(self.chief, path + 'scan/', {'lot': self.lot.id})
        self.assertFalse(body['complete'])
        code, body = self.as_(self.chief, path + 'validate/')
        self.assertEqual(code, 400)  # incomplete
        code, body = self.as_(self.chief, path + 'scan/', {'item': self.garrots[0].iid})
        self.assertTrue(body['complete'])
        # pris pour cette liste : refuse dans une autre
        other = self.new_list()  # une autre liste, verifiee par un secouriste
        code, body = self.as_(self.other, f"/api/checklists/{other['id']}/scan/", {'item': self.fresh[0].iid})
        self.assertEqual(code, 400)
        code, body = self.as_(self.chief, path + 'validate/')
        # sortie : retour a scanner, items sortis, lots PAS en vérif recommandee pendant le poste (seulement au retour)
        self.assertEqual((body['status'], body['status_label']), ('validated', 'Sortie'))
        self.assertEqual(Items.objects.get(iid=self.fresh[0].iid).status, ItemStatus.OUT)
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertFalse(body['verif_recommended'])
        code, _ = self.as_(self.chief, path + 'scan/', {'item': self.fresh[2].iid})
        self.assertEqual(code, 400)  # validee : plus de verif
        code, body = self.as_(self.chief, path + 'return-scan/', {'item': self.fresh[0].iid})
        self.assertEqual(code, 200, body)  # retour par la liste
        self.assertEqual(Items.objects.get(iid=self.fresh[0].iid).status, ItemStatus.ACTIVE)
        code, body = self.as_(self.chief, path + 'return-scan/', {'lot': self.lot.id})
        self.assertEqual(code, 200, body)
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertEqual((body['verif_recommended'], body['back_from_list']), (True, 'Poste de secours'))
        # le lot rentre est libre pour une autre liste
        other_list = self.new_list()
        code, body = self.as_(self.chief, f"/api/checklists/{other_list['id']}/scan/", {'lot': self.lot.id})
        self.assertEqual(code, 200, body)


    def test_return_to_original_storage(self) -> None:
        """Retour d'une liste : chaque item revient dans le rangement d'ou il est sorti (ou le stock sans rangement),
        sans vérif recommandee du rangement avant storage_verif_outings sorties."""
        storage_type = LotType.objects.create(type='armoir', name='Armoire', storage=True)
        armoire = Lots(lot_type=storage_type, name='Armoire 1', name_short='A1', created_by='test', parent=Lots.stock())
        armoire.save()
        tiroir = Lots(lot_type=storage_type, name='Tiroir', name_short='T', created_by='test', parent=Lots.stock())
        tiroir.save()
        Items.objects.filter(iid=self.fresh[0].iid).update(location=armoire)
        Items.objects.filter(iid=self.fresh[1].iid).update(location=tiroir)
        checklist = self.new_list()
        path = f"/api/checklists/{checklist['id']}/"
        for data in ({'item': self.fresh[0].iid}, {'item': self.fresh[1].iid}, {'item': self.garrots[0].iid},
                     {'lot': self.lot.id}):
            code, body = self.as_(self.chief, path + 'scan/', data)
            self.assertEqual(code, 200, body)
        code, body = self.as_(self.chief, path + 'validate/')
        self.assertEqual(body['status'], 'validated')
        self.assertIsNone(Items.objects.get(iid=self.fresh[0].iid).location_id)
        code, body = self.as_(self.chief, path + 'return-scan/', {'item': self.fresh[0].iid})
        self.assertEqual((code, body['message']), (200, 'Compresses : à ranger dans Armoire 1'))
        code, body = self.as_(self.chief, path + 'return-scan/', {'item': self.fresh[1].iid})
        self.assertEqual(body['message'], 'Compresses : à ranger dans Tiroir')
        code, body = self.as_(self.chief, path + 'return-scan/', {'item': self.garrots[0].iid})
        self.assertEqual(body['message'], 'Garrot : remis en stock')  # sorti du stock sans rangement
        items = {item.iid: item for item in Items.objects.filter(iid__in=[self.fresh[0].iid, self.fresh[1].iid,
                                                                           self.garrots[0].iid])}
        self.assertEqual([(items[item.iid].location_id, items[item.iid].status)
                          for item in (self.fresh[0], self.fresh[1], self.garrots[0])],
                         [(armoire.id, ItemStatus.ACTIVE), (tiroir.id, ItemStatus.ACTIVE), (None, ItemStatus.ACTIVE)])
        # rangements ni orange ni en vérif dans les taches : une seule sortie
        self.assertFalse(Lots.objects.get(id=armoire.id).verif_recommended)
        code, body = self.call('GET', '/api/tasks/')
        self.assertNotIn('stock_verif', [row['category'] for row in body['tasks']])

    def test_reopen_back_to_storage(self) -> None:
        storage_type = LotType.objects.create(type='armoir', name='Armoire', storage=True)
        armoire = Lots(lot_type=storage_type, name='Armoire 1', name_short='A1', created_by='test', parent=Lots.stock())
        armoire.save()
        Items.objects.filter(iid=self.fresh[0].iid).update(location=armoire)
        checklist = self.new_list()
        path = f"/api/checklists/{checklist['id']}/"
        for data in ({'item': self.fresh[0].iid}, {'item': self.fresh[1].iid}, {'item': self.garrots[0].iid},
                     {'lot': self.lot.id}):
            self.as_(self.chief, path + 'scan/', data)
        self.as_(self.chief, path + 'validate/')
        code, body = self.as_(self.chief, path + 'reopen/')
        self.assertEqual(code, 200, body)
        self.assertEqual(Items.objects.get(iid=self.fresh[0].iid).location_id, armoire.id)
        self.assertEqual((Items.objects.get(iid=self.fresh[1].iid).location_id,
                          Items.objects.get(iid=self.fresh[1].iid).status), (None, ItemStatus.ACTIVE))

    def test_legacy_close_return(self) -> None:
        """Ancienne liste sortie (retour a scanner) : la fenetre « Clôturer le retour ? » montre ce qui n'est pas
        rentre ; clore met le lot pas rescanne en vérif recommandee et compte le consommable pas rentre utilise."""
        checklist = self.new_list()
        path = f"/api/checklists/{checklist['id']}/"
        for data in ({'item': self.fresh[0].iid}, {'item': self.fresh[1].iid}, {'lot': self.lot.id},
                     {'item': self.garrots[0].iid}):
            code, body = self.as_(self.chief, path + 'scan/', data)
            self.assertEqual(code, 200, body)
        self.as_(self.chief, path + 'validate/')
        # comme une liste sortie avant le depart d'aujourd'hui : retour a scanner, lot pas encore recommande
        Checklist.objects.filter(id=checklist['id']).update(status=ChecklistStatus.VALIDATED, returned=None)
        Lots.objects.filter(id=self.lot.id).update(verif_recommended=False, back_from_list='')
        code, body = self.as_(self.chief, path + 'return-scan/', {'item': self.fresh[0].iid})
        self.assertEqual(code, 200, body)
        code, body = self.as_(self.chief, path)
        preview = body['return_preview']
        self.assertEqual((preview['back'], preview['lots']), (1, [self.lot.name]))
        self.assertEqual((preview['used'], len(preview['still_out'])), (['Compresses'], 1))
        code, body = self.as_(self.chief, path + 'close-return/')
        self.assertEqual(code, 200, body)
        self.assertEqual(body['status'], 'returned')
        self.assertNotIn('return_preview', body)
        self.assertEqual(Items.objects.get(iid=self.fresh[1].iid).status, ItemStatus.MISSING)
        self.assertEqual(Items.objects.get(iid=self.fresh[0].iid).status, ItemStatus.ACTIVE)
        lot = Lots.objects.get(id=self.lot.id)
        self.assertEqual((lot.verif_recommended, lot.back_from_list), (True, 'Poste de secours'))

    def test_reopen_departure(self) -> None:
        """Depart valide par erreur : rouvrable 30 minutes, le materiel revient ou il etait, le lot n'est plus en
        verif recommandee a cause de la liste, les scans restent."""
        place = {item.iid: Items.objects.get(iid=item.iid).location_id for item in self.fresh[:2]}
        checklist = self.new_list()
        path = f"/api/checklists/{checklist['id']}/"
        for data in ({'item': self.fresh[0].iid}, {'item': self.fresh[1].iid}, {'lot': self.lot.id},
                     {'item': self.garrots[0].iid}):
            code, body = self.as_(self.chief, path + 'scan/', data)
            self.assertEqual(code, 200, body)
        code, body = self.as_(self.chief, path + 'validate/')
        self.assertEqual((body['status'], body['can_reopen']), ('validated', True))
        code, body = self.as_(self.other, path)
        self.assertFalse(body['can_reopen'])  # ni auteur, ni dans la liste
        code, body = self.as_(self.other, path + 'reopen/')
        self.assertEqual(code, 400)
        code, body = self.as_(self.chief, path + 'reopen/')
        self.assertEqual(code, 200, body)
        self.assertEqual((body['status'], body['complete'], body['can_reopen']), ('draft', True, False))
        for iid, location in place.items():
            item = Items.objects.get(iid=iid)
            self.assertEqual((item.status, item.location_id), (ItemStatus.ACTIVE, location))
        lot = Lots.objects.get(id=self.lot.id)
        self.assertEqual((lot.verif_recommended, lot.back_from_list), (False, ''))
        # revalidee, puis trop tard pour rouvrir
        code, body = self.as_(self.chief, path + 'validate/')
        self.assertEqual(code, 200, body)
        Checklist.objects.filter(id=checklist['id']).update(validated=timezone.now() - timedelta(minutes=31))
        code, body = self.as_(self.chief, path + 'reopen/')
        self.assertEqual(code, 400)
        self.assertIn('30 minutes', body['error'])


class TasksTests(ApiTestCase):
    """Liste de taches de la gestion (Gestion de la TECH) et bouton « Marquer ce lot comme utilisé »."""

    def badge(self, user: Secouristes) -> dict[str, str]:
        return {'matricule': user.matricule, 'key': user.new_key}

    def fill_lot(self) -> list[Items]:
        items = self.create(self.compresses, self.today + timedelta(days=200), 2)
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid for item in items], 'user': 'M001'})
        return items

    def categories(self, body: dict[str, Any]) -> list[str]:
        # les types sous le minimum donnent aussi des commandes : hors de ces tests
        return [row['category'] for row in body['tasks'] if row['category'] != 'orders']

    def test_priorities(self) -> None:
        # lot jamais verifie : invalide ; signalement : en tete
        code, body = self.call('GET', '/api/tasks/')
        self.assertEqual(code, 200)
        self.assertEqual(self.categories(body), ['invalid'])
        self.assertEqual(body['tasks'][0]['lot'], self.lot.id)
        self.assertIn('jamais vérifié', body['tasks'][0]['detail'])
        code, _ = self.call('POST', '/api/reports/', {'lot': self.lot.id, 'comment': 'fermeture cassée', 'usable': True})
        self.assertEqual(code, 201)
        code, body = self.call('GET', '/api/tasks/')
        self.assertEqual(self.categories(body), ['reports', 'invalid'])
        self.assertEqual([row['key'] for row in body['categories']][:4], ['reports', 'seal', 'invalid', 'recommended'])
        self.assertEqual(body['categories'][0]['count'], 1)

    def test_mark_used_and_stock_verif(self) -> None:
        self.fill_lot()
        code, body = self.call('GET', '/api/tasks/')
        self.assertNotIn('recommended', self.categories(body))
        # tout secouriste, depuis le web, sans la cle du lot
        code, lot = self.call('POST', f'/api/lots/{self.lot.id}/used/', {'user': self.badge(self.user)}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(lot['state']['code'], 'used')
        self.assertEqual(lot['state']['kind'], 'warn')
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/used/', {}, local=False)
        self.assertEqual(code, 403)
        code, body = self.call('GET', '/api/tasks/')
        recommended = [row for row in body['tasks'] if row['category'] == 'recommended']
        self.assertEqual(len(recommended), 1)
        self.assertIn('marqué utilisé', recommended[0]['detail'])
        # une verif complete leve la recommandation
        iids = list(Items.objects.filter(location=self.lot).values_list('iid', flat=True))
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': iids, 'user': 'M001'})
        code, lot = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertEqual(lot['state']['code'], 'verified')
        self.assertIsNone(lot['marked_used'])
        # sorties du stock : vérif du stock proposee au bout de storage_verif_outings sorties (10 par defaut), jusqu'a
        # la prochaine verif du stock
        self.assertEqual(models.ServerSettings.get().storage_verif_outings, 10)
        models.ServerSettings.objects.filter(pk=1).update(storage_verif_outings=2)
        out = self.create(self.garrot, None, 4)
        code, _ = self.call('POST', '/api/items/out/', {'items': [out[0].iid], 'user': 'M001'})
        self.assertEqual(code, 200)
        code, body = self.call('GET', '/api/tasks/')
        self.assertNotIn('stock_verif', self.categories(body))  # une seule sortie : pas encore
        code, _ = self.call('POST', '/api/items/out/', {'items': [out[1].iid, out[2].iid], 'user': 'M001'})
        code, body = self.call('GET', '/api/tasks/')
        stock = [row for row in body['tasks'] if row['category'] == 'stock_verif']
        self.assertEqual(len(stock), 1)
        self.assertIn('2 sorties', stock[0]['detail'])
        self.assertIn('3 × Garrot', stock[0]['detail'])
        out = [out[0], out[3]]
        self.call('POST', '/api/stock/verif/', {'items': [item.iid for item in out[1:]], 'user': 'M001'})
        code, body = self.call('GET', '/api/tasks/')
        self.assertNotIn('stock_verif', self.categories(body))

    def test_unsorted_last_priority(self) -> None:
        """Items du stock sans rangement : a ranger, en toute derniere priorite, seulement s'il y a des rangements."""
        self.create(self.compresses, self.today + timedelta(days=200), 2)
        code, body = self.call('GET', '/api/tasks/')
        self.assertNotIn('unsorted', self.categories(body))  # pas de rangement : stock en vrac
        storage_type = LotType.objects.create(type='armoir', name='Armoire', storage=True)
        Lots(lot_type=storage_type, name='Armoire', name_short='A', created_by='test', parent=Lots.stock()).save()
        code, body = self.call('GET', '/api/tasks/')
        self.assertEqual(body['categories'][-1]['key'], 'unsorted')
        self.assertEqual(self.categories(body)[-1], 'unsorted')
        row = body['tasks'][-1]
        self.assertEqual((row['level'], row['link']), ('info', 'inventory'))
        self.assertIn('2 × Compresses', row['detail'])

    def test_storage_verif_outings_setting(self) -> None:
        code, body = self.call('PATCH', '/api/server-settings/', {'storage_verif_outings': 5})
        self.assertEqual((code, body['storage_verif_outings']), (200, 5))
        code, _ = self.call('PATCH', '/api/server-settings/', {'storage_verif_outings': 0})
        self.assertEqual(code, 400)

    def test_mark_used_sealed_refused(self) -> None:
        self.fill_lot()
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001'})
        self.assertEqual(code, 200)
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/used/', {'user': self.badge(self.user)}, local=False)
        self.assertEqual(code, 400)
        self.assertIn("étiquette d'ouverture", body['error'])

    def test_upcoming_expiries_not_tasks(self) -> None:
        # lot valide dont des items periment dans les 30 jours : pas une tache, seulement un evenement a venir
        items = (self.create(self.compresses, self.today + timedelta(days=20), 1)
                 + self.create(self.compresses, self.today + timedelta(days=1), 1))
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid for item in items], 'user': 'M001'})
        self.create(self.compresses, self.today + timedelta(days=5), 2)  # stock
        self.create(self.compresses, self.today + timedelta(days=40), 1)  # au-dela de 30 jours
        code, body = self.call('GET', '/api/tasks/')
        self.assertEqual(code, 200)
        self.assertFalse([row for row in body['tasks'] if 'Péremption' in row['title']])
        self.assertEqual(body['count'], len(body['tasks']))
        self.assertNotIn('upcoming', body)
        code, body = self.call('GET', '/api/events/?days=30')
        self.assertEqual(code, 200)
        # plus une carte par peremption : regroupees par mois a la fin, repliees
        self.assertFalse([row for row in body['events'] if row['kind'] == 'expiry'])
        upcoming = [row for group in body['groups'] if group['kind'] == 'expiry' for row in group['rows']]
        self.assertEqual(sorted((row['days'], row['level'], row['text']) for row in upcoming),
                         [(1, 'warn', '1 × Compresses'), (5, 'soon', '2 × Compresses'), (20, 'info', '1 × Compresses')])
        self.assertEqual(sum(group['count'] for group in body['groups'] if group['kind'] == 'expiry'), 4)
        self.assertEqual({row['key']: row['count'] for row in body['kinds']}['expiry'], 4)
        # par lot, puis type, puis date : le lot avant le stock
        group = body['groups'][0]
        self.assertTrue(group['title'].startswith('Péremptions d'))
        lots = [row['lot'] for row in group['rows']]
        self.assertEqual(lots, sorted(lots, key=lambda lot: lot is None))
        stock = next(row for row in upcoming if row['lot'] is None)
        self.assertEqual((stock['section'], stock['link']), ('Stock', 'stock'))
        # horizon plus long : la peremption a 40 jours apparait
        code, body = self.call('GET', '/api/events/?days=90')
        self.assertIn(40, [row['days'] for group in body['groups'] for row in group['rows']])
        # web : badge gestion/admin, refuse a un secouriste
        code, _ = self.call('POST', '/api/events/summary/', {'user': self.badge(self.user)}, local=False)
        self.assertEqual(code, 403)

    def test_order_placed(self) -> None:
        orders = lambda body: [row for row in body['tasks'] if row['category'] == 'orders']
        code, body = self.call('GET', '/api/tasks/')
        suggestion = next(row for row in orders(body) if row['type'] == 'compre')
        quantity = suggestion['quantity']
        self.assertGreaterEqual(quantity, 10)
        # secouriste : refuse
        code, _ = self.call('POST', '/api/orders/', {'user': self.badge(self.user), 'type': 'compre',
                                                     'quantity': quantity}, local=False)
        self.assertEqual(code, 403)
        code, body = self.call('POST', '/api/orders/', {'type': 'compre', 'quantity': quantity})
        self.assertEqual(code, 201)
        self.assertFalse([row for row in orders(body) if row.get('type') == 'compre'])
        count = body['count']
        self.assertEqual([(row['type'], row['quantity'], row['received']) for row in body['orders_placed']],
                         [('compre', quantity, 0)])
        # livraison partielle puis complete : la commande se clot toute seule
        self.create(self.compresses, self.today + timedelta(days=300), quantity - 1)
        code, body = self.call('GET', '/api/tasks/')
        self.assertEqual(body['orders_placed'][0]['received'], quantity - 1)
        self.assertEqual(body['count'], count)
        self.create(self.compresses, self.today + timedelta(days=300), 1)
        code, body = self.call('GET', '/api/tasks/')
        self.assertEqual(body['orders_placed'], [])
        self.assertIsNotNone(models.StockOrder.objects.get().delivered)

    def test_order_cancel_and_late(self) -> None:
        code, body = self.call('POST', '/api/orders/', {'type': 'compre', 'quantity': 20})
        order = body['orders_placed'][0]['id']
        # pas livree au bout de 30 jours : de nouveau une tache
        models.StockOrder.objects.filter(pk=order).update(placed=timezone.now() - timedelta(days=31))
        code, body = self.call('GET', '/api/tasks/')
        late = [row for row in body['tasks'] if row.get('order') == order]
        self.assertEqual(len(late), 1)
        self.assertIn('pas livrée', late[0]['title'])
        self.assertTrue(body['orders_placed'][0]['late'])
        # decochee : la suggestion revient
        code, body = self.call('POST', f'/api/orders/{order}/cancel/', {})
        self.assertEqual(code, 200)
        self.assertEqual(body['orders_placed'], [])
        self.assertTrue([row for row in body['tasks'] if row.get('type') == 'compre' and 'quantity' in row])

    def test_web_access(self) -> None:
        code, _ = self.call('POST', '/api/tasks/summary/', {'user': self.badge(self.user)}, local=False)
        self.assertEqual(code, 403)
        self.user.role = 'gestion'
        self.user.save()
        code, body = self.call('POST', '/api/tasks/summary/', {'user': self.badge(self.user)}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(body['count'], len(body['tasks']))
