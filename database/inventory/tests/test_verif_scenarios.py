"""Tests du back : scenarios de vérif communs au serveur, au front web et au poste."""

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)


class VerifScenariosTests(TestCase):
    """Scenarios de vérif communs au serveur, au front web et au poste (tests/verif-scenarios/*.json, voir leur
    LISEZMOI.md) : meme lot pour chaque item scanne, memes lots complets, memes perimes a jeter. Le serveur les joue
    ici (assign_items, _complete_lots, perform_verif), le front web sous node (web.test.js, lance ci-dessous si node
    est installe) et le poste dans app/tests/verif_scenarios_test.cpp (ctest)."""

    DIR = Path(__file__).resolve().parents[3] / 'tests' / 'verif-scenarios'

    def scenarios(self):
        files = sorted(self.DIR.glob('*.json'))
        self.assertTrue(files, f'aucun scenario dans {self.DIR}')
        return [(path.name, json.loads(path.read_text(encoding='utf-8'))) for path in files]

    def build(self, scenario):
        """Types, lots (noms numerotes : les sous-lots se suivent dans l'ordre du fichier) et items du scenario."""
        types = {}
        for code, flags in scenario['types'].items():
            types[code] = ItemType.objects.create(
                type=code, name=code, perissable=flags.get('perissable', True), consumable=flags.get('consumable', True),
                tear_off=flags.get('tear_off', False), physical=flags.get('physical', True))
        lots = {}
        for index, data in enumerate(scenario['lots']):
            lot_type = LotType.objects.create(type=f'sc{index:04d}', name=data['id'], storage=data.get('storage', False))
            for code, quantity in data.get('requirements', {}).items():
                LotRequirements.objects.create(lot_type=lot_type, item_type=types[code], quantity=quantity)
            lot = Lots(lot_type=lot_type, name=f'{index:02d} {data["id"]}', name_short=data['id'][:8], created_by='test',
                       parent=lots.get(data.get('parent')))
            lot.save()
            lots[data['id']] = lot
        iids = {}
        today = timezone.localdate()
        placed = [(item, lots[data['id']]) for data in scenario['lots'] for item in data.get('items', [])]
        placed += [(item, None) for item in scenario.get('elsewhere', [])]
        for item, lot in placed:
            peremption = date(2020, 1, 1) if item.get('expired') else today + timedelta(days=3650)
            created = ItemsPacks.objects.add_items(types[item['type']], peremption, 1, 'M:TEST', location=lot)
            iids[item['id']] = created[0].iid
        return lots, iids

    def play(self, scenario):
        lots, iids = self.build(scenario)
        label = {iid: name for name, iid in iids.items()}
        lot_label = {lot.id: name for name, lot in lots.items()}
        root = lots[scenario['lots'][0]['id']]
        group = services.verif_lots([root])
        self.assertEqual([lot_label[lot.id] for lot in group], [data['id'] for data in scenario['lots']])
        scans = [scan for scan in scenario['scans'] if 'item' in scan]
        order = [iids[scan['item']] for scan in scans]
        places = {iids[scan['item']]: lots[scan['place']].id for scan in scans if scan.get('place')}
        checked = {}
        for scan in scenario['scans']:
            if 'check' in scan:
                checked.setdefault(lots[scan['lot']].id, []).append(scan['check'])
        today = timezone.localdate()
        scanned = list(Items.objects.select_related('pack__item_type').filter(iid__in=order).order_by('iid'))
        target = services.assign_items(group, scanned, today, places, cascade=True)
        not_seen = list(Items.objects.select_related('pack__item_type')
                        .filter(location__in=group, status=ItemStatus.ACTIVE).exclude(iid__in=order))
        complete = services._complete_lots(group, scanned, not_seen, target, today, checked=checked)
        report = services.perform_verif(root, order, 'M:TEST', places=places, checked=checked)
        return {
            'target': {label[iid]: lot_label[lot_id] for iid, lot_id in target.items()},
            'complete': sorted(lot_label[lot_id] for lot_id in complete),
            'discard': sorted(label[iid] for iid in report['replaced']),
        }

    def test_scenarios_server(self):
        for name, scenario in self.scenarios():
            with self.subTest(scenario=name), transaction.atomic():
                expected = scenario['expected']
                result = self.play(scenario)
                self.assertEqual(result['target'], expected['target'], f'{name} : lot de chaque item')
                self.assertEqual(result['complete'], sorted(expected['complete']), f'{name} : lots complets')
                self.assertEqual(result['discard'], sorted(expected.get('discard', [])), f'{name} : perimes a jeter')
                transaction.set_rollback(True)

    def test_scenarios_web(self):
        """Le front web joue les memes scenarios (node, s'il est installe : sinon test saute)."""
        import shutil
        import subprocess
        node = shutil.which('node')
        if node is None:
            self.skipTest('node absent : scenarios du front web non joues')
        result = subprocess.run([node, '--test', str(self.DIR / 'web.test.js')], capture_output=True, text=True,
                                timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout[-4000:] + result.stderr[-2000:])
