# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          movements.py
#       -\-    _|__
#        |\___/  . \        Created on 02 Oct. 2026 at 09:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Journal des mouvements d'items (consommation des lots pour les previsions de stock)."""

from django.utils import timezone

from .models import ABSENCE_KINDS, ItemMovement, ItemStatus, MovementKind


def movement(item, kind, at, by, from_lot_id=None, to_lot_id=None, verif=None):
    """Mouvement non enregistre (a passer a ItemMovement.objects.bulk_create)."""
    return ItemMovement(item=item, item_type_id=item.pack.item_type_id, kind=kind, at=at, by=by[:64],
                        from_lot_id=from_lot_id, to_lot_id=to_lot_id, verif=verif)


def absence(item, at, by, verif=None):
    """Premiere absence d'un item a une verif : utilise s'il etait encore bon, jete s'il etait perime."""
    kind = MovementKind.DISCARDED if item.is_expired(timezone.localdate(at)) else MovementKind.USED
    return movement(item, kind, at, by, from_lot_id=item.location_id, verif=verif)


def may_be_absent(item) -> bool:
    """Un item manque a une verif, disparu ou supprime peut avoir une absence ouverte dans le journal."""
    return item.missed_verifs > 0 or item.status != ItemStatus.ACTIVE


def found_again(iids, at, by):
    """Items revus (scannes, deplaces, restaures) : leurs absences ouvertes sont annulees.

    Cas d'une verif mal faite : l'item note absent (donc compte comme utilise) est retrouve plus tard, dans son lot
    ou ailleurs. Il ne doit plus compter dans la consommation.
    """
    iids = list(iids)
    if not iids:
        return 0
    return ItemMovement.objects.filter(item_id__in=iids, kind__in=ABSENCE_KINDS, cancelled__isnull=True).update(
        cancelled=at, cancelled_by=by[:64]
    )
