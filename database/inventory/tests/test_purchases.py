# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          tests/test_purchases.py
#       -\-    _|__
#        |\___/  . \        Created on 26 Nov. 2026 at 18:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Commandes fournisseurs : jours feries, saisie, reception, etats, statistiques, suggestions groupees."""

from __future__ import annotations

from datetime import date, timedelta
from typing import Any

from django.test import SimpleTestCase
from django.utils import timezone

from .base import ApiTestCase
from .. import holidays_fr
from ..models import Items, ItemType, StockOrder, Supplier, SupplierOrder


class HolidaysTests(SimpleTestCase):
    def test_easter_and_moving_holidays(self) -> None:
        self.assertEqual(holidays_fr.easter(2026), date(2026, 4, 5))
        self.assertEqual(holidays_fr.easter(2027), date(2027, 3, 28))
        days = holidays_fr.holidays(2026)
        self.assertEqual(days[date(2026, 4, 6)], 'Lundi de Pâques')
        self.assertEqual(days[date(2026, 5, 14)], 'Ascension')
        self.assertEqual(days[date(2026, 5, 25)], 'Lundi de Pentecôte')
        self.assertEqual(len(days), 11)

    def test_business_days(self) -> None:
        # vendredi 6 novembre -> jeudi 12 : lundi, mardi, (mercredi 11 ferie), jeudi
        self.assertEqual(holidays_fr.business_days_between(date(2026, 11, 6), date(2026, 11, 12)), 3)
        self.assertEqual(holidays_fr.business_days_between(date(2026, 11, 6), date(2026, 11, 9)), 1)
        self.assertEqual(holidays_fr.business_days_between(date(2026, 11, 9), date(2026, 11, 9)), 0)
        # jeudi 24 decembre + 1 jour ouvre : vendredi 25 ferie, week-end -> lundi 28
        self.assertEqual(holidays_fr.add_business_days(date(2026, 12, 24), 1), date(2026, 12, 28))
        self.assertEqual(holidays_fr.add_business_days(date(2026, 12, 24), 0), date(2026, 12, 24))


class PurchaseTests(ApiTestCase):
    def order(self, supplier: str = 'Medisafe', number: str = 'A1', lines: list[dict[str, Any]] | None = None,
              ordered: date | None = None, shipping: str = '8,50') -> tuple[int, Any]:
        return self.call('POST', '/api/purchases/orders/', {
            'supplier': supplier, 'number': number, 'ordered': (ordered or self.today).isoformat(),
            'shipping': shipping,
            'lines': lines if lines is not None else [{'type': 'compre', 'quantity': 20, 'unit_price': '0,12'},
                                                      {'type': 'garrot', 'quantity': 2, 'unit_price': 9}]})

    def receive(self, order: int, receptions: list[dict[str, Any]]) -> tuple[int, Any]:
        return self.call('POST', f'/api/purchases/orders/{order}/receive/', {'receptions': receptions})

    def test_create_and_supplier_suggestions(self) -> None:
        code, body = self.order()
        self.assertEqual(code, 201)
        self.assertEqual(body['label'], f"{self.today.strftime('%d/%m/%Y')} · Medisafe · n° A1")
        self.assertEqual(body['state'], 'waiting')
        self.assertEqual(body['shipping'], 8.5)
        self.assertEqual(body['items_total'], 20 * 0.12 + 18)
        self.assertEqual(body['total'], 20 * 0.12 + 18 + 8.5)
        # meme fournisseur sans casse ni accents : pas de doublon
        code, body = self.order(supplier='  médisafe ', number='A2')
        self.assertEqual(code, 201)
        self.assertEqual(body['supplier'], 'Medisafe')
        self.assertEqual(Supplier.objects.count(), 1)
        # meme date, meme fournisseur, meme numero : deja enregistree
        code, body = self.order(number='A1')
        self.assertEqual(code, 400)
        self.assertIn('déjà enregistrée', body['error'])
        code, body = self.call('GET', '/api/purchases/suppliers/')
        self.assertEqual([(row['name'], row['orders']) for row in body], [('Medisafe', 2)])
        # refus : type inconnu, sans ligne, personne de connecte
        self.assertEqual(self.order(number='B', lines=[{'type': 'zzzzzz', 'quantity': 1}])[0], 400)
        self.assertEqual(self.order(number='B', lines=[])[0], 400)
        code, _ = self.call('GET', '/api/purchases/orders/', session=False)
        self.assertEqual(code, 403)

    def test_order_hides_suggestion_until_received(self) -> None:
        tasks = lambda body: [row['type'] for row in body['tasks'] if row['category'] == 'orders' and 'quantity' in row]
        code, body = self.call('GET', '/api/tasks/')
        self.assertIn('compre', tasks(body))
        code, order = self.order(lines=[{'type': 'compre', 'quantity': 20, 'unit_price': '0.1'}])
        code, body = self.call('GET', '/api/tasks/')
        self.assertNotIn('compre', tasks(body))
        self.assertIn('Medisafe', body['orders_placed'][0]['supplier'])
        # une reception hors commande ne remplit pas la ligne de commande fournisseur
        self.create(self.compresses, self.today + timedelta(days=300), 20)
        code, body = self.call('GET', '/api/tasks/')
        self.assertEqual(body['orders_placed'][0]['received'], 0)
        # reception de la commande, en deux dates de peremption : la ligne est livree
        code, body = self.receive(order['id'], [
            {'line': order['lines'][0]['id'], 'count': 12, 'peremption': (self.today + timedelta(days=400)).isoformat()},
            {'line': order['lines'][0]['id'], 'count': 8, 'peremption': (self.today + timedelta(days=500)).isoformat()},
        ])
        self.assertEqual(code, 201)
        self.assertEqual(len(body['items']), 20)
        self.assertEqual([len(row['items']) for row in body['receptions']], [12, 8])
        self.assertEqual(body['order']['state'], 'received')
        self.assertEqual(body['order']['measured_delay'], 0)
        self.assertEqual(Items.objects.filter(order_line_id=order['lines'][0]['id']).count(), 20)
        self.assertIsNotNone(StockOrder.objects.get(pk=order['lines'][0]['id']).delivered)
        code, body = self.call('GET', '/api/tasks/')
        self.assertEqual(body['orders_placed'], [])

    def test_states_and_attention_first(self) -> None:
        old = self.today - timedelta(days=40)
        code, late = self.order(number='OLD', ordered=old)
        code, waiting = self.order(number='NEW')
        code, partial = self.order(number='PART')
        self.receive(partial['id'], [{'line': partial['lines'][1]['id'], 'count': 1}])
        code, body = self.call('GET', '/api/purchases/orders/')
        self.assertEqual([row['number'] for row in body], ['OLD', 'PART', 'NEW'])
        self.assertEqual([row['state'] for row in body], ['late', 'partial', 'waiting'])
        self.assertIn('relancer Medisafe', body[0]['detail'])
        self.assertEqual(body[1]['received'], 1)
        # le delai mesure (0 jour ouvre, recue le jour meme) sert d'estimation aux autres commandes du fournisseur
        self.assertEqual(body[2]['delay'], 0)
        # annuler une commande deja reçue en partie : refuse, la clore
        code, _ = self.call('POST', f"/api/purchases/orders/{partial['id']}/cancel/", {})
        self.assertEqual(code, 400)
        code, body = self.call('POST', f"/api/purchases/orders/{partial['id']}/close/", {})
        self.assertEqual(body['state'], 'closed')
        self.assertEqual(body['lines'][0]['remaining'], 0)
        # payee : seulement ce qui est venu
        self.assertEqual(body['items_total'], 9.0)
        code, body = self.call('POST', f"/api/purchases/orders/{partial['id']}/close/", {'reopen': True})
        self.assertIn(body['state'], ('partial', 'late'))
        code, body = self.call('POST', f"/api/purchases/orders/{waiting['id']}/cancel/", {})
        self.assertEqual(body['state'], 'cancelled')
        code, body = self.call('GET', '/api/purchases/orders/?open=1')
        self.assertEqual({row['number'] for row in body}, {'OLD', 'PART'})
        # ligne d'une commande en retard : tache « Commande pas livrée »
        code, body = self.call('GET', '/api/tasks/')
        self.assertTrue([row for row in body['tasks'] if 'pas livrée' in row['title'] and 'OLD' in row['detail']])

    def test_update_order(self) -> None:
        code, order = self.order()
        self.receive(order['id'], [{'line': order['lines'][1]['id'], 'count': 2}])
        lines = [{'id': order['lines'][0]['id'], 'type': 'compre', 'quantity': 30, 'unit_price': '0.1'},
                 {'id': order['lines'][1]['id'], 'type': 'garrot', 'quantity': 1, 'unit_price': 9}]
        data = {'supplier': 'Medisafe', 'number': 'A1-bis', 'shipping': 0, 'lines': lines}
        code, body = self.call('POST', f"/api/purchases/orders/{order['id']}/", data)
        self.assertEqual(code, 400)  # 2 garrots deja recus
        lines[1]['quantity'] = 2
        code, body = self.call('POST', f"/api/purchases/orders/{order['id']}/", data)
        self.assertEqual(code, 200)
        self.assertEqual((body['number'], body['lines'][0]['quantity'], body['shipping']), ('A1-bis', 30, 0))
        # une ligne recue ne peut pas disparaitre
        code, _ = self.call('POST', f"/api/purchases/orders/{order['id']}/", {**data, 'lines': lines[:1]})
        self.assertEqual(code, 400)

    def test_stats_and_grouped_suggestions(self) -> None:
        year_ago = self.today - timedelta(days=20)
        code, first = self.order(supplier='Medisafe', number='1', ordered=year_ago, shipping='8',
                                 lines=[{'type': 'compre', 'quantity': 10, 'unit_price': '0.20'},
                                        {'type': 'garrot', 'quantity': 2, 'unit_price': 10}])
        self.receive(first['id'], [
            {'line': first['lines'][0]['id'], 'count': 10, 'peremption': (self.today + timedelta(days=900)).isoformat()},
            {'line': first['lines'][1]['id'], 'count': 2}])
        code, second = self.order(supplier='Pharma+', number='2', ordered=self.today - timedelta(days=10),
                                  shipping='0', lines=[{'type': 'compre', 'quantity': 10, 'unit_price': '0.30'}])
        code, body = self.call('GET', '/api/purchases/stats/')
        self.assertEqual(code, 200)
        compresses = next(row for row in body['types'] if row['type'] == 'compre')
        self.assertEqual((compresses['last_price'], compresses['last_supplier']), (0.3, 'Pharma+'))
        self.assertEqual((compresses['best_price'], compresses['best_supplier']), (0.2, 'Medisafe'))
        self.assertEqual(compresses['change'], 50.0)
        self.assertEqual(body['year']['orders'], 2)
        self.assertEqual(body['year']['shipping'], 8.0)
        medisafe = next(row for row in body['suppliers'] if row['name'] == 'Medisafe')
        self.assertEqual(medisafe['delays'], 1)
        # 10 compresses (prix de leur ligne) + 2 garrots en stock
        self.assertEqual(body['stock']['value'], 10 * 0.2 + 2 * 10)
        self.assertEqual(body['stock']['unpriced'], 0)
        self.assertTrue(body['holidays'])
        # suggestions : sous le minimum ; la commande de compresses en attente chez Pharma+ cache sa suggestion
        ItemType.objects.filter(type='compre').update(min_quantity=50)
        ItemType.objects.filter(type='garrot').update(min_quantity=5)
        code, body = self.call('GET', '/api/purchases/suggestions/')
        suggested = {line['type']: group['supplier'] for group in body['groups'] for line in group['lines']}
        self.assertNotIn('compre', suggested)
        StockOrder.objects.filter(supplier_order_id=second['id']).update(cancelled=timezone.now())
        code, body = self.call('GET', '/api/purchases/suggestions/')
        groups = {group['supplier']: group for group in body['groups']}
        # le moins cher de l'annee : Medisafe, pour les deux types, une seule livraison (8 €)
        self.assertEqual({line['type'] for line in groups['Medisafe']['lines']}, {'compre', 'garrot'})
        self.assertEqual(groups['Medisafe']['shipping'], 8.0)
        self.assertIn('le moins cher', next(line['why'] for line in groups['Medisafe']['lines']
                                            if line['type'] == 'compre'))
