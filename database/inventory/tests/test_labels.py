"""Tests du back : etiquettes demandees depuis le web, compteur d'impressions, outil de debug des codes."""

from __future__ import annotations

from typing import TYPE_CHECKING

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)

if TYPE_CHECKING:
    from datetime import date, datetime
    from inventory.models import Items, Lots, Verifs


class DebugCodesTests(ApiTestCase):
    """Outil de debug des codes du front web : administrateurs seulement, jamais de badge."""

    def test_admin_only_and_no_badges(self) -> None:
        items = self.create(self.compresses, self.today + timedelta(days=60), 2)
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid for item in items], 'user': 'M001'})
        self.call('POST', f'/api/lots/{self.lot.id}/seal/', {'user': 'M001'})
        self.garrot.label_style = 'barcode'
        self.garrot.save()
        garrot = self.create(self.garrot, None, 1)[0]
        # refuse sans connexion et aux non-admins (badge d'un secouriste), sur l'API publique comme au poste
        code, _ = self.call('POST', '/api/debug/codes/', {}, local=False)
        self.assertEqual(code, 403)
        code, _ = self.call('POST', '/api/debug/codes/', {'user': {'matricule': 'M001', 'key': self.user.new_key}},
                            local=False)
        self.assertEqual(code, 403)
        self.user.role = 'gestion'
        self.user.save()
        code, _ = self.call('POST', '/api/debug/codes/', {}, session=views.session_token(self.user))
        self.assertEqual(code, 403)
        # admin : badge + session sur l'API publique
        admin = {'matricule': 'P001', 'key': self.operator.new_key, 'session': self.operator_session}
        code, body = self.call('POST', '/api/debug/codes/', {'user': admin}, local=False)
        self.assertEqual(code, 200)
        lot = next(row for row in body['lots'] if row['id'] == self.lot.id)
        self.assertEqual([row['kind'] for row in lot['codes']], ['public', 'key', 'seal', 'sealopen'])
        self.lot.refresh_from_db()
        self.assertIn(self.lot.verif_key, lot['codes'][1]['code'])
        by_iid = {row['iid']: row for row in body['items']}
        self.assertEqual(by_iid[items[0].iid]['lot'], self.lot.id)
        self.assertTrue(by_iid[items[0].iid]['code'].endswith('/item?id=' + compact_iid(items[0].iid)))
        self.assertEqual(by_iid[garrot.iid]['code'], compact_iid(garrot.iid))  # code-barres : iid court seul
        # secouristes listes pour la connexion de depannage (jamais les admins), mais aucun badge
        self.assertEqual([row['matricule'] for row in body['users']], ['M001'])
        text = json.dumps({key: value for key, value in body.items() if key != 'users'})
        for user in (self.user, self.operator):
            self.assertNotIn('badge?', text)
            self.assertNotIn(user.matricule, text)
        self.assertNotIn('badge?', json.dumps(body['users']))


class LabelErrorTests(TestCase):
    """Message « Scannez l'étiquette … » : le nom du lot et de ses sous-lots, jamais « lot ou sous-lot »."""

    def test_names(self) -> None:
        self.assertEqual(views.de_name('Armoire 1'), "d'Armoire 1")
        self.assertEqual(views.de_name('Le sac O2'), 'du sac O2')
        self.assertEqual(views.de_names(['Sac de soin', 'Sac O2', 'B']), 'de Sac de soin, de Sac O2 ou de B')
        lot_type = models.LotType.objects.create(type='SACT', name='Sac', created_by='test')
        bplus = Lots.objects.create(lot_type=lot_type, name='B+', created_by='test')
        Lots.objects.create(lot_type=lot_type, name='Sac O2', parent=bplus, created_by='test')
        Lots.objects.create(lot_type=lot_type, name='Sac de soin', parent=bplus, created_by='test')
        self.assertTrue(views.label_error([bplus]).startswith(
            "Scannez l'étiquette de B+ (ou celle de Sac de soin ou de Sac O2) pour valider"))
        # B+ de referentiel (agregat de sacs, sans etiquette) : seulement celles de ses sacs
        lot_type.referentiel = True
        lot_type.save()
        self.assertTrue(views.label_error([bplus]).startswith("Scannez l'étiquette de Sac de soin ou de Sac O2 pour"))


class LabelPrintTests(ApiTestCase):
    """Etiquettes d'items demandees depuis le web par un admin, imprimees au poste meme sans personne de connecte."""

    def admin(self) -> dict[str, str]:
        return {'matricule': 'P001', 'key': self.operator.new_key, 'session': self.operator_session}

    def ask(self, **data: Any) -> tuple[int, Any]:
        return self.call('POST', '/api/label-prints/', {'user': self.admin(), **data}, local=False)

    def test_gestion_and_admins_only_on_the_web(self) -> None:
        code, _ = self.call('POST', '/api/label-prints/', {'type': 'garrot', 'count': 2}, local=False)
        self.assertEqual(code, 403)
        secouriste = {'matricule': 'M001', 'key': self.user.new_key}
        for role in ('normal', 'chef'):
            self.user.role = role
            self.user.save()
            code, _ = self.call('POST', '/api/label-prints/list/', {'user': secouriste}, local=False)
            self.assertEqual(code, 403)
        self.user.role = 'gestion'
        self.user.save()
        gestion = secouriste
        code, body = self.call('POST', '/api/label-prints/list/', {'user': gestion}, local=False)
        self.assertEqual(code, 200)
        code, asked = self.call('POST', '/api/label-prints/', {'user': gestion, 'type': 'garrot', 'count': 2},
                                local=False)
        self.assertEqual(code, 201)
        code, _ = self.call('POST', f"/api/label-prints/{asked['id']}/cancel/", {'user': gestion}, local=False)
        self.assertEqual(code, 200)
        code, body = self.call('POST', '/api/label-prints/list/', {'user': self.admin()}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual([row['type'] for row in body['types']], ['compre', 'garrot'])

    def test_request_then_print_without_anyone_logged_in(self) -> None:
        code, body = self.ask(type='compre', count=3)
        self.assertEqual(code, 400)  # perissable : date obligatoire
        peremption = (self.today + timedelta(days=400)).isoformat()
        code, body = self.ask(type='compre', count=3, peremption=peremption)
        self.assertEqual(code, 201)
        self.assertTrue(body['pending'])
        self.assertEqual(Items.objects.count(), 0)  # rien de cree avant l'impression
        # route du poste : pas sur l'API publique
        self.assertEqual(self.call('GET', '/api/label-prints/pending/', local=False)[0], 404)
        code, listing = self.call('GET', '/api/label-prints/pending/', session=False)
        self.assertEqual(code, 200)
        self.assertEqual([row['id'] for row in listing['pending']], [body['id']])
        # tache de la gestion tant que ce n'est pas imprime
        code, tasks = self.call('GET', '/api/tasks/')
        self.assertTrue(any(row['link'] == 'label_prints' for row in tasks['tasks']))
        code, printed = self.call('POST', f"/api/label-prints/{body['id']}/print/", {}, session=False)
        self.assertEqual(code, 200)
        self.assertEqual(len(printed['items']), 3)
        self.assertEqual(printed['items'][0]['peremption'], peremption)
        self.assertTrue(all(item['in_stock'] for item in printed['items']))
        created = Items.objects.filter(iid__in=[item['iid'] for item in printed['items']])
        self.assertEqual({item.added_by for item in created}, {'M:P001'})
        operation = Operation.objects.get(kind='reception')
        self.assertEqual(operation.by, 'M:P001')
        self.assertEqual(operation.details['label_print'], body['id'])
        # une seule fois
        self.assertEqual(self.call('POST', f"/api/label-prints/{body['id']}/print/", {}, session=False)[0], 409)
        code, listing = self.call('GET', '/api/label-prints/pending/', session=False)
        self.assertEqual(listing['pending'], [])
        self.assertNotIn('items', listing['recent'][0])  # interrogee toutes les 20 s : sans les items
        code, full = self.call('GET', f"/api/label-prints/{body['id']}/detail/", session=False)
        self.assertEqual(len(full['items']), 3)  # Réimprimer
        code, tasks = self.call('GET', '/api/tasks/')
        self.assertFalse(any(row['link'] == 'label_prints' for row in tasks['tasks']))
        # imprimee : plus annulable
        code, _ = self.call('POST', f"/api/label-prints/{body['id']}/cancel/", {'user': self.admin()}, local=False)
        self.assertEqual(code, 409)

    def test_cancel_and_limits(self) -> None:
        self.assertEqual(self.ask(type='garrot', count=0)[0], 400)
        self.assertEqual(self.ask(type='garrot', count=1000)[0], 400)
        self.assertEqual(self.ask(type='nope00', count=1)[0], 404)
        self.garrot.archived = True
        self.garrot.save()
        self.assertEqual(self.ask(type='garrot', count=1)[0], 400)
        self.garrot.archived = False
        self.garrot.save()
        code, body = self.ask(type='garrot', count=2, peremption='2030-01-01')
        self.assertEqual(code, 201)
        self.assertIsNone(body['peremption'])  # pas perissable : date ignoree
        code, cancelled = self.call('POST', f"/api/label-prints/{body['id']}/cancel/", {'user': self.admin()},
                                    local=False)
        self.assertEqual(code, 200)
        self.assertFalse(cancelled['pending'])
        self.assertEqual(self.call('POST', f"/api/label-prints/{body['id']}/print/", {}, session=False)[0], 409)
        self.assertEqual(Items.objects.count(), 0)

    def test_expires_after_24_hours(self) -> None:
        code, body = self.ask(type='garrot', count=2)
        models.LabelPrint.objects.filter(id=body['id']).update(created=timezone.now() - timedelta(hours=25))
        code, listing = self.call('GET', '/api/label-prints/pending/', session=False)
        self.assertEqual(listing['pending'], [])
        code, tasks = self.call('GET', '/api/tasks/')
        self.assertFalse(any(row['link'] == 'label_prints' for row in tasks['tasks']))
        self.assertEqual(self.call('POST', f"/api/label-prints/{body['id']}/print/", {}, session=False)[0], 410)
        self.assertEqual(Items.objects.count(), 0)
        code, web = self.call('POST', '/api/label-prints/list/', {'user': self.admin()}, local=False)
        self.assertTrue(web['prints'][0]['expired'])

    def label(self, width: int = 10, height: int = 3) -> dict[str, Any]:
        import base64
        return {'description': 'Lot 1/1', 'width': width, 'height': height, 'ppm': 8,
                'bits': base64.b64encode(bytes((width * height + 7) // 8)).decode()}

    def test_labels_from_another_computer(self) -> None:
        # front d'un autre ordinateur (API distante, cle de front), personne de connecte ou non
        _, key = FrontKey.create('Ordi du TECH')
        remote = {'qrprotec.role': 'remote', 'HTTP_X_QRPROTEC_KEY': key}
        send = lambda data: self.client.post('/api/label-prints/send/', json.dumps(data),
                                             content_type='application/json', **remote)
        self.assertEqual(send({'title': 'Lot B+', 'labels': []}).status_code, 400)
        bad = self.label()
        bad['bits'] = 'AAAA'
        self.assertEqual(send({'title': 'Lot B+', 'labels': [bad]}).status_code, 400)
        response = send({'title': 'Lot B+ 1 (clé)', 'labels': [self.label(), self.label()]})
        self.assertEqual(response.status_code, 201)
        sent = response.json()
        self.assertEqual((sent['kind'], sent['count'], sent['front']), ('labels', 2, 'Ordi du TECH'))
        self.assertEqual(sent['what'], '2 étiquettes : Lot B+ 1 (clé)')
        # pas sur l'API publique
        self.assertEqual(self.call('POST', '/api/label-prints/send/', {'labels': [self.label()]}, local=False)[0], 404)
        code, listing = self.call('GET', '/api/label-prints/pending/', session=False)
        self.assertEqual([row['id'] for row in listing['pending']], [sent['id']])
        self.assertNotIn('labels', listing['pending'][0])
        code, printed = self.call('POST', f"/api/label-prints/{sent['id']}/print/", {}, session=False)
        self.assertEqual(code, 200)
        self.assertEqual(len(printed['labels']), 2)
        self.assertEqual(printed['labels'][0]['width'], 10)
        self.assertEqual(Items.objects.count(), 0)
        # pas listee parmi les etiquettes d'items du web
        code, web = self.call('POST', '/api/label-prints/list/', {'user': self.admin()}, local=False)
        self.assertEqual(web['prints'], [])

    def test_labels_keep_their_label_type(self) -> None:
        # type d'etiquette du modele (rouleau) : transmis au poste, qui attend que ce rouleau soit charge
        _, key = FrontKey.create('Ordi du TECH')
        remote = {'qrprotec.role': 'remote', 'HTTP_X_QRPROTEC_KEY': key}
        send = lambda labels: self.client.post('/api/label-prints/send/', json.dumps({'title': 'Lot', 'labels': labels}),
                                               content_type='application/json', **remote)
        typed = self.label()
        typed['label_type'] = {'name': ' PET 40x30 ', 'width_mm': 40, 'height_mm': 30, 'rotation': 90,
                               'support': 'transfer', 'extra': 'ignore'}
        bad = self.label()
        bad['label_type'] = {'name': 'PET', 'width_mm': 40, 'height_mm': 30, 'rotation': 45}
        self.assertEqual(send([bad]).status_code, 400)
        bad['label_type'] = {'name': '', 'width_mm': 40, 'height_mm': 30}
        self.assertEqual(send([bad]).status_code, 400)
        response = send([typed, self.label()])
        self.assertEqual(response.status_code, 201)
        code, printed = self.call('POST', f"/api/label-prints/{response.json()['id']}/print/", {}, session=False)
        self.assertEqual(code, 200)
        self.assertEqual(printed['labels'][0]['label_type'], {'name': 'PET 40x30', 'width_mm': 40.0, 'height_mm': 30.0,
                                                              'rotation': 90, 'support': 'transfer'})
        self.assertNotIn('label_type', printed['labels'][1])
        # marge du type transmise si elle est donnee, bornee a 0..10 mm
        bad['label_type'] = {'name': 'PET', 'width_mm': 40, 'height_mm': 30, 'margin_mm': 12}
        self.assertEqual(send([bad]).status_code, 400)
        typed['label_type']['margin_mm'] = 1
        response = send([typed])
        code, printed = self.call('POST', f"/api/label-prints/{response.json()['id']}/print/", {}, session=False)
        self.assertEqual(printed['labels'][0]['label_type']['margin_mm'], 1.0)
        # une marge par cote : transmises telles quelles, bornees de meme
        typed['label_type'].update({'margin_top_mm': 2, 'margin_bottom_mm': 0, 'margin_left_mm': 0.5,
                                    'margin_right_mm': 1.5})
        response = send([typed])
        code, printed = self.call('POST', f"/api/label-prints/{response.json()['id']}/print/", {}, session=False)
        sent = printed['labels'][0]['label_type']
        self.assertEqual((sent['margin_top_mm'], sent['margin_bottom_mm'], sent['margin_left_mm'],
                          sent['margin_right_mm']), (2.0, 0.0, 0.5, 1.5))
        bad['label_type'] = {'name': 'PET', 'width_mm': 40, 'height_mm': 30, 'margin_left_mm': -1}
        self.assertEqual(send([bad]).status_code, 400)

    def test_one_queue_per_requester(self) -> None:
        # plusieurs portables envoient en meme temps : une file par personne qui demande, puis par ordinateur
        self.user.role = 'gestion'
        self.user.save()
        gestion = {'matricule': 'M001', 'key': self.user.new_key}
        first = self.ask(type='garrot', count=2)[1]
        second = self.call('POST', '/api/label-prints/', {'user': gestion, 'type': 'garrot', 'count': 3},
                           local=False)[1]
        third = self.ask(type='garrot', count=1)[1]
        _, key = FrontKey.create('Portable 3')
        sent = self.client.post('/api/label-prints/send/', json.dumps({'title': 'Lot', 'labels': [self.label()]}),
                                content_type='application/json', **{'qrprotec.role': 'remote',
                                                                    'HTTP_X_QRPROTEC_KEY': key}).json()
        self.assertEqual(sent['queue'], 'front:Portable 3')  # personne de connecte sur le portable
        code, listing = self.call('GET', '/api/label-prints/pending/', session=False)
        self.assertEqual(code, 200)
        queues = listing['queues']
        self.assertEqual([queue['queue'] for queue in queues], ['M:P001', 'M:M001', 'front:Portable 3'])
        self.assertEqual([queue['matricule'] for queue in queues], ['P001', 'M001', ''])
        self.assertEqual(queues[0]['ids'], [first['id'], third['id']])
        self.assertEqual((queues[0]['requests'], queues[0]['labels']), (2, 3))
        self.assertEqual(queues[1]['ids'], [second['id']])
        self.assertEqual(queues[2]['name'], 'Portable 3')
        self.assertEqual({row['queue'] for row in listing['pending']}, {'M:P001', 'M:M001', 'front:Portable 3'})

    def test_admins_manage_queues_remotely(self) -> None:
        self.user.role = 'gestion'
        self.user.save()
        gestion = {'matricule': 'M001', 'key': self.user.new_key}
        mine = self.ask(type='garrot', count=2)[1]
        other = self.call('POST', '/api/label-prints/', {'user': gestion, 'type': 'garrot', 'count': 3},
                          local=False)[1]
        # gestion et admins (poste : menu Impression > Files d'impression), pas les secouristes
        code, body = self.call('POST', '/api/label-prints/queues/', {'user': gestion}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual([queue['queue'] for queue in body['queues']], ['M:P001', 'M:M001'])
        self.user.role = 'normal'
        self.user.save()
        for path in ('/api/label-prints/queues/', '/api/label-prints/queues/launch/'):
            code, _ = self.call('POST', path, {'user': gestion, 'queue': 'M:M001'}, local=False)
            self.assertEqual(code, 403)
        self.user.role = 'gestion'
        self.user.save()
        code, web = self.call('POST', '/api/label-prints/list/', {'user': gestion}, local=False)
        self.assertNotIn('queues', web)
        code, web = self.call('POST', '/api/label-prints/list/', {'user': self.admin()}, local=False)
        self.assertEqual([queue['queue'] for queue in web['queues']], ['M:P001', 'M:M001'])
        self.assertNotIn('matricule', web['queues'][0])  # web : pas besoin du matricule
        code, body = self.call('POST', '/api/label-prints/queues/', {'user': self.admin()}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual([row['id'] for row in body['queues'][1]['prints']], [other['id']])
        # lancee a distance : le poste la voit a son prochain passage
        code, body = self.call('POST', '/api/label-prints/queues/launch/', {'user': self.admin(), 'queue': 'M:M001'},
                               local=False)
        self.assertEqual((code, body['launched']), (200, 1))
        code, listing = self.call('GET', '/api/label-prints/pending/', session=False)
        launched = {queue['queue']: queue['launch_requested'] for queue in listing['queues']}
        self.assertIsNone(launched['M:P001'])
        self.assertIsNotNone(launched['M:M001'])
        self.assertTrue(listing['queues'][1]['launch_requested_by'])
        # annulee a distance (aussi depuis le poste d'un admin)
        code, body = self.call('POST', '/api/label-prints/queues/cancel/', {'queue': 'M:P001'})
        self.assertEqual((code, body['cancelled']), (200, 1))
        self.assertFalse(models.LabelPrint.objects.get(id=mine['id']).pending)
        self.assertEqual(self.call('POST', '/api/label-prints/queues/cancel/', {'queue': 'M:P001'})[0], 404)
        self.assertEqual(self.call('POST', '/api/label-prints/queues/stop/', {'queue': 'M:M001'})[0], 404)


class PrintCountTests(ApiTestCase):
    """Compteur d'impressions : etiquettes envoyees a l'imprimante, par mois et par categorie d'etiquette."""

    def send(self, batch: str, counts: list[dict[str, Any]], session: bool = False) -> tuple[int, dict[str, Any]]:
        return self.call('POST', '/api/print-counts/', {'batch': batch, 'counts': counts}, session=session)

    def row(self, day: date, count: int, width: int = 40, height: int = 30, support: str = 'direct') -> dict[str, Any]:
        return {'day': day.isoformat(), 'width_mm': width, 'height_mm': height, 'support': support, 'count': count}

    def test_counts_by_month_and_category(self) -> None:
        first = self.today.replace(day=1)
        last_month = first - timedelta(days=1)
        code, body = self.send('batch-0001', [self.row(self.today, 12), self.row(first, 3),
                                              self.row(self.today, 5, 60, 40, 'transfer')])
        self.assertEqual(code, 201)
        self.assertEqual(body['counted'], 20)
        self.send('batch-0002', [self.row(self.today, 1), self.row(last_month, 7)])
        code, body = self.call('GET', f'/api/print-counts/?year={self.today.year}')
        self.assertEqual(code, 200)
        month = self.today.month - 1
        direct, transfer = body['categories']  # les plus imprimees d'abord
        self.assertEqual(direct['label'], '40 × 30 mm, thermique direct')
        self.assertEqual(direct['months'][month], 16)
        self.assertEqual(transfer['label'], '60 × 40 mm, transfert thermique')
        self.assertEqual(transfer['total'], 5)
        self.assertEqual(body['months_total'][month], 21)
        if last_month.year == self.today.year:
            self.assertEqual(direct['months'][month - 1], 7)
            self.assertEqual(body['total'], 28)
        self.assertEqual(body['current_month'], self.today.month)
        self.assertIn(self.today.year, body['years'])

    def test_resent_batch_not_counted_twice(self) -> None:
        self.assertEqual(self.send('batch-0001', [self.row(self.today, 4)])[0], 201)
        code, body = self.send('batch-0001', [self.row(self.today, 4)])
        self.assertEqual(code, 200)
        self.assertTrue(body['duplicate'])
        self.assertEqual(PrintCount.objects.get().count, 4)

    def test_fronts_counted_apart_then_summed(self) -> None:
        self.send('batch-0001', [self.row(self.today, 2)])
        key = FrontKey.create('Bureau')[1]
        response = self.client.post('/api/print-counts/', json.dumps({'batch': 'batch-0002',
                                                                      'counts': [self.row(self.today, 3)]}),
                                    content_type='application/json', HTTP_X_QRPROTEC_KEY=key,
                                    HTTP_X_FORWARDED_FOR='192.0.2.7', REMOTE_ADDR='127.0.0.1',
                                    **{'qrprotec.role': 'remote'})
        self.assertEqual(response.status_code, 201)
        self.assertEqual(sorted(PrintCount.objects.values_list('front', 'count')), [('', 2), ('Bureau', 3)])
        self.assertEqual(self.call('GET', '/api/print-counts/')[1]['total'], 5)

    def test_invalid_counts_refused(self) -> None:
        for counts in ([], [self.row(self.today, 0)], [self.row(self.today, 1, support='laser')],
                       [self.row(self.today, 1, width=1000)], [self.row(self.today + timedelta(days=5), 1)],
                       [{'day': 'hier', 'width_mm': 40, 'height_mm': 30, 'support': 'direct', 'count': 1}]):
            self.assertEqual(self.send('batch-0001', counts)[0], 400, counts)
        self.assertEqual(self.send('court', [self.row(self.today, 1)])[0], 400)
        self.assertFalse(PrintCount.objects.exists())
        self.assertFalse(PrintCountBatch.objects.exists())

    def test_access(self) -> None:
        # envoi : tout poste, pas l'API publique ; lecture : gestion et admin connectes au poste
        self.assertEqual(self.call('POST', '/api/print-counts/', {}, local=False)[0], 404)
        self.assertEqual(self.call('GET', '/api/print-counts/', session=False)[0], 403)
        self.assertEqual(self.call('GET', '/api/print-counts/', session=views.session_token(self.user))[0], 403)
        self.assertEqual(self.call('GET', '/api/print-counts/')[0], 200)


class FunStatsTests(ApiTestCase):
    """Stats fun de l'AG (5 appuis sur « Prévisions » du web) : gestion et admin, noms courts, vérifs en sessions."""

    def verif(self, by: str, at: datetime, lot: Lots | None = None, participants: tuple[()] | list[str] = (),
              present: int = 0, expired_items: list[Items] | tuple[()] = (), started: datetime | None = None,
              complete: bool = True) -> Verifs:
        verif = Verifs.objects.create(lot=lot or self.lot, by=by, datetime=at, received=at, started=started,
                                      participants=list(participants), present_count=present, complete=complete)
        for item in expired_items:
            models.VerifItem.objects.create(verif=verif, item=item, expired=True)
        return verif

    def test_fun_stats(self) -> None:
        paul = Secouristes(matricule='M002', nom='Martin', prenom='Paul')
        paul.save()
        sub = Lots(lot_type=self.lot_type, name='Sac B', name_short='B', created_by='test', parent=self.lot)
        sub.save()
        now = timezone.now().replace(hour=12, minute=0, second=0, microsecond=0)
        old = self.create(self.compresses, self.today - timedelta(days=400), 1)
        # cascade : Sac A puis son sous-lot 10 min apres = une seule vérif, a deux
        self.verif('M:M001', now - timedelta(days=3), participants=['M:M002'], present=4, expired_items=old)
        self.verif('M:M001', now - timedelta(days=3, minutes=-10), lot=sub, participants=['M:M002'], present=2)
        self.verif('M:M001', now - timedelta(days=2), participants=['M:M002', 'D:Léo (invité)'])
        self.verif('M:M002', now - timedelta(days=1), present=1)
        self.verif('M:M001', now - timedelta(days=500))  # hors des 12 derniers mois
        # refuse aux secouristes, accepte a la gestion (badge sur l'API publique)
        code, _ = self.call('POST', '/api/stats/fun/', {'user': {'matricule': 'M001', 'key': self.user.new_key}},
                            local=False)
        self.assertEqual(code, 403)
        self.user.role = 'gestion'
        self.user.save()
        code, body = self.call('POST', '/api/stats/fun/', {'user': {'matricule': 'M001', 'key': self.user.new_key}},
                               local=False)
        self.assertEqual(code, 200)
        self.assertEqual(body['totals']['verifs'], 3)
        self.assertEqual(body['totals']['lots'], 4)
        self.assertEqual(body['totals']['expired'], 1)
        self.assertEqual(body['totals']['team_verifs'], 2)
        awards = {award['key']: award for award in body['awards']}
        self.assertEqual([(row['name'], row['value']) for row in awards['verifs']['podium']],
                         [('Paul', 3), ('Jeanne', 2)])
        self.assertEqual(awards['duos']['podium'][0]['name'], 'Jeanne & Paul')
        self.assertEqual(awards['duos']['podium'][0]['value'], 2)
        self.assertEqual(awards['expired']['podium'][0]['value'], 1)
        self.assertIn('Compresses', awards['fossil']['record']['detail'])
        self.assertEqual(awards['pet']['podium'][0]['name'], 'Sac A')
        text = json.dumps(body)
        self.assertNotIn('Dupont', text)  # jamais de nom de famille
        self.assertNotIn('Martin', text)
        self.assertNotIn('Léo', text)     # ni invites ni noms declares dans les podiums
        # annee civile : la vérif d'il y a 500 jours
        year = timezone.localtime(now - timedelta(days=500)).year
        self.assertIn(year, body['years'])
        code, body = self.call('POST', '/api/stats/fun/', {'period': year}, session=views.session_token(self.user))
        self.assertEqual(code, 200)
        self.assertEqual(body['period'], str(year))

    def test_fun_stats_durations(self) -> None:
        paul = Secouristes(matricule='M002', nom='Martin', prenom='Paul')
        paul.save()
        sac_b = Lots(lot_type=self.lot_type, name='Sac B', name_short='B', created_by='test')
        sac_b.save()
        sub = Lots(lot_type=self.lot_type, name='Poche', name_short='P', created_by='test', parent=self.lot)
        sub.save()
        now = timezone.now().replace(hour=12, minute=0, second=0, microsecond=0)

        def at(days: int, minutes: float = 0) -> datetime:
            return now - timedelta(days=days) + timedelta(minutes=minutes)

        # Sac A : Jeanne seule en 10 min puis 20 min, en cascade (Sac A puis sa poche, 6 min + 4 min) ; Paul en 5 min
        self.verif('M:M001', at(10, 10), present=20, started=at(10))
        self.verif('M:M001', at(9, 6), present=30, started=at(9))
        self.verif('M:M001', at(9, 20), lot=sub, present=10, started=at(9, 6))
        fast = self.verif('M:M002', at(8, 5), present=12, started=at(8))
        for second in range(0, 50, 5):  # 10 scans en 45 s
            models.VerifItem.objects.create(verif=fast, item=self.create(self.garrot, None, 1)[0],
                                            scanned_at=at(8, 1) + timedelta(seconds=second))
        # Sac B : a deux en 3 min 30 s ; incomplete ; sans duree (avant le chronometrage) ; oubliee (5 h)
        self.verif('M:M001', at(7, 3.5), lot=sac_b, participants=['M:M002'], present=15, started=at(7))
        self.verif('M:M001', at(6, 1), lot=sac_b, present=4, started=at(6), complete=False)
        self.verif('M:M002', at(5, 1), lot=sac_b, present=4)
        self.verif('M:M002', at(4, 300), lot=sac_b, present=4, started=at(4))
        self.user.role = 'gestion'
        self.user.save()
        code, body = self.call('POST', '/api/stats/fun/', {}, session=views.session_token(self.user))
        self.assertEqual(code, 200)
        tiles = {tile['label']: tile['value'] for tile in body['time_totals']}
        # 10 + 20 + 5 + 3,5 + 1 min = 39 min 30 s chronometrees (ni la vérif sans debut ni celle de 5 h)
        self.assertEqual(tiles['passées en vérif'], '39 min')
        self.assertEqual(tiles['vérifs chronométrées'], '5')
        self.assertIn('43 min', tiles.values())  # a deux : 3 min 30 s chacun en plus
        awards = {award['key']: award for award in body['awards']}
        self.assertEqual([(row['name'], row['value']) for row in awards['time']['podium']],
                         [('Jeanne', '34 min'), ('Paul', '8 min')])
        self.assertEqual(awards['quickest']['record']['value'], '3 min 30 s')
        self.assertIn('Sac B', awards['quickest']['record']['detail'])
        self.assertEqual(awards['slowest']['record']['value'], '20 min 00 s')
        self.assertIn('Sac A', awards['slowest']['record']['detail'])  # la cascade se nomme du lot du haut
        self.assertEqual(awards['burst']['record']['value'], '10 scans en 1 min')
        self.assertEqual(awards['burst']['record']['name'], 'Paul')
        lots = {lot['name']: lot for lot in body['lots']}
        self.assertEqual(body['lots'][0]['name'], 'Sac A')  # le plus verifie d'abord
        self.assertEqual(lots['Sac A']['count'], 3)
        self.assertEqual(lots['Sac A']['fastest']['value'], '5 min 00 s')
        self.assertEqual(lots['Sac A']['fastest']['name'], 'Paul')
        self.assertEqual(lots['Sac A']['slowest']['value'], '20 min 00 s')
        self.assertEqual([(row['name'], row['value']) for row in lots['Sac A']['podium']],
                         [('Paul', '5 min 00 s'), ('Jeanne', '10 min 00 s')])
        self.assertEqual(lots['Sac B']['count'], 1)
        self.assertEqual(lots['Sac B']['fastest']['name'], 'Jeanne & Paul')
        self.assertNotIn('Martin', json.dumps(body))
