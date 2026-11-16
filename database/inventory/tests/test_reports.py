"""Tests du back : signalements, desactivation, revisions."""

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)


@override_settings(QRPROTEC={**settings.QRPROTEC, 'NOTIFY_SYNC': True})
class ProblemReportTests(ApiTestCase):
    def setUp(self):
        super().setUp()
        from .. import webpush
        self.sent = []  # textes des notifications web
        original = webpush.notify_event
        webpush.notify_event = lambda field, title, body, **web: (self.sent.append(body) or 1)
        self.addCleanup(setattr, webpush, 'notify_event', original)
        self.badge = {'matricule': 'M001', 'key': self.user.new_key}
        self.items = self.create(self.compresses, self.today + timedelta(days=30), 2)
        Items.objects.filter(iid__in=[item.iid for item in self.items]).update(location=self.lot)

    def report(self, data, local=False):
        payload = {'user': self.badge, **data} if not local else data
        return self.call('POST', '/api/reports/', payload, local=local)

    def test_login_required(self):
        code, _ = self.call('POST', '/api/reports/', {'item': self.items[0].iid, 'usable': True}, local=False)
        self.assertEqual(code, 403)
        code, _ = self.call('POST', '/api/reports/', {'item': self.items[0].iid, 'usable': True}, session=False)
        self.assertEqual(code, 403)
        code, body = self.report({'item': self.items[0].iid})  # la question « utilisable ? » est obligatoire
        self.assertEqual(code, 400)

    def test_report_notifies_admins_and_lists(self):
        with self.captureOnCommitCallbacks(execute=True):
            code, body = self.report({'item': self.items[0].iid, 'comment': 'Emballage déchiré', 'usable': True})
        self.assertEqual(code, 201)
        self.assertEqual(body['report']['color'], 'red')
        self.assertEqual(body['report']['lot'], self.lot.id)
        self.assertEqual(body['report']['type'], 'compre')
        self.assertTrue(body['item']['reported'])
        self.assertFalse(body['item']['expired'])
        self.assertEqual(len(self.sent), 1)
        self.assertIn('Emballage déchiré', self.sent[0])
        # suivi en lecture seule : tout utilisateur connecte
        code, body = self.call('POST', '/api/reports/list/', {'user': self.badge}, local=False)
        self.assertEqual(code, 200)
        self.assertFalse(body['can_manage'])
        self.assertEqual(body['counts']['new'], 1)
        self.assertEqual(body['reports'][0]['comment'], 'Emballage déchiré')
        code, _ = self.call('POST', '/api/reports/list/', {}, local=False)
        self.assertEqual(code, 403)
        # signalement d'un type d'item d'un lot, depuis le poste
        code, body = self.report({'lot': self.lot.id, 'type': 'compre', 'usable': False}, local=True)
        self.assertEqual(code, 201)
        self.assertEqual(body['report']['target'], 'Compresses (Sac A)')
        self.assertTrue(Operation.objects.filter(kind='report').exists())

    def test_admin_update(self):
        _, body = self.report({'item': self.items[0].iid, 'usable': True})
        report_id = body['report']['id']
        # un secouriste ne gere pas les signalements
        code, _ = self.call('POST', f'/api/reports/{report_id}/', {'user': self.badge, 'status': 'progress'}, local=False)
        self.assertEqual(code, 403)
        code, body = self.call('POST', f'/api/reports/{report_id}/', {'status': 'progress', 'usable': False,
                                                                      'note': 'Commandé'})
        self.assertEqual(code, 200)
        self.assertEqual((body['status'], body['color'], body['note']), ('progress', 'orange', 'Commandé'))
        self.assertTrue(Items.objects.get(iid=self.items[0].iid).unusable)
        code, body = self.call('POST', f'/api/reports/{report_id}/', {'usable': True})
        self.assertEqual(body['color'], 'green')
        code, body = self.call('POST', f'/api/reports/{report_id}/', {'status': 'resolved'})
        self.assertEqual(body['color'], 'green')
        item = Items.objects.get(iid=self.items[0].iid)
        self.assertFalse(item.reported or item.unusable)
        code, body = self.call('POST', '/api/reports/list/', {})
        self.assertTrue(body['can_manage'])
        self.assertEqual(body['reports'], [])
        code, body = self.call('POST', '/api/reports/list/', {'show': 'all'})
        self.assertEqual(len(body['reports']), 1)

    def test_three_messages_at_most(self):
        """Trois messages au plus : le signalement, la reponse en cours (remplacee a chaque bouton « En cours ») et
        celle de la resolution (effacee si le signalement est rouvert)."""
        _, body = self.report({'item': self.items[0].iid, 'usable': True, 'comment': 'Emballage déchiré'})
        url = f"/api/reports/{body['report']['id']}/"
        code, body = self.call('POST', url, {'status': 'progress', 'usable': True, 'note': 'Je regarde demain'})
        self.assertEqual((code, body['note']), (200, 'Je regarde demain'))
        code, body = self.call('POST', url, {'status': 'progress', 'usable': False, 'note': '  Commandé  '})
        self.assertEqual((body['note'], body['color']), ('Commandé', 'orange'))
        code, body = self.call('POST', '/api/reports/list/', {'user': self.badge}, local=False)
        self.assertEqual(body['reports'][0]['note'], 'Commandé')
        code, body = self.call('GET', f'/api/items/{self.items[0].iid}/')
        self.assertEqual(body['reports'][0]['note'], 'Commandé')
        # zone de texte laissee telle quelle en resolvant : pas de message de resolution
        code, body = self.call('POST', url, {'status': 'resolved', 'resolution': 'ok', 'resolution_note': 'Commandé'})
        self.assertEqual((body['note'], body['resolution_note']), ('Commandé', ''))
        self.call('POST', url, {'status': 'progress'})
        code, body = self.call('POST', url, {'status': 'resolved', 'resolution': 'ok', 'resolution_note': 'Remplacé'})
        self.assertEqual((body['comment'], body['note'], body['resolution_note']),
                         ('Emballage déchiré', 'Commandé', 'Remplacé'))
        code, body = self.call('POST', url, {'status': 'progress'})  # rouvert : la resolution n'a plus cours
        self.assertEqual((body['note'], body['resolution_note']), ('Commandé', ''))
        code, body = self.call('POST', url, {'status': 'progress', 'usable': True, 'note': ''})
        self.assertEqual(body['note'], '')

    def test_list_puts_reports_to_handle_first(self):
        _, first = self.report({'item': self.items[0].iid, 'usable': True})
        self.call('POST', f"/api/reports/{first['report']['id']}/", {'status': 'resolved'})
        _, usable = self.report({'item': self.items[0].iid, 'usable': True})
        self.call('POST', f"/api/reports/{usable['report']['id']}/", {'status': 'progress'})
        _, broken = self.report({'item': self.items[1].iid, 'usable': False})
        self.call('POST', f"/api/reports/{broken['report']['id']}/", {'status': 'progress'})
        _, new = self.report({'lot': self.lot.id, 'type': 'compre', 'usable': True}, local=True)
        code, body = self.call('POST', '/api/reports/list/', {'show': 'all'})
        # nouveau (rouge), en cours inutilisable (orange), en cours utilisable, puis resolu
        self.assertEqual([row['id'] for row in body['reports']],
                         [new['report']['id'], broken['report']['id'], usable['report']['id'], first['report']['id']])

    def test_unusable_item_must_be_replaced(self):
        broken, other = self.items
        self.report({'item': broken.iid, 'usable': False})
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/')
        self.assertFalse(body['complete'])
        self.assertEqual(body['requirements'][0]['present'], 1)
        # verif : l'item inutilisable scanne reste a remplacer
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [broken.iid, other.iid]})
        self.assertFalse(report['complete'])
        self.assertEqual(report['unusable'], [broken.iid])
        # remplacant scanne : l'item inutilisable sort du lot
        new = self.create(self.compresses, self.today + timedelta(days=300), 1)[0]
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [broken.iid, other.iid, new.iid]})
        self.assertTrue(report['complete'])
        self.assertEqual(report['replaced'], [broken.iid])

    def test_lot_reported_unusable_shows_on_its_fiche(self):
        # signalement d'un lot « non utilisable » : rouge et visible de tous sur sa fiche, meme apres une verif complete
        _, body = self.report({'lot': self.lot.id, 'comment': 'Sac déchiré', 'usable': False})
        report_id = body['report']['id']
        self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid for item in self.items]})
        code, lot = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertEqual(code, 200)
        self.assertTrue(lot['complete'])
        self.assertTrue(lot['unusable'])
        self.assertEqual((lot['state']['kind'], lot['state']['code']), ('bad', 'unusable'))
        self.assertEqual([(row['comment'], row['usable']) for row in lot['reports']], [('Sac déchiré', False)])
        self.assertNotIn('nom', lot['reports'][0])
        # liste des lots (etat calcule pour toute la liste a la fois) : meme etat
        code, lots = self.call('GET', '/api/lots/')
        row = next(row for row in lots if row['id'] == self.lot.id)
        self.assertEqual(row['state']['code'], 'unusable')
        self.assertEqual(len(row['reports']), 1)
        # utilisable apres prise en charge : signalement toujours affiche, le lot redevient valide
        self.call('POST', f'/api/reports/{report_id}/', {'status': 'progress', 'usable': True})
        _, lot = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertFalse(lot['unusable'])
        self.assertEqual(lot['state']['code'], 'verified')
        self.assertEqual(len(lot['reports']), 1)
        # resolu : plus rien sur la fiche
        self.call('POST', f'/api/reports/{report_id}/', {'status': 'resolved'})
        _, lot = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertEqual(lot['reports'], [])

    def test_item_type_report_listed_on_lot(self):
        self.report({'lot': self.lot.id, 'type': 'compre', 'usable': True}, local=True)
        _, lot = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertEqual([row['type_name'] for row in lot['reports']], ['Compresses'])
        self.assertFalse(lot['unusable'])
        # le signalement d'un item n'est pas un signalement du lot : il suit l'item (et le rend a remplacer)
        self.report({'item': self.items[0].iid, 'usable': False})
        _, lot = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertEqual(len(lot['reports']), 1)
        self.assertFalse(lot['unusable'])

    def test_reported_item_never_missing(self):
        reported, other = self.items
        self.report({'item': reported.iid, 'usable': True})
        for _ in range(3):
            code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [other.iid]})
            self.assertNotIn(reported.iid, report['missing'])
        item = Items.objects.get(iid=reported.iid)
        self.assertEqual((item.status, item.missed_verifs), (ItemStatus.ACTIVE, 0))

    def test_resolve_non_compliant_item(self):
        disabled, removed = self.items
        _, body = self.report({'item': disabled.iid, 'usable': False, 'comment': 'Date effacée'})
        first = body['report']['id']
        _, body = self.report({'item': removed.iid, 'usable': False})
        second = body['report']['id']
        # l'issue se donne en resolvant
        code, _ = self.call('POST', f'/api/reports/{first}/', {'status': 'progress', 'resolution': 'disabled'})
        self.assertEqual(code, 400)
        code, body = self.call('POST', f'/api/reports/{first}/', {'status': 'resolved', 'resolution': 'disabled'})
        self.assertEqual(code, 200)
        self.assertEqual((body['resolution'], body['resolution_label']), ('disabled', 'Désactivé'))
        item = Items.objects.get(iid=disabled.iid)
        self.assertTrue(item.disabled and not item.unusable and not item.reported)
        self.assertIn('Date effacée', item.disabled_reason)
        self.assertTrue(item.needs_replacement())
        code, body = self.call('POST', f'/api/reports/{second}/', {'status': 'resolved', 'resolution': 'removed'})
        self.assertEqual(code, 200)
        self.assertEqual(Items.objects.get(iid=removed.iid).status, ItemStatus.DELETED)
        # rouvert : l'issue est effacee, l'item reste desactive (reactivation a la main)
        code, body = self.call('POST', f'/api/reports/{first}/', {'status': 'progress'})
        self.assertEqual(body['resolution'], '')
        self.assertTrue(Items.objects.get(iid=disabled.iid).disabled)
        # signalement d'un lot : pas d'item a desactiver
        _, body = self.report({'lot': self.lot.id, 'usable': True})
        code, _ = self.call('POST', f"/api/reports/{body['report']['id']}/", {'status': 'resolved',
                                                                              'resolution': 'disabled'})
        self.assertEqual(code, 400)


class ItemDisableTests(ApiTestCase):
    def setUp(self):
        super().setUp()
        self.items = self.create(self.compresses, self.today + timedelta(days=30), 3)
        Items.objects.filter(iid__in=[item.iid for item in self.items[:2]]).update(location=self.lot)

    def disable(self, data, **kwargs):
        return self.call('POST', '/api/items/disable/', data, **kwargs)

    def test_disable_one_item(self):
        target, other, _ = self.items
        code, _ = self.disable({'items': [target.iid]})
        self.assertEqual(code, 400)  # raison obligatoire
        code, _ = self.disable({'items': [target.iid], 'reason': 'Rappel'}, local=False)
        self.assertIn(code, (403, 405))  # au poste seulement (route absente de l'API publique)
        gestion = Secouristes(matricule='G001', nom='Gestion', prenom='Gil', role='gestion')
        gestion.renew_key()
        gestion.save()
        code, _ = self.disable({'items': [target.iid], 'reason': 'Rappel'}, session=views.session_token(gestion))
        self.assertEqual(code, 403)  # administrateurs seulement
        code, body = self.disable({'items': [target.iid], 'reason': 'Rappel produit'})
        self.assertEqual((code, body['count']), (200, 1))
        _, item = self.call('GET', f'/api/items/{target.iid}/')
        self.assertTrue(item['disabled'] and item['expired'] and not item['outdated'])
        self.assertEqual(item['disabled_reason'], 'Rappel produit')
        self.assertTrue(Operation.objects.filter(kind='item_disable').exists())
        # comme un perime : le lot est incomplet, le remplacant sort l'item desactive du lot
        code, lot = self.call('GET', f'/api/lots/{self.lot.id}/')
        self.assertFalse(lot['complete'])
        new = self.create(self.compresses, self.today + timedelta(days=300), 1)[0]
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [target.iid, other.iid, new.iid]})
        self.assertTrue(report['complete'])
        self.assertEqual(report['replaced'], [target.iid])
        # reactivation
        code, body = self.disable({'items': [target.iid], 'disabled': False})
        self.assertEqual(body['count'], 1)
        self.assertFalse(Items.objects.get(iid=target.iid).disabled)

    def test_receptions_and_whole_reception(self):
        later = self.create(self.compresses, self.today + timedelta(days=30), 2)  # autre reception, meme paquet
        code, rows = self.call('GET', '/api/receptions/?type=compre')
        self.assertEqual(code, 200)
        self.assertEqual([row['count'] for row in rows], [2, 3])
        code, rows = self.call('GET', f'/api/receptions/?item={self.items[1].iid}')
        self.assertEqual((len(rows), rows[0]['item'], rows[0]['count']), (1, self.items[0].iid, 3))
        code, body = self.disable({'reception': self.items[1].iid, 'reason': 'Rappel sérum phy'})
        self.assertEqual(body['count'], 3)
        self.assertFalse(Items.objects.get(iid=later[0].iid).disabled)
        code, rows = self.call('GET', '/api/receptions/?type=compre')
        self.assertEqual(rows[1]['disabled'], 3)
        code, stock = self.call('GET', '/api/stock/')
        row = next(row for row in stock if row['type'] == 'compre')
        self.assertEqual((row['stock_fresh'], row['stock_expired']), (2, 1))
        self.assertEqual((row['lots_fresh'], row['lots_expired']), (0, 2))

    def test_sealed_pack_report_and_disable(self):
        date = (self.today + timedelta(days=200)).isoformat()
        _, body = self.call('POST', '/api/items/batch/',
                            {'type': 'compre', 'peremption': date, 'count': 3, 'sealed_pack': True, 'user': 'M001'})
        pack_id = body['sealed_pack']['id']
        badge = {'matricule': 'M001', 'key': self.user.new_key}
        code, body = self.call('POST', '/api/reports/', {'user': badge, 'pack': pack_id, 'usable': True,
                                                         'comment': 'Boîte écrasée'}, local=False)
        self.assertEqual(code, 201)
        self.assertEqual(body['report']['pack'], pack_id)
        self.assertEqual(body['report']['target'], f'Paquet de Compresses ({pack_id})')
        report_id = body['report']['id']
        # tous les items du paquet sont signales : popup au scan, avec le commentaire
        iid = Items.objects.filter(sealed_pack_id=pack_id).first().iid
        _, item = self.call('GET', f'/api/items/{iid}/', local=False)
        self.assertTrue(item['reported'])
        self.assertEqual(item['reports'][0]['comment'], 'Boîte écrasée')
        _, pack = self.call('GET', f'/api/packs/{pack_id}/', local=False)
        self.assertEqual((pack['reported'], pack['disabled']), (1, 0))
        # resolu : paquet desactive
        code, body = self.call('POST', f'/api/reports/{report_id}/', {'status': 'resolved', 'resolution': 'disabled'})
        self.assertEqual(code, 200)
        self.assertEqual(Items.objects.filter(sealed_pack_id=pack_id, disabled=True, reported=False).count(), 3)
        # desactivation directe d'un paquet (reactivation ici)
        code, body = self.disable({'pack': pack_id, 'disabled': False})
        self.assertEqual(body['count'], 3)


class RevisionTests(ApiTestCase):
    """Revision periodique du materiel non consommable : date de prochaine revision, taches, retard bloquant ou non."""

    def setUp(self):
        super().setUp()
        from .. import webpush
        self.sent = []
        self.addCleanup(setattr, webpush, 'notify_event', webpush.notify_event)
        webpush.notify_event = lambda field, title, body, **web: self.sent.append((field, body)) or 1
        self.aspi = ItemType.objects.create(type='aspimu', name='Aspirateur', consumable=False, revision_months=3,
                                            revision_tasks=['Batterie chargée', 'Tuyau propre'])
        LotRequirements.objects.create(lot_type=self.lot_type, item_type=self.aspi, quantity=1)

    def test_new_item_due_after_interval(self):
        item = self.create(self.aspi, None, 1)[0]
        self.assertEqual(item.revision_due, models.add_months(self.today, 3))
        code, body = self.call('GET', f'/api/items/{item.iid}/', local=False)
        self.assertTrue(body['revision'])
        self.assertEqual(body['revision_tasks'], ['Batterie chargée', 'Tuyau propre'])
        self.assertFalse(body['revision_late'])
        self.assertEqual(models.add_months(date(2026, 1, 31), 1), date(2026, 2, 28))

    def test_revise_requires_all_tasks_and_sets_next_due(self):
        item = self.create(self.aspi, None, 1)[0]
        code, body = self.call('POST', f'/api/items/{item.iid}/revision/', {'tasks': ['Batterie chargée']})
        self.assertEqual(code, 400)
        code, body = self.call('POST', f'/api/items/{item.iid}/revision/',
                               {'tasks': ['Batterie chargée', 'Tuyau propre']})
        self.assertEqual(code, 200)
        self.assertEqual(body['revision_due'], models.add_months(self.today, 3).isoformat())
        self.assertEqual(len(body['revisions']), 1)
        later = (self.today + timedelta(days=200)).isoformat()
        code, body = self.call('POST', f'/api/items/{item.iid}/revision/',
                               {'tasks': ['Batterie chargée', 'Tuyau propre'], 'next_due': later})
        self.assertEqual(body['revision_due'], later)
        code, body = self.call('POST', f'/api/items/{item.iid}/revision/',
                               {'tasks': ['Batterie chargée', 'Tuyau propre'], 'next_due': self.today.isoformat()})
        self.assertEqual(code, 400)
        code, _ = self.call('POST', f'/api/items/{item.iid}/revision/', {'tasks': []}, session=False)
        self.assertEqual(code, 403)
        garrot = self.create(self.garrot, None, 1)[0]
        code, _ = self.call('POST', f'/api/items/{garrot.iid}/revision/', {'tasks': []})
        self.assertEqual(code, 400)

    def test_late_revision_blocks_only_when_not_usable(self):
        item = self.create(self.aspi, None, 1)[0]
        Items.objects.filter(iid=item.iid).update(location=self.lot, revision_due=self.today - timedelta(days=1))
        item.refresh_from_db()
        self.assertTrue(item.revision_late())
        self.assertFalse(item.needs_replacement())
        code, lot = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertEqual(lot['revision_late_count'], 1)
        self.assertEqual(lot['expired_count'], 0)
        self.aspi.revision_usable = False
        self.aspi.save()
        item.refresh_from_db()
        self.assertTrue(item.needs_replacement())
        code, lot = self.call('GET', f'/api/lots/{self.lot.id}/', local=False)
        self.assertEqual(lot['expired_count'], 1)
        self.assertEqual(lot['next_revision'], (self.today - timedelta(days=1)).isoformat())

    def test_enabling_revision_dates_existing_items(self):
        ItemType.objects.filter(type='aspimu').update(revision_months=0)
        item = self.create(ItemType.objects.get(type='aspimu'), None, 1)[0]
        self.assertIsNone(item.revision_due)
        code, body = self.call('PATCH', '/api/item-types/aspimu/', {'revision_months': 6, 'revision_usable': False})
        self.assertEqual((body['revision_months'], body['revision_usable']), (6, False))
        item.refresh_from_db()
        self.assertEqual(item.revision_due, models.add_months(self.today, 6))

    def test_daily_revision_summary(self):
        from django.core.management import call_command
        late, soon, later = self.create(self.aspi, None, 3)
        Items.objects.filter(iid=late.iid).update(location=self.lot, revision_due=self.today - timedelta(days=2))
        Items.objects.filter(iid=soon.iid).update(revision_due=self.today + timedelta(days=3))
        call_command('check_alerts', stdout=StringIO())
        self.assertIn(('revision_daily', 'révisions à faire, en retard : Aspirateur (Sac A) ; cette semaine : '
                                         'Aspirateur (stock)'), self.sent)
