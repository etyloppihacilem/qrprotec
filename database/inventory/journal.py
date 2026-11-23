# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          journal.py
#       -\-    _|__
#        |\___/  . \        Created on 05 Oct. 2026 at 08:30
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Journal des operations : qui fait quoi et quand (verifs, scelles, reassorts, receptions, gestion...)."""

from __future__ import annotations

import threading
from collections import Counter
from typing import TYPE_CHECKING, Any

from django.utils import timezone

from .idendity import SOURCE_VERIFIED
from .models import Operation, Secouristes

if TYPE_CHECKING:
    from datetime import datetime
    from inventory.models import Items, Lots, OperationKind
    from rest_framework.request import Request

MAX_LISTED_ITEMS = 500  # iids gardes dans le detail d'une operation


# Requete API en cours (posee par views.handle_errors) : le journal y lit le moyen d'authentification
_current = threading.local()


def set_request(request: Request | None) -> Request | None:
    """Pose la requete en cours et rend la precedente (a remettre a la fin)."""
    previous = getattr(_current, 'request', None)
    _current.request = request
    return previous


def record(kind: OperationKind, by: str, summary: str = '', lot: Lots | None = None, at: datetime | None = None,
           **details: Any) -> Operation:
    """Ajoute une operation au journal. `lot` : le lot concerne (son nom est garde tel qu'il etait). `details.auth` :
    moyen d'authentification de la requete en cours (web : moyen de connexion ; poste principal : carte, PIN... ;
    poste distant : en plus le nom de sa cle API), voir sessions.auth_label."""
    if 'auth' not in details:
        from .sessions import auth_label  # sessions importe les modeles, comme ce module
        label = auth_label(getattr(_current, 'request', None))
        if label:
            details['auth'] = label
    return Operation.objects.create(
        at=at or timezone.now(), by=(by or '')[:64], kind=kind, lot=lot, lot_name=lot.name if lot else '',
        summary=summary[:255], details=details,
    )


def types_summary(items: list[Items]) -> str:
    """« 3 × Compresse, 1 × Gants » pour une liste d'items (pack.item_type charge)."""
    counts = Counter(item.pack.item_type.name for item in items)
    return ', '.join(f'{count} × {name}' for name, count in sorted(counts.items()))


def item_ids(items: list[Items]) -> list[str]:
    return [item.iid for item in items][:MAX_LISTED_ITEMS]


def pl(count: int, word: str, plural_form: str | None = None) -> str:
    """Accord en nombre : singulier pour 0 et 1, pluriel au-dela (par defaut le mot suivi d'un s)."""
    return (plural_form or word + 's') if count > 1 else word


def plural(count: int, word: str, plural_form: str | None = None) -> str:
    """« 3 items », « 1 item », « 0 item »."""
    return f'{count} {pl(count, word, plural_form)}'


def names(identities: set[str] | list[str]) -> dict[str, str]:
    """Noms affiches des identites stockees (une seule requete pour tous les matricules)."""
    identities = set(identities)
    matricules = {value.partition(':')[2] for value in identities if value.startswith(SOURCE_VERIFIED + ':')}
    users = {user.matricule: str(user) for user in Secouristes.objects.filter(matricule__in=matricules)}
    result = {}
    for value in identities:
        source, _, payload = value.partition(':')
        if source == SOURCE_VERIFIED:
            result[value] = users.get(payload, f'Matricule inconnu ({payload})')
        else:
            result[value] = payload or value
    return result
