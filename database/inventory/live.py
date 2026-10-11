# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          live.py
#       -\-    _|__
#        |\___/  . \        Created on 11 Oct. 2026 at 10:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Suivi des vérifs en direct : relais WebSocket entre les telephones qui scannent et ceux qui suivent.

    telephone qui verifie --(wss /ws/live/scanner)--> serveur --(wss /ws/live/watch)--> chef de poste

Chaque page de scan publie l'etat de sa verif en cours (lot, items scannes, reste a scanner) ; le serveur le
garde en memoire et le diffuse aux pages de suivi, qui exigent un badge valide. Une verif validee reste
affichee quelques minutes comme terminee ; les verifs passees viennent de la base (POST /api/verifs/recent/).
Les sessions vivent en memoire, dans le processus de `manage.py serve` (comme le telephone-douchette).

Messages (JSON, trames texte) :
  scanner -> serveur : {"type": "hello", "id": "...", "token": "...", "user": {...} | "name": "..."},
                       {"type": "state", "lot": {"id", "name"}, "lots": [...], "scanned": [...], "count": n,
                        "remaining": n}, {"type": "done", "complete": bool, "partial": bool, "present": n},
                       {"type": "cancel"}, {"type": "ping"}
  serveur -> scanner : {"type": "hello", "id": "...", "token": "..."}
  suivi -> serveur   : {"type": "hello", "user": {"matricule", "key"}}, {"type": "ping"}
  serveur -> suivi   : {"type": "snapshot", "sessions": [...]}, {"type": "update", "session": {...}},
                       {"type": "remove", "id": "..."}, {"type": "error", "message": "..."}
"""

import hmac
import json
import secrets
import threading
import time

from django.db import close_old_connections
from django.utils import timezone

from .remote_scanner import ALPHABET, WebSocketClosed

MAX_SESSIONS = 200          # verifs suivies en meme temps (au-dela, les nouvelles ne sont pas publiees)
MAX_SCANNED = 150           # derniers scans gardes par verif
LOST_KEEP = 15 * 60         # verif dont le telephone est deconnecte : gardee (rechargement de la page)
DONE_KEEP = 10 * 60         # verif terminee : encore affichee comme terminee
REAPER_PERIOD = 10.0


def _text(value, size):
    return str(value or '')[:size]


def _number(value):
    try:
        return max(0, min(100000, int(value)))
    except (TypeError, ValueError):
        return 0


def public_name(identity):
    """Nom affiche dans le suivi (identite stockee M:matricule ou D:nom), anonymise comme sur l'API publique."""
    from .idendity import anonymized, identity_name
    try:
        with anonymized():
            return identity_name(identity)
    finally:
        close_old_connections()


def badge_identity(credentials):
    """Identite stockee d'un badge valide ({"matricule", "key"}), None sinon. Acces base : appele hors verrou."""
    from .models import Secouristes
    from .idendity import SOURCE_VERIFIED
    if not isinstance(credentials, dict):
        return None
    try:
        user = Secouristes.objects.filter(matricule=str(credentials.get('matricule', ''))[:16]).first()
        if user is None or not user.active or not user.check_key(credentials.get('key')):
            return None
        return f'{SOURCE_VERIFIED}:{user.matricule}'
    finally:
        close_old_connections()


def scanner_identity(message):
    """Nom affiche d'une page de scan : badge verifie, sinon nom declare (marque comme tel), sinon personne."""
    from .idendity import SOURCE_DECLARED
    identity = badge_identity(message.get('user'))
    if identity:
        return public_name(identity), True
    name = _text(message.get('name'), 30).strip()
    if name:
        return public_name(f'{SOURCE_DECLARED}:{name}'), False
    return '', False


class LiveSession:
    def __init__(self):
        self.id = ''.join(secrets.choice(ALPHABET) for _ in range(12))
        self.token = ''.join(secrets.choice(ALPHABET) for _ in range(24))
        self.who = ''
        self.verified = False
        self.started = timezone.now()
        self.updated = self.started
        self.state = None          # dernier etat publie (None : rien a montrer)
        self.done = None           # {"complete", "partial", "present", "at"} une fois validee
        self.socket = None
        self.lost = time.monotonic()

    def public(self):
        state = self.state or {}
        return {
            'id': self.id,
            'who': self.who,
            'verified': self.verified,
            'started': self.started.isoformat(timespec='seconds'),
            'updated': self.updated.isoformat(timespec='seconds'),
            'connected': self.socket is not None,
            'lot': state.get('lot'),
            'lots': state.get('lots', []),
            'scanned': state.get('scanned', []),
            'count': state.get('count', 0),
            'remaining': state.get('remaining', 0),
            'done': self.done,
        }


def clean_state(message):
    """Etat publie par une page de scan, borne et nettoye (rien n'est cru : c'est de l'affichage)."""
    lot = message.get('lot')
    if not isinstance(lot, dict) or not lot.get('id'):
        return None
    scanned = []
    for entry in (message.get('scanned') if isinstance(message.get('scanned'), list) else [])[-MAX_SCANNED:]:
        if isinstance(entry, dict):
            scanned.append({'name': _text(entry.get('name'), 64), 'expired': bool(entry.get('expired')),
                            'error': bool(entry.get('error')), 'extra': bool(entry.get('extra'))})
    lots = message.get('lots') if isinstance(message.get('lots'), list) else []
    return {
        'lot': {'id': _text(lot.get('id'), 16), 'name': _text(lot.get('name'), 64)},
        'lots': [_text(name, 64) for name in lots[:20]],
        'scanned': scanned,
        'count': _number(message.get('count')),
        'remaining': _number(message.get('remaining')),
    }


class LiveHub:
    """Verifs en cours et pages de suivi, en memoire, partagees par les serveurs du meme processus."""

    def __init__(self):
        self.lock = threading.RLock()
        self.sessions = {}
        self.watchers = set()
        self._reaper = None

    # -- diffusion -----------------------------------------------------------------------------------------------------

    def visible(self):
        with self.lock:
            return [session.public() for session in self.sessions.values() if session.state or session.done]

    def broadcast(self, message):
        with self.lock:
            watchers = list(self.watchers)
        for websocket in watchers:
            websocket.send_json(message)

    def _changed(self, session):
        if session.state or session.done:
            self.broadcast({'type': 'update', 'session': session.public()})
        else:
            self.broadcast({'type': 'remove', 'id': session.id})

    def remove(self, session_id):
        with self.lock:
            session = self.sessions.pop(session_id, None)
        if session is not None:
            self.broadcast({'type': 'remove', 'id': session_id})
        return session is not None

    # -- ramasseur -----------------------------------------------------------------------------------------------------

    def reap(self, now=None):
        """Retire les verifs terminees depuis DONE_KEEP et celles dont le telephone a disparu depuis LOST_KEEP."""
        now = time.monotonic() if now is None else now
        expired = []
        with self.lock:
            for session in self.sessions.values():
                if session.socket is not None:
                    continue
                keep = DONE_KEEP if session.done else LOST_KEEP
                if now - session.lost > keep:
                    expired.append(session.id)
        for session_id in expired:
            self.remove(session_id)
        return expired

    def _start_reaper(self):
        with self.lock:
            if self._reaper is None or not self._reaper.is_alive():
                self._reaper = threading.Thread(target=self._reap_loop, daemon=True, name='live-reaper')
                self._reaper.start()

    def _reap_loop(self):
        while True:
            time.sleep(REAPER_PERIOD)
            try:
                self.reap()
            except Exception:  # le ramasseur ne doit jamais mourir
                pass
            with self.lock:
                if not self.sessions:
                    self._reaper = None
                    return

    # -- page de scan --------------------------------------------------------------------------------------------------

    def attach_scanner(self, message, websocket):
        """Reprend la session {"id", "token"} si elle existe encore (rechargement de la page), sinon en cree une."""
        who, verified = scanner_identity(message)
        with self.lock:
            session = self.sessions.get(_text(message.get('id'), 16))
            if session is None or not hmac.compare_digest(session.token.encode(), _text(message.get('token'), 32).encode()):
                if len(self.sessions) >= MAX_SESSIONS:
                    return None
                session = LiveSession()
                self.sessions[session.id] = session
            previous = session.socket
            session.socket = websocket
            session.who = who
            session.verified = verified
        if previous is not None and previous is not websocket:
            previous.close()
        self._start_reaper()
        return session

    def run_scanner(self, websocket):
        session = None
        try:
            hello = self._parse(websocket.receive())
            if hello.get('type') != 'hello':
                return
            session = self.attach_scanner(hello, websocket)
            if session is None:
                websocket.send_json({'type': 'error', 'message': 'trop de vérifs suivies en même temps'})
                return
            websocket.send_json({'type': 'hello', 'id': session.id, 'token': session.token})
            self._changed(session)
            while True:
                message = self._parse(websocket.receive())
                kind = message.get('type')
                if kind == 'state':
                    with self.lock:
                        session.state = clean_state(message)
                        session.done = None
                        session.updated = timezone.now()
                    self._changed(session)
                elif kind == 'identity':
                    who, verified = scanner_identity(message)
                    with self.lock:
                        session.who, session.verified = who, verified
                    self._changed(session)
                elif kind == 'done':
                    with self.lock:
                        session.done = {'complete': bool(message.get('complete')), 'partial': bool(message.get('partial')),
                                        'present': _number(message.get('present')),
                                        'at': timezone.now().isoformat(timespec='seconds')}
                        session.updated = timezone.now()
                    self._changed(session)
                elif kind == 'cancel':
                    with self.lock:
                        session.state = None
                        session.done = None
                    self._changed(session)
        except WebSocketClosed:
            pass
        finally:
            if session is not None:
                with self.lock:
                    current = session.socket is websocket
                    if current:
                        session.socket = None
                        session.lost = time.monotonic()
                if current:
                    self._changed(session)

    # -- page de suivi -------------------------------------------------------------------------------------------------

    def run_watcher(self, websocket):
        try:
            hello = self._parse(websocket.receive())
            if hello.get('type') != 'hello' or not badge_identity(hello.get('user')):
                websocket.send_json({'type': 'error', 'message': 'Scannez votre badge pour suivre les vérifs.',
                                     'login_required': True})
                return
            with self.lock:
                self.watchers.add(websocket)
            websocket.send_json({'type': 'snapshot', 'sessions': self.visible()})
            while True:
                websocket.receive()  # pings du client
        except WebSocketClosed:
            pass
        finally:
            with self.lock:
                self.watchers.discard(websocket)

    @staticmethod
    def _parse(text):
        try:
            message = json.loads(text)
        except ValueError:
            return {}
        return message if isinstance(message, dict) else {}


hub = LiveHub()
