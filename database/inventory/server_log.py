# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          server_log.py
#       -\-    _|__
#        |\___/  . \        Created on 08 Nov. 2026 at 15:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Gestionnaire du journal (settings.LOGGING) qui transforme les erreurs du back en notification « Problème de
serveur » (notifications.server_problem). Module sans import de l'appli au chargement : le journal est configure
avant que Django ait charge les modeles."""

import logging
import threading

_state = threading.local()


class ServerProblemHandler(logging.Handler):
    """Chaque erreur du back (500 de django.request, erreur d'un thread de l'appli) devient un « Problème de
    serveur ». L'enregistrement en base et l'envoi se font dans un thread : l'erreur peut venir d'une base verrouillee,
    la requete en cours n'attend pas. Les refus de securite (hote inconnu, robots) ne sont pas des pannes et ne
    previennent personne."""

    IGNORED = ('django.security', 'inventory.webpush')

    def emit(self, record):
        if record.levelno < logging.ERROR or record.name.startswith(self.IGNORED):
            return
        if getattr(_state, 'active', False):  # erreur pendant l'enregistrement lui-meme : pas de boucle
            return
        try:
            text = record.getMessage().splitlines()[0][:150]
            if record.exc_info and record.exc_info[1] is not None:
                error = record.exc_info[1]
                detail = str(error).splitlines()[0][:120] if str(error) else ''
                text = f'{text} ({type(error).__name__}{" : " + detail if detail else ""})'
        except Exception:  # noqa: BLE001 - le journal ne doit jamais faire echouer l'appelant
            text = record.name
        from django.conf import settings
        if settings.QRPROTEC.get('NOTIFY_SYNC'):
            self.record(text, close=False)
        else:
            threading.Thread(target=self.record, args=(text,), daemon=True).start()

    @staticmethod
    def record(text, close=True):
        from django.db import close_old_connections

        from . import notifications
        _state.active = True
        if close:
            close_old_connections()
        try:
            notifications.server_problem('server', text)
        except Exception:  # noqa: BLE001 - base indisponible : l'erreur reste dans le journal
            pass
        finally:
            _state.active = False
            if close:
                close_old_connections()
