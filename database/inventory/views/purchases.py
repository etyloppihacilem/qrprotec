# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          views/purchases.py
#       -\-    _|__
#        |\___/  . \        Created on 26 Nov. 2026 at 18:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Commandes fournisseurs (poste seulement, roles gestion et admin) : saisie, reception, statistiques, suggestions.
Le calcul est dans purchases.py."""

from __future__ import annotations

from typing import TYPE_CHECKING

from .common import *  # noqa: F401,F403
from .local import receive_items, reception_response
from .. import purchases
from ..models import SupplierOrder

if TYPE_CHECKING:
    from rest_framework.request import Request
    from rest_framework.response import Response


def _order(order_id: int) -> SupplierOrder:
    return get_object_or_404(SupplierOrder.objects.select_related('supplier'), pk=order_id)


def _purchase_call(call: Callable[[], Any]) -> Any:
    try:
        return call()
    except purchases.PurchaseError as error:
        raise ApiError(str(error))


@api_view(['GET'])
@handle_errors
def purchase_suppliers(request: Request) -> Response:
    """Fournisseurs deja enregistres : suggeres a la saisie du nom (comme les modeles d'etiquettes)."""
    require_front(request, GESTION)
    return Response(purchases.suppliers_list())


@api_view(['GET', 'POST'])
@handle_errors
def purchase_orders(request: Request) -> Response:
    """GET : commandes, ce qui merite l'attention d'abord (?open=1 : celles qui attendent une livraison, page de
    reception). POST : nouvelle commande {"user", "supplier" (nom, cree s'il est nouveau), "number", "ordered"
    (AAAA-MM-JJ), "shipping", "note", "lines": [{"type", "quantity", "unit_price"}]}."""
    require_front(request, GESTION)
    if request.method == 'GET':
        only_open = request.query_params.get('open') in ('1', 'true')
        return Response(purchases.orders_list(only_open=only_open))
    identity = operation_identity(request)
    order = _purchase_call(lambda: purchases.create_order(request.data, identity))
    return Response(purchases.order_detail(order.id), status=status.HTTP_201_CREATED)


@api_view(['GET', 'POST'])
@handle_errors
def purchase_order_detail(request: Request, order_id: int) -> Response:
    """GET : une commande. POST : correction (memes champs que la creation ; les lignes gardent leur "id")."""
    require_front(request, GESTION)
    order = _order(order_id)
    if request.method == 'POST':
        identity = operation_identity(request)
        _purchase_call(lambda: purchases.update_order(order, request.data, identity))
    return Response(purchases.order_detail(order.id))


@api_view(['POST'])
@handle_errors
def purchase_order_cancel(request: Request, order_id: int) -> Response:
    """Annuler une commande saisie par erreur ou jamais passee (rien recu)."""
    require_front(request, GESTION)
    order = _order(order_id)
    identity = operation_identity(request)
    _purchase_call(lambda: purchases.cancel_order(order, identity))
    return Response(purchases.order_detail(order.id))


@api_view(['POST'])
@handle_errors
def purchase_order_close(request: Request, order_id: int) -> Response:
    """Clore une commande reçue en partie (le reste ne viendra pas) ; {"reopen": true} la rouvre."""
    require_front(request, GESTION)
    order = _order(order_id)
    identity = operation_identity(request)
    reopen = bool(request.data.get('reopen'))
    _purchase_call(lambda: purchases.close_order(order, identity, reopen))
    return Response(purchases.order_detail(order.id))


@api_view(['POST'])
@handle_errors
def purchase_order_receive(request: Request, order_id: int) -> Response:
    """Reception d'une commande, toutes les lignes d'un coup : {"user", "receptions": [{"line", "count",
    "peremption", "location", "sealed_pack", "packs"}]} (une meme ligne peut revenir avec plusieurs dates de
    peremption). Cree les items rattaches a leur ligne et renvoie {"items", "sealed_packs", "receptions": [items et
    paquets de chaque reception, dans l'ordre], "order"} : le poste imprime toutes les etiquettes ensemble."""
    require_front(request, GESTION)
    order = _order(order_id)
    if order.cancelled is not None:
        raise ApiError('Commande annulée : rien à réceptionner')
    rows = request.data.get('receptions')
    if not isinstance(rows, list) or not rows:
        raise ApiError('Rien à réceptionner')
    if len(rows) > purchases.MAX_LINES:
        raise ApiError(f'{purchases.MAX_LINES} réceptions au plus à la fois')
    lines = {line.id: line for line in order.lines.select_related('item_type')}
    identity = operation_identity(request)
    label = purchases.order_label(order)
    now = timezone.now()
    created_all: list[Items] = []
    packs_all: list[SealedPacks] = []
    receptions: list[dict[str, Any]] = []
    with transaction.atomic():
        for row in rows:
            if not isinstance(row, dict):
                raise ApiError('Réception invalide')
            line = lines.get(parse_int(row.get('line'), 'line'))
            if line is None:
                raise ApiError("Cette ligne n'est pas dans la commande")
            created, sealed_packs = receive_items(line.item_type, row, identity, now, order_line=line,
                                                  order_label=label)
            created_all += created
            packs_all += sealed_packs
            receptions.append(reception_response(created, sealed_packs))
        for line in lines.values():
            purchases.mark_delivered(line)
        notifications.check_stock_levels(sorted({line.item_type_id for line in lines.values()}))
    response = reception_response(created_all, packs_all)
    response['receptions'] = receptions
    response['order'] = purchases.order_detail(order.id)
    return Response(response, status=status.HTTP_201_CREATED)


@api_view(['GET'])
@handle_errors
def purchase_suggestions(request: Request) -> Response:
    """Commandes recommandees groupees par fournisseur (le moins cher connu), pour economiser la livraison."""
    require_front(request, GESTION)
    return Response(purchases.suggestions())


@api_view(['GET'])
@handle_errors
def purchase_stats(request: Request) -> Response:
    """Statistiques des achats : depenses, fournisseurs, prix par type, valeur du stock, estimations."""
    require_front(request, GESTION)
    return Response(purchases.stats())
