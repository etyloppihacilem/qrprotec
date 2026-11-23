"""Tests du back : annulation des operations du journal (undo.py)."""

from __future__ import annotations

from typing import TYPE_CHECKING

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)
from ..models import OperationChange, ProblemReport

if TYPE_CHECKING:
    from inventory.models import Operation


@override_settings(QRPROTEC={**settings.QRPROTEC, 'NOTIFY_SYNC': True})
class UndoTests(ApiTestCase):
    def setUp(self) -> None:
        super().setUp()
        from .. import webpush
        original = webpush.notify_event
        webpush.notify_event = lambda *args, **kwargs: 1
        self.addCleanup(setattr, webpush, 'notify_event', original)
        self.badge = {'matricule': 'M001', 'key': self.user.new_key}
        self.items = self.create(self.compresses, self.today + timedelta(days=60), 3)
        Items.objects.filter(iid__in=[item.iid for item in self.items]).update(location=self.lot)

    def last(self, kind: str) -> Operation:
        return Operation.objects.filter(kind=kind).latest('id')

    def cancel(self, operation: Operation, reason: str = 'Erreur de saisie', **kwargs: Any) -> tuple[int, Any]:
        return self.call('POST', f'/api/operations/{operation.id}/cancel/', {'reason': reason}, **kwargs)

    def verif(self, iids: list[str]) -> dict[str, Any]:
        code, report = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': iids})
        self.assertEqual(code, 200, report)
        return report

    def test_mark_used_cancelled_then_restored(self) -> None:
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/used/', {'user': self.badge}, local=False)
        self.assertEqual(code, 200)
        self.lot.refresh_from_db()
        self.assertTrue(self.lot.verif_recommended)
        self.assertEqual(self.lot.outings, 1)
        used = self.last('lot_used')
        self.assertIsNotNone(used.batch)

        code, body = self.call('GET', f'/api/operations/{used.id}/cancel/')
        self.assertEqual(code, 200, body)
        self.assertEqual((body['together'], body['dependents']), ([], []))
        code, line = self.cancel(used, 'Bouton appuyé par erreur')
        self.assertEqual(code, 200, line)
        self.assertEqual(line['kind'], 'cancel')
        self.assertIn('Bouton appuyé par erreur', line['summary'])
        self.lot.refresh_from_db()
        self.assertFalse(self.lot.verif_recommended)
        self.assertIsNone(self.lot.marked_used)
        self.assertEqual((self.lot.outings, self.lot.last_used_by), (0, ''))
        used.refresh_from_db()
        self.assertEqual(used.cancelled_by_id, line['id'])
        self.assertTrue(self.cancel(used)[0] == 400)  # deja annulee

        # le journal marque la ligne annulee, avec le motif
        code, body = self.call('GET', '/api/operations/')
        rows = {row['id']: row for row in body['operations']}
        self.assertEqual(rows[used.id]['cancelled']['reason'], 'Bouton appuyé par erreur')
        self.assertTrue(rows[used.id]['undoable'])
        self.assertTrue(rows[line['id']]['undoable'])

        # annuler l'annulation retablit l'operation
        cancel_line = Operation.objects.get(id=line['id'])
        code, again = self.cancel(cancel_line, 'Finalement utilisé')
        self.assertEqual(code, 200, again)
        self.assertTrue(again['summary'].startswith('Annulation annulée'))
        self.lot.refresh_from_db()
        self.assertTrue(self.lot.verif_recommended)
        self.assertEqual(self.lot.outings, 1)
        used.refresh_from_db()
        cancel_line.refresh_from_db()
        self.assertIsNone(used.cancelled_by_id)
        self.assertEqual(cancel_line.cancelled_by_id, again['id'])
        # et l'operation peut de nouveau etre annulee
        code, _ = self.cancel(used, 'Vraiment une erreur')
        self.assertEqual(code, 200)
        self.lot.refresh_from_db()
        self.assertFalse(self.lot.verif_recommended)

    def test_verif_cancelled_restores_lot_and_items(self) -> None:
        self.call('POST', f'/api/lots/{self.lot.id}/used/', {'user': self.badge}, local=False)
        missing = self.items[2]
        report = self.verif([item.iid for item in self.items[:2]])
        verif = self.last('verif')
        self.lot.refresh_from_db()
        self.assertFalse(self.lot.verif_recommended)
        self.assertIsNotNone(self.lot.last_verif)
        self.assertTrue(Verifs.objects.filter(id=report['verif_id']).exists())
        self.assertEqual(Items.objects.get(iid=missing.iid).missed_verifs, 1)

        code, line = self.cancel(verif, 'Vérif bâclée')
        self.assertEqual(code, 200, line)
        self.lot.refresh_from_db()
        self.assertTrue(self.lot.verif_recommended)  # utilise, toujours a verifier
        self.assertIsNone(self.lot.last_verif)
        self.assertFalse(Verifs.objects.filter(id=report['verif_id']).exists())
        item = Items.objects.get(iid=missing.iid)
        self.assertEqual((item.missed_verifs, item.location_id), (0, self.lot.id))

    def test_verif_with_restock_cancelled(self) -> None:
        fresh = self.create(self.compresses, self.today + timedelta(days=90), 1)[0]  # en stock
        self.verif([self.items[0].iid, self.items[1].iid, fresh.iid])
        self.assertEqual(Items.objects.get(iid=fresh.iid).location_id, self.lot.id)
        code, _ = self.cancel(self.last('verif'))
        self.assertEqual(code, 200)
        self.assertIsNone(Items.objects.get(iid=fresh.iid).location_id)  # rendu au stock
        self.assertTrue(all(op.cancelled_by_id for op in Operation.objects.exclude(kind='cancel')))

    def test_later_verif_kept(self) -> None:
        self.verif([item.iid for item in self.items[:2]])
        first = self.last('verif')
        report = self.verif([item.iid for item in self.items])
        self.lot.refresh_from_db()
        second_at = self.lot.last_verif
        code, line = self.cancel(first)
        self.assertEqual(code, 200, line)
        self.lot.refresh_from_db()
        self.assertEqual(self.lot.last_verif, second_at)  # la vérif d'apres reste
        self.assertTrue(Verifs.objects.filter(id=report['verif_id']).exists())
        self.assertEqual(Items.objects.get(iid=self.items[2].iid).missed_verifs, 0)
        self.assertIsNone(self.last('verif').cancelled_by_id)

    def test_report_cancel_takes_replies(self) -> None:
        code, body = self.call('POST', '/api/reports/', {'user': self.badge, 'item': self.items[0].iid,
                                                          'usable': False, 'comment': 'Déchiré'}, local=False)
        self.assertEqual(code, 201, body)
        report_id = body['report']['id']
        created = self.last('report')
        code, _ = self.call('POST', f'/api/reports/{report_id}/', {'status': 'progress', 'note': 'Commandé'})
        self.assertEqual(code, 200)
        reply = self.last('report')
        self.assertNotEqual(reply.id, created.id)
        self.assertTrue(Items.objects.get(iid=self.items[0].iid).unusable)

        code, body = self.call('GET', f'/api/operations/{created.id}/cancel/')
        self.assertEqual([row['id'] for row in body['dependents']], [reply.id])
        code, line = self.cancel(created, 'Signalement en double')
        self.assertEqual(code, 200, line)
        self.assertIn('qui en dépendait', line['summary'])
        self.assertFalse(ProblemReport.objects.filter(id=report_id).exists())
        item = Items.objects.get(iid=self.items[0].iid)
        self.assertFalse(item.reported or item.unusable)
        reply.refresh_from_db()
        self.assertEqual(reply.cancelled_by_id, line['id'])

        # annuler l'annulation remet le signalement et sa reponse
        code, _ = self.cancel(Operation.objects.get(id=line['id']), 'Pas un doublon')
        self.assertEqual(code, 200)
        report = ProblemReport.objects.get(id=report_id)
        self.assertEqual((report.status, report.note), ('progress', 'Commandé'))
        self.assertTrue(Items.objects.get(iid=self.items[0].iid).unusable)

    def test_rules(self) -> None:
        self.call('POST', f'/api/lots/{self.lot.id}/used/', {'user': self.badge}, local=False)
        used = self.last('lot_used')
        code, _ = self.cancel(used, '  ')
        self.assertEqual(code, 400)  # motif obligatoire
        manager = Secouristes(matricule='G001', nom='Gest', prenom='Ion', role='gestion')
        manager.renew_key()
        manager.save()
        code, _ = self.cancel(used, session=views.session_token(manager))
        self.assertEqual(code, 403)  # admins seulement
        code, _ = self.cancel(used, local=False)
        self.assertEqual(code, 404)  # route du poste
        old = Operation.objects.create(kind='lot_used', by='M:M001', summary='avant')
        code, body = self.cancel(old)
        self.assertEqual(code, 400)
        self.assertIn('avant', body['error'])
        # une requete qui ne fait que lire ne garde rien
        count = OperationChange.objects.count()
        self.call('POST', '/api/lots/summary/', {}, local=False)
        self.assertEqual(OperationChange.objects.count(), count)


@override_settings(QRPROTEC={**settings.QRPROTEC, 'NOTIFY_SYNC': True})
class GuestActivationUndoTests(ApiTestCase):
    """Annuler l'activation d'une carte d'invite annule tout ce que l'invite a fait ensuite, jusqu'a la reactivation."""

    def setUp(self) -> None:
        super().setUp()
        from .. import webpush
        original = webpush.notify_event
        webpush.notify_event = lambda *args, **kwargs: 1
        self.addCleanup(setattr, webpush, 'notify_event', original)

    def test_cancel_activation_cancels_guest_actions(self) -> None:
        from urllib.parse import parse_qs, urlparse
        code, body = self.call('POST', '/api/users/guests/', {'count': 1})
        query = parse_qs(urlparse(body['cards'][0]['badge_url']).query)
        matricule, key = query['m'][0], query['key'][0]
        code, _ = self.call('POST', '/api/auth/', {'matricule': matricule, 'key': key, 'guest_nom': 'Martin',
                                                   'guest_prenom': 'Léa', 'guest_antenne': 'Lyon 3'}, local=False)
        self.assertEqual(code, 200)
        badge = {'matricule': matricule, 'key': key}
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/used/', {'user': badge}, local=False)
        self.assertEqual(code, 200)
        used = Operation.objects.filter(kind='lot_used').latest('id')
        self.assertEqual(used.by, f'M:{matricule}')
        # une operation d'un autre secouriste ne depend pas de l'invite
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/used/',
                            {'user': {'matricule': 'M001', 'key': self.user.new_key}}, local=False)
        other = Operation.objects.filter(kind='lot_used').latest('id')
        activation = Operation.objects.filter(kind='user', details__guest_activation=True).latest('id')
        code, body = self.call('GET', f'/api/operations/{activation.id}/cancel/')
        self.assertEqual(code, 200, body)
        self.assertIn(used.id, [row['id'] for row in body['dependents']])
        self.assertNotIn(other.id, [row['id'] for row in body['dependents']])
        code, line = self.call('POST', f'/api/operations/{activation.id}/cancel/', {'reason': 'Carte prêtée par erreur'})
        self.assertEqual(code, 200, line)
        used.refresh_from_db()
        other.refresh_from_db()
        self.assertIsNotNone(used.cancelled_by_id)
        self.assertIsNone(other.cancelled_by_id)
        card = Secouristes.objects.get(matricule=matricule)
        self.assertIsNone(card.guest_activated)  # de nouveau a activer
        self.assertEqual((card.nom, card.prenom, card.guest_antenna), ('', '', ''))
