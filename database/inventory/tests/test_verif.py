"""Tests du back : vérif, places des items, vérifs recentes."""

from __future__ import annotations

from typing import TYPE_CHECKING

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)

if TYPE_CHECKING:
    from inventory.models import Items


class VerifTests(ApiTestCase):
    def test_items_to_check(self) -> None:
        """Item a cocher (gobelets) : sans etiquette ni stock, la vérif le coche a la main ; non coche, il manque
        jusqu'a la prochaine vérif. Sans "checked" (ancien front), rien ne change."""
        code, body = self.call('POST', '/api/item-types/', {'type': 'gobele', 'name': 'Gobelets', 'physical': False})
        self.assertEqual(code, 201)
        self.assertFalse(body['physical'])
        code, _ = self.call('POST', '/api/item-types/', {'type': 'sucres', 'name': 'Sucres', 'physical': False,
                                                         'perissable': True})
        self.assertEqual(code, 400)
        gobelets = ItemType.objects.get(type='gobele')
        with self.assertRaises(ValueError):
            self.create(gobelets, None, 5)
        LotRequirements.objects.create(lot_type=self.lot_type, item_type=gobelets, quantity=5)
        url = f'/api/lots/{self.lot.id}/'
        code, body = self.call('GET', url)
        row = next(row for row in body['requirements'] if row['type'] == 'gobele')
        self.assertTrue(row['check'])
        self.assertEqual((row['present'], row['required']), (5, 5))  # confiance entre deux vérifs
        fresh = [item.iid for item in self.create(self.compresses, self.today + timedelta(days=90), 2)]
        report = self.call('POST', f'{url}verif/', {'items': fresh, 'checked': {}, 'user': 'M001'})[1]
        self.assertFalse(report['complete'])
        self.assertEqual(Lots.objects.get(id=self.lot.id).unchecked, ['gobele'])
        row = next(row for row in report['requirements'] if row['type'] == 'gobele')
        self.assertEqual(row['present'], 0)
        # verif partielle : le lot n'est complet qu'avec ses items a cocher
        report = self.call('POST', f'{url}verif/', {'items': fresh, 'checked': {}, 'partial': True})[1]
        self.assertFalse(report['lots'][0]['verified'])
        report = self.call('POST', f'{url}verif/', {'items': fresh, 'checked': {self.lot.id: ['gobele']},
                                                    'partial': True})[1]
        self.assertTrue(report['complete'])
        self.assertEqual(Lots.objects.get(id=self.lot.id).unchecked, [])
        # ancien front : les items a cocher gardent leur etat
        Lots.objects.filter(id=self.lot.id).update(unchecked=['gobele'])
        self.call('POST', f'{url}verif/', {'items': fresh})
        self.assertEqual(Lots.objects.get(id=self.lot.id).unchecked, ['gobele'])
        # pas dans l'etat des stocks ; un type qui a deja des etiquettes ne devient pas un item a cocher
        self.assertNotIn('gobele', [row['type'] for row in services.stock_status()])
        code, _ = self.call('PATCH', '/api/item-types/compre/', {'physical': False})
        self.assertEqual(code, 400)

    def test_verif_replacement_and_missing(self) -> None:
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

    def test_expired_not_replaced_stays(self) -> None:
        old = self.create(self.compresses, self.today - timedelta(days=1), 1)[0]
        Items.objects.filter(iid=old.iid).update(location=self.lot)
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [old.iid]})
        self.assertEqual(report['expired'], [old.iid])
        self.assertFalse(report['complete'])

    def test_expired_extra_with_fresh_already_in_lot(self) -> None:
        # le lot a deja son minimum de compresses fraiches : le perime retrouve est retire (a jeter), vérif verte
        old = self.create(self.compresses, self.today - timedelta(days=1), 1)[0]
        fresh = self.create(self.compresses, self.today + timedelta(days=30), 2)
        Items.objects.filter(iid__in=[old.iid] + [item.iid for item in fresh]).update(location=self.lot)
        code, body = self.call('GET', f'/api/lots/{self.lot.id}/')
        self.assertTrue(body['complete'])  # un perime en trop ne rend pas le lot incomplet
        self.assertEqual(body['expired_count'], 1)
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/',
                                 {'items': [old.iid] + [item.iid for item in fresh]})
        self.assertEqual(code, 200)
        self.assertTrue(report['complete'])
        self.assertEqual((report['replaced'], report['expired']), ([old.iid], []))
        old.refresh_from_db()
        self.assertEqual((old.status, old.location_id), (ItemStatus.REPLACED, None))
        # verif partielle : le lot est valide lui aussi
        other = self.create(self.compresses, self.today - timedelta(days=2), 1)[0]
        Items.objects.filter(iid=other.iid).update(location=self.lot)
        code, report = self.call('POST', '/api/verifs/', {'lots': [self.lot.id], 'partial': True,
                                                          'items': [other.iid] + [item.iid for item in fresh]})
        self.assertEqual(code, 200)
        self.assertTrue(report['complete'])
        self.assertEqual(report['replaced'], [other.iid])

    def test_expired_short_lot_stays_to_replace(self) -> None:
        # une seule compresse fraiche sur deux : le perime reste dans le lot, a remplacer, vérif incomplete
        old = self.create(self.compresses, self.today - timedelta(days=1), 1)[0]
        fresh = self.create(self.compresses, self.today + timedelta(days=30), 1)[0]
        Items.objects.filter(iid__in=[old.iid, fresh.iid]).update(location=self.lot)
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [old.iid, fresh.iid]})
        self.assertFalse(report['complete'])
        self.assertEqual((report['replaced'], report['expired']), ([], [old.iid]))
        self.assertEqual(Items.objects.get(iid=old.iid).location_id, self.lot.id)

    def test_expired_from_elsewhere_is_discarded(self) -> None:
        # perime scanne qui n'etait pas dans le lot (stock) : jamais range dans le lot, a jeter, meme lot court
        stray = self.create(self.compresses, self.today - timedelta(days=1), 1)[0]
        fresh = self.create(self.compresses, self.today + timedelta(days=30), 1)[0]
        Items.objects.filter(iid=fresh.iid).update(location=self.lot)
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [stray.iid, fresh.iid]})
        self.assertEqual(code, 200)
        self.assertEqual((report['replaced'], report['expired']), ([stray.iid], []))
        stray.refresh_from_db()
        self.assertEqual((stray.status, stray.location_id), (ItemStatus.REPLACED, None))
        # lot complet : rien de rouge, le perime venu d'ailleurs est seulement a jeter
        more = self.create(self.compresses, self.today + timedelta(days=30), 1)[0]
        other = self.create(self.compresses, self.today - timedelta(days=3), 1)[0]
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/',
                                 {'items': [other.iid, fresh.iid, more.iid]})
        self.assertTrue(report['complete'])
        self.assertEqual((report['replaced'], report['expired']), ([other.iid], []))
        verif = Verifs.objects.filter(lot=self.lot).latest('id')
        self.assertEqual((verif.complete, verif.expired_count, verif.replaced_count), (True, 0, 1))

    def test_stock_expiring_soon_split(self) -> None:
        self.create(self.compresses, self.today + timedelta(days=10), 2)
        self.create(self.compresses, self.today + timedelta(days=200), 1)
        in_lot = self.create(self.compresses, self.today + timedelta(days=10), 1)[0]
        Items.objects.filter(iid=in_lot.iid).update(location=self.lot)
        self.create(self.compresses, self.today - timedelta(days=1), 1)
        code, stock = self.call('GET', '/api/stock/')
        row = next(row for row in stock if row['type'] == 'compre')
        self.assertEqual((row['stock_fresh'], row['stock_expiring_soon']), (3, 2))
        self.assertEqual((row['lots_fresh'], row['lots_expiring_soon']), (1, 1))

    def test_add_items_and_back_to_stock(self) -> None:
        items = self.create(self.garrot, None, 2)
        iids = [item.iid for item in items]
        code, body = self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': iids})
        self.assertEqual(body['moved'], iids)
        self.assertEqual(Items.objects.filter(location=self.lot).count(), 2)
        code, body = self.call('POST', '/api/items/to-stock/', {'items': iids[:1]})
        self.assertEqual(Items.objects.filter(location=self.lot).count(), 1)


class VerifPlacesTests(ApiTestCase):
    """Verif de sous-lots : un item venu du stock va dans le lot dont l'etiquette a ete scannee en dernier."""

    def setUp(self) -> None:
        super().setUp()
        vps_type = LotType.objects.create(type='vpsvps', name='VPS')
        self.vps = Lots(lot_type=vps_type, name='VPS', name_short='V', created_by='test')
        self.vps.save()
        self.armoire1 = Lots(lot_type=self.lot_type, name='Armoire 1', name_short='A1', created_by='test', parent=self.vps)
        self.armoire1.save()
        self.armoire2 = Lots(lot_type=self.lot_type, name='Armoire 2', name_short='A2', created_by='test', parent=self.vps)
        self.armoire2.save()

    def location(self, item: Items) -> str | None:
        return Items.objects.get(iid=item.iid).location_id

    def test_stock_item_goes_to_last_scanned_lot(self) -> None:
        stock = self.create(self.compresses, self.today + timedelta(days=100), 2)
        code, _ = self.call('POST', '/api/verifs/', {
            'lots': [{'id': self.vps.id}], 'items': [item.iid for item in stock],
            'places': {stock[0].iid: self.armoire2.id, stock[1].iid: self.vps.id}})
        self.assertEqual(code, 200)
        # armoire 1 attendait aussi des compresses, mais c'est l'armoire 2 qui a ete scannee
        self.assertEqual(self.location(stock[0]), self.armoire2.id)
        # etiquette du VPS : un de ses sous-lots qui en attend encore
        self.assertEqual(self.location(stock[1]), self.armoire1.id)

    def test_item_of_a_sibling_lot_moves_unless_its_label_was_scanned(self) -> None:
        found = self.create(self.compresses, self.today + timedelta(days=100), 2)
        self.call('POST', f'/api/lots/{self.armoire2.id}/add/', {'items': [item.iid for item in found]})
        # vérif de l'armoire 1 seule : l'item de l'armoire 2 y est deplace
        self.call('POST', '/api/verifs/', {'lots': [{'id': self.armoire1.id}], 'items': [found[0].iid],
                                           'places': {found[0].iid: self.armoire1.id}})
        self.assertEqual(self.location(found[0]), self.armoire1.id)
        # armoire 2 ajoutee a la vérif (son etiquette scannee) : son item y a bien ete trouve
        self.call('POST', '/api/verifs/', {'lots': [{'id': self.armoire1.id}, {'id': self.armoire2.id}],
                                           'items': [found[1].iid], 'places': {found[1].iid: self.armoire1.id}})
        self.assertEqual(self.location(found[1]), self.armoire2.id)


class RecentVerifsTests(ApiTestCase):
    def test_recent_verifs_need_a_badge_and_group_lots(self) -> None:
        sub = Lots(lot_type=self.lot_type, name='Sac A1', name_short='A1', created_by='test', parent=self.lot)
        sub.save()
        items = self.create(self.compresses, self.today + timedelta(days=200), 4)
        self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [item.iid for item in items[:2]]})
        self.call('POST', f'/api/lots/{sub.id}/add/', {'items': [item.iid for item in items[2:]]})
        self.call('POST', '/api/verifs/', {'lots': [{'id': self.lot.id}], 'items': [item.iid for item in items]})
        self.call('POST', f'/api/lots/{sub.id}/verif/', {'items': [items[2].iid]})
        code, _ = self.call('POST', '/api/verifs/recent/', {}, local=False)
        self.assertEqual(code, 403)
        badge = {'matricule': self.user.matricule, 'key': self.user.new_key}
        code, rows = self.call('POST', '/api/verifs/recent/', {'user': badge}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(len(rows), 2)
        latest, grouped = rows
        self.assertEqual([lot['id'] for lot in latest['lots']], [sub.id])
        self.assertFalse(latest['complete'])
        # verif groupee (lot et sous-lot au meme instant) : une seule ligne
        self.assertEqual({lot['id'] for lot in grouped['lots']}, {self.lot.id, sub.id})
        self.assertEqual(grouped['present'], 4)
        self.assertEqual(grouped['by'], 'Admin')  # nom court de l'API publique (prenom seul, pas d'homonyme)

    def test_offline_verif_dated_at_scan_time(self) -> None:
        items = self.create(self.compresses, self.today + timedelta(days=200), 2)
        self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [item.iid for item in items]})
        badge = {'matricule': self.user.matricule, 'key': self.user.new_key}
        # faite hors connexion il y a 3 h d'apres l'appareil, dont l'horloge avance d'un jour : seul compte l'ecart
        # entre la validation et l'envoi
        skew = timedelta(days=1)
        device_now = timezone.now() + skew
        code, _ = self.call('POST', '/api/verifs/', {
            'lots': [{'id': self.lot.id}], 'items': [item.iid for item in items],
            'done_at': (device_now - timedelta(hours=3)).isoformat(), 'sent_at': device_now.isoformat()})
        self.assertEqual(code, 200)
        verif = Verifs.objects.get(lot=self.lot)
        self.assertAlmostEqual((verif.received - verif.datetime).total_seconds(), 3 * 3600, delta=60)
        self.lot.refresh_from_db()
        self.assertEqual(self.lot.last_verif, verif.datetime)
        operation = Operation.objects.filter(kind=OperationKind.VERIF, lot=self.lot).latest('id')
        self.assertEqual(operation.at, verif.datetime)
        _, rows = self.call('POST', '/api/verifs/recent/', {'user': badge}, local=False)
        self.assertEqual(rows[0]['at'], ser._date(verif.datetime))
        self.assertEqual(rows[0]['sent'], ser._date(verif.received))
        _, report = self.call('POST', f'/api/verifs/{verif.id}/report/', {'user': badge}, local=False)
        self.assertEqual(report['sent'], ser._date(verif.received))
        self.assertEqual(report['lots'][0]['present_count'], 2)
        # vérif plus récente faite en ligne, puis une vieille vérif hors connexion du même lot : enregistrée à son
        # heure, sans ramener la dernière vérif du lot en arrière
        self.call('POST', '/api/verifs/', {'lots': [{'id': self.lot.id}], 'items': [item.iid for item in items]})
        self.lot.refresh_from_db()
        latest = self.lot.last_verif
        self.call('POST', '/api/verifs/', {
            'lots': [{'id': self.lot.id}], 'items': [items[0].iid],
            'done_at': (timezone.now() - timedelta(hours=1)).isoformat(), 'sent_at': timezone.now().isoformat()})
        self.lot.refresh_from_db()
        self.assertEqual(self.lot.last_verif, latest)
        self.assertEqual(Verifs.objects.filter(lot=self.lot).count(), 3)

    def test_verif_sent_right_away_keeps_reception_time(self) -> None:
        items = self.create(self.compresses, self.today + timedelta(days=200), 1)
        self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [items[0].iid]})
        badge = {'matricule': self.user.matricule, 'key': self.user.new_key}
        now = timezone.now()
        # envoyee tout de suite (ecart sous VERIF_SENT_GAP), heure de l'appareil dans le futur, ou valeurs illisibles :
        # datee de sa reception, sans heure d'envoi montree
        for done, sent in ((now - timedelta(seconds=5), now), (now + timedelta(hours=2), now), ('hier', 'demain')):
            before = timezone.now()
            self.call('POST', '/api/verifs/', {
                'lots': [{'id': self.lot.id}], 'items': [items[0].iid],
                'done_at': done if isinstance(done, str) else done.isoformat(),
                'sent_at': sent if isinstance(sent, str) else sent.isoformat()})
            verif = Verifs.objects.filter(lot=self.lot).latest('id')
            self.assertGreaterEqual(verif.datetime, before)
            self.assertIsNone(ser.verif_sent(verif))
        _, rows = self.call('POST', '/api/verifs/recent/', {'user': badge}, local=False)
        self.assertIsNone(rows[0]['sent'])

    def test_recent_verifs_join_the_steps_of_a_cascade(self) -> None:
        sub = Lots(lot_type=self.lot_type, name='Sac A1', name_short='A1', created_by='test', parent=self.lot)
        sub.save()
        other = Lots(lot_type=self.lot_type, name='Trousse', name_short='T', created_by='test')
        other.save()
        items = self.create(self.compresses, self.today + timedelta(days=200), 3)
        self.call('POST', f'/api/lots/{sub.id}/add/', {'items': [items[0].iid]})
        self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [items[1].iid]})
        self.call('POST', f'/api/lots/{other.id}/add/', {'items': [items[2].iid]})
        # cascade : le sac valide a part, puis le lot du haut ; une autre verif ensuite sur un autre lot
        self.call('POST', '/api/verifs/', {'lots': [{'id': sub.id}], 'items': [items[0].iid]})
        self.call('POST', '/api/verifs/', {'lots': [{'id': self.lot.id}], 'items': [items[1].iid], 'exclude': [sub.id]})
        self.call('POST', '/api/verifs/', {'lots': [{'id': other.id}], 'items': [items[2].iid]})
        badge = {'matricule': self.user.matricule, 'key': self.user.new_key}
        _, rows = self.call('POST', '/api/verifs/recent/', {'user': badge}, local=False)
        self.assertEqual(len(rows), 2)
        trousse, cascade = rows
        self.assertEqual([lot['id'] for lot in trousse['top']], [other.id])
        self.assertEqual({lot['id'] for lot in cascade['lots']}, {self.lot.id, sub.id})
        self.assertEqual([lot['id'] for lot in cascade['top']], [self.lot.id])
        self.assertEqual(len(cascade['ids']), 2)
        self.assertEqual(cascade['present'], 2)
        # une nouvelle vérif du sac juste après : une autre ligne, le sac est déjà dans la cascade
        self.call('POST', '/api/verifs/', {'lots': [{'id': sub.id}], 'items': [items[0].iid]})
        _, rows = self.call('POST', '/api/verifs/recent/', {'user': badge}, local=False)
        self.assertEqual([[lot['id'] for lot in row['top']] for row in rows], [[sub.id], [other.id], [self.lot.id]])
        # étape plus vieille que CASCADE_GAP : plus dans la même vérif
        first_step = Verifs.objects.filter(lot=sub).order_by('id').first()
        Verifs.objects.filter(id=first_step.id).update(datetime=timezone.now() - timedelta(hours=2))
        _, rows = self.call('POST', '/api/verifs/recent/', {'user': badge}, local=False)
        self.assertEqual([row['ids'] for row in rows][-1], [first_step.id])
        self.assertEqual(len(rows), 4)

    def test_recent_verifs_name_sub_lots_unless_all_there(self) -> None:
        storage_type = LotType.objects.create(type='armoir', name='Armoire', storage=True)
        stock = Lots.stock()
        armoires = []
        for name in ('Armoire serum', 'Armoire bandes'):
            armoire = Lots(lot_type=storage_type, name=name, name_short=name[-3:], created_by='test', parent=stock)
            armoire.save()
            armoires.append(armoire)
        sacs = []
        for name in ('Sac de soin', 'Sac O2'):
            sac = Lots(lot_type=self.lot_type, name=name, name_short=name[-2:], created_by='test', parent=self.lot)
            sac.save()
            sacs.append(sac)
        casque = Lots(lot_type=self.lot_type, name='Casques', name_short='C', created_by='test', parent=self.lot,
                      optional=True)
        casque.save()
        badge = {'matricule': self.user.matricule, 'key': self.user.new_key}

        def names(*lots: tuple[Lots, int], minutes: int = 0) -> list[str]:
            Verifs.objects.all().delete()
            at = timezone.now() - timedelta(minutes=minutes)
            for lot, present in lots:
                Verifs.objects.create(lot=lot, by='test', datetime=at, complete=True, present_count=present)
            _, rows = self.call('POST', '/api/verifs/recent/', {'user': badge}, local=False)
            return [lot['name'] for lot in rows[0]['top']]

        # Stock et un seul de ses rangements : le nom du rangement, pas « Stock + 1 sous-lot »
        self.assertEqual(names((stock, 0), (armoires[0], 18)), ['Armoire serum'])
        # les deux rangements : « Stock »
        self.assertEqual(names((stock, 0), (armoires[0], 18), (armoires[1], 3)), ['Stock'])
        # deux rangements sans le Stock : leurs noms
        self.assertEqual(set(names((armoires[0], 18), (armoires[1], 3))), {'Armoire serum', 'Armoire bandes'})
        # le Stock seul
        self.assertEqual(names((stock, 0)), ['Stock'])
        # les deux sacs (le facultatif n'est pas exigé) avec le lot : le lot
        self.assertEqual(names((self.lot, 0), (sacs[0], 5), (sacs[1], 2)), [self.lot.name])
        # un sac seul avec le lot sans contenu à lui : le sac ; avec du contenu à lui : les deux
        self.assertEqual(names((self.lot, 0), (sacs[0], 5)), ['Sac de soin'])
        self.assertEqual(set(names((self.lot, 4), (sacs[0], 5))), {self.lot.name, 'Sac de soin'})
        # carte terminée du suivi en direct (cascade : une vérif par étape) : nom et compte sans le Stock cadre
        Verifs.objects.all().delete()
        first = Verifs.objects.create(lot=armoires[0], by='test', complete=True, present_count=15)
        second = Verifs.objects.create(lot=stock, by='test', complete=True,
                                       datetime=timezone.now() + timedelta(seconds=5))
        self.assertEqual(services.VerifTitles().for_verifs([first.id, second.id]), (
            [{'id': armoires[0].id, 'name': 'Armoire serum', 'complete': True, 'parent': stock.id}], 1))
        # compte rendu : sous-lots attendus de chaque lot, pour que le front écarte le Stock cadre
        _, report = self.call('POST', f'/api/verifs/{second.id}/report/', {'user': badge}, local=False)
        self.assertEqual(set(report['lots'][0]['children']), {armoires[0].id, armoires[1].id})

    def test_verif_report_lists_each_lot_of_a_grouped_verif(self) -> None:
        sub = Lots(lot_type=self.lot_type, name='Sac A1', name_short='A1', created_by='test', parent=self.lot)
        sub.save()
        items = self.create(self.compresses, self.today + timedelta(days=200), 4)
        self.call('POST', f'/api/lots/{self.lot.id}/add/', {'items': [item.iid for item in items[:2]]})
        self.call('POST', f'/api/lots/{sub.id}/add/', {'items': [item.iid for item in items[2:]]})
        self.call('POST', '/api/verifs/', {'lots': [{'id': self.lot.id}], 'items': [item.iid for item in items[:3]]})
        badge = {'matricule': self.user.matricule, 'key': self.user.new_key}
        _, rows = self.call('POST', '/api/verifs/recent/', {'user': badge}, local=False)
        code, _ = self.call('POST', f'/api/verifs/{rows[0]["id"]}/report/', {}, local=False)
        self.assertEqual(code, 403)
        code, report = self.call('POST', f'/api/verifs/{rows[0]["id"]}/report/', {'user': badge}, local=False)
        self.assertEqual(code, 200)
        self.assertEqual(report['by'], 'Admin')
        lots = {lot['id']: lot for lot in report['lots']}
        self.assertEqual(set(lots), {self.lot.id, sub.id})
        self.assertEqual((lots[self.lot.id]['depth'], lots[sub.id]['depth']), (0, 1))
        self.assertEqual(lots[sub.id]['parent'], self.lot.id)
        self.assertEqual(lots[sub.id]['present_count'], 1)
        self.assertEqual(lots[sub.id]['missing_count'], 1)
        self.assertEqual([(row['type_name'], row['count']) for row in lots[sub.id]['missing']],
                         [(self.compresses.name, 1)])
        self.assertEqual(sum(row['count'] for row in lots[self.lot.id]['present']), 2)
        code, _ = self.call('POST', '/api/verifs/999999/report/', {'user': badge}, local=False)
        self.assertEqual(code, 404)

    def test_live_names_are_anonymized(self) -> None:
        from ..live import public_name
        Secouristes.objects.create(matricule='M002', nom='Durand', prenom='Jeanne')
        self.assertEqual(public_name('M:M001'), 'Jeanne Dup.')
        self.assertEqual(public_name('D:Paul Martin'), 'Paul M.')
