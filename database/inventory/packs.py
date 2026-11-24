# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          packs.py
#       -\-    _|__
#        |\___/  . \        Created on 24 Nov. 2026 at 17:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Paquets fermes (SealedPacks) : ouverture et paquets pre-etiquetes.

- Ouverture : au poste (fiche du paquet, les etiquettes des items partent apres l'apercu), ou depuis la fiche du
  paquet du web (gestion et admins) : le paquet est marque ouvert et les etiquettes de ses items sont envoyees au
  poste (« Impressions en attente », LabelPrint de type pack). Une fois ouvert, l'etiquette du paquet ne compte plus
  en verif : on scanne les items un par un.
- Paquet pre-etiquete (gestion et admins, web ou poste) : un paquet ouvert dont les etiquettes ont ete imprimees, les
  items etiquetes puis remis dans le paquet. « J'ai déjà étiqueté le contenu » imprime une etiquette de paquet ouvert
  (pack?id=ID&o=CLE), posee de sorte qu'elle se dechire a l'ouverture, comme un scelle. Elle compte en verif les items
  qu'elle couvre (open_label_items), tant qu'elle est valable :
  - plus valable des qu'un item couvert est scanne seul dans une verif (meme dans son lot : le paquet a ete ouvert),
    voir break_open_labels, appele par services.perform_verif ;
  - ni quand un item couvert a quitte le stock (utilise, supprime, sorti, remplace) ou n'est plus dans le meme lot que
    les autres (open_label_problem, calcule a chaque lecture).
  Une nouvelle etiquette a une nouvelle cle : une ancienne ne vaut plus rien. Pas de notification au TECH quand elle
  est dechiree : ouvrir un paquet pre-etiquete est normal.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from django.db import transaction
from django.utils import timezone

from . import journal
from .models import (
    Items, ItemStatus, LabelPrint, LabelPrintKind, OperationKind, SealedPacks, SeenWhile, generate_key,
)

if TYPE_CHECKING:
    from datetime import datetime

# items encore couverts par une etiquette de paquet ouvert : en stock ou dans un lot (disparu compris : un paquet
# oublie a une verif, retrouve ensuite avec son etiquette)
COVERED_STATUSES = (ItemStatus.ACTIVE, ItemStatus.MISSING)


class PackError(Exception):
    def __init__(self, message: str, code: int = 400) -> None:
        super().__init__(message)
        self.message = message
        self.code = code


def mark_opened(pack: SealedPacks, identity: str, now: datetime | None = None) -> bool:
    """Marque le paquet ouvert (une seule fois : un paquet deja ouvert garde sa date). Repond s'il vient de l'etre."""
    if pack.opened is not None:
        return False
    now = now or timezone.now()
    with transaction.atomic():
        pack.opened = now
        pack.opened_by = identity[:32]
        pack.save(update_fields=['opened', 'opened_by'])
        Items.objects.filter(sealed_pack=pack).update(last_seen=now, last_seen_by=identity,
                                                      last_seen_while=SeenWhile.OPEN)
        journal.record(OperationKind.PACK_OPEN, identity, f'{pack.count} × {pack.item_type.name} (paquet {pack.id})',
                       at=now, pack=pack.id, type=pack.item_type_id, count=pack.count)
    return True


def mark_labels_printed(pack: SealedPacks, now: datetime | None = None) -> None:
    """Etiquettes des items imprimees au poste : le contenu peut desormais etre pre-etiquete."""
    if pack.labels_printed is None:
        pack.labels_printed = now or timezone.now()
        pack.save(update_fields=['labels_printed'])


def open_label_problem(pack: SealedPacks) -> str:
    """Pourquoi l'etiquette de paquet ouvert ne vaut plus rien ('' : valable, ou pas d'etiquette)."""
    if not pack.open_label_key:
        return ''
    if pack.open_label_broken is not None:
        return pack.open_label_broken_reason or 'Paquet ouvert'
    covered = list(Items.objects.filter(iid__in=pack.open_label_items).only('iid', 'status', 'location_id'))
    if len(covered) < len(pack.open_label_items) or any(item.status not in COVERED_STATUSES for item in covered):
        return 'Un item du paquet a été utilisé ou retiré du stock'
    if len({item.location_id for item in covered}) > 1:
        return 'Un item du paquet a changé de lot'
    return ''


def open_label_dict(pack: SealedPacks, key: str | None = None) -> dict[str, Any] | None:
    """Etat de l'etiquette de paquet ouvert (None : le paquet n'en a pas). key : cle de l'etiquette scannee,
    comparee a celle du paquet (scanned : true si c'est la bonne, false si c'est une ancienne)."""
    from .serializers import _date, _who
    if not pack.open_label_key:
        return None
    problem = open_label_problem(pack)
    data: dict[str, Any] = {
        'at': _date(pack.open_label_at),
        'by': _who(pack.open_label_by),
        'count': len(pack.open_label_items),
        'valid': not problem,
        'problem': problem,
        'broken': _date(pack.open_label_broken),
    }
    if key is not None:
        data['scanned'] = pack.check_open_label(key)
    return data


def candidates(pack: SealedPacks) -> list[Items]:
    """Items qu'une nouvelle etiquette de paquet ouvert couvrirait : ceux de l'etiquette actuelle si elle est encore
    valable (reimpression), sinon ceux du paquet qui n'ont pas ete revus depuis son ouverture (pas scannes seuls,
    pas deplaces, pas utilises)."""
    if pack.open_label_key and not open_label_problem(pack):
        return list(Items.objects.filter(iid__in=pack.open_label_items).order_by('iid'))
    return list(Items.objects.filter(sealed_pack=pack, status=ItemStatus.ACTIVE, deleted__isnull=True,
                                     last_seen_while=SeenWhile.OPEN).order_by('iid'))


def can_prelabel(pack: SealedPacks) -> str:
    """'' si le paquet peut etre pre-etiquete, sinon pourquoi."""
    if pack.opened is None:
        return "Ouvrez d'abord le paquet et imprimez les étiquettes de ses items"
    if pack.labels_printed is None:
        return "Les étiquettes des items n'ont pas encore été imprimées au poste"
    if not candidates(pack):
        return ('Tous les items du paquet ont déjà servi ou ont été scannés seuls : il ne peut plus être '
                'pré-étiqueté, scannez ses items un par un')
    return ''


def prelabel(pack: SealedPacks, identity: str, count: int | None = None) -> SealedPacks:
    """« J'ai déjà étiqueté le contenu » : nouvelle etiquette de paquet ouvert (nouvelle cle), qui couvre les items
    candidats. count : nombre d'items confirme par la personne (refuse s'il ne correspond plus)."""
    problem = can_prelabel(pack)
    if problem:
        raise PackError(problem, 409)
    covered = candidates(pack)
    if count is not None and count != len(covered):
        raise PackError(f'Le paquet contient maintenant {len(covered)} items, pas {count} : rouvrez sa fiche', 409)
    now = timezone.now()
    with transaction.atomic():
        pack.open_label_key = generate_key(16)
        pack.open_label_at = now
        pack.open_label_by = identity[:64]
        pack.open_label_items = [item.iid for item in covered]
        pack.open_label_broken = None
        pack.open_label_broken_reason = ''
        pack.save(update_fields=['open_label_key', 'open_label_at', 'open_label_by', 'open_label_items',
                                 'open_label_broken', 'open_label_broken_reason'])
        journal.record(OperationKind.PACK_PRELABEL, identity,
                       f'{len(covered)} × {pack.item_type.name} (paquet {pack.id}, contenu déjà étiqueté)', at=now,
                       pack=pack.id, type=pack.item_type_id, count=len(covered))
    return pack


def break_open_labels(iids: list[str], now: datetime) -> list[SealedPacks]:
    """Verif : les etiquettes de paquet ouvert dont une partie seulement des items a ete scannee ne valent plus rien
    (un item scanne seul : le paquet a ete ouvert). Tous scannes (etiquette du paquet scannee, ou chaque item un par
    un) : l'etiquette reste valable. Repond les paquets touches."""
    scanned = set(iids)
    pack_ids = set(Items.objects.filter(iid__in=scanned, sealed_pack__isnull=False)
                   .values_list('sealed_pack_id', flat=True))
    if not pack_ids:
        return []
    broken = []
    for pack in SealedPacks.objects.filter(id__in=pack_ids, open_label_broken__isnull=True).exclude(open_label_key=''):
        covered = set(pack.open_label_items)
        if covered & scanned and not covered <= scanned:
            pack.open_label_broken = now
            pack.open_label_broken_reason = 'Un item du paquet a été scanné seul : le paquet a été ouvert'
            pack.save(update_fields=['open_label_broken', 'open_label_broken_reason'])
            broken.append(pack)
    return broken


def pending_print(pack: SealedPacks, open_label: bool) -> LabelPrint | None:
    from .label_prints import pending_rows
    return pending_rows().filter(kind=LabelPrintKind.PACK, sealed_pack=pack, open_label=open_label).first()


def send_item_labels(pack: SealedPacks, by: str) -> LabelPrint:
    """Fiche du paquet du web (gestion et admins) : ouvre le paquet et envoie les etiquettes de ses items au poste.
    Une demande deja en attente pour ce paquet est reprise telle quelle."""
    from .label_prints import live
    mark_opened(pack, by)
    row = pending_print(pack, open_label=False)
    if row is None:
        count = Items.objects.filter(sealed_pack=pack, deleted__isnull=True).count()
        if not count:
            raise PackError("Plus aucun item dans ce paquet : rien à imprimer")
        row = LabelPrint.objects.create(by=by[:64], kind=LabelPrintKind.PACK, item_type=pack.item_type, count=count,
                                        peremption=pack.peremption, sealed_pack=pack)
        live.changed()
    return row


def send_open_label(pack: SealedPacks, by: str, count: int | None = None) -> LabelPrint:
    """Fiche du paquet du web (gestion et admins) : « J'ai déjà étiqueté le contenu », l'etiquette de paquet ouvert
    est envoyee au poste."""
    from .label_prints import live
    prelabel(pack, by, count)
    previous = pending_print(pack, open_label=True)
    if previous is not None:  # meme etiquette : la cle est lue a l'impression, la demande en attente suffit
        return previous
    row = LabelPrint.objects.create(by=by[:64], kind=LabelPrintKind.PACK, item_type=pack.item_type, count=1,
                                    peremption=pack.peremption, sealed_pack=pack, open_label=True)
    live.changed()
    return row
