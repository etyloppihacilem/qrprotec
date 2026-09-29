# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          remote_scanner.py
#       -\-    _|__
#        |\___/  . \        Created on 30 Sep. 2026 at 14:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Telephone utilise comme douchette : relais WebSocket entre la page de scan du telephone et le front.

    telephone --(wss, API publique)--> serveur --(ws, API locale)--> front ImGui

Le front cree une session (POST /api/remote-scanner/) et affiche un QR code vers
<base>/scanner?s=ID&k=CLE. La page du telephone ouvre /ws/scanner/phone?s=ID&k=CLE, le front
/ws/scanner/front?s=ID (API locale uniquement). Les codes scannes par le telephone sont relayes au front,
qui les traite comme un scan de douchette ; le front peut renvoyer un signal d'erreur (produit perime,
code inconnu) que le telephone restitue (flash rouge, vibration).

Une session se ferme quand le telephone (ou le front) reste deconnecte plus de `timeout` secondes : il
faut alors generer un nouveau QR code. Les sessions vivent en memoire, dans le processus de
`manage.py serve` qui sert les deux API (les WebSockets sont geres par cette commande, voir
inventory/management/commands/serve.py).

Messages (JSON, trames texte) :
  telephone -> serveur : {"type": "scan", "code": "...", "id": n}, {"type": "ping"}
  serveur -> telephone : {"type": "hello", ...}, {"type": "ack", "id": n, "ok": bool, "message": "..."},
                         {"type": "feedback", "result": "error", "message": "..."},
                         {"type": "front", "connected": bool}, {"type": "closed", "reason": "..."}
  front -> serveur     : {"type": "feedback", ...}, {"type": "close"}, {"type": "ping"}
  serveur -> front     : {"type": "hello", ...}, {"type": "scan", "code": "...", "id": n},
                         {"type": "phone", "connected": bool, "agent": "..."}, {"type": "closed", ...}
"""

import base64
import hashlib
import hmac
import json
import logging
import secrets
import socket
import string
import struct
import threading
import time

logger = logging.getLogger(__name__)

WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
ALPHABET = string.ascii_letters + string.digits
RECEIVE_TIMEOUT = 45.0   # les clients envoient un ping toutes les 15 s
REAPER_PERIOD = 2.0
MIN_TIMEOUT, MAX_TIMEOUT = 60, 24 * 3600
MAX_MESSAGE = 64 * 1024

OP_CONTINUATION, OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA


class WebSocketClosed(Exception):
    pass


def accept_key(key: str) -> str:
    return base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()


class WebSocket:
    """Connexion WebSocket cote serveur (RFC 6455, trames texte, sans extensions)."""

    def __init__(self, rfile, wfile, sock):
        self.rfile = rfile
        self.wfile = wfile
        self.sock = sock
        self.send_lock = threading.Lock()
        self.closed = False

    def _read_exact(self, size):
        data = b''
        while len(data) < size:
            try:
                chunk = self.rfile.read(size - len(data))
            except OSError as exc:  # delai depasse (client muet) ou connexion coupee
                raise WebSocketClosed(str(exc))
            if not chunk:
                raise WebSocketClosed('connexion fermee')
            data += chunk
        return data

    def _send_frame(self, opcode, payload=b''):
        header = bytes([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header += bytes([length])
        elif length < 65536:
            header += bytes([126]) + struct.pack('!H', length)
        else:
            header += bytes([127]) + struct.pack('!Q', length)
        with self.send_lock:
            if self.closed:
                raise WebSocketClosed('connexion fermee')
            try:
                self.wfile.write(header + payload)
                self.wfile.flush()
            except OSError as exc:
                self.closed = True
                raise WebSocketClosed(str(exc))

    def send_json(self, message):
        try:
            self._send_frame(OP_TEXT, json.dumps(message, ensure_ascii=False).encode())
            return True
        except WebSocketClosed:
            return False

    def receive(self):
        """Retourne le prochain message texte (str). Repond aux pings, leve WebSocketClosed a la fermeture."""
        message = b''
        while True:
            first, second = self._read_exact(2)
            opcode = first & 0x0F
            final = bool(first & 0x80)
            masked = bool(second & 0x80)
            length = second & 0x7F
            if length == 126:
                length = struct.unpack('!H', self._read_exact(2))[0]
            elif length == 127:
                length = struct.unpack('!Q', self._read_exact(8))[0]
            if length > MAX_MESSAGE:
                self.close(1009)
                raise WebSocketClosed('message trop long')
            mask = self._read_exact(4) if masked else b''
            payload = self._read_exact(length)
            if masked:
                payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
            if opcode == OP_PING:
                self._send_frame(OP_PONG, payload)
                continue
            if opcode == OP_PONG:
                continue
            if opcode == OP_CLOSE:
                self.close()
                raise WebSocketClosed('fermee par le client')
            if opcode in (OP_TEXT, OP_BINARY, OP_CONTINUATION):
                message += payload
                if len(message) > MAX_MESSAGE:
                    self.close(1009)
                    raise WebSocketClosed('message trop long')
                if final:
                    return message.decode('utf-8', errors='replace')

    def close(self, code=1000):
        if self.closed:
            return
        try:
            self._send_frame(OP_CLOSE, struct.pack('!H', code))
        except WebSocketClosed:
            pass
        self.closed = True
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


class Session:
    def __init__(self, timeout):
        self.id = ''.join(secrets.choice(ALPHABET) for _ in range(12))
        self.key = ''.join(secrets.choice(ALPHABET) for _ in range(24))
        self.timeout = timeout
        now = time.monotonic()
        self.created = time.time()
        self.front = None
        self.phone = None
        self.phone_agent = ''
        self.phone_since = None      # connexion du telephone en cours depuis
        self.phone_lost = now        # telephone absent depuis (creation = jamais connecte)
        self.front_lost = now
        self.scans = 0
        self.closed = False

    def status(self):
        now = time.monotonic()
        return {
            'id': self.id,
            'timeout': self.timeout,
            'phone_connected': self.phone is not None,
            'front_connected': self.front is not None,
            'phone_agent': self.phone_agent,
            'scans': self.scans,
            # secondes avant fermeture si le telephone reste deconnecte (None si connecte)
            'expires_in': None if self.phone is not None else max(0, int(self.timeout - (now - self.phone_lost))),
        }


class Hub:
    """Sessions en memoire, partagees par les serveurs public et local du meme processus."""

    def __init__(self):
        self.lock = threading.RLock()
        self.sessions = {}
        self.enabled = False  # passe a True quand `manage.py serve` gere les WebSockets
        self._reaper = None

    # -- sessions ------------------------------------------------------------------------------------------------------

    def create(self, timeout):
        timeout = max(MIN_TIMEOUT, min(MAX_TIMEOUT, int(timeout)))
        session = Session(timeout)
        with self.lock:
            self.sessions[session.id] = session
        self._start_reaper()
        return session

    def get(self, session_id):
        with self.lock:
            return self.sessions.get(session_id)

    def check_key(self, session_id, key):
        session = self.get(session_id)
        if session is None or not key or not hmac.compare_digest(session.key.encode(), str(key).encode()):
            return None
        return session

    def close(self, session_id, reason):
        with self.lock:
            session = self.sessions.pop(session_id, None)
        if session is None:
            return False
        session.closed = True
        for connection in (session.phone, session.front):
            if connection is not None:
                connection.send_json({'type': 'closed', 'reason': reason})
                connection.close()
        logger.info('Session douchette %s fermee : %s', session.id, reason)
        return True

    def _start_reaper(self):
        with self.lock:
            if self._reaper is None or not self._reaper.is_alive():
                self._reaper = threading.Thread(target=self._reap_loop, daemon=True, name='remote-scanner-reaper')
                self._reaper.start()

    def reap(self, now=None):
        """Ferme les sessions dont le telephone ou le front est deconnecte depuis trop longtemps."""
        now = time.monotonic() if now is None else now
        expired = []
        with self.lock:
            for session in self.sessions.values():
                minutes = max(1, round(session.timeout / 60))
                if session.phone is None and now - session.phone_lost > session.timeout:
                    expired.append((session.id, f'téléphone déconnecté depuis plus de {minutes} min'))
                elif session.front is None and now - session.front_lost > session.timeout:
                    expired.append((session.id, f'poste déconnecté depuis plus de {minutes} min'))
        for session_id, reason in expired:
            self.close(session_id, reason)
        return [session_id for session_id, _ in expired]

    def _reap_loop(self):
        while True:
            time.sleep(REAPER_PERIOD)
            try:
                self.reap()
            except Exception:  # le ramasseur ne doit jamais mourir
                logger.exception('Erreur du ramasseur de sessions douchette')
            with self.lock:
                if not self.sessions:
                    self._reaper = None
                    return

    # -- connexions ----------------------------------------------------------------------------------------------------

    def run_phone(self, session, websocket, agent=''):
        with self.lock:
            if session.closed:
                websocket.send_json({'type': 'closed', 'reason': 'session fermée'})
                return
            previous = session.phone
            session.phone = websocket
            session.phone_agent = agent[:120]
            session.phone_since = time.monotonic()
            front = session.front
        if previous is not None:
            previous.send_json({'type': 'closed', 'reason': 'remplacé par une nouvelle connexion'})
            previous.close()
        websocket.send_json({'type': 'hello', 'role': 'phone', 'front_connected': front is not None,
                             'timeout': session.timeout})
        if front is not None:
            front.send_json({'type': 'phone', 'connected': True, 'agent': session.phone_agent})
        try:
            while True:
                message = self._parse(websocket.receive())
                if message.get('type') == 'scan':
                    self._relay_scan(session, websocket, message)
        except WebSocketClosed:
            pass
        finally:
            with self.lock:
                current = session.phone is websocket
                if current:
                    session.phone = None
                    session.phone_since = None
                    session.phone_lost = time.monotonic()
                front = session.front
            if current and front is not None and not session.closed:
                front.send_json({'type': 'phone', 'connected': False, 'expires_in': session.timeout})

    def _relay_scan(self, session, websocket, message):
        code = str(message.get('code', '')).strip()[:2048]
        scan_id = message.get('id')
        if not code:
            return
        with self.lock:
            front = session.front
        delivered = front is not None and front.send_json({'type': 'scan', 'code': code, 'id': scan_id})
        if delivered:
            session.scans += 1
        websocket.send_json({'type': 'ack', 'id': scan_id, 'ok': delivered,
                             'message': 'reçu par le poste' if delivered else 'poste déconnecté : scan non transmis'})

    def run_front(self, session, websocket):
        with self.lock:
            if session.closed:
                websocket.send_json({'type': 'closed', 'reason': 'session fermée'})
                return
            previous = session.front
            session.front = websocket
            phone = session.phone
        if previous is not None:
            previous.send_json({'type': 'closed', 'reason': 'remplacé par une nouvelle connexion'})
            previous.close()
        websocket.send_json({'type': 'hello', 'role': 'front', **session.status()})
        if phone is not None:
            phone.send_json({'type': 'front', 'connected': True})
        try:
            while True:
                message = self._parse(websocket.receive())
                kind = message.get('type')
                if kind == 'feedback':
                    with self.lock:
                        phone = session.phone
                    if phone is not None:
                        phone.send_json({'type': 'feedback', 'result': str(message.get('result', 'error'))[:16],
                                         'message': str(message.get('message', ''))[:200]})
                elif kind == 'close':
                    self.close(session.id, 'fermée depuis le poste')
                    return
        except WebSocketClosed:
            pass
        finally:
            with self.lock:
                current = session.front is websocket
                if current:
                    session.front = None
                    session.front_lost = time.monotonic()
                phone = session.phone
            if current and phone is not None and not session.closed:
                phone.send_json({'type': 'front', 'connected': False})

    @staticmethod
    def _parse(text):
        try:
            message = json.loads(text)
        except ValueError:
            return {}
        return message if isinstance(message, dict) else {}


hub = Hub()
