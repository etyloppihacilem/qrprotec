# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          live.py
#       -\-    _|__
#        |\___/  . \        Created on 11 Oct. 2026 at 10:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Suivi des vérifs en direct, et vérifs à plusieurs : relais WebSocket entre les telephones qui scannent et ceux qui
suivent.

    telephone qui verifie --(wss /ws/live/scanner)--> serveur --(wss /ws/live/watch)--> chef de poste

Chaque page de scan publie l'etat de sa verif en cours (lot, items scannes, reste a scanner) ; le serveur le
garde en memoire et le diffuse aux pages de suivi, qui exigent un badge valide. Une verif validee reste
affichee quelques minutes comme terminee ; les verifs passees viennent de la base (POST /api/verifs/recent/).
Les sessions vivent en memoire, dans le processus de `manage.py serve` (comme le telephone-douchette).

Vérif à plusieurs : une session (une verif) a un ou plusieurs participants, chacun sur son telephone. On rejoint une
verif en cours depuis la page de suivi (badge exige). Une verif qui commence sur un lot global ou une autre est deja en
cours (meme lot, ou un autre de ses sous-lots) la rejoint automatiquement : les deux verifs sont fusionnees en une
verif commune de tous leurs lots (hello "merged"), sauf pour qui a quitte la verif commune (hello "solo"). Les scans de chacun sont partages avec les autres, qui voient les memes attendus. Chacun valide sa partie ;
le dernier participant connecte qui valide envoie la verif (POST /api/verifs/ avec "live") : tous les participants
sont enregistres sur la verif et le compte rendu est envoye a tous. Les cles des lots (etiquettes privees) restent
sur le serveur : celui qui a rejoint la verif n'en a pas besoin pour l'envoyer.

Messages (JSON, trames texte) :
  scanner -> serveur : {"type": "hello", "id", "pid", "token", "join": id de la verif a rejoindre (facultatif),
                        "solo": bool (nouvelle verif a ne pas fusionner),
                        "user": {...} | "name": "..."},
                       {"type": "state", "lot": {"id", "name"}, "root": id du lot global, "lots": [...],
                        "scanned": [...], "count": n, "remaining": n, "entries": [scans partages],
                        "keys": [{"id", "key"}]},
                       {"type": "validate", "partial": bool}, {"type": "unvalidate"},
                       {"type": "done", "complete": bool, "partial": bool, "present": n},
                       {"type": "identity", ...}, {"type": "cancel"}, {"type": "ping"}
  serveur -> scanner : {"type": "hello", "id", "pid", "token", "merged": bool, "who"}, {"type": "error", "message"},
                       {"type": "team", "participants": [...], "lots": [...], "keyed": [...], "entries": [...],
                        "nearby": [...]},
                       {"type": "validate", "submit": bool, "waiting": [noms]}, {"type": "submit", "partial": bool},
                       {"type": "report", "report": {...}}
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
MAX_PARTICIPANTS = 8        # personnes sur une meme verif
MAX_SCANNED = 150           # derniers scans gardes par verif (affichage du suivi)
MAX_ENTRIES = 300           # scans partages par participant
MAX_LOTS = 20               # lots d'une verif groupee
LOST_KEEP = 15 * 60         # verif dont les telephones sont deconnectes : gardee (rechargement de la page), puis
                            # abandonnee, ou enregistree par le serveur si elle est complete ou si quelqu'un a valide
DONE_KEEP = 10 * 60         # verif terminee : encore affichee comme terminee
GRACE = 20                  # participant deconnecte depuis GRACE s : il ne retient plus l'envoi de la verif commune
REAPER_PERIOD = 10.0


def _text(value, size):
    return str(value or '')[:size]


def _number(value):
    try:
        return max(0, min(100000, int(value)))
    except (TypeError, ValueError):
        return 0


def _token(size):
    return ''.join(secrets.choice(ALPHABET) for _ in range(size))


def public_name(identity):
    """Nom affiche dans le suivi (identite stockee M:matricule ou D:nom), anonymise comme sur l'API publique."""
    from .idendity import anonymized, identity_name
    try:
        with anonymized():
            return identity_name(identity)
    finally:
        close_old_connections()


def badge_identity(credentials):
    """Identite stockee d'un badge valide ({"matricule", "key"}, ou "session" apres une connexion par nom et PIN),
    None sinon. Acces base : appele hors verrou."""
    from .idendity import SOURCE_VERIFIED
    from .sessions import credentials_user
    if not isinstance(credentials, dict):
        return None
    try:
        user = credentials_user(credentials)
        if user is None:
            return None
        return f'{SOURCE_VERIFIED}:{user.matricule}'
    finally:
        close_old_connections()


def scanner_identity(message):
    """(identite stockee, nom affiche, badge verifie) d'une page de scan : badge verifie, sinon nom declare (marque
    comme tel), sinon personne."""
    from .idendity import SOURCE_DECLARED
    identity = badge_identity(message.get('user'))
    if identity:
        return identity, public_name(identity), True
    name = _text(message.get('name'), 30).strip()
    if name:
        identity = f'{SOURCE_DECLARED}:{name}'
        return identity, public_name(identity), False
    return '', '', False


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
        'root': _text(message.get('root') or lot.get('id'), 16),
        'lots': [_text(name, 64) for name in lots[:MAX_LOTS]],
        'scanned': scanned,
        'count': _number(message.get('count')),
        'remaining': _number(message.get('remaining')),
    }


def clean_entries(value):
    """Scans d'un participant, partages avec les autres participants (attendus et liste des scans chez eux). Ce ne
    sont que des iids : la verif envoyee est controlee par le serveur comme toute autre verif."""
    entries = []
    for entry in (value if isinstance(value, list) else [])[:MAX_ENTRIES]:
        if not isinstance(entry, dict) or entry.get('kind') not in ('item', 'pack'):
            continue
        info = entry.get('info') if isinstance(entry.get('info'), dict) else None
        items = entry.get('items') if isinstance(entry.get('items'), list) else []
        entries.append({
            'code': _text(entry.get('code'), 200),
            'kind': entry['kind'],
            'iid': _text(entry.get('iid'), 32),
            'expired': bool(entry.get('expired')),
            'error': _text(entry.get('error'), 64),
            'items': [_text(iid, 32) for iid in items[:500]],
            'place': _text(entry.get('place'), 16),  # lot de la derniere etiquette scannee avant (assign_items)
            'info': None if info is None else {
                'type': _text(info.get('type'), 16), 'type_name': _text(info.get('type_name'), 64),
                'peremption': _text(info.get('peremption'), 32) or None, 'count': _number(info.get('count')),
            },
        })
    return entries


def clean_keys(value):
    """Lots de la verif et leurs cles ([{"id", "key"}]) : gardes sur le serveur, jamais renvoyes."""
    result = []
    for entry in (value if isinstance(value, list) else [])[:MAX_LOTS]:
        if isinstance(entry, dict) and entry.get('id'):
            result.append((_text(entry.get('id'), 16), _text(entry.get('key'), 64)))
    return result


class Participant:
    def __init__(self, identity='', who='', verified=False):
        self.id = _token(12)
        self.token = _token(24)
        self.identity = identity   # identite stockee (M:matricule ou D:nom), enregistree sur la verif
        self.who = who
        self.verified = verified
        self.socket = None
        self.lost = time.monotonic()
        self.state = None          # dernier etat publie (None : rien a montrer)
        self.updated = None
        self.entries = []
        self.validated = False     # a valide sa partie de la verif
        self.partial = False

    def public(self):
        return {'who': self.who, 'verified': self.verified, 'connected': self.socket is not None,
                'validated': self.validated}


class LiveSession:
    def __init__(self):
        self.id = _token(12)
        self.participants = {}     # id -> Participant, dans l'ordre d'arrivee
        self.lots = {}             # id -> cle (vide si inconnue), dans l'ordre d'ajout
        self.started = timezone.now()
        self.updated = self.started
        self.done = None           # {"complete", "partial", "present", "at"} une fois validee
        self.submitting = None     # participant qui envoie la verif commune
        self.saving = False        # verif sans participant en cours d'enregistrement par le serveur
        self.solo = False          # « Quitter » la verif commune : pas de fusion automatique
        self.finished = 0          # fin de la verif (time.monotonic), pour DONE_KEEP

    @property
    def state(self):
        """Dernier etat publie par un participant (chacun publie toute la verif, scans des autres compris)."""
        published = [p for p in self.participants.values() if p.state]
        return max(published, key=lambda p: p.updated).state if published else None

    @property
    def connected(self):
        return any(p.socket is not None for p in self.participants.values())

    @property
    def lost(self):
        return max((p.lost for p in self.participants.values()), default=0)

    @property
    def who(self):
        return ', '.join(p.who or 'Non identifié' for p in self.participants.values())

    def public(self):
        state = self.state or {}
        participants = list(self.participants.values())
        return {
            'id': self.id,
            'who': self.who,
            'verified': bool(participants) and all(p.verified for p in participants),
            'participants': [p.public() for p in participants],
            'started': self.started.isoformat(timespec='seconds'),
            'updated': self.updated.isoformat(timespec='seconds'),
            'connected': self.connected,
            'lot': state.get('lot'),
            'lots': state.get('lots', []),
            'scanned': state.get('scanned', []),
            'count': state.get('count', 0),
            'remaining': state.get('remaining', 0),
            'done': self.done,
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

    def _nearby(self, session):
        """Autres verifs en cours sur le meme lot global : on peut les rejoindre depuis le scanner."""
        state = session.state
        if not state or session.done:
            return []
        return [{'id': other.id, 'who': other.who, 'lot': other.state['lot']} for other in self.sessions.values()
                if other is not session and not other.done and other.state and other.state['root'] == state['root']]

    def _team(self, session, me):
        return {
            'type': 'team',
            'id': session.id,
            'participants': [dict(p.public(), me=p is me) for p in session.participants.values()],
            'lots': list(session.lots),
            'keyed': [lot for lot, key in session.lots.items() if key],
            'entries': [dict(entry, who=p.who) for p in session.participants.values() if p is not me
                        for entry in p.entries],
            'nearby': self._nearby(session),
        }

    def _changed(self, session):
        """Une verif a change : pages de suivi, participants, et participants des verifs du meme lot global."""
        with self.lock:
            alive = self.sessions.get(session.id) is session
            root = (session.state or {}).get('root')
            related = [session] + [other for other in self.sessions.values() if other is not session and root
                                   and (other.state or {}).get('root') == root]
            messages = [(p.socket, self._team(s, p)) for s in related if self.sessions.get(s.id) is s
                        for p in s.participants.values() if p.socket is not None]
            public = session.public() if alive and (session.state or session.done) else None
        for websocket, message in messages:
            websocket.send_json(message)
        if public:
            self.broadcast({'type': 'update', 'session': public})
        else:
            self.broadcast({'type': 'remove', 'id': session.id})

    def remove(self, session_id):
        with self.lock:
            session = self.sessions.pop(session_id, None)
        if session is not None:
            self.broadcast({'type': 'remove', 'id': session_id})
        return session is not None

    def _leave(self, session, participant):
        """Retire un participant (il rejoint une autre verif) ; une verif sans participant disparait."""
        session.participants.pop(participant.id, None)
        if session.submitting == participant.id:
            session.submitting = None
        if not session.participants:
            self.sessions.pop(session.id, None)

    # -- ramasseur -----------------------------------------------------------------------------------------------------

    def reap(self, now=None):
        """Retire les verifs terminees depuis DONE_KEEP et celles dont les telephones ont disparu depuis LOST_KEEP
        (abandonnees), sauf celles qui sont completes ou qu'un participant a validees : le serveur les enregistre.
        Verif a plusieurs dont tous les participants encore connectes ont valide (les autres sont partis) : un de
        ceux qui ont valide est charge de l'envoyer."""
        now = time.monotonic() if now is None else now
        expired = []
        submits = []
        unattended = []
        with self.lock:
            for session in self.sessions.values():
                if not session.connected:
                    keep = DONE_KEEP if session.done else LOST_KEEP
                    if session.saving or now - max(session.lost, session.finished) <= keep:
                        continue
                    if not session.done and not session.saving and self._worth_saving(session):
                        session.saving = True
                        unattended.append(session)
                    else:
                        expired.append(session.id)
                    continue
                participant = self._submitter(session, now)
                if participant is not None:
                    session.submitting = participant.id
                    submits.append((participant.socket, {'type': 'submit', 'partial': participant.partial}))
        for session_id in expired:
            self.remove(session_id)
        for session in unattended:
            if not self.save_unattended(session):
                self.remove(session.id)
        for websocket, message in submits:
            websocket.send_json(message)
        return expired

    @staticmethod
    def _worth_saving(session):
        """Verif sans participant a enregistrer plutot qu'a abandonner : quelqu'un a valide sa partie, ou tout
        ce qui etait attendu a ete scanne."""
        state = session.state or {}
        complete = bool(state.get('count')) and not state.get('remaining')
        return any(p.validated for p in session.participants.values()) or complete

    def save_unattended(self, session):
        """Enregistre la verif commune comme si son dernier participant l'avait envoyee : scans de tous, cles des lots
        gardees par le serveur, au nom de celui qui a valide (sinon du premier participant). False si impossible."""
        from . import services
        from .views import verif_lots
        with self.lock:
            participants = list(session.participants.values())
            entries = [entry for p in participants for entry in p.entries if not entry['error']]
            lots = [{'id': lot_id, 'key': key} for lot_id, key in session.lots.items()]
            validated = [p for p in participants if p.validated]
            partial = any(p.partial for p in validated)
            identities = [p.identity for p in validated + participants if p.identity and (p.validated or p.entries)]
        identities = list(dict.fromkeys(identities))
        iids = [entry['iid'] for entry in entries if entry['kind'] == 'item']
        iids += [iid for entry in entries if entry['kind'] == 'pack' for iid in entry['items']]
        places = {iid: entry['place'] for entry in entries if entry['place']
                  for iid in ([entry['iid']] if entry['kind'] == 'item' else entry['items'])}
        if not identities or not lots or not iids:
            return False
        try:
            report = services.perform_verif(verif_lots(lots, False), iids, identities[0], partial, identities[1:],
                                            places)
        except Exception:  # cle expiree, lot archive... : la verif est abandonnee
            with self.lock:
                session.saving = False
            return False
        finally:
            close_old_connections()
        self.finish({'id': session.id, 'pid': None}, report)
        return True

    @staticmethod
    def _submitter(session, now):
        if session.done or len(session.participants) < 2:
            return None
        submitting = session.participants.get(session.submitting)
        if submitting is not None and (submitting.socket is not None or now - submitting.lost < GRACE):
            return None
        participants = list(session.participants.values())
        if any(not p.validated and (p.socket is not None or now - p.lost < GRACE) for p in participants):
            return None
        ready = [p for p in participants if p.validated and p.socket is not None]
        return ready[-1] if ready else None

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

    def _find(self, credentials):
        """(verif, participant) designes par {"id", "pid", "token"}, (None, None) si inconnus."""
        if not isinstance(credentials, dict):
            return None, None
        session = self.sessions.get(_text(credentials.get('id'), 16))
        participant = session.participants.get(_text(credentials.get('pid'), 16)) if session else None
        if participant is None or not hmac.compare_digest(participant.token.encode(),
                                                          _text(credentials.get('token'), 32).encode()):
            return None, None
        return session, participant

    def attach_scanner(self, message, websocket):
        """Reprend le participant {"id", "pid", "token"} s'il existe encore (rechargement de la page), lui fait
        rejoindre la verif "join" (badge exige), sinon cree une nouvelle verif. Renvoie (verif, participant, erreur)."""
        identity, who, verified = scanner_identity(message)
        left = stale = None
        with self.lock:
            session, participant = self._find(message)
            join = _text(message.get('join'), 16)
            if join and (session is None or session.id != join):
                target = self.sessions.get(join)
                if not verified:
                    return None, None, 'Scannez votre badge pour rejoindre une vérif.'
                if target is None or target.done:
                    return None, None, 'Cette vérif est terminée.'
                if len(target.participants) >= MAX_PARTICIPANTS:
                    return None, None, 'Trop de participants sur cette vérif.'
                if participant is not None:
                    self._leave(session, participant)
                    left = session
                    if participant.socket is not websocket:
                        stale = participant.socket
                session, participant = target, Participant()
                session.participants[participant.id] = participant
            elif participant is None:
                if len(self.sessions) >= MAX_SESSIONS:
                    return None, None, 'trop de vérifs suivies en même temps'
                session, participant = LiveSession(), Participant()
                session.solo = bool(message.get('solo'))
                session.participants[participant.id] = participant
                self.sessions[session.id] = session
            previous = participant.socket
            participant.socket = websocket
            participant.identity, participant.who, participant.verified = identity, who, verified
        if previous is not None and previous is not websocket:
            previous.close()
        if stale is not None:
            stale.close()
        if left is not None:
            self._changed(left)
        self._start_reaper()
        return session, participant, None

    def _session_of(self, participant, hint):
        """Verif ou se trouve le participant (une fusion a pu le deplacer), None s'il l'a quittee."""
        if self.sessions.get(hint.id) is hint and hint.participants.get(participant.id) is participant:
            return hint
        return next((session for session in self.sessions.values()
                     if session.participants.get(participant.id) is participant), None)

    def _merge(self, session):
        """Une verif est deja en cours sur le meme lot global (meme lot, ou un autre sous-lot du meme lot) : les
        verifs sont fusionnees dans la plus ancienne, une verif commune de tous leurs lots. Les participants des autres
        recoivent un nouveau hello (meme pid et jeton) marque "merged". Renvoie la verif commune, None sinon."""
        with self.lock:
            root = (session.state or {}).get('root')
            if not root or session.done or session.solo or session.saving or self.sessions.get(session.id) is not session:
                return None
            group = [session] + [other for other in self.sessions.values()
                                 if other is not session and not other.done and not other.solo and not other.saving
                                 and other.state and other.state['root'] == root]
            if len(group) < 2:
                return None
            group.sort(key=lambda other: other.started)
            target = group[0]
            moving = []
            for other in group[1:]:
                if len(target.participants) + len(other.participants) > MAX_PARTICIPANTS:
                    continue
                for participant in other.participants.values():
                    target.participants[participant.id] = participant
                    moving.append(participant)
                for lot_id, key in other.lots.items():
                    if key or lot_id not in target.lots:
                        target.lots[lot_id] = key
                self.sessions.pop(other.id, None)
            if not moving:
                return None
            removed = [other.id for other in group[1:] if other.id not in self.sessions]
            target.updated = timezone.now()
            hellos = [(p.socket, {'type': 'hello', 'id': target.id, 'pid': p.id, 'token': p.token, 'merged': True,
                                  'who': ', '.join(o.who or 'Non identifié' for o in target.participants.values()
                                                   if o not in moving)})
                      for p in moving if p.socket is not None]
        for websocket, message in hellos:
            websocket.send_json(message)
        for session_id in removed:
            self.broadcast({'type': 'remove', 'id': session_id})
        return target

    def _restart(self, participant, websocket):
        """Nouveau scan apres la fin d'une verif : le participant repart sur une nouvelle verif (la verif terminee
        reste affichee, ce telephone n'y est plus connecte)."""
        with self.lock:
            fresh = LiveSession()
            moved = Participant(participant.identity, participant.who, participant.verified)
            moved.socket = websocket
            fresh.participants[moved.id] = moved
            self.sessions[fresh.id] = fresh
            participant.socket = None
            participant.lost = time.monotonic()
        websocket.send_json({'type': 'hello', 'id': fresh.id, 'pid': moved.id, 'token': moved.token})
        return fresh, moved

    def run_scanner(self, websocket):
        session = participant = None
        try:
            hello = self._parse(websocket.receive())
            if hello.get('type') != 'hello':
                return
            session, participant, problem = self.attach_scanner(hello, websocket)
            if problem:
                websocket.send_json({'type': 'error', 'message': problem, 'join': bool(hello.get('join'))})
                return
            websocket.send_json({'type': 'hello', 'id': session.id, 'pid': participant.id, 'token': participant.token})
            self._changed(session)
            while True:
                message = self._parse(websocket.receive())
                kind = message.get('type')
                with self.lock:
                    session = self._session_of(participant, session)
                if session is None:
                    return  # parti sur une autre verif
                if kind == 'state':
                    if session.done:
                        previous = session
                        session, participant = self._restart(participant, websocket)
                        self._changed(previous)
                    with self.lock:
                        participant.state = clean_state(message)
                        participant.entries = clean_entries(message.get('entries'))
                        for lot_id, key in clean_keys(message.get('keys')):
                            if key or lot_id not in session.lots:
                                session.lots[lot_id] = key
                        participant.updated = session.updated = timezone.now()
                    merged = self._merge(session)
                    self._changed(merged or session)
                elif kind == 'identity':
                    identity, who, verified = scanner_identity(message)
                    with self.lock:
                        participant.identity, participant.who, participant.verified = identity, who, verified
                    self._changed(session)
                elif kind == 'validate':
                    with self.lock:
                        participant.validated = True
                        participant.partial = bool(message.get('partial'))
                        waiting = [p.who or 'Non identifié' for p in session.participants.values()
                                   if p is not participant and p.socket is not None and not p.validated]
                        busy = session.submitting not in (None, participant.id) and session.submitting in session.participants
                        submit = not waiting and not busy and not session.done
                        if submit:
                            session.submitting = participant.id
                    websocket.send_json({'type': 'validate', 'submit': submit, 'waiting': waiting})
                    self._changed(session)
                elif kind == 'unvalidate':
                    with self.lock:
                        participant.validated = False
                        if session.submitting == participant.id:
                            session.submitting = None
                    self._changed(session)
                elif kind == 'done':
                    with self.lock:
                        session.done = {'complete': bool(message.get('complete')), 'partial': bool(message.get('partial')),
                                        'present': _number(message.get('present')),
                                        'at': timezone.now().isoformat(timespec='seconds')}
                        session.updated = timezone.now()
                        session.finished = time.monotonic()
                    self._changed(session)
                elif kind == 'cancel':
                    with self.lock:
                        participant.state = None
                        participant.entries = []
                        participant.validated = False
                    self._changed(session)
        except WebSocketClosed:
            pass
        finally:
            if participant is not None:
                with self.lock:
                    session = self._session_of(participant, session) or session
                    current = participant.socket is websocket
                    if current:
                        participant.socket = None
                        participant.lost = time.monotonic()
                if current:
                    self._changed(session)

    # -- envoi de la verif (vue POST /api/verifs/) ---------------------------------------------------------------------

    def verif_team(self, credentials):
        """Verif a plusieurs envoyee par un de ses participants ({"id", "pid", "token"}) : cles des lots connues du
        serveur et identites des participants (celui qui envoie, ceux qui ont valide ou scanne). None si inconnue."""
        with self.lock:
            session, participant = self._find(credentials)
            if session is None or session.done or session.saving:
                return None
            identities = [p.identity for p in session.participants.values()
                          if p.identity and (p is participant or p.validated or p.entries)]
            return {'id': session.id, 'pid': participant.id, 'keys': dict(session.lots),
                    'identities': list(dict.fromkeys(identities))}

    def finish(self, team, report):
        """Verif enregistree : affichee comme terminee, compte rendu envoye aux autres participants."""
        with self.lock:
            session = self.sessions.get(team['id'])
            if session is None:
                return
            session.done = {'complete': bool(report.get('complete')), 'partial': bool(report.get('partial')),
                            'present': len(report.get('present') or []),
                            'at': timezone.now().isoformat(timespec='seconds')}
            session.updated = timezone.now()
            session.submitting = None
            session.saving = False
            session.finished = time.monotonic()
            sockets = [p.socket for p in session.participants.values() if p.id != team['pid'] and p.socket is not None]
        for websocket in sockets:
            websocket.send_json({'type': 'report', 'report': report})
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
