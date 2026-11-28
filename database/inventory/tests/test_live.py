"""Tests du back : douchette telephone et suivi en direct (WebSocket)."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any
from collections.abc import Callable

from .base import *  # noqa: F401,F403
from .base import setUpModule, tearDownModule  # noqa: F401 (PIN facultatif pendant ces tests)

if TYPE_CHECKING:
    import threading

    from inventory.live import LiveSession
    from inventory.models import Items


class RemoteScannerTests(TransactionTestCase):
    """Telephone-douchette : vrai serveur `serve` (deux ports) et clients WebSocket bruts."""
    # TransactionTestCase : le serveur tourne dans d'autres threads, avec leurs propres connexions a la base. Sous la
    # transaction d'un TestCase (BEGIN IMMEDIATE, voir DATABASES), leurs ecritures seraient refusees.

    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        import threading
        from django.core.servers.basehttp import WSGIServer, get_internal_wsgi_application
        from ..management.commands.serve import run_server
        from ..middleware import RoleWSGIHandler
        from ..remote_scanner import hub
        cls.hub = hub
        hub.enabled = True
        application = get_internal_wsgi_application()
        cls.ports = {}
        for role in ('public', 'local', 'remote'):
            port = cls._free_port()
            cls.ports[role] = port
            threading.Thread(target=run_server, daemon=True,
                             args=('127.0.0.1', port, RoleWSGIHandler(application, role), WSGIServer, role)).start()
        import time
        time.sleep(0.3)

    @staticmethod
    def _free_port() -> int:
        import socket
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            return sock.getsockname()[1]

    def http(self, role: str, method: str, path: str, body: dict[str, Any] | None = None) -> tuple[int, Any]:
        import http.client
        connection = http.client.HTTPConnection('127.0.0.1', self.ports[role], timeout=5)
        connection.request(method, path, json.dumps(body) if body is not None else None,
                           {'Content-Type': 'application/json'})
        response = connection.getresponse()
        data = response.read()
        is_json = response.getheader('Content-Type', '').startswith('application/json')
        return response.status, (json.loads(data) if is_json and data else None)

    def ws(self, role: str, path: str, headers: dict[str, str] | None = None) -> RawWebSocket:
        return RawWebSocket('127.0.0.1', self.ports[role], path, headers)

    def create_session(self, minutes: int = 5) -> tuple[dict[str, Any], str, str]:
        code, body = self.http('local', 'POST', '/api/remote-scanner/', {'timeout_minutes': minutes})
        self.assertEqual(code, 201)
        from urllib.parse import parse_qs, urlsplit
        query = parse_qs(urlsplit(body['url']).query)
        return body, query['s'][0], query['k'][0]

    def test_relay_between_phone_and_front(self) -> None:
        body, session_id, key = self.create_session()
        self.assertTrue(body['url'].startswith('https://example.com/scanner?'))
        front = self.ws('local', f'/ws/scanner/front?s={session_id}')
        self.assertEqual(front.receive()['type'], 'hello')
        phone = self.ws('public', f'/ws/scanner/phone?s={session_id}&k={key}')
        hello = phone.receive()
        self.assertEqual(hello, {**hello, 'type': 'hello', 'front_connected': True})
        self.assertEqual(front.receive(), {**front.last, 'type': 'phone', 'connected': True})
        phone.send({'type': 'scan', 'code': 'compre20271231000000A1', 'id': 7})
        self.assertEqual(front.receive(), {'type': 'scan', 'code': 'compre20271231000000A1', 'id': 7})
        self.assertEqual(phone.receive()['ok'], True)
        front.send({'type': 'feedback', 'result': 'error', 'message': 'PÉRIMÉ'})
        self.assertEqual(phone.receive(), {'type': 'feedback', 'result': 'error', 'message': 'PÉRIMÉ'})
        # fermeture depuis le poste : le telephone est prevenu, la cle ne marche plus
        front.send({'type': 'close'})
        self.assertEqual(phone.receive()['type'], 'closed')
        code, _ = self.http('public', 'GET', f'/api/remote-scanner/check/?s={session_id}&k={key}')
        self.assertEqual(code, 404)

    def test_access_rules(self) -> None:
        body, session_id, key = self.create_session()
        # mauvaise cle, route du poste sur l'API publique, creation sur l'API publique
        self.assertEqual(self.ws('public', f'/ws/scanner/phone?s={session_id}&k=faux').status, 404)
        self.assertEqual(self.ws('public', f'/ws/scanner/front?s={session_id}').status, 404)
        code, _ = self.http('public', 'POST', '/api/remote-scanner/', {})
        self.assertEqual(code, 404)
        code, check = self.http('public', 'GET', f'/api/remote-scanner/check/?s={session_id}&k={key}')
        self.assertEqual(code, 200)
        # les requetes HTTP ordinaires passent toujours
        code, health = self.http('public', 'GET', '/api/health/')
        self.assertTrue(health['remote_scanner'])

    def test_label_prints_live(self) -> None:
        # poste principal : chaque changement de la file d'impression lui est signale aussitot
        import base64
        from ..print_live import hub as print_hub
        poste = self.ws('local', '/ws/label-prints')
        self.assertEqual(poste.status, 101)
        self.assertEqual(poste.receive(), {'type': 'hello'})
        poste.send({'type': 'ping'})
        self.assertEqual(poste.receive(), {'type': 'pong'})
        label = {'description': 'Lot 1/1', 'width': 8, 'height': 1, 'ppm': 8, 'bits': base64.b64encode(b'\0').decode()}
        code, sent = self.http('local', 'POST', '/api/label-prints/send/', {'title': 'Lot', 'labels': [label]})
        self.assertEqual(code, 201)
        self.assertEqual(poste.receive(), {'type': 'changed'})
        code, _ = self.http('local', 'POST', f"/api/label-prints/{sent['id']}/print/", {})
        self.assertEqual(code, 200)
        self.assertEqual(poste.receive(), {'type': 'changed'})
        poste.close()
        import time
        deadline = time.monotonic() + 5
        while print_hub.count() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertEqual(print_hub.count(), 0)

    def test_label_prints_live_access(self) -> None:
        # pas sur l'API publique ; sur l'API distante, seulement la cle du poste principal
        from ..models import FrontKey
        self.assertEqual(self.ws('public', '/ws/label-prints').status, 404)
        self.assertEqual(self.ws('remote', '/ws/label-prints').status, 401)
        front, key = FrontKey.create('Ordi du TECH')
        self.assertEqual(self.ws('remote', '/ws/label-prints', {'X-QRProtec-Key': key}).status, 403)
        front.poste = True
        front.save(update_fields=['poste'])
        kiosk = self.ws('remote', '/ws/label-prints', {'X-QRProtec-Key': key})
        self.assertEqual(kiosk.status, 101)
        self.assertEqual(kiosk.receive(), {'type': 'hello'})
        kiosk.close()

    def test_session_expires_after_disconnection(self) -> None:
        import time
        body, session_id, key = self.create_session(minutes=1)
        front = self.ws('local', f'/ws/scanner/front?s={session_id}')
        front.receive()
        phone = self.ws('public', f'/ws/scanner/phone?s={session_id}&k={key}')
        phone.receive()
        front.receive()
        phone.close()
        self.assertEqual(front.receive(), {**front.last, 'type': 'phone', 'connected': False})
        session = self.hub.get(session_id)
        self.assertEqual(self.hub.reap(time.monotonic() + 30), [])       # encore dans le delai
        self.assertEqual(self.hub.reap(session.phone_lost + 61), [session_id])
        self.assertEqual(front.receive()['type'], 'closed')
        self.assertEqual(self.ws('public', f'/ws/scanner/phone?s={session_id}&k={key}').status, 404)


class FakeSocket:
    """WebSocket factice pour LiveHub : messages a recevoir (file), messages envoyes (liste)."""

    def __init__(self, *incoming: Any) -> None:
        import queue
        self.incoming: queue.Queue[str | None] = queue.Queue()
        for message in incoming:
            self.push(message)
        self.sent: list[dict[str, Any]] = []
        self.closed = False

    def push(self, message: Any) -> None:
        self.incoming.put(json.dumps(message) if message is not None else None)

    def receive(self) -> str:
        from ..remote_scanner import WebSocketClosed
        message = self.incoming.get(timeout=5)
        if message is None:
            raise WebSocketClosed('fin')
        return message

    def send_json(self, message: dict[str, Any]) -> bool:
        self.sent.append(message)
        return True

    def close(self, code: int = 1000) -> None:
        self.closed = True


class LiveHubTests(ApiTestCase):
    """Suivi des vérifs en direct : publication par la page de scan, diffusion aux pages de suivi."""

    def setUp(self) -> None:
        super().setUp()
        from unittest import mock
        from .. import live
        self.live = live
        self.hub = live.LiveHub()
        # les connexions tournent dans des threads : le badge est verifie sans la base de test
        self.users = users = {self.user.matricule: self.user.new_key}
        patcher = mock.patch.object(live, 'badge_identity', side_effect=lambda credentials: (
            f'M:{credentials["matricule"]}' if isinstance(credentials, dict)
            and users.get(credentials.get('matricule')) == credentials.get('key') else None))
        patcher.start()
        self.addCleanup(patcher.stop)
        names = mock.patch.object(live, 'public_name', side_effect=lambda identity: {'M:M001': 'Jeanne', 'M:M002': 'Paul'}
                                  .get(identity, identity.partition(':')[2]))
        names.start()
        self.addCleanup(names.stop)
        # noms de la carte terminee (services.VerifTitles) : lus dans le thread de la WebSocket, qui ne peut pas lire
        # les lots ecrits par la transaction du test (« database table is locked » de la base de test en memoire
        # partagee, impossible sur le fichier en WAL de la prod) ; la vraie lecture : test_done_card_names
        self.name_done = live.LiveHub._name_done
        titles = mock.patch.object(live.LiveHub, '_name_done', staticmethod(
            lambda session: session.done and session.done.get('verifs') and session.done.setdefault('names', ['Sac A'])))
        titles.start()
        self.addCleanup(titles.stop)
        self.badge = {'matricule': 'M001', 'key': self.user.new_key}

    def run_socket(self, target: Callable[[Any], None], socket: FakeSocket) -> threading.Thread:
        import threading
        thread = threading.Thread(target=target, args=(socket,), daemon=True)
        thread.start()
        return thread

    def wait(self, condition: Callable[[], Any]) -> None:
        import time
        for _ in range(200):
            if condition():
                return
            time.sleep(0.01)
        self.fail('condition jamais remplie')

    def test_watcher_needs_a_badge(self) -> None:
        socket = FakeSocket({'type': 'hello', 'user': {'matricule': 'M001', 'key': 'faux'}})
        self.hub.run_watcher(socket)
        self.assertEqual(socket.sent[0]['type'], 'error')
        self.assertTrue(socket.sent[0]['login_required'])
        self.assertFalse(self.hub.watchers)

    def test_scans_are_relayed_live_then_done(self) -> None:
        watcher = FakeSocket({'type': 'hello', 'user': self.badge})
        watching = self.run_socket(self.hub.run_watcher, watcher)
        self.wait(lambda: watcher.sent)
        self.assertEqual(watcher.sent[0], {'type': 'snapshot', 'sessions': []})
        scanner = FakeSocket({'type': 'hello', 'user': self.badge})
        scanning = self.run_socket(self.hub.run_scanner, scanner)
        self.wait(lambda: scanner.sent)
        hello = scanner.sent[0]
        self.assertEqual(hello['type'], 'hello')
        scanner.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'lots': [],
                      'scanned': [{'name': 'Compresses', 'expired': True}], 'count': 1, 'remaining': 1})
        self.wait(lambda: any(m['type'] == 'update' for m in watcher.sent))
        update = next(m for m in watcher.sent if m['type'] == 'update')['session']
        self.assertEqual(update['who'], 'Jeanne')
        self.assertTrue(update['verified'])
        self.assertTrue(update['connected'])
        self.assertEqual(update['lot'], {'id': self.lot.id, 'name': 'Sac A'})
        self.assertEqual(update['scanned'], [{'name': 'Compresses', 'expired': True, 'error': False, 'extra': False, 'discard': False}])
        self.assertNotIn('token', update)
        scanner.push({'type': 'done', 'complete': True, 'present': 2, 'verif': 42})
        self.wait(lambda: watcher.sent[-1]['type'] == 'update' and watcher.sent[-1]['session']['done'])
        self.assertEqual(watcher.sent[-1]['session']['done']['present'], 2)
        # vérif enregistree : son compte rendu s'ouvre depuis la carte du suivi
        self.assertEqual(watcher.sent[-1]['session']['done']['verifs'], [42])
        # page de scan fermee : la verif terminee reste affichee DONE_KEEP, puis disparait
        scanner.push(None)
        scanning.join(5)
        session = self.hub.sessions[hello['id']]
        self.assertEqual(self.hub.reap(session.lost + 60), [])
        self.assertEqual(self.hub.reap(session.lost + self.live.DONE_KEEP + 1), [hello['id']])
        self.wait(lambda: watcher.sent[-1] == {'type': 'remove', 'id': hello['id']})
        watcher.push(None)
        watching.join(5)
        self.assertFalse(self.hub.watchers)

    def test_done_card_names(self) -> None:
        """Carte terminee du suivi : noms de services.VerifTitles (vraie base, lue ici dans le thread du test)."""
        from unittest import mock
        verif = Verifs.objects.create(lot=self.lot, by='test', complete=True, present_count=2)
        session = self.live.LiveSession()
        session.done = {'complete': True, 'lots': 3, 'verifs': [verif.id]}
        with mock.patch.object(self.live, 'close_old_connections') as close:
            self.name_done(session)  # le vrai, remplace dans setUp pour les threads
        self.assertEqual(session.done['names'], [self.lot.name])
        self.assertEqual(session.done['lots'], 1)
        close.assert_called_once()

    def test_full_verif_roles(self) -> None:
        """« Rendre la vérif complète » : badge des roles gestion et admin seulement."""
        badge = {'matricule': 'M001', 'key': self.user.new_key}
        for role, allowed in (('normal', False), ('chef', False), ('gestion', True), ('admin', True)):
            self.user.role = role
            self.user.save()
            self.assertEqual(self.live.is_gestion(badge), allowed, role)
        self.user.active = False
        self.user.save()
        self.assertFalse(self.live.is_gestion(badge))

    def test_full_verif_shared_by_admin(self) -> None:
        """« Rendre la vérif complète » : la gestion ou un admin l'active depuis la page de suivi, tous les participants
        le voient, un secouriste ne peut pas."""
        from unittest import mock
        admins = mock.patch.object(self.live, 'is_gestion',
                                   side_effect=lambda credentials: isinstance(credentials, dict)
                                   and credentials.get('matricule') == 'P001')
        admins.start()
        self.addCleanup(admins.stop)
        scanner = FakeSocket({'type': 'hello', 'user': self.badge})
        scanning = self.run_socket(self.hub.run_scanner, scanner)
        self.wait(lambda: scanner.sent)
        session_id = scanner.sent[0]['id']
        scanner.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'optional': True})
        self.wait(lambda: self.hub.sessions[session_id].state)
        self.assertFalse(self.hub.sessions[session_id].public()['full'])
        self.assertTrue(self.hub.sessions[session_id].public()['optional'])
        # un secouriste (page de scan) : refuse
        scanner.push({'type': 'full', 'on': True, 'user': self.badge})
        # un admin depuis la page de suivi : partage avec les participants
        self.assertTrue(self.hub.set_full(session_id, True, {'matricule': 'P001', 'key': 'x'}))
        self.wait(lambda: any(m['type'] == 'team' and m['full'] for m in scanner.sent))
        self.assertTrue(self.hub.sessions[session_id].public()['full'])
        self.assertFalse(self.hub.set_full(session_id, False, self.badge))
        self.assertTrue(self.hub.sessions[session_id].full)
        scanner.push(None)
        scanning.join(5)

    def test_restock_list_is_shared(self) -> None:
        """Ecran de reassort du web : cases de la liste de courses et lots au reassort de fin de vérif en cascade,
        partages avec tous les participants, videes par "reset"."""
        scanner = FakeSocket({'type': 'hello', 'user': self.badge})
        scanning = self.run_socket(self.hub.run_scanner, scanner)
        self.wait(lambda: scanner.sent)
        session_id = scanner.sent[0]['id']
        scanner.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}})
        self.wait(lambda: self.hub.sessions[session_id].state)
        scanner.push({'type': 'restock', 'check': 'compre', 'on': True})
        scanner.push({'type': 'restock', 'later': self.lot.id, 'on': True})
        self.wait(lambda: any(m['type'] == 'team' and m['restock']['later'] for m in scanner.sent))
        team = [m for m in scanner.sent if m['type'] == 'team'][-1]
        self.assertEqual(list(team['restock']['checked']), ['compre'])
        self.assertEqual(team['restock']['later'], [self.lot.id])
        scanner.push({'type': 'restock', 'check': 'compre', 'on': False})
        self.wait(lambda: not self.hub.sessions[session_id].restock_checked)
        scanner.push({'type': 'restock', 'reset': True})
        self.wait(lambda: not self.hub.sessions[session_id].restock_later)
        scanner.push(None)
        scanning.join(5)

    def test_reload_resumes_the_session(self) -> None:
        first = FakeSocket({'type': 'hello', 'name': 'Paul Martin'})
        thread = self.run_socket(self.hub.run_scanner, first)
        self.wait(lambda: first.sent)
        hello = first.sent[0]
        first.push(None)
        thread.join(5)
        second = FakeSocket({'type': 'hello', 'id': hello['id'], 'pid': hello['pid'], 'token': hello['token'],
                             'name': 'Paul Martin'})
        thread = self.run_socket(self.hub.run_scanner, second)
        self.wait(lambda: second.sent)
        self.assertEqual((second.sent[0]['id'], second.sent[0]['pid']), (hello['id'], hello['pid']))
        participant = self.hub.sessions[hello['id']].participants[hello['pid']]
        self.assertEqual((participant.who, participant.verified, participant.identity),
                         ('Paul Martin', False, 'D:Paul Martin'))
        # mauvais jeton : nouvelle session
        third = FakeSocket({'type': 'hello', 'id': hello['id'], 'pid': hello['pid'], 'token': 'faux'})
        other = self.run_socket(self.hub.run_scanner, third)
        self.wait(lambda: third.sent)
        self.assertNotEqual(third.sent[0]['id'], hello['id'])
        for socket, running in ((second, thread), (third, other)):
            socket.push(None)
            running.join(5)

    def join_team(self, keyed: bool = True
                 ) -> tuple[FakeSocket, FakeSocket, dict[str, str], dict[str, str], list[Items]]:
        """Jeanne commence la verif du lot, Paul (badge) la rejoint depuis la page de suivi. keyed : Jeanne a scanne
        l'etiquette cle du lot (sinon son etiquette publique)."""
        from ..models import Secouristes
        paul = Secouristes(matricule='M002', nom='Martin', prenom='Paul')
        paul.renew_key()
        paul.save()
        self.users['M002'] = paul.new_key
        self.paul = {'matricule': 'M002', 'key': paul.new_key}
        items = self.create(self.compresses, self.today + timedelta(days=200), 2)
        first = FakeSocket({'type': 'hello', 'user': self.badge})
        self.run_socket(self.hub.run_scanner, first)
        self.wait(lambda: first.sent)
        hello = first.sent[0]
        entry = {'code': items[0].iid, 'kind': 'item', 'iid': items[0].iid, 'info': {'type_name': 'Compresses'}}
        first.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'root': self.lot.id,
                    'scanned': [{'name': 'Compresses'}], 'count': 1, 'remaining': 1, 'entries': [entry],
                    'keys': [{'id': self.lot.id, 'key': self.lot.verif_key if keyed else ''}],
                    'labels': [] if keyed else [self.lot.id]})
        self.wait(lambda: self.hub.sessions[hello['id']].state)
        # nom declare : on ne rejoint pas une verif
        stranger = FakeSocket({'type': 'hello', 'join': hello['id'], 'name': 'Inconnu'})
        self.hub.run_scanner(stranger)
        self.assertEqual(stranger.sent[0]['type'], 'error')
        second = FakeSocket({'type': 'hello', 'join': hello['id'], 'user': self.paul})
        self.run_socket(self.hub.run_scanner, second)
        self.wait(lambda: any(m['type'] == 'team' for m in second.sent))
        joined = second.sent[0]
        self.assertEqual(joined['id'], hello['id'])
        team = next(m for m in second.sent if m['type'] == 'team')
        self.assertEqual(team['lots'], [self.lot.id])
        self.assertEqual(team['keyed'], [self.lot.id] if keyed else [])
        self.assertEqual(team['labeled'], [] if keyed else [self.lot.id])
        self.assertNotIn(self.lot.verif_key, json.dumps(second.sent))  # la cle reste sur le serveur
        self.assertEqual([e['iid'] for e in team['entries']], [items[0].iid])
        self.assertEqual(team['entries'][0]['who'], 'Jeanne')
        self.assertEqual([p['who'] for p in team['participants']], ['Jeanne', 'Paul'])
        second.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'root': self.lot.id,
                     'entries': [{'code': items[1].iid, 'kind': 'item', 'iid': items[1].iid}], 'count': 2})
        self.wait(lambda: any(m['type'] == 'team' and m['entries'] for m in first.sent))
        self.wait(lambda: any(m['type'] == 'team' and len(m['participants']) == 2 and m['entries']
                              and m['entries'][-1]['iid'] == items[1].iid for m in first.sent))
        return first, second, hello, joined, items

    def test_team_check_is_sent_by_the_last_participant(self) -> None:
        from unittest import mock
        first, second, hello, joined, items = self.join_team()
        public = self.hub.visible()[0]
        self.assertEqual([p['who'] for p in public['participants']], ['Jeanne', 'Paul'])
        # Jeanne valide sa partie : Paul est encore connecte, rien n'est envoye
        first.push({'type': 'validate'})
        self.wait(lambda: any(m['type'] == 'validate' for m in first.sent))
        answer = next(m for m in first.sent if m['type'] == 'validate')
        self.assertEqual(answer, {'type': 'validate', 'submit': False, 'waiting': ['Paul']})
        # Paul valide en dernier : c'est lui qui envoie la verif, sans la cle du lot
        second.push({'type': 'validate'})
        self.wait(lambda: any(m['type'] == 'validate' for m in second.sent))
        self.assertTrue(next(m for m in second.sent if m['type'] == 'validate')['submit'])
        with mock.patch.object(self.live, 'hub', self.hub):
            code, report = self.call('POST', '/api/verifs/', {
                'lots': [{'id': self.lot.id}], 'items': [item.iid for item in items], 'user': self.paul,
                'live': {'id': joined['id'], 'pid': joined['pid'], 'token': joined['token']}}, local=False)
        self.assertEqual(code, 200, report)
        self.assertTrue(report['complete'])
        self.assertEqual(report['participants'], ['Paul', 'Jeanne'])
        verif = Verifs.objects.get(lot=self.lot)
        self.assertEqual((verif.by, verif.participants), ('M:M002', ['M:M001']))
        # le compte rendu arrive chez Jeanne, la verif est terminee pour le suivi
        self.wait(lambda: any(m['type'] == 'report' for m in first.sent))
        self.assertTrue(next(m for m in first.sent if m['type'] == 'report')['report']['complete'])
        self.assertFalse(any(m['type'] == 'report' for m in second.sent))
        self.assertTrue(self.hub.sessions[hello['id']].done['complete'])
        code, rows = self.call('POST', '/api/verifs/recent/', {'user': self.badge}, local=False)
        self.assertEqual((rows[0]['by'], rows[0]['participants']), ('Paul', ['Jeanne']))
        for socket in (first, second):
            socket.push(None)

    def test_team_label_counts_for_the_last_participant(self) -> None:
        """Sans etiquette cle : l'etiquette publique scannee par Jeanne vaut pour la verif envoyee par Paul."""
        from unittest import mock
        first, second, hello, joined, items = self.join_team(keyed=False)
        payload = {'lots': [{'id': self.lot.id}], 'items': [item.iid for item in items], 'user': self.paul}
        code, body = self.call('POST', '/api/verifs/', payload, local=False)
        self.assertEqual(code, 403)
        self.assertTrue(body['label_required'])
        with mock.patch.object(self.live, 'hub', self.hub):
            code, report = self.call('POST', '/api/verifs/', dict(payload, live={
                'id': joined['id'], 'pid': joined['pid'], 'token': joined['token']}), local=False)
        self.assertEqual(code, 200, report)
        for socket in (first, second):
            socket.push(None)

    def test_validated_participant_sends_once_the_others_left(self) -> None:
        first, second, hello, joined, items = self.join_team()
        first.push({'type': 'validate', 'partial': True})
        self.wait(lambda: any(m['type'] == 'validate' for m in first.sent))
        second.push(None)  # Paul ferme la page sans valider
        session = self.hub.sessions[hello['id']]
        paul = session.participants[joined['pid']]
        self.wait(lambda: paul.socket is None)
        self.hub.reap(paul.lost + 1)  # encore dans le delai
        self.assertFalse(any(m['type'] == 'submit' for m in first.sent))
        self.hub.reap(paul.lost + self.live.GRACE + 1)
        self.assertEqual(first.sent[-1], {'type': 'submit', 'partial': True})
        self.assertEqual(session.submitting, hello['pid'])
        self.hub.reap(paul.lost + self.live.GRACE + 2)  # une seule demande
        self.assertEqual(sum(m['type'] == 'submit' for m in first.sent), 1)
        first.push(None)

    def test_checks_of_the_same_global_lot_are_merged(self) -> None:
        sub_a = Lots(lot_type=self.lot_type, name='Armoire 1', name_short='A1', created_by='test', parent=self.lot)
        sub_a.save()
        sub_b = Lots(lot_type=self.lot_type, name='Armoire 2', name_short='A2', created_by='test', parent=self.lot)
        sub_b.save()

        def start(hello: dict[str, Any], lot: Lots, key: str | None) -> FakeSocket:
            socket = FakeSocket(hello)
            self.run_socket(self.hub.run_scanner, socket)
            self.wait(lambda: socket.sent)
            socket.push({'type': 'state', 'lot': {'id': lot.id, 'name': lot.name}, 'root': self.lot.id,
                         'keys': [{'id': lot.id, 'key': key}]})
            return socket

        first = start({'type': 'hello', 'user': self.badge}, sub_a, 'cle-a')
        self.wait(lambda: self.hub.visible())
        # quelqu'un commence la vérif d'un autre sous-lot du meme lot : il rejoint la vérif commune
        second = start({'type': 'hello', 'name': 'Paul Martin'}, sub_b, 'cle-b')
        self.wait(lambda: any(m['type'] == 'hello' and m.get('merged') for m in second.sent))
        merged = next(m for m in second.sent if m.get('merged'))
        self.assertEqual(merged['id'], first.sent[0]['id'])
        self.assertEqual(merged['who'], 'Jeanne')
        self.assertEqual(len(self.hub.sessions), 1)
        session = self.hub.sessions[merged['id']]
        self.assertEqual(list(session.lots), [sub_a.id, sub_b.id])
        self.wait(lambda: any(m['type'] == 'team' and len(m['participants']) == 2 for m in first.sent))
        # la connexion de Paul suit la vérif commune
        second.push({'type': 'validate'})
        self.wait(lambda: any(m['type'] == 'validate' for m in second.sent))
        self.assertTrue(session.participants[merged['pid']].validated)
        # « Quitter » : nouvelle vérif seule, jamais fusionnee
        third = start({'type': 'hello', 'name': 'Luc', 'solo': True}, sub_b, '')
        self.wait(lambda: any(other.state for other in list(self.hub.sessions.values()) if other is not session))
        self.assertEqual(len(self.hub.sessions), 2)
        self.assertFalse(any(m.get('merged') for m in third.sent))
        for socket in (first, second, third):
            socket.push(None)

    def start_guest_team(self, verified: bool = True
                        ) -> tuple[tuple[FakeSocket, FakeSocket], dict[str, Any], list[Items]]:
        """Jeanne (badge, sinon un premier invite) commence la vérif du lot ; un invite sans compte la rejoint en
        commencant la vérif du meme lot (fusion). Renvoie (sockets, hello de l'invite, items)."""
        items = self.create(self.compresses, self.today + timedelta(days=200), 2)

        def start(hello: dict[str, Any], item: Items) -> FakeSocket:
            socket = FakeSocket(hello)
            self.run_socket(self.hub.run_scanner, socket)
            self.wait(lambda: socket.sent)
            entry = {'code': item.iid, 'kind': 'item', 'iid': item.iid, 'info': {'type_name': 'Compresses'}}
            socket.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'root': self.lot.id,
                         'entries': [entry], 'keys': [{'id': self.lot.id, 'key': self.lot.verif_key}]})
            return socket

        first = start({'type': 'hello', 'user': self.badge} if verified else {'type': 'hello', 'guest': True}, items[0])
        self.wait(lambda: self.hub.visible())
        second = start({'type': 'hello', 'guest': True, 'name': ''}, items[1])
        self.wait(lambda: any(m['type'] == 'hello' and m.get('merged') for m in second.sent))
        return (first, second), next(m for m in second.sent if m.get('merged')), items

    def test_guest_sends_the_team_check_in_the_name_of_a_connected_rescuer(self) -> None:
        from unittest import mock
        sockets, hello, items = self.start_guest_team()
        session = self.hub.sessions[hello['id']]
        self.assertEqual(sorted(p['who'] for p in session.people()), ['Invité', 'Jeanne'])
        live = {'id': hello['id'], 'pid': hello['pid'], 'token': hello['token']}
        with mock.patch.object(self.live, 'hub', self.hub):
            code, report = self.call('POST', '/api/verifs/', {
                'lots': [{'id': self.lot.id}], 'items': [item.iid for item in items], 'guest': True, 'name': ' Lucas ',
                'live': live}, local=False)
        self.assertEqual(code, 200, report)
        verif = Verifs.objects.get(lot=self.lot)
        self.assertEqual((verif.by, verif.participants), ('M:M001', ['D:Lucas (invité)']))
        self.assertEqual(report['participants'], ['Jeanne', 'Lucas (invité)'])
        for socket in sockets:
            socket.push(None)

    def test_team_check_needs_a_connected_rescuer(self) -> None:
        from unittest import mock
        sockets, hello, items = self.start_guest_team(verified=False)
        live = {'id': hello['id'], 'pid': hello['pid'], 'token': hello['token']}
        with mock.patch.object(self.live, 'hub', self.hub):
            code, body = self.call('POST', '/api/verifs/', {
                'lots': [{'id': self.lot.id}], 'items': [item.iid for item in items], 'guest': True, 'live': live},
                local=False)
        self.assertEqual(code, 403)
        self.assertTrue(body['login_required'])
        self.assertFalse(Verifs.objects.filter(lot=self.lot).exists())
        # abandonnee sans personne : pas enregistree non plus par le serveur
        session = self.hub.sessions[hello['id']]
        for participant in session.participants.values():
            participant.validated = True
        self.assertFalse(self.hub.save_unattended(session))
        for socket in sockets:
            socket.push(None)

    def test_guest_identity(self) -> None:
        from ..idendity import guest_identity, declared_short_name
        self.assertEqual(guest_identity(''), 'D:Invité')
        self.assertEqual(guest_identity('  invité '), 'D:Invité')
        self.assertEqual(guest_identity('Jean  Martin'), 'D:Jean Martin (invité)')
        self.assertEqual(declared_short_name('Jean Martin (invité)'), 'Jean M. (invité)')

    def test_same_person_on_two_pages_is_shown_once(self) -> None:
        """Meme utilisateur sur deux pages (etiquette cle ouverte avec l'appareil photo dans un nouvel onglet) : les
        vérifs du meme lot global sont fusionnees, mais il n'apparait qu'une fois ; sa validation vaut pour les deux."""
        def start(level: Lots) -> FakeSocket:
            socket = FakeSocket({'type': 'hello', 'user': self.badge})
            self.run_socket(self.hub.run_scanner, socket)
            self.wait(lambda: socket.sent)
            socket.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'root': self.lot.id,
                         'cascade': True, 'level': {'id': self.lot.id, 'name': level}, 'entries': []})
            return socket

        first = start('Armoire 1')
        self.wait(lambda: self.hub.visible())
        second = start('Armoire 2')
        self.wait(lambda: any(m.get('merged') for m in second.sent))
        session = next(iter(self.hub.sessions.values()))
        self.assertEqual(len(session.participants), 2)
        self.wait(lambda: self.hub.visible() and len(self.hub.visible()[0]['participants']) == 1)
        row = self.hub.visible()[0]
        self.assertEqual((row['who'], row['participants'][0]['who']), ('Jeanne', 'Jeanne'))
        self.wait(lambda: any(m['type'] == 'team' and len(m['participants']) == 1 for m in second.sent))
        team = [m for m in second.sent if m['type'] == 'team'][-1]
        self.assertEqual([(p['who'], p['me']) for p in team['participants']], [('Jeanne', True)])
        second.push({'type': 'validate'})
        self.wait(lambda: any(m['type'] == 'validate' for m in second.sent))
        answer = next(m for m in second.sent if m['type'] == 'validate')
        self.assertEqual((answer['submit'], answer['waiting']), (True, []))
        self.assertTrue(all(p.validated for p in session.participants.values()))
        for socket in (first, second):
            socket.push(None)

    def test_cascade_step_keeps_the_team_check(self) -> None:
        """Vérif en cascade a deux : Jeanne valide son armoire, la vérif commune continue pour Paul."""
        from unittest import mock
        sub_a = Lots(lot_type=self.lot_type, name='Armoire 1', name_short='A1', created_by='test', parent=self.lot)
        sub_a.save()
        sub_b = Lots(lot_type=self.lot_type, name='Armoire 2', name_short='A2', created_by='test', parent=self.lot)
        sub_b.save()
        mine = self.create(self.compresses, self.today + timedelta(days=200), 2)
        theirs = self.create(self.compresses, self.today + timedelta(days=200), 1)

        def start(hello: dict[str, Any], level: Lots, entries: list[Any], keys: list[Any]) -> FakeSocket:
            socket = FakeSocket(hello)
            self.run_socket(self.hub.run_scanner, socket)
            self.wait(lambda: socket.sent)
            socket.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'root': self.lot.id,
                         'cascade': True, 'level': {'id': level.id, 'name': level.name}, 'keys': keys,
                         'entries': [{'code': item.iid, 'kind': 'item', 'iid': item.iid, 'place': level.id}
                                     for item in entries]})
            return socket

        first = start({'type': 'hello', 'user': self.badge}, sub_a, mine,
                      [{'id': self.lot.id, 'key': self.lot.verif_key}])
        self.wait(lambda: self.hub.visible())
        second = start({'type': 'hello', 'name': 'Paul Martin'}, sub_b, theirs, [])
        self.wait(lambda: any(m.get('merged') for m in second.sent))
        merged = next(m for m in second.sent if m.get('merged'))
        session = self.hub.sessions[merged['id']]
        self.wait(lambda: len(session.participants) == 2 and all(p.entries for p in session.participants.values()))
        hello = first.sent[0]
        # Jeanne valide l'armoire 1 sans cle : celle du lot global, gardee par le serveur, suffit
        with mock.patch.object(self.live, 'hub', self.hub):
            code, report = self.call('POST', '/api/verifs/', {
                'lots': [{'id': sub_a.id}], 'items': [item.iid for item in mine], 'user': self.badge, 'step': True,
                'live': {'id': hello['id'], 'pid': hello['pid'], 'token': hello['token']}}, local=False)
        self.assertEqual(code, 200, report)
        self.assertEqual(report['participants'], ['Jeanne'])  # Paul n'a rien scanne dans l'armoire 1
        self.assertIsNone(session.done)  # la vérif commune continue
        self.assertEqual(set(session.steps), {sub_a.id})
        self.assertEqual(session.steps[sub_a.id]['by'], 'Jeanne')
        self.assertEqual(session.participants[hello['pid']].entries, [])
        self.assertEqual(len(session.participants[merged['pid']].entries), 1)
        self.wait(lambda: any(m['type'] == 'step' for m in second.sent))
        step = next(m for m in second.sent if m['type'] == 'step')
        self.assertEqual(([row['id'] for row in step['lots']], step['by']), ([sub_a.id], 'Jeanne'))
        self.wait(lambda: any(m['type'] == 'team' and sub_a.id in m['done'] for m in second.sent))
        team = [m for m in second.sent if m['type'] == 'team'][-1]
        self.assertEqual({row['who']: (row['level'] or {}).get('name') for row in team['participants']},
                         {'Jeanne': 'Armoire 1', 'Paul Martin': 'Armoire 2'})
        # Jeanne a fini : elle quitte la vérif, Paul continue seul ; abandonnee, rien de plus n'est enregistre par le
        # serveur, la vérif s'affiche terminee avec les lots deja valides
        first.push({'type': 'leave'})
        self.wait(lambda: len(session.participants) == 1)
        self.assertIn(session.id, self.hub.sessions)
        second.push(None)
        self.wait(lambda: not session.connected)
        self.hub.reap(session.lost + self.live.LOST_KEEP + 1)
        self.assertEqual(Verifs.objects.filter(lot=sub_b).count(), 0)
        self.assertEqual((session.done['lots'], session.done['complete']), (1, True))
        self.assertEqual(self.hub.visible()[0]['done']['lots'], 1)
        self.hub.reap(session.finished + self.live.DONE_KEEP + 1)
        self.assertNotIn(session.id, self.hub.sessions)

    def test_cascade_closes_when_the_global_lot_is_validated(self) -> None:
        """Vérif en cascade : valider le lot global termine la vérif en direct, meme si un participant deconnecte y
        est encore (page fermee, ancienne vérif fusionnee) ; elle n'apparait plus en cours dans le suivi."""
        from unittest import mock
        sub_a = Lots(lot_type=self.lot_type, name='Armoire 1', name_short='A1', created_by='test', parent=self.lot)
        sub_a.save()
        mine = self.create(self.compresses, self.today + timedelta(days=200), 2)
        stale = FakeSocket({'type': 'hello', 'name': 'Paul Martin'})
        self.run_socket(self.hub.run_scanner, stale)
        self.wait(lambda: stale.sent)
        stale.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'root': self.lot.id, 'cascade': True,
                    'entries': []})
        self.wait(lambda: self.hub.visible())
        stale.push(None)
        self.wait(lambda: not self.hub.visible()[0]['connected'])
        scanner = FakeSocket({'type': 'hello', 'user': self.badge})
        self.run_socket(self.hub.run_scanner, scanner)
        self.wait(lambda: scanner.sent)
        scanner.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'root': self.lot.id, 'cascade': True,
                      'level': {'id': self.lot.id, 'name': 'Sac A'}, 'keys': [{'id': self.lot.id, 'key': self.lot.verif_key}],
                      'entries': [{'code': item.iid, 'kind': 'item', 'iid': item.iid, 'place': sub_a.id} for item in mine]})
        self.wait(lambda: any(m.get('merged') for m in scanner.sent))
        hello = next(m for m in scanner.sent if m.get('merged'))
        session = self.hub.sessions[hello['id']]
        self.assertEqual(len(session.participants), 2)
        with mock.patch.object(self.live, 'hub', self.hub):
            code, report = self.call('POST', '/api/verifs/', {
                'lots': [{'id': sub_a.id}], 'items': [item.iid for item in mine], 'user': self.badge, 'step': True,
                'live': {'id': hello['id'], 'pid': hello['pid'], 'token': hello['token']}}, local=False)
            self.assertEqual(code, 200, report)
            self.assertIsNone(session.done)
            # lot global valide (rien a y scanner directement) : la vérif est terminee pour tous
            code, report = self.call('POST', '/api/verifs/', {
                'lots': [{'id': self.lot.id}], 'exclude': [sub_a.id], 'user': self.badge, 'step': True, 'finish': True,
                'live': {'id': hello['id'], 'pid': hello['pid'], 'token': hello['token']}}, local=False)
        self.assertEqual(code, 200, report)
        self.assertEqual(session.done['lots'], 2)
        # une verif par etape : le compte rendu du suivi les reunit
        self.assertEqual(session.done['verifs'], list(Verifs.objects.filter(lot__in=[sub_a, self.lot])
                                                      .order_by('id').values_list('id', flat=True)))
        self.assertEqual(len(session.done['verifs']), 2)
        self.wait(lambda: self.hub.visible() and self.hub.visible()[0]['done'])
        scanner.push({'type': 'leave'})  # le message de la page arrive ensuite : sans effet
        scanner.push(None)
        self.assertEqual(self.hub.visible()[0]['done']['lots'], 2)

    def test_cascade_leave_without_validated_lot_drops_the_check(self) -> None:
        """Dernier participant connecte qui quitte une vérif en cascade sans rien valider : elle disparait du suivi."""
        scanner = FakeSocket({'type': 'hello', 'user': self.badge})
        self.run_socket(self.hub.run_scanner, scanner)
        self.wait(lambda: scanner.sent)
        scanner.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'root': self.lot.id, 'cascade': True,
                      'entries': []})
        self.wait(lambda: self.hub.visible())
        scanner.push({'type': 'leave'})
        self.wait(lambda: not self.hub.sessions)

    def lonely_check(self, validate: bool, remaining: int) -> tuple[LiveSession, list[str]]:
        """Verif d'un seul participant qui ferme sa page ; renvoie la session apres le delai d'abandon."""
        items = self.create(self.compresses, self.today + timedelta(days=200), 2)
        scanner = FakeSocket({'type': 'hello', 'user': self.badge})
        thread = self.run_socket(self.hub.run_scanner, scanner)
        self.wait(lambda: scanner.sent)
        hello = scanner.sent[0]
        scanner.push({'type': 'state', 'lot': {'id': self.lot.id, 'name': 'Sac A'}, 'count': 2, 'remaining': remaining,
                      'entries': [{'code': item.iid, 'kind': 'item', 'iid': item.iid} for item in items],
                      'keys': [{'id': self.lot.id, 'key': self.lot.verif_key}]})
        if validate:
            scanner.push({'type': 'validate'})
        scanner.push(None)
        thread.join(5)
        session = self.hub.sessions[hello['id']]
        self.assertEqual(self.hub.reap(session.lost + 60), [])  # rechargement de la page possible
        return session, self.hub.reap(session.lost + self.live.LOST_KEEP + 1)

    def test_abandoned_check_is_dropped(self) -> None:
        session, expired = self.lonely_check(validate=False, remaining=3)
        self.assertEqual(expired, [session.id])
        self.assertFalse(Verifs.objects.exists())

    def test_validated_check_without_participants_is_saved(self) -> None:
        session, expired = self.lonely_check(validate=True, remaining=3)
        self.assertEqual(expired, [])
        verif = Verifs.objects.get(lot=self.lot)
        self.assertEqual((verif.by, verif.present_count), ('M:M001', 2))
        self.assertIsNotNone(session.done)
        # affichee comme terminee DONE_KEEP a partir de son enregistrement
        self.assertEqual(self.hub.reap(session.finished + 60), [])
        self.assertEqual(self.hub.reap(session.finished + self.live.DONE_KEEP + 1), [session.id])

    def test_complete_check_without_participants_is_saved(self) -> None:
        session, expired = self.lonely_check(validate=False, remaining=0)
        self.assertEqual(expired, [])
        self.assertTrue(Verifs.objects.get(lot=self.lot).complete)

    def test_state_is_bounded(self) -> None:
        state = self.live.clean_state({'lot': {'id': 'x' * 50, 'name': 'n' * 500}, 'count': -3, 'remaining': 'a',
                                       'scanned': [{'name': 'i'}] * 1000, 'lots': ['a'] * 100})
        self.assertEqual(len(state['lot']['id']), 16)
        self.assertEqual(len(state['lot']['name']), 64)
        self.assertEqual((state['count'], state['remaining']), (0, 0))
        self.assertEqual(len(state['scanned']), self.live.MAX_SCANNED)
        self.assertEqual(len(state['lots']), 20)
        self.assertIsNone(self.live.clean_state({'lot': None}))

    def test_cascade_progress_is_public(self) -> None:
        """Vérif en cascade : count/remaining sont l'avancement de tout le lot, la page de suivi le sait (Avancement)."""
        session, participant = self.live.LiveSession(), self.live.Participant()
        participant.state = self.live.clean_state({'lot': {'id': 'a', 'name': 'B+'}, 'cascade': True, 'count': 26, 'remaining': 4})
        session.participants['x'] = participant
        public = session.public()
        self.assertTrue(public['cascade'])
        self.assertEqual((public['count'], public['remaining']), (26, 4))


class RawWebSocket:
    """Client WebSocket minimal pour les tests (trames texte masquees)."""

    def __init__(self, host: str, port: int, path: str, headers: dict[str, str] | None = None) -> None:
        import base64
        import os
        import socket
        self.sock = socket.create_connection((host, port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        extra = ''.join(f'{name}: {value}\r\n' for name, value in (headers or {}).items())
        self.sock.sendall((f'GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n'
                           f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n'
                           f'{extra}\r\n').encode())
        head = b''
        while b'\r\n\r\n' not in head:
            chunk = self.sock.recv(1)
            if not chunk:
                break
            head += chunk
        self.status = int(head.split(b' ')[1]) if head else 0
        self.last = None

    def send(self, message: dict[str, Any]) -> None:
        import os
        payload = json.dumps(message).encode()
        mask = os.urandom(4)
        header = bytes([0x81, 0x80 | len(payload)]) if len(payload) < 126 else \
            bytes([0x81, 0x80 | 126]) + len(payload).to_bytes(2, 'big')
        self.sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def _exact(self, size: int) -> bytes:
        data = b''
        while len(data) < size:
            chunk = self.sock.recv(size - len(data))
            if not chunk:
                raise ConnectionError('fermee')
            data += chunk
        return data

    def receive(self) -> dict[str, Any] | None:
        while True:
            first, second = self._exact(2)
            length = second & 0x7F
            if length == 126:
                length = int.from_bytes(self._exact(2), 'big')
            payload = self._exact(length)
            if first & 0x0F == 0x1:
                self.last = json.loads(payload)
                return self.last
            if first & 0x0F == 0x8:
                raise ConnectionError('fermee par le serveur')

    def close(self) -> None:
        self.sock.sendall(bytes([0x88, 0x80]) + b'\0\0\0\0')
        self.sock.close()
