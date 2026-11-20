"""Tests du back : fiches PDF, journal des operations, rapports d'etat."""

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)


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
        from ..qr import qr_matrix
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


class LotStateReportTests(LotSheetTests.__base__):
    """Rapport d'etat d'un lot a une date donnee (PDF, admins, front web)."""

    def setUp(self):
        super().setUp()
        self.admin_badge = {'matricule': 'P001', 'key': self.operator.new_key,
                            'session': views.session_token(self.operator)}
        global_type = LotType.objects.create(type='bplus0', name='B+')
        self.root = Lots(lot_type=global_type, name='B+ 1', name_short='B1', created_by='test')
        self.root.save()
        self.lot.parent = self.root
        self.lot.save()
        o2_type = LotType.objects.create(type='sacoxy', name='Sac O2')
        self.o2 = Lots(lot_type=o2_type, name='Sac O2 1', name_short='O1', created_by='test', parent=self.root)
        self.o2.save()
        now = timezone.now()
        self.ago = lambda days: now - timedelta(days=days)
        Lots.objects.filter(id__in=[self.root.id, self.lot.id, self.o2.id]).update(created=self.ago(30))
        self.items = self.create(self.compresses, self.today + timedelta(days=200), 3)
        i1, i2, i3 = self.items
        Items.objects.filter(iid__in=[i1.iid, i2.iid]).update(added=self.ago(10))
        Items.objects.filter(iid=i3.iid).update(added=self.ago(1), location=self.lot)
        Items.objects.filter(iid=i2.iid).update(location=self.lot)
        move = lambda item, days, src, dst, kind=MovementKind.MOVE, **extra: ItemMovement.objects.create(
            item=item, item_type=self.compresses, kind=kind, at=self.ago(days), by='M:M001', from_lot=src, to_lot=dst,
            **extra)
        move(i1, 8, None, self.lot)
        move(i2, 8, None, self.lot)
        move(i1, 3, self.lot, None)  # i1 ressorti vers le stock il y a 3 jours
        # i2 note absent il y a 4 jours, retrouve il y a 2 jours
        move(i2, 4, self.lot, None, kind=MovementKind.USED, cancelled=self.ago(2))
        Verifs.objects.create(lot=self.lot, datetime=self.ago(7), received=self.ago(7), by='M:M001', complete=True,
                              present_count=2, participants=['M:M001', 'M:P001'])

    def report(self, days=None, **extra):
        at = '' if days is None else timezone.localtime(self.ago(days)).strftime('%Y-%m-%dT%H:%M')
        response = self.client.post(f'/api/lots/{self.root.id}/state-report/',
                                    json.dumps({'user': self.admin_badge, 'at': at, **extra}),
                                    content_type='application/json')
        return response

    def text(self, response):
        self.assertEqual(response.status_code, 200, response.content[:300])
        self.assertEqual(response['Content-Type'], 'application/pdf')
        return '\n'.join(LotSheetTests.page_texts(self, response.content))

    def test_state_at_past_date(self):
        i1, i2, i3 = self.items
        text = self.text(self.report(5, antenna='  Paris   15 '))
        self.assertIn(f'({i1.iid})', text)
        self.assertIn(f'({i2.iid})', text)
        self.assertNotIn(i3.iid, text)  # ajoute apres la date du rapport
        self.assertIn('(Complet \xe0 cette date \\(tous les sous-lots\\))', text)
        self.assertIn('Derni\xe8re v\xe9rif : ', text)
        self.assertIn('par Jeanne Dupont, M001 \\(avec Admin Poste, P001\\)', text)
        self.assertNotIn('hors connexion', text)
        self.assertIn('Sous-lot \xb7 Sac A', text)
        self.assertIn('Sous-lot \xb7 Sac O2 1', text)
        self.assertIn('(Certifi\xe9 par le TECH Paris 15)', text)
        self.assertIn('Signature :', text)
        self.assertIn('par Admin Poste, P001', text)  # qui a edite le rapport : nom complet et matricule
        self.assertEqual(models.ServerSettings.get().antenna_name, 'Paris 15')
        op = Operation.objects.get(kind='lot_report')
        self.assertEqual((op.lot_id, op.by), (self.root.id, 'M:P001'))

    def test_offline_verif_shows_reception_time(self):
        # vérif faite hors connexion il y a 7 jours, reçue par le serveur il y a 6 jours
        Verifs.objects.filter(lot=self.lot).update(received=self.ago(6))
        text = self.text(self.report(5))
        self.assertIn('faite hors connexion, re\xe7ue par le serveur le', ' '.join(text.split()))

    def test_absent_and_moved_items(self):
        i1, i2, i3 = self.items
        text = self.text(self.report(3.5))  # i2 absent (pas encore retrouve), i1 encore dans le sac
        # le rapport prouve ce qui etait dans le lot : i2 n'y figure pas, son type a une ligne « manquant »
        self.assertNotIn(i2.iid, text)
        self.assertIn(i1.iid, text)
        self.assertNotIn('absent', text)
        self.assertIn('(manquant)', text)
        self.assertIn('Incomplet \xe0 cette date : Sac A', text)
        text = self.text(self.report())  # maintenant : i2 retrouve, i3 ajoute, i1 sorti
        self.assertNotIn(i1.iid, text)
        self.assertIn(i2.iid, text)
        self.assertIn(i3.iid, text)
        self.assertNotIn('absent', text)
        self.assertIn('(Certifi\xe9 par le TECH)', text)

    def test_expired_at_report_date(self):
        old = self.create(self.compresses, self.today - timedelta(days=1), 1)[0]
        Items.objects.filter(iid=old.iid).update(location=self.lot, added=self.ago(9))
        text = self.text(self.report(5))
        self.assertIn(old.iid, text)
        self.assertNotIn('(p\xe9rim\xe9)', text)  # encore bon il y a 5 jours
        text = self.text(self.report())
        self.assertIn('(p\xe9rim\xe9)', text)

    def test_admins_only_and_past_dates(self):
        badge = {'matricule': 'M001', 'key': self.user.new_key}
        response = self.client.post(f'/api/lots/{self.root.id}/state-report/', json.dumps({'user': badge}),
                                    content_type='application/json')
        self.assertEqual(response.status_code, 403)
        response = self.client.post(f'/api/lots/{self.root.id}/state-report/', '{}', content_type='application/json')
        self.assertEqual(response.status_code, 403)
        response = self.report(-2)
        self.assertEqual(response.status_code, 400)
        self.assertIn('venir', response.json()['error'])
        response = self.client.post(f'/api/lots/{self.root.id}/state-report/',
                                    json.dumps({'user': self.admin_badge, 'at': 'hier'}),
                                    content_type='application/json')
        self.assertEqual(response.status_code, 400)
        self.assertFalse(Operation.objects.filter(kind='lot_report').exists())
