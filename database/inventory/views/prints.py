# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          views/prints.py
#       -\-    _|__
#        |\___/  . \        Created on 16 Nov. 2026 at 19:45
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Etiquettes d'items demandees depuis le web (admins) et imprimees au poste (voir label_prints.py)."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any
from collections.abc import Callable

from .common import *  # noqa: F401,F403
from .public import *  # noqa: F401,F403
from .local import *  # noqa: F401,F403
from .notif_settings import *  # noqa: F401,F403
from .douchette import *  # noqa: F401,F403
from .lists import *  # noqa: F401,F403

if TYPE_CHECKING:
    from rest_framework.request import Request


# Etiquettes d'items demandees depuis le web (admins) et imprimees au poste (voir label_prints.py)
# ----------------------------------------------------------------------------------------------------------------------

def _label_print_call(function: Callable[..., Any], *args: Any) -> Any:
    try:
        return function(*args)
    except label_prints.PrintError as exc:
        raise ApiError(exc.message, exc.code)


@api_view(['POST'])
@handle_errors
def label_prints_create(request: Request) -> Response:
    """Web, roles gestion et admin : {"user", "type", "count", "peremption"} → demande envoyee aux postes (fenetre
    « Impressions en attente »). Les items ne sont crees qu'a l'impression."""
    user = connected_user(request, GESTION)
    data = request.data
    count = parse_int(data.get('count', 1), 'count', 1, label_prints.MAX_COUNT)
    row = _label_print_call(label_prints.create, f'{SOURCE_VERIFIED}:{user.matricule}', str(data.get('type', '')),
                            count, parse_date(data.get('peremption'), 'peremption'))
    return Response(label_prints.print_dict(row), status=status.HTTP_201_CREATED)


@api_view(['POST'])
@handle_errors
def label_prints_list(request: Request) -> Response:
    """Web, roles gestion et admin : types d'items proposes et demandes d'impression (en attente, puis des 7 derniers
    jours)."""
    user = connected_user(request, GESTION)
    data = label_prints.web_list()
    if user.role == Role.ADMIN:  # files d'impression du poste, toutes demandes confondues, gerees a distance
        data['queues'] = label_prints.admin_queues()['queues']
    return Response(data)


@api_view(['POST'])
@handle_errors
def label_print_queues(request: Request) -> Response:
    """Gestion et admins, du web ou d'un autre ordinateur : files d'impression en attente au poste (une par personne
    qui demande), avec leurs demandes."""
    connected_user(request, GESTION)
    return Response(label_prints.admin_queues(matricules=is_front(request)))


@api_view(['POST'])
@handle_errors
def label_print_queue_action(request: Request, action: str) -> Response:
    """Gestion et admins, du web ou d'un autre ordinateur : {"queue"} → « launch » (le poste imprime la file a son prochain
    passage, toutes les 20 s) ou « cancel » (toute la file annulee)."""
    user = connected_user(request, GESTION)
    by = f'{SOURCE_VERIFIED}:{user.matricule}'
    queue = request.data.get('queue')
    if action == 'launch':
        return Response({'launched': _label_print_call(label_prints.launch_queue, queue, by)})
    if action == 'cancel':
        return Response({'cancelled': _label_print_call(label_prints.cancel_queue, queue, by)})
    raise ApiError('Action inconnue : launch ou cancel', status.HTTP_404_NOT_FOUND)


@api_view(['POST'])
@handle_errors
def label_print_cancel(request: Request, print_id: int) -> Response:
    """Web, roles gestion et admin : annule une demande pas encore imprimee."""
    user = connected_user(request, GESTION)
    row = _label_print_call(label_prints.cancel, print_id, f'{SOURCE_VERIFIED}:{user.matricule}')
    return Response(label_prints.print_dict(row))


def require_poste(request: Request) -> None:
    """Routes des postes qui ne demandent personne de connecte : la machine suffit (API locale ou cle de front)."""
    if not is_front(request):
        raise ApiError("Réservé aux postes", status.HTTP_403_FORBIDDEN)


@api_view(['GET'])
@handle_errors
def label_prints_poste(request: Request) -> Response:
    """Poste, meme sans personne de connecte : impressions en attente et imprimees dans les dernieres 24h."""
    require_poste(request)
    return Response(label_prints.poste_list())


@api_view(['POST'])
@handle_errors
def label_prints_send(request: Request) -> Response:
    """Front d'un autre ordinateur (API distante, cle de front) : « Imprimer » envoie au poste les etiquettes deja
    rendues {"title", "labels": [{description, width, height, ppm, bits}]}. Le poste principal (API locale, ou cle marquee
    poste) les imprime depuis sa fenetre « Impressions en attente »."""
    require_poste(request)
    row = _label_print_call(label_prints.create_labels, front_identity(request),
                            getattr(request, 'qrprotec_front', ''), request.data.get('title', ''),
                            request.data.get('labels'))
    return Response(label_prints.print_dict(row), status=status.HTTP_201_CREATED)


@api_view(['POST'])
@handle_errors
def server_problem_report(request: Request) -> Response:
    """Poste d'une autre machine (kiosk relie a un back distant) : sa sauvegarde ou son envoi sur la machine distante
    a echoue {"source": "backup" | "sync", "message"}. Notification « Problème de serveur » des admins, avec le nom
    de la cle du poste (le detail est dans le journal du poste)."""
    require_poste(request)
    source = request.data.get('source')
    if source not in ('backup', 'sync'):
        raise ApiError('source : backup ou sync attendu')
    front = getattr(request, 'qrprotec_front', '') or 'poste'
    key = f'{source}:{front}'[:32]
    if request.data.get('resolved'):
        notifications.resolve_server_problem(key)
        return Response({'sent': False})
    sent = notifications.server_problem(key, str(request.data.get('message') or ''), where=front,
                                        journal='journalctl -u qrprotec-backup sur ce poste')
    return Response({'sent': sent})


@api_view(['GET'])
@handle_errors
def label_print_detail(request: Request, print_id: int) -> Response:
    """Poste : demande complete (items crees ou images), pour « Réimprimer »."""
    require_poste(request)
    return Response(_label_print_call(label_prints.detail, print_id))


@api_view(['POST'])
@handle_errors
def label_print_print(request: Request, print_id: int) -> Response:
    """Poste, meme sans personne de connecte : « Lancer l'impression ». Cree les items (reception au nom de l'admin
    qui les a demandes) et repond la demande avec ses items, dont le poste imprime les etiquettes."""
    require_poste(request)
    return Response(_label_print_call(label_prints.print_now, print_id, getattr(request, 'qrprotec_front', '')))
