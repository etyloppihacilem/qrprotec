# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          print_live.py
#       -\-    _|__
#        |\___/  . \        Created on 23 Nov. 2026 at 21:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Impressions en attente en direct : WebSocket entre le poste principal et le serveur (/ws/label-prints).

Le poste interroge /api/label-prints/pending/ toutes les 20 s. Des qu'il y voit une nouvelle demande, il ouvre ce
WebSocket : chaque changement de la file (nouvelle demande du web ou d'un autre ordinateur, file lancee ou annulee a
distance, impression faite) lui est signale aussitot, et il relit la file sans attendre son prochain tour. Sans nouvelle
impression pendant 15 min, le poste ferme le WebSocket et reprend l'interrogation toutes les 20 s (le delai est tenu par
le poste, voir app/src/app/label_prints.cpp).

Le serveur ne fait que signaler ({"type": "changed"}) : le poste relit la file par l'API habituelle, avec ses droits.
Reserve au poste principal (API locale, ou cle de front marquee poste, comme /api/label-prints/pending/).

Messages (JSON, trames texte) :
  serveur -> poste : {"type": "hello"} a la connexion (le poste relit la file : rien de rate pendant la connexion),
                     {"type": "changed"} a chaque changement de la file, {"type": "pong"}
  poste -> serveur : {"type": "ping"} toutes les 15 s (sinon le serveur ferme au bout de RECEIVE_TIMEOUT)

Les connexions vivent en memoire, dans le processus de `manage.py serve` (comme le telephone-douchette).
"""

from __future__ import annotations

import json
import logging
import threading
from typing import TYPE_CHECKING

from django.db import transaction

from .remote_scanner import WebSocketClosed

if TYPE_CHECKING:
    from .remote_scanner import WebSocket

logger = logging.getLogger(__name__)


class PrintWatchers:
    """Postes connectes en direct a la file d'impression."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.sockets: set[WebSocket] = set()

    def run(self, websocket: WebSocket) -> None:
        """Garde la connexion d'un poste jusqu'a sa fermeture (par le poste, ou poste muet)."""
        with self.lock:
            self.sockets.add(websocket)
        try:
            websocket.send_json({'type': 'hello'})
            while True:
                try:
                    message = json.loads(websocket.receive())
                except ValueError:
                    continue
                if isinstance(message, dict) and message.get('type') == 'ping':
                    websocket.send_json({'type': 'pong'})
        except WebSocketClosed:
            pass
        finally:
            with self.lock:
                self.sockets.discard(websocket)

    def count(self) -> int:
        with self.lock:
            return len(self.sockets)

    def broadcast(self) -> None:
        """Signale le changement a chaque poste connecte ; une connexion morte est oubliee."""
        with self.lock:
            sockets = list(self.sockets)
        for websocket in sockets:
            if not websocket.send_json({'type': 'changed'}):
                with self.lock:
                    self.sockets.discard(websocket)

    def changed(self) -> None:
        """A appeler apres tout changement de la file : signale une fois la transaction validee (le poste qui relit
        aussitot voit le changement). Jamais d'erreur pour la requete qui a fait le changement."""
        def send() -> None:
            try:
                self.broadcast()
            except Exception:  # un signal perdu est rattrape au prochain tour du poste
                logger.exception('Impressions en direct : signal non envoyé')

        if self.sockets:
            transaction.on_commit(send)


hub = PrintWatchers()
