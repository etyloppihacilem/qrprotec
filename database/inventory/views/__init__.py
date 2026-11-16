# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          views/__init__.py
#       -\-    _|__
#        |\___/  . \        Created on 16 Nov. 2026 at 19:45
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""API JSON, en plusieurs fichiers : common (aides), public (vues des deux API), local (gestion), notif_settings,
douchette, lists (listes de materiel), prints (etiquettes demandees depuis le web). Tout est reexporte ici :
urls.py, live.py et les tests ecrivent views.<nom> comme avant le decoupage."""

from .common import *  # noqa: F401,F403
from .public import *  # noqa: F401,F403
from .local import *  # noqa: F401,F403
from .notif_settings import *  # noqa: F401,F403
from .douchette import *  # noqa: F401,F403
from .lists import *  # noqa: F401,F403
from .prints import *  # noqa: F401,F403
from .common import _checklist_call  # noqa: F401
