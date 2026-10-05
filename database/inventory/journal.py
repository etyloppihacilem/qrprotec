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

from collections import Counter

from django.utils import timezone

from .idendity import SOURCE_VERIFIED
from .models import Operation, Secouristes

MAX_LISTED_ITEMS = 500  # iids gardes dans le detail d'une operation


def record(kind, by, summary='', lot=None, at=None, **details):
    """Ajoute une operation au journal. `lot` : le lot concerne (son nom est garde tel qu'il etait)."""
    return Operation.objects.create(
        at=at or timezone.now(), by=(by or '')[:64], kind=kind, lot=lot, lot_name=lot.name if lot else '',
        summary=summary[:255], details=details,
    )


def types_summary(items):
    """« 3 × Compresse, 1 × Gants » pour une liste d'items (pack.item_type charge)."""
    counts = Counter(item.pack.item_type.name for item in items)
    return ', '.join(f'{count} × {name}' for name, count in sorted(counts.items()))


def item_ids(items):
    return [item.iid for item in items][:MAX_LISTED_ITEMS]


def plural(count, word):
    return f"{count} {word}{'s' if count > 1 else ''}"


def names(identities):
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
