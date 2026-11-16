# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          views/douchette.py
#       -\-    _|__
#        |\___/  . \        Created on 16 Nov. 2026 at 19:45
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Telephone utilise comme douchette (voir remote_scanner.py)."""

from .common import *  # noqa: F401,F403
from .public import *  # noqa: F401,F403
from .local import *  # noqa: F401,F403
from .notif_settings import *  # noqa: F401,F403


# Telephone utilise comme douchette (voir remote_scanner.py)
# ----------------------------------------------------------------------------------------------------------------------

def _scanner_session_dict(session):
    data = session.status()
    data['url'] = ser.public_url('scanner', s=session.id, k=session.key)
    return data


@api_view(['POST'])
@handle_errors
def remote_scanner_sessions(request):
    """Cree une session : le front affiche le QR code de `url`, puis se connecte a /ws/scanner/front?s=ID."""
    if not scanner_hub.enabled:
        raise ApiError("Le téléphone-douchette nécessite le serveur `manage.py serve` (WebSockets)",
                       status.HTTP_503_SERVICE_UNAVAILABLE)
    minutes = parse_int(request.data.get('timeout_minutes', 5), 'timeout_minutes', 1, 24 * 60)
    session = scanner_hub.create(minutes * 60)
    return Response(_scanner_session_dict(session), status=status.HTTP_201_CREATED)


@api_view(['GET', 'DELETE'])
@handle_errors
def remote_scanner_session(request, session_id):
    session = scanner_hub.get(session_id)
    if session is None:
        raise ApiError('Session fermée', status.HTTP_404_NOT_FOUND)
    if request.method == 'DELETE':
        scanner_hub.close(session_id, 'fermée depuis le poste')
        return Response({'closed': True})
    return Response(_scanner_session_dict(session))


@api_view(['GET'])
def remote_scanner_check(request):
    """Public (telephone) : la session existe-t-elle encore ? Exige la cle du QR code."""
    session = scanner_hub.check_key(request.query_params.get('s', ''), request.query_params.get('k', ''))
    if session is None:
        return error('Session fermée : scannez un nouveau QR code sur le poste', status.HTTP_404_NOT_FOUND)
    return Response({'open': True, 'front_connected': session.front is not None})
