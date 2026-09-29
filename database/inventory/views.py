# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          views.py
#       -\-    _|__
#        |\___/  . \        Created on 29 Sep. 2026 at 14:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""API JSON.

Deux familles de vues :
- publiques (servies sur les deux ports) : toute ecriture exige la cle de l'objet modifie
  (ex: cle du lot pour une verif), sauf si la requete arrive par l'API locale ;
- locales (servies uniquement sur le port local) : gestion complete sans cle.
"""

import re
from datetime import date

from django.db import IntegrityError, transaction
from django.shortcuts import get_object_or_404
from django.utils import timezone
from rest_framework import status
from rest_framework.decorators import api_view
from rest_framework.response import Response

from . import notifications
from . import serializers as ser
from . import services
from .idendity import identity_from_request
from .models import (
    TYPE_LENGTH, Items, ItemsPacks, ItemType, LotRequirements, Lots, LotType, NotificationSettings, SealedPacks,
    Secouristes, SeenWhile, SmsRecipient, Verifs, qrprotec_setting,
)

CODE_RE = re.compile(r'^[A-Za-z0-9]{%d}$' % TYPE_LENGTH)
MATRICULE_RE = re.compile(r'^[A-Za-z0-9_-]{1,16}$')


class ApiError(Exception):
    def __init__(self, message, code=status.HTTP_400_BAD_REQUEST):
        super().__init__(message)
        self.message = message
        self.code = code


def is_local(request) -> bool:
    return getattr(request, 'qrprotec_local', False)


def error(message, code=status.HTTP_400_BAD_REQUEST):
    return Response({'error': message}, status=code)


def handle_errors(view):
    def wrapper(request, *args, **kwargs):
        try:
            return view(request, *args, **kwargs)
        except ApiError as exc:
            return error(exc.message, exc.code)
        except ValueError as exc:
            return error(str(exc))
    wrapper.__name__ = view.__name__
    wrapper.__doc__ = view.__doc__
    return wrapper


def parse_date(value, field):
    if value in (None, ''):
        return None
    try:
        return date.fromisoformat(str(value))
    except ValueError:
        raise ApiError(f"{field} : date invalide (format AAAA-MM-JJ attendu)")


def parse_int(value, field, minimum=0, maximum=None):
    try:
        number = int(value)
    except (TypeError, ValueError):
        raise ApiError(f"{field} : entier attendu")
    if number < minimum or (maximum is not None and number > maximum):
        raise ApiError(f"{field} : valeur hors limites")
    return number


def iid_list(data):
    items = data.get('items', [])
    if not isinstance(items, list) or not all(isinstance(iid, str) for iid in items):
        raise ApiError("items : liste d'iids attendue")
    return [iid.strip() for iid in items if iid.strip()]


def require_lot_key(request, lot):
    """Sur l'API publique, toute ecriture sur un lot exige sa cle."""
    if is_local(request):
        return
    if not lot.check_key(request.data.get('key')):
        raise ApiError("Cle du lot invalide ou expiree", status.HTTP_403_FORBIDDEN)


# ----------------------------------------------------------------------------------------------------------------------
# Vues publiques (disponibles sur les deux API)
# ----------------------------------------------------------------------------------------------------------------------

@api_view(['GET'])
def health(request):
    return Response({
        'status': 'ok',
        'api': 'local' if is_local(request) else 'public',
        'today': timezone.localdate().isoformat(),
        'public_base_url': qrprotec_setting('PUBLIC_BASE_URL'),
    })


@api_view(['POST'])
@handle_errors
def auth(request):
    """Confirme l'identite d'un secouriste (matricule + cle de badge, valable un an)."""
    matricule = str(request.data.get('matricule', ''))
    user = Secouristes.objects.filter(matricule=matricule).first()
    if user is None or not user.check_key(request.data.get('key')):
        raise ApiError("Badge invalide ou expire", status.HTTP_403_FORBIDDEN)
    return Response(ser.user_dict(user))


@api_view(['GET'])
@handle_errors
def item_detail(request, iid):
    item = get_object_or_404(Items.objects.select_related('pack__item_type', 'location'), iid=iid)
    return Response(ser.item_dict(item))


@api_view(['GET'])
@handle_errors
def lot_detail(request, lot_id):
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    local = is_local(request)
    if not local and not lot.active:
        raise ApiError("Lot inconnu", status.HTTP_404_NOT_FOUND)
    data = ser.lot_dict(lot, local=local, with_items=True)
    # QR code de scelle : ?seal=CODE -> 'valid' (scelle intact), 'unsealed' (brise), 'wrong' (ancien scelle)
    seal = request.query_params.get('seal')
    if seal is not None:
        data['seal_check'] = 'valid' if lot.check_seal(seal) else ('wrong' if lot.is_sealed else 'unsealed')
    return Response(data)


@api_view(['POST'])
@handle_errors
def lot_unseal(request, lot_id):
    """Scelle brise (ouverture du lot). Sur l'API publique, exige la cle du lot (etiquette privee)."""
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    require_lot_key(request, lot)
    identity = identity_from_request(request.data, is_local(request))
    with transaction.atomic():
        services.break_seal(lot, identity, str(request.data.get('reason', '')).strip()[:64] or 'ouverture')
    return Response(ser.lot_dict(lot, local=is_local(request), with_items=True))


@api_view(['POST'])
@handle_errors
def lot_verif(request, lot_id):
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    require_lot_key(request, lot)
    identity = identity_from_request(request.data, is_local(request))
    report = services.perform_verif(lot, iid_list(request.data), identity)
    return Response(report)


@api_view(['POST'])
@handle_errors
def lot_add_items(request, lot_id):
    lot = get_object_or_404(Lots, id=lot_id)
    require_lot_key(request, lot)
    identity = identity_from_request(request.data, is_local(request))
    return Response(services.move_items(iid_list(request.data), lot, identity, SeenWhile.ADD))


@api_view(['GET'])
@handle_errors
def sealed_pack_detail(request, pack_id):
    sealed_pack = get_object_or_404(SealedPacks.objects.select_related('item_type'), id=pack_id)
    return Response(ser.sealed_pack_dict(sealed_pack))


# ----------------------------------------------------------------------------------------------------------------------
# Vues locales (gestion, sans cle)
# ----------------------------------------------------------------------------------------------------------------------

@api_view(['GET', 'POST'])
@handle_errors
def item_types(request):
    if request.method == 'GET':
        return Response([ser.item_type_dict(item_type) for item_type in ItemType.objects.order_by('name')])
    data = request.data
    code = str(data.get('type', '')).strip()
    if not CODE_RE.match(code):
        raise ApiError(f"Le code du type doit faire exactement {TYPE_LENGTH} caracteres alphanumeriques")
    if ItemType.objects.filter(type=code).exists():
        raise ApiError(f"Le type {code} existe deja", status.HTTP_409_CONFLICT)
    name = str(data.get('name', '')).strip()
    if not name:
        raise ApiError("Nom obligatoire")
    item_type = ItemType.objects.create(
        type=code,
        name=name[:64],
        description=str(data.get('description', '')),
        min_quantity=parse_int(data.get('min_quantity', 0), 'min_quantity'),
        perissable=bool(data.get('perissable', False)),
        default_pack_size=parse_int(data.get('default_pack_size', 1), 'default_pack_size', 1),
    )
    return Response(ser.item_type_dict(item_type), status=status.HTTP_201_CREATED)


@api_view(['GET', 'PATCH'])
@handle_errors
def item_type_detail(request, type_code):
    item_type = get_object_or_404(ItemType, type=type_code)
    if request.method == 'PATCH':
        data = request.data
        if 'name' in data:
            item_type.name = str(data['name'])[:64]
        if 'description' in data:
            item_type.description = str(data['description'])
        if 'min_quantity' in data:
            item_type.min_quantity = parse_int(data['min_quantity'], 'min_quantity')
        if 'default_pack_size' in data:
            item_type.default_pack_size = parse_int(data['default_pack_size'], 'default_pack_size', 1)
        if 'perissable' in data:
            perissable = bool(data['perissable'])
            if not perissable and ItemsPacks.objects.filter(item_type=item_type, peremption__isnull=False).exists():
                raise ApiError("Des items dates existent deja pour ce type")
            item_type.perissable = perissable
        item_type.save()
    return Response(ser.item_type_dict(item_type))


@api_view(['GET'])
@handle_errors
def items(request):
    queryset = Items.objects.select_related('pack__item_type', 'location').order_by('-added', 'iid')
    params = request.query_params
    if params.get('type'):
        queryset = queryset.filter(pack__item_type_id=params['type'])
    if params.get('location') == 'stock':
        queryset = queryset.filter(location__isnull=True)
    elif params.get('location'):
        queryset = queryset.filter(location_id=params['location'])
    if params.get('status'):
        queryset = queryset.filter(status=params['status'])
    if params.get('q'):
        queryset = queryset.filter(iid__icontains=params['q'])
    limit = parse_int(params.get('limit', 200), 'limit', 1, 5000)
    today = timezone.localdate()
    return Response([ser.item_dict(item, today) for item in queryset[:limit]])


@api_view(['POST'])
@handle_errors
def items_batch(request):
    """Reception : cree `count` items (et optionnellement un paquet scelle qui les contient)."""
    data = request.data
    item_type = get_object_or_404(ItemType, type=str(data.get('type', '')))
    peremption = parse_date(data.get('peremption'), 'peremption')
    count = parse_int(data.get('count', 1), 'count', 1, 10000)
    identity = identity_from_request(data, True)
    location = None
    if data.get('location'):
        location = get_object_or_404(Lots, id=data['location'])
    with transaction.atomic():
        sealed_pack = None
        if data.get('sealed_pack'):
            sealed_pack = SealedPacks.objects.create(
                item_type=item_type,
                peremption=peremption if item_type.perissable else None,
                count=count,
                created_by=identity[:32],
            )
        created = ItemsPacks.objects.add_items(
            item_type, peremption, count, identity[:32], location=location, sealed_pack=sealed_pack
        )
        notifications.check_stock_levels([item_type.type])
    today = timezone.localdate()
    response = {'items': [ser.item_dict(item, today) for item in created]}
    if sealed_pack is not None:
        response['sealed_pack'] = ser.sealed_pack_dict(sealed_pack)
    return Response(response, status=status.HTTP_201_CREATED)


@api_view(['POST'])
@handle_errors
def item_delete(request, iid):
    """Marque un item comme supprime (ce n'est pas la voie normale : les verifs detectent les disparitions)."""
    item = get_object_or_404(Items.objects.select_related('pack__item_type'), iid=iid)
    reason = str(request.data.get('reason', '')).strip()
    if not reason:
        raise ApiError("Une raison est obligatoire pour supprimer un item")
    services.mark_deleted(item, identity_from_request(request.data, True)[:32], reason)
    return Response(ser.item_dict(item))


@api_view(['POST'])
@handle_errors
def item_restore(request, iid):
    item = get_object_or_404(Items.objects.select_related('pack__item_type'), iid=iid)
    services.restore(item, identity_from_request(request.data, True))
    return Response(ser.item_dict(item))


@api_view(['POST'])
@handle_errors
def items_to_stock(request):
    identity = identity_from_request(request.data, True)
    return Response(services.move_items(iid_list(request.data), None, identity, SeenWhile.REMOVE))


@api_view(['GET'])
@handle_errors
def stock(request):
    soon = parse_int(request.query_params.get('soon_days', 30), 'soon_days', 0, 3650)
    # les peremptions font baisser le stock sans evenement : on verifie les seuils a chaque consultation
    notifications.check_stock_levels()
    return Response(services.stock_status(soon))


@api_view(['POST'])
@handle_errors
def stock_verif(request):
    identity = identity_from_request(request.data, True)
    return Response(services.perform_verif(None, iid_list(request.data), identity))


@api_view(['GET'])
@handle_errors
def sealed_packs(request):
    queryset = SealedPacks.objects.select_related('item_type').order_by('-created')
    if request.query_params.get('opened') == '0':
        queryset = queryset.filter(opened__isnull=True)
    return Response([ser.sealed_pack_dict(pack) for pack in queryset[:500]])


@api_view(['POST'])
@handle_errors
def sealed_pack_open(request, pack_id):
    sealed_pack = get_object_or_404(SealedPacks.objects.select_related('item_type'), id=pack_id)
    identity = identity_from_request(request.data, True)
    now = timezone.now()
    # un paquet deja ouvert garde sa date d'ouverture : l'appel sert alors a reimprimer les etiquettes
    if sealed_pack.opened is None:
        with transaction.atomic():
            sealed_pack.opened = now
            sealed_pack.opened_by = identity[:32]
            sealed_pack.save(update_fields=['opened', 'opened_by'])
            Items.objects.filter(sealed_pack=sealed_pack).update(
                last_seen=now, last_seen_by=identity, last_seen_while=SeenWhile.OPEN
            )
    today = timezone.localdate()
    data = ser.sealed_pack_dict(sealed_pack, with_items=False)
    data['items'] = [
        ser.item_dict(item, today)
        for item in sealed_pack.items.select_related('pack__item_type', 'location').order_by('iid')
    ]
    return Response(data)


@api_view(['POST'])
@handle_errors
def sealed_pack_close(request, pack_id):
    """Annule une ouverture faite par erreur : le paquet redevient ferme (les etiquettes deja imprimees
    restent valables, les items sont les memes)."""
    sealed_pack = get_object_or_404(SealedPacks.objects.select_related('item_type'), id=pack_id)
    sealed_pack.opened = None
    sealed_pack.opened_by = ''
    sealed_pack.save(update_fields=['opened', 'opened_by'])
    return Response(ser.sealed_pack_dict(sealed_pack))


@api_view(['GET', 'POST'])
@handle_errors
def lot_types(request):
    if request.method == 'GET':
        return Response([ser.lot_type_dict(lot_type) for lot_type in LotType.objects.order_by('name')])
    data = request.data
    code = str(data.get('type', '')).strip()
    if not CODE_RE.match(code):
        raise ApiError(f"Le code du type de lot doit faire exactement {TYPE_LENGTH} caracteres alphanumeriques")
    if LotType.objects.filter(type=code).exists():
        raise ApiError(f"Le type de lot {code} existe deja", status.HTTP_409_CONFLICT)
    name = str(data.get('name', '')).strip()
    if not name:
        raise ApiError("Nom obligatoire")
    lot_type = LotType.objects.create(
        type=code, name=name[:64], description=str(data.get('description', '')),
        created_by=identity_from_request(data, True)[:32],
    )
    return Response(ser.lot_type_dict(lot_type), status=status.HTTP_201_CREATED)


@api_view(['GET', 'PATCH'])
@handle_errors
def lot_type_detail(request, type_code):
    lot_type = get_object_or_404(LotType, type=type_code)
    if request.method == 'PATCH':
        if 'name' in request.data:
            lot_type.name = str(request.data['name'])[:64]
        if 'description' in request.data:
            lot_type.description = str(request.data['description'])
        lot_type.save()
    return Response(ser.lot_type_dict(lot_type))


@api_view(['PUT'])
@handle_errors
def lot_type_requirements(request, type_code):
    """Remplace la liste des exigences : [{"type": "serphy", "quantity": 4}, ...]."""
    lot_type = get_object_or_404(LotType, type=type_code)
    rows = request.data.get('requirements', [])
    if not isinstance(rows, list):
        raise ApiError("requirements : liste attendue")
    with transaction.atomic():
        LotRequirements.objects.filter(lot_type=lot_type).delete()
        for row in rows:
            item_type = get_object_or_404(ItemType, type=str(row.get('type', '')))
            quantity = parse_int(row.get('quantity', 1), 'quantity', 1)
            location = str(row.get('location', '') or '').strip()[:64]
            try:
                LotRequirements.objects.create(
                    lot_type=lot_type, item_type=item_type, quantity=quantity, location=location
                )
            except IntegrityError:
                raise ApiError(f"Le type {item_type.type} apparait plusieurs fois")
        lot_type.version += 1
        lot_type.valid_version = lot_type.version
        lot_type.save(update_fields=['version', 'valid_version'])
    return Response(ser.lot_type_dict(lot_type))


@api_view(['GET', 'POST'])
@handle_errors
def lots(request):
    if request.method == 'GET':
        queryset = Lots.objects.select_related('lot_type').order_by('lot_type__name', 'name')
        if request.query_params.get('all') != '1':
            queryset = queryset.filter(active=True)
        return Response([ser.lot_dict(lot, local=True) for lot in queryset])
    data = request.data
    lot_type = get_object_or_404(LotType, type=str(data.get('lot_type', '')))
    name = str(data.get('name', '')).strip()
    if not name:
        raise ApiError("Nom obligatoire")
    lot = Lots(
        lot_type=lot_type,
        name=name[:64],
        name_short=str(data.get('name_short', '') or name)[:16],
        created_by=identity_from_request(data, True)[:32],
    )
    lot.save()
    return Response(ser.lot_dict(lot, local=True, with_items=True), status=status.HTTP_201_CREATED)


@api_view(['PATCH'])
@handle_errors
def lot_update(request, lot_id):
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    data = request.data
    for field, length in (('name', 64), ('name_short', 16)):
        if field in data:
            setattr(lot, field, str(data[field])[:length])
    for field in ('active',):
        if field in data:
            setattr(lot, field, bool(data[field]))
    lot.save()
    return Response(ser.lot_dict(lot, local=True, with_items=True))


@api_view(['POST'])
@handle_errors
def lot_seal(request, lot_id):
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    identity = identity_from_request(request.data, True)
    services.seal_lot(lot, identity, str(request.data.get('seal_number', '')).strip(), bool(request.data.get('force')))
    return Response(ser.lot_dict(lot, local=True, with_items=True))


@api_view(['POST'])
@handle_errors
def lot_rotate_key(request, lot_id):
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    lot.rotate_key()
    lot.save(update_fields=['verif_key', 'verif_key_expires'])
    return Response(ser.lot_dict(lot, local=True, with_items=True))


@api_view(['GET'])
@handle_errors
def lot_verifs(request, lot_id):
    lot = get_object_or_404(Lots, id=lot_id)
    return Response([ser.verif_dict(verif) for verif in Verifs.objects.filter(lot=lot).order_by('-datetime')[:100]])


@api_view(['GET'])
def setup(request):
    """Etat de premiere configuration : le front propose de creer un responsable s'il n'y en a aucun
    avec un badge valide (premiere installation, ou tous les badges responsables expires)."""
    today = timezone.localdate()
    admins = Secouristes.objects.filter(privileged=True, active=True, key_expires__gte=today).count()
    return Response({
        'users': Secouristes.objects.count(),
        'admins': admins,
        'needs_admin': admins == 0,
    })


@api_view(['GET', 'POST'])
@handle_errors
def users(request):
    if request.method == 'GET':
        return Response([ser.user_dict(user, local=True) for user in Secouristes.objects.order_by('nom', 'prenom')])
    data = request.data
    matricule = str(data.get('matricule', '')).strip()
    if not MATRICULE_RE.match(matricule):
        raise ApiError("Matricule invalide (1 a 16 caracteres alphanumeriques)")
    if Secouristes.objects.filter(matricule=matricule).exists():
        raise ApiError(f"Le matricule {matricule} existe deja", status.HTTP_409_CONFLICT)
    nom = str(data.get('nom', '')).strip()
    prenom = str(data.get('prenom', '')).strip()
    if not nom or not prenom:
        raise ApiError("Nom et prenom obligatoires")
    user = Secouristes(
        matricule=matricule, nom=nom[:32], prenom=prenom[:32], privileged=bool(data.get('privileged', False))
    )
    user.renew_key()
    user.save()
    return Response(ser.user_dict(user, local=True), status=status.HTTP_201_CREATED)


@api_view(['GET', 'PATCH'])
@handle_errors
def user_detail(request, matricule):
    user = get_object_or_404(Secouristes, matricule=matricule)
    if request.method == 'PATCH':
        data = request.data
        for field in ('nom', 'prenom'):
            if field in data:
                setattr(user, field, str(data[field]).strip()[:32])
        for field in ('privileged', 'active'):
            if field in data:
                setattr(user, field, bool(data[field]))
        user.save()
    return Response(ser.user_dict(user, local=True))


@api_view(['POST'])
@handle_errors
def user_renew_key(request, matricule):
    user = get_object_or_404(Secouristes, matricule=matricule)
    user.renew_key()
    user.save(update_fields=['key', 'key_expires'])
    return Response(ser.user_dict(user, local=True))


# ----------------------------------------------------------------------------------------------------------------------
# Notifications SMS (API Free Mobile)
# ----------------------------------------------------------------------------------------------------------------------

def _notification_response():
    return Response(ser.notification_settings_dict(NotificationSettings.get(), SmsRecipient.objects.order_by('name')))


@api_view(['GET', 'PATCH'])
@handle_errors
def notification_settings(request):
    if request.method == 'PATCH':
        settings_row = NotificationSettings.get()
        if 'enabled' in request.data:
            settings_row.enabled = bool(request.data['enabled'])
        events = request.data.get('events', {})
        if not isinstance(events, dict):
            raise ApiError('events : objet attendu')
        for event in notifications.EVENTS:
            if event in events:
                setattr(settings_row, event, bool(events[event]))
        settings_row.save()
    return _notification_response()


def _recipient_fields(recipient, data, creating):
    for field, length in (('name', 64), ('user', 32), ('password', 64)):
        if field in data:
            value = str(data[field] or '').strip()[:length]
            if field == 'password' and not value and not creating:
                continue  # champ vide : cle inchangee
            setattr(recipient, field, value)
    if 'active' in data:
        recipient.active = bool(data['active'])
    if not recipient.name or not recipient.user or not recipient.password:
        raise ApiError('Nom, identifiant et clé API obligatoires')


@api_view(['POST'])
@handle_errors
def sms_recipients(request):
    recipient = SmsRecipient()
    _recipient_fields(recipient, request.data, True)
    recipient.save()
    return _notification_response()


@api_view(['PATCH', 'DELETE'])
@handle_errors
def sms_recipient_detail(request, recipient_id):
    recipient = get_object_or_404(SmsRecipient, id=recipient_id)
    if request.method == 'DELETE':
        recipient.delete()
    else:
        _recipient_fields(recipient, request.data, False)
        recipient.save()
    return _notification_response()


@api_view(['POST'])
@handle_errors
def sms_test(request):
    """Envoie un SMS de test (a un destinataire, ou a tous les actifs), meme si les notifications sont coupees."""
    recipient_id = request.data.get('recipient')
    if recipient_id:
        recipients = [get_object_or_404(SmsRecipient, id=recipient_id)]
    else:
        recipients = list(SmsRecipient.objects.filter(active=True))
    if not recipients:
        raise ApiError('Aucun destinataire actif')
    identity = identity_from_request(request.data, True)
    count = notifications.send(f'QRProtec : SMS de test envoyé par {services.display_name(identity)}.', recipients)
    return Response({'sent': count})
