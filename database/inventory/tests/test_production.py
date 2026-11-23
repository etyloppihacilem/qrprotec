"""Tests du back : production (concurrence, journaux, requetes, defauts surs, purge)."""

from __future__ import annotations

from typing import Any

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)


class ConcurrencyTests(TransactionTestCase):
    """Requetes envoyees en meme temps, chacune dans son thread avec sa connexion a la base (audit de production :
    « database is locked » a deux verifs simultanees, essais de PIN en parallele comptes une seule fois)."""
    # TransactionTestCase : sous la transaction d'un TestCase, les autres threads ne pourraient pas ecrire. Et une
    # copie de la base de test dans un fichier, comme en production : la base en memoire partagee entre threads
    # repond « database table is locked » sans jamais attendre, le test ne voudrait rien dire.

    @classmethod
    def setUpClass(cls) -> None:
        import sqlite3
        import tempfile
        from django.db import connections
        cls.memory_settings = connections.settings['default']
        cls.memory_connection = connections['default']
        cls.memory_connection.ensure_connection()
        cls.directory = tempfile.TemporaryDirectory()
        path = f'{cls.directory.name}/test.sqlite3'
        with sqlite3.connect(path) as target:
            cls.memory_connection.connection.backup(target)  # schema deja migre
        target.close()
        # connections est propre a chaque thread : les threads des requetes liront ces reglages
        connections.settings['default'] = {**cls.memory_settings, 'NAME': path}
        connections['default'] = connections.create_connection('default')
        super().setUpClass()

    @classmethod
    def tearDownClass(cls) -> None:
        from django.db import connections
        super().tearDownClass()
        connections['default'].close()
        connections.settings['default'] = cls.memory_settings
        connections['default'] = cls.memory_connection
        cls.directory.cleanup()

    setUp = ApiTestCase.setUp
    create = ApiTestCase.create

    def parallel(self, requests: list[tuple[str, str, dict[str, Any], dict[str, str]]]) -> list[tuple[int, Any]]:
        """Lance les requetes (method, path, data, extra) ensemble ; renvoie les (code, corps) dans l'ordre."""
        import threading
        from django.db import connection
        from django.test import Client
        barrier = threading.Barrier(len(requests))
        results = [None] * len(requests)

        def run(index: int, method: str, path: str, data: Any, extra: dict[str, Any]) -> None:
            try:
                client = Client()
                barrier.wait()
                response = client.generic(method, path, json.dumps(data), content_type='application/json', **extra)
                is_json = response.get('Content-Type', '').startswith('application/json')
                results[index] = (response.status_code, response.json() if is_json else None)
            finally:
                connection.close()

        threads = [threading.Thread(target=run, args=(index, *request)) for index, request in enumerate(requests)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(30)
        return results

    def test_simultaneous_verifs_all_saved(self) -> None:
        lots, requests = [], []
        for number in range(6):
            lot = Lots(lot_type=self.lot_type, name=f'Sac {number}', name_short=f'S{number}', created_by='test')
            lot.save()
            items = self.create(self.compresses, self.today + timedelta(days=100), 2)
            lots.append(lot)
            requests.append(('POST', f'/api/lots/{lot.id}/verif/', {'items': [item.iid for item in items]},
                             {**LOCAL, 'HTTP_X_QRPROTEC_SESSION': self.operator_session}))
        results = self.parallel(requests)
        self.assertEqual([code for code, _ in results], [200] * len(lots))
        for lot in lots:
            self.assertEqual(Items.objects.filter(location=lot.id, status=ItemStatus.ACTIVE).count(), 2)

    def test_lots_summary_cached_until_database_changes(self) -> None:
        """« État des lots » garde son calcul tant que la base ne change pas (datacache), et le refait des qu'une autre
        connexion (ici une verif dans un autre thread) a ecrit."""
        from .. import datacache
        datacache.clear()
        self.assertIsNotNone(datacache.data_version())  # base dans un fichier : cache actif
        summary = lambda: self.parallel([('POST', '/api/lots/summary/', {}, {})])[0]
        summary()  # premier appel : cree la ligne des reglages du serveur (une ecriture)
        with mock.patch.object(ser, 'lot_list', wraps=ser.lot_list) as lot_list:
            first, second, third = summary(), summary(), summary()
            self.assertEqual(lot_list.call_count, 1)
        self.assertEqual(second, third)
        self.assertEqual(first, second)
        count = lambda body: next(row['item_count'] for row in body if row['id'] == self.lot.id)
        self.assertEqual(count(first[1]), 0)
        items = self.create(self.compresses, self.today + timedelta(days=100), 2)
        code, _ = self.parallel([('POST', f'/api/lots/{self.lot.id}/verif/', {'items': [item.iid for item in items]},
                                  {**LOCAL, 'HTTP_X_QRPROTEC_SESSION': self.operator_session})])[0]
        self.assertEqual(code, 200)
        code, body = summary()
        self.assertEqual((code, count(body)), (200, 2))
        datacache.clear()

    def test_parallel_wrong_pins_are_all_counted(self) -> None:
        admin = Secouristes(matricule='A001', nom='Ad', prenom='Min', role='admin')
        admin.renew_key()
        admin.set_pin('482100')
        admin.save()
        attempts = 12
        data = {'matricule': 'A001', 'key': admin.new_key}
        requests: list[tuple[str, str, dict[str, Any], dict[str, Any]]] = [('POST', '/api/auth/', {**data, 'pin': f'{1000 + number}'}, {}) for number in range(attempts)]
        results = self.parallel(requests)
        self.assertTrue(all(code == 403 for code, _ in results))
        admin.refresh_from_db()
        # avant : chaque essai relisait 0 et ecrivait 1. Maintenant les 5 premiers comptent et posent le blocage
        # temporaire, les suivants sont refuses sans etre comptes.
        self.assertEqual(admin.pin_failures_total, models.PIN_MAX_FAILURES)
        self.assertTrue(admin.pin_locked())
        # le bon PIN est refuse pendant le blocage
        code, _ = self.parallel([('POST', '/api/auth/', {**data, 'pin': '482100'}, {})])[0]
        self.assertEqual(code, 403)

    def test_parallel_name_logins_are_all_counted(self) -> None:
        cache.clear()
        self.user.set_pin('246800')
        self.user.save()
        attempts = 8
        requests = [('POST', '/api/auth/name/', {'nom': 'Dupont', 'prenom': 'Jeanne', 'pin': f'{1000 + number}'},
                     {'REMOTE_ADDR': f'10.0.0.{number}'}) for number in range(attempts)]
        results = self.parallel(requests)
        self.assertTrue(all(code == 403 for code, _ in results))
        self.user.refresh_from_db()
        self.assertEqual(self.user.name_login_failures, attempts)
        self.assertIsNotNone(self.user.name_login_locked_until)  # pause posee au 5e essai faux

    def test_parallel_name_logins_respect_address_limit(self) -> None:
        cache.clear()
        attempts = views.NAME_LOGIN_ADDRESS_LIMIT + 4
        requests = [('POST', '/api/auth/name/', {'nom': 'Inconnu', 'prenom': 'Personne', 'pin': '123400'},
                     {'REMOTE_ADDR': '10.0.0.1'}) for _ in range(attempts)]
        codes = sorted(code for code, _ in self.parallel(requests))
        self.assertEqual(codes, [403] * views.NAME_LOGIN_ADDRESS_LIMIT + [429] * 4)


class LoggingTests(TestCase):
    def test_server_errors_reach_stderr(self) -> None:
        """En production (DEBUG=0), les erreurs 500 doivent aller dans le journal, pas seulement par mail."""
        import logging
        logger = logging.getLogger('django.request')
        self.assertTrue(logger.isEnabledFor(logging.ERROR))
        handlers = logging.getLogger('django').handlers
        self.assertTrue(any(type(handler) is logging.StreamHandler for handler in handlers))
        self.assertTrue(logging.getLogger('inventory.live').isEnabledFor(logging.WARNING))


class ServeLogTests(TestCase):
    def test_secrets_redacted_from_access_log(self) -> None:
        from inventory.management.commands.serve import redact_secrets
        self.assertEqual(redact_secrets('GET /ws/scanner/phone?s=XpBL&k=QveV HTTP/1.1'),
                         'GET /ws/scanner/phone?s=***&k=*** HTTP/1.1')
        self.assertEqual(redact_secrets('GET /verif?lot=AB12&key=secret HTTP/1.1'),
                         'GET /verif?lot=AB12&key=*** HTTP/1.1')
        self.assertEqual(redact_secrets('GET /pinreset?m=M001&t=jeton HTTP/1.1'), 'GET /pinreset?m=M001&t=*** HTTP/1.1')
        # autres parametres (ex : ?type=, ?lot=) intacts, valeurs non textuelles laissees telles quelles
        self.assertEqual(redact_secrets('GET /api/receptions/storage/?type=compre'),
                         'GET /api/receptions/storage/?type=compre')
        self.assertEqual(redact_secrets(404), 404)


class QueryCountTests(ApiTestCase):
    """Listes de lots et d'utilisateurs : un nombre de requetes SQL qui ne grandit pas avec le nombre de lots."""

    def add_lots(self, count: int) -> None:
        for index in range(count):
            parent = Lots(lot_type=self.lot_type, name=f'Global {index}', name_short='G', created_by='test')
            parent.save()
            child = Lots(lot_type=self.lot_type, name=f'Sous-lot {index}', name_short='S', created_by='test', parent=parent)
            child.save()
            items = self.create(self.compresses, self.today + timedelta(days=100), 2)
            services.perform_verif(child, [item.iid for item in items], 'M:P001')

    def setUp(self) -> None:
        super().setUp()
        models.ServerSettings.get()  # cree une fois pour toutes, hors des comptes

    def count(self, method: str, path: str, data: dict[str, dict[str, str]] | None = None, local: bool = True) -> int:
        from django.db import connection
        from django.test.utils import CaptureQueriesContext
        with CaptureQueriesContext(connection) as queries:
            code, body = self.call(method, path, data, local=local)
        self.assertEqual(code, 200, body)

        return len(queries)

    def test_lot_lists_do_not_grow_with_lots(self) -> None:
        manager = Secouristes(matricule='G001', nom='Gest', prenom='Ion', role='gestion')
        manager.renew_key()
        manager.save()
        badge = {'user': {'matricule': manager.matricule, 'key': manager.new_key}}
        self.add_lots(2)
        # premiere requete d'ecriture de la connexion : verifie la capture des ecritures (undo.py) d'un test precedent
        self.count('POST', '/api/lots/summary/', {}, local=False)
        before = [self.count('POST', '/api/lots/summary/', {}, local=False), self.count('GET', '/api/lots/'),
                  self.count('POST', '/api/tasks/summary/', badge, local=False)]
        self.add_lots(6)
        after = [self.count('POST', '/api/lots/summary/', {}, local=False), self.count('GET', '/api/lots/'),
                 self.count('POST', '/api/tasks/summary/', badge, local=False)]
        self.assertEqual(before, after)

    def test_users_list_reads_settings_once(self) -> None:
        for index in range(5):
            Secouristes(matricule=f'U{index}', nom='Nom', prenom='Prenom').save()
        before = self.count('GET', '/api/users/')
        for index in range(5, 15):
            Secouristes(matricule=f'U{index}', nom='Nom', prenom='Prenom').save()
        self.assertEqual(self.count('GET', '/api/users/'), before)


class ProductionDefaultsTests(TestCase):
    """Hors RPM, le back ne demarre pas en DEBUG ni avec la cle secrete publique du depot."""

    def settings_error(self, **env: str) -> tuple[int, str]:
        import os
        import subprocess
        import sys
        from pathlib import Path
        environment = {key: value for key, value in os.environ.items() if not key.startswith('QRPROTEC_')}
        # memes modules que ce processus (RPM : PYTHONPATH relatif database:vendor, faux depuis un autre dossier)
        environment.update(env, DJANGO_SETTINGS_MODULE='qrprotecDB.settings',
                           PYTHONPATH=os.pathsep.join(path for path in map(os.path.abspath, sys.path) if path))
        result = subprocess.run(
            [sys.executable, '-c', 'from django.conf import settings; print(settings.DEBUG, settings.ALLOWED_HOSTS)'],
            cwd=Path(__file__).resolve().parents[2], env=environment, capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    def test_secret_key_required_without_debug(self) -> None:
        code, output = self.settings_error()
        self.assertNotEqual(code, 0)
        self.assertIn('QRPROTEC_SECRET_KEY manquante', output)
        from qrprotecDB.settings import _INSECURE_SECRET_KEY
        code, output = self.settings_error(QRPROTEC_SECRET_KEY=_INSECURE_SECRET_KEY)  # cle publique du depot
        self.assertNotEqual(code, 0)
        code, output = self.settings_error(QRPROTEC_SECRET_KEY='une-cle-aleatoire')
        self.assertEqual(code, 0, output)
        self.assertIn("False ['localhost', '127.0.0.1', '[::1]']", output)  # DEBUG coupe, aucune IP en plus

    def test_debug_is_explicit(self) -> None:
        code, output = self.settings_error(QRPROTEC_DEBUG='1')
        self.assertEqual(code, 0, output)
        self.assertIn("True ['localhost', '127.0.0.1', '[::1]']", output)


class PurgeHistoryTests(ApiTestCase):
    """Retention de l'historique : au-dela de 3 ans tout part, sauf ce dont l'etat actuel depend."""

    def test_purge_keeps_current_state(self) -> None:
        from inventory.management.commands.purge_history import purge
        from inventory.models import ItemMovement, Operation, Verifs
        items = self.create(self.compresses, self.today + timedelta(days=100), 2)
        services.perform_verif(self.lot, [item.iid for item in items], 'M:P001')  # vieille verif
        services.perform_verif(self.lot, [items[0].iid], 'M:P001')  # derniere verif, vieille aussi
        old = timezone.now() - timedelta(days=4 * 365)
        Verifs.objects.update(datetime=old)
        ItemMovement.objects.update(at=old)
        Operation.objects.update(at=old)
        last = Verifs.objects.filter(lot=self.lot).latest('id')
        moves_before = ItemMovement.objects.count()
        dry = purge(3, dry_run=True)
        self.assertEqual(Verifs.objects.count(), 2)  # --dry-run ne supprime rien
        counts = purge(3)
        self.assertEqual(counts['verifs'], dry['verifs'])
        self.assertEqual(counts['items'], 0)  # items encore dans le lot
        self.assertEqual(list(Verifs.objects.values_list('id', flat=True)), [last.id])  # derniere verif gardee
        # dernier mouvement de chaque item garde
        self.assertEqual(ItemMovement.objects.count(), min(moves_before, len({m.item_id for m in ItemMovement.objects.all()})))
        for item in items:
            self.assertTrue(ItemMovement.objects.filter(item=item).exists() or moves_before == 0)
        self.assertEqual(Operation.objects.count(), 0)
        # rien d'ancien : rien a supprimer, et une retention plus longue garde tout
        self.assertFalse(any(purge(5).values()))

    def test_purge_gone_items_without_references(self) -> None:
        from inventory.management.commands.purge_history import purge
        from inventory.models import Items, ItemsPacks, ItemStatus, ProblemReport, VerifItem
        gone, recent, reported = self.create(self.compresses, self.today + timedelta(days=100), 3)
        old = timezone.now() - timedelta(days=6 * 365)
        Items.objects.filter(iid__in=[gone.iid, reported.iid]).update(status=ItemStatus.REPLACED, last_seen=old)
        Items.objects.filter(iid=recent.iid).update(status=ItemStatus.MISSING, last_seen=timezone.now())
        ProblemReport.objects.create(by='M:P001', item=reported, comment='x')  # signalement ouvert : garde
        counts = purge(5)
        self.assertEqual(counts['items'], 1)
        self.assertEqual(set(Items.objects.values_list('iid', flat=True)), {recent.iid, reported.iid})
        # le compteur du paquet reste : un iid deja imprime n'est jamais redonne
        self.assertTrue(ItemsPacks.objects.filter(id=gone.pack_id).exists())
        again = self.create(self.compresses, self.today + timedelta(days=100), 1)[0]
        self.assertNotIn(again.iid, (gone.iid, recent.iid, reported.iid))

    def test_weekly_and_disabled(self) -> None:
        out = StringIO()
        call_command('purge_history', '--years', '0', stdout=out)
        self.assertIn('desactivee', out.getvalue())
