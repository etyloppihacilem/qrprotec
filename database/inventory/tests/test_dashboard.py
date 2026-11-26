# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          tests/test_dashboard.py
#       -\-    _|__
#        |\___/  . \        Created on 26 Nov. 2026 at 21:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Tableau de bord de la TECH : droits, score de réassort, items en plus, sections communes au web et au poste."""

from __future__ import annotations

import json
from datetime import timedelta
from typing import Any

from .base import ApiTestCase, setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)
from ..models import Items


class DashboardTests(ApiTestCase):
    def add(self, items: list[Items]) -> None:
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/add/',
                            {'items': [item.iid for item in items], 'key': self.lot.verif_key,
                             'user': {'matricule': 'M001', 'key': self.user.new_key}}, local=False)
        self.assertEqual(code, 200)

    def verif(self, items: list[Items]) -> None:
        code, _ = self.call('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid for item in items],
                                                                        'user': 'M001'})
        self.assertEqual(code, 200)

    @staticmethod
    def part(body: dict[str, Any], key: str) -> dict[str, Any]:
        return next(section for section in body['sections'] if section['key'] == key)

    def test_rights_and_sections(self) -> None:
        web = {'user': {'matricule': 'M001', 'key': self.user.new_key}}
        self.assertEqual(self.call('POST', '/api/dashboard/summary/', web, local=False)[0], 403)
        self.user.role = 'gestion'
        self.user.save()
        code, body = self.call('POST', '/api/dashboard/summary/', web, local=False)
        self.assertEqual(code, 200, body)
        self.assertEqual([part['key'] for part in body['sections']],
                         ['lots', 'restock', 'consumption', 'spending', 'reports', 'lists', 'labels', 'software', 'fun'])
        for part in body['sections']:
            for chart in part['charts']:
                self.assertEqual(len(chart['labels']), 12)
                for series in chart['series']:
                    self.assertEqual(len(series['values']), 12)
        self.assertNotIn('Dupont', json.dumps(body))  # web : jamais de nom de famille
        # lot jamais verifie : rouge, dans les alertes
        lots = self.part(body, 'lots')
        self.assertEqual(lots['tiles'][0]['value'], '0 %')
        self.assertTrue(any('jamais vérifié' in alert['text'] for alert in body['alerts']))
        # poste : GET avec la session du poste
        code, body = self.call('GET', '/api/dashboard/')
        self.assertEqual(code, 200)
        self.assertEqual(self.call('GET', '/api/dashboard/', session=False)[0], 403)

    def test_restock_score_and_extras(self) -> None:
        later = self.today + timedelta(days=200)
        first = self.create(self.compresses, later, 2)
        self.add(first)
        self.verif(first)  # complet : reassort utile
        more = self.create(self.compresses, later, 3)
        self.add(more)
        self.verif([first[0], more[0]])  # complet, mais 2 items reassortis introuvables
        garrot = self.create(self.garrot, None, 1)
        self.add(garrot)
        self.verif(garrot)  # incomplet : le reassort n'a pas suffi
        extra = self.create(self.compresses, later, 1)
        self.add(extra)  # pas encore de vérif
        code, body = self.call('GET', '/api/dashboard/')
        self.assertEqual(code, 200)
        restock = self.part(body, 'restock')
        tiles = {tile['label']: tile for tile in restock['tiles']}
        self.assertEqual(tiles['score de réassort']['value'], '67 %')
        self.assertEqual(tiles['score de réassort']['level'], 'warn')
        self.assertEqual(tiles['réassorts sur 12 mois']['value'], '4')
        self.assertEqual(tiles['en attente de vérif']['value'], '1')
        self.assertEqual(tiles['items introuvables']['value'], '2')
        # garrot : pas attendu dans le lot, compte comme item en plus
        self.assertIn('Garrot', json.dumps(restock['tables']))
        table = next(table for table in restock['tables'] if table['title'].startswith('Lots où'))
        self.assertEqual(table['rows'][0]['cells'], ['Sac A', 3, 1, 2])
        self.assertEqual(table['rows'][0]['lot'], self.lot.id)
        chart = restock['charts'][0]
        self.assertEqual(sum(chart['series'][0]['values']), 2)
        self.assertEqual(sum(chart['series'][1]['values']), 1)
        self.assertTrue(any(alert['section'] == 'restock' for alert in body['alerts']))
        # scores globaux : reassorts 67 % moins 33 % d'items reassortis introuvables (2 sur 6 suivis)
        scores = {row['key']: row for row in body['scores']}
        self.assertEqual(list(scores), ['lots', 'restock', 'consumption', 'reports', 'lists'])
        self.assertEqual(scores['restock']['value'], 34)
        self.assertEqual(scores['restock']['level'], 'bad')
        self.assertIsNone(scores['lists']['value'])  # pas encore de liste : gris
        self.assertIsNotNone(body['overall']['value'])
