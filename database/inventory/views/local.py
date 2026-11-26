# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          views/local.py
#       -\-    _|__
#        |\___/  . \        Created on 16 Nov. 2026 at 19:45
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Vues locales (gestion, sans cle) : types, lots, items, stock, utilisateurs, journal, impressions."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from .common import *  # noqa: F401,F403
from .public import *  # noqa: F401,F403
from .public import _with_out

if TYPE_CHECKING:
    from django.http.response import HttpResponse
    from datetime import datetime
    from inventory.models import LotType, LotTypeSlot, Lots, SealedPacks, Secouristes, StockOrder
    from rest_framework.request import Request
    from rest_framework.response import Response
    from django.http import HttpRequest


# Vues locales (gestion, sans cle)
# ----------------------------------------------------------------------------------------------------------------------

@api_view(['GET', 'POST'])
@handle_errors
def item_types(request: Request) -> Response:
    """Types d'items, archives compris (champ archived) : les fronts ne proposent que les types actifs."""
    if request.method != 'GET':
        require_front(request, GESTION)
    if request.method == 'GET':
        return Response([ser.item_type_dict(item_type) for item_type in ItemType.objects.order_by('name')])
    data = request.data
    code = str(data.get('type', '')).strip()
    if not CODE_RE.match(code):
        raise ApiError(f"Le code du type doit faire exactement {TYPE_LENGTH} caracteres alphanumeriques")
    if ItemType.objects.filter(type=code).exists():
        raise ApiError(f"Le type {code} existe deja", status.HTTP_409_CONFLICT)
    name = str(data.get('name', '')).strip()[:64]
    if not name:
        raise ApiError("Nom obligatoire")
    ensure_name_free(ItemType.objects.all(), name, "Le type d'item")
    check_tear_off_tasks(bool(data.get('tear_off', False)), parse_tasks(data.get('tasks')))
    check_physical(bool(data.get('physical', True)), bool(data.get('perissable', False)),
                   bool(data.get('tear_off', False)), parse_int(data.get('revision_months', 0), 'revision_months')
                   + parse_int(data.get('disinfection_months', 0), 'disinfection_months'))
    with transaction.atomic():
        return _create_item_type(request, data, code, name)


def _create_item_type(request: Request, data: dict[str, Any], code: str, name: str) -> Response:
    item_type = ItemType.objects.create(
        type=code,
        name=name,
        description=str(data.get('description', '')),
        min_quantity=parse_int(data.get('min_quantity', 0), 'min_quantity'),
        perissable=bool(data.get('perissable', False)),
        default_pack_size=parse_int(data.get('default_pack_size', 1), 'default_pack_size', 1),
        tear_off=bool(data.get('tear_off', False)),
        label_style=parse_label_style(data.get('label_style', LabelStyle.QR)),
        consumable=bool(data.get('consumable', True)),
        physical=bool(data.get('physical', True)),
        tasks=parse_tasks(data.get('tasks')),
        revision_months=parse_int(data.get('revision_months', 0), 'revision_months', 0, MAX_REVISION_MONTHS),
        revision_tasks=parse_tasks(data.get('revision_tasks')),
        revision_usable=bool(data.get('revision_usable', True)),
        disinfection_months=parse_int(data.get('disinfection_months', 0), 'disinfection_months', 0, MAX_REVISION_MONTHS),
        disinfection_tasks=parse_tasks(data.get('disinfection_tasks')),
        disinfection_usable=bool(data.get('disinfection_usable', True)),
        lost_object=bool(data.get('lost_object', False)),
    )
    if any(key in data for key in ('is_group', 'unit', 'group', 'fills', 'default_subtype')):
        apply_units(item_type, data)
        item_type.save()
    journal.record(OperationKind.CATALOG, front_identity(request), f"type d'item créé : {item_type}", type=code)
    return Response(ser.item_type_dict(item_type), status=status.HTTP_201_CREATED)


@api_view(['GET', 'PATCH'])
@handle_errors
def item_type_detail(request: Request, type_code: str) -> Response:
    if request.method != 'GET':
        require_front(request, GESTION)
    item_type = get_object_or_404(ItemType, type=type_code)
    if request.method == 'PATCH':
        data = request.data
        if 'name' in data:
            name = str(data['name']).strip()[:64]
            if not name:
                raise ApiError("Nom obligatoire")
            ensure_name_free(ItemType.objects.exclude(type=item_type.type), name, "Le type d'item")
            item_type.name = name
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
        if 'tear_off' in data:
            item_type.tear_off = bool(data['tear_off'])
        if 'label_style' in data:
            item_type.label_style = parse_label_style(data['label_style'])
        if 'consumable' in data:
            item_type.consumable = bool(data['consumable'])
        if 'tasks' in data:
            item_type.tasks = parse_tasks(data['tasks'])
        if 'lost_object' in data:
            item_type.lost_object = bool(data['lost_object'])
        check_tear_off_tasks(item_type.tear_off, item_type.tasks)
        if 'physical' in data and bool(data['physical']) != item_type.physical:
            if not data['physical'] and ItemsPacks.objects.filter(item_type=item_type).exists():
                raise ApiError("Des items de ce type ont déjà des étiquettes : il ne peut pas devenir un item à cocher")
            item_type.physical = bool(data['physical'])
        had = {kind: item_type.has_maintenance(kind) for kind in MAINTENANCE}
        if 'revision_months' in data:
            item_type.revision_months = parse_int(data['revision_months'], 'revision_months', 0, MAX_REVISION_MONTHS)
        if 'revision_tasks' in data:
            item_type.revision_tasks = parse_tasks(data['revision_tasks'])
        if 'revision_usable' in data:
            item_type.revision_usable = bool(data['revision_usable'])
        if 'disinfection_months' in data:
            item_type.disinfection_months = parse_int(data['disinfection_months'], 'disinfection_months', 0,
                                                      MAX_REVISION_MONTHS)
        if 'disinfection_tasks' in data:
            item_type.disinfection_tasks = parse_tasks(data['disinfection_tasks'])
        if 'disinfection_usable' in data:
            item_type.disinfection_usable = bool(data['disinfection_usable'])
        check_physical(item_type.physical, item_type.perissable, item_type.tear_off,
                       item_type.revision_months + item_type.disinfection_months)
        if 'archived' in data:
            archived = bool(data['archived'])
            if archived:
                # un type encore attendu dans un lot rendrait ces lots incomplets pour toujours
                users = list(LotType.objects.filter(archived=False, requirements__item_type=item_type)
                             .order_by('name').values_list('name', flat=True).distinct())
                if users:
                    raise ApiError("Ce type est encore dans le contenu attendu de : " + ', '.join(users)
                                   + ". Retirez-le de ces types de lots d'abord.")
                lots = list(Lots.objects.filter(active=True, content__item_type=item_type, content__extra__gt=0)
                            .order_by('name').values_list('name', flat=True).distinct())
                if lots:
                    raise ApiError("Ce type est encore en plus dans le contenu des lots : " + ', '.join(lots)
                                   + ". Retirez-le de ces lots d'abord.")
            item_type.archived = archived
        apply_units(item_type, data)
        item_type.save()
        for kind, fields in MAINTENANCE.items():
            if item_type.has_maintenance(kind) and not had[kind]:
                # revision ou desinfection activee : premiere echeance des items deja la dans la periodicite, sauf date
                # deja prevue (le TECH peut ensuite la saisir a la main, item par item)
                Items.objects.filter(pack__item_type=item_type, **{f'{fields["due"]}__isnull': True}).update(
                    **{fields['due']: add_months(timezone.localdate(), getattr(item_type, fields['months']))})
        journal.record(OperationKind.CATALOG, front_identity(request),
                       f"type d'item modifié : {item_type} ({changed_fields(data, ITEM_TYPE_FIELDS)})",
                       type=item_type.type, fields=sorted(data.keys()))
    return Response(ser.item_type_dict(item_type))


ITEM_TYPE_FIELDS = {
    'name': 'nom', 'description': 'description', 'min_quantity': 'stock minimum',
    'default_pack_size': 'taille de paquet', 'perissable': 'périssable', 'tear_off': 'étiquette à déchirer',
    'label_style': "style d'étiquette",
    'consumable': 'consommable', 'physical': 'à cocher', 'tasks': 'tâches', 'revision_months': 'périodicité de révision',
    'revision_tasks': 'tâches de révision', 'revision_usable': 'utilisable en retard de révision',
    'disinfection_months': 'périodicité de désinfection', 'disinfection_tasks': 'tâches de désinfection',
    'disinfection_usable': 'utilisable en retard de désinfection',
    'archived': 'archivage', 'is_group': 'type parent', 'unit': 'unité', 'group': 'sous-type de', 'fills': 'unités remplies',
    'default_subtype': 'sous-type par défaut', 'lost_object': 'message objet perdu',
}
LOT_TYPE_FIELDS = {'name': 'nom', 'description': 'description', 'storage': 'rangement', 'unique': 'lot unique',
                   'seal_required': 'scellé obligatoire', 'archived': 'archivage', 'referentiel': 'référentiel',
                   'part_of': 'rangement de'}


def apply_referentiel(lot_type: LotType, data: dict[str, Any]) -> None:
    """Champs referentiel (bool) et part_of (code du referentiel dont ce type est un rangement, '' : aucun) d'un
    type de lot. Un referentiel n'a pas d'emplacements (ils sont effaces) et n'est le rangement de personne ; un
    rangement du stock n'est ni l'un ni l'autre. A enregistrer ensuite (save)."""
    referentiel = bool(data.get('referentiel', lot_type.referentiel))
    part_of = lot_type.part_of
    if 'part_of' in data:
        code = str(data.get('part_of') or '').strip()
        part_of = get_object_or_404(LotType, type=code) if code else None
    if lot_type.storage and (referentiel or part_of is not None):
        raise ApiError("Un rangement du stock n'est ni un référentiel ni le rangement d'un référentiel")
    if referentiel and part_of is not None:
        raise ApiError("Un référentiel n'est le rangement d'aucun autre : retirez « Rangement de » d'abord")
    if part_of is not None and (part_of.type == lot_type.type or not part_of.referentiel):
        raise ApiError(f"{part_of.name} n'est pas un référentiel : cochez « Référentiel » sur ce type d'abord")
    if not referentiel and lot_type.referentiel and lot_type.type and lot_type.rangements.exists():
        names = ', '.join(lot_type.rangements.order_by('name').values_list('name', flat=True))
        raise ApiError(f"Des rangements répartissent ce référentiel ({names}) : détachez-les d'abord")
    if referentiel and not lot_type.referentiel and lot_type.type:
        # la liste obligatoire n'a pas d'emplacements : ils sont portes par les rangements
        if lot_type.requirements.exclude(location='').exists() or lot_type.locations:
            lot_type.requirements.update(location='')
            lot_type.locations = []
            services.bump_lot_type_version(lot_type)
    lot_type.referentiel = referentiel
    lot_type.part_of = part_of


@api_view(['GET'])
@handle_errors
def items(request: Request) -> Response:
    require_front(request, GESTION)
    queryset = Items.objects.select_related('pack__item_type', 'location__lot_type').order_by('-added', 'iid')
    params = request.query_params
    if params.get('type'):
        queryset = queryset.filter(pack__item_type_id=params['type'])
    if params.get('location') == 'stock':
        queryset = queryset.filter(services.in_stock_q())  # rangements du stock compris
    elif params.get('location'):
        queryset = queryset.filter(location_id=params['location'])
    if params.get('status'):
        queryset = queryset.filter(status=params['status'])
    if params.get('q'):
        queryset = queryset.filter(iid__icontains=params['q'])
    limit = parse_int(params.get('limit', 200), 'limit', 1, 5000)
    today = timezone.localdate()
    return Response([ser.item_dict(item, today) for item in queryset[:limit]])


def storage_target(lot_id: str | None) -> Lots | None:
    """Rangement du stock ou ranger des items recus (None : le stock, sans rangement). Le lot du stock lui-meme
    vaut le stock ; un lot qui n'est pas un rangement du stock est refuse (un lot se remplit par reassort)."""
    if not lot_id:
        return None
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=str(lot_id))
    if lot.is_stock:
        return None
    if not lot.lot_type.storage:
        raise ApiError(f"{lot.name} n'est pas un rangement du stock")
    if not lot.active:
        raise ApiError(f"{lot.name} est archivé")
    return lot


@api_view(['POST'])
@handle_errors
def items_batch(request: Request) -> Response:
    """Reception : cree `count` items, ou `packs` paquets scelles de `count` items chacun (sealed_pack)."""
    require_front(request, GESTION)
    data = request.data
    item_type = get_object_or_404(ItemType, type=str(data.get('type', '')))
    identity = operation_identity(request, data)
    with transaction.atomic():
        created, sealed_packs = receive_items(item_type, data, identity, timezone.now())
        notifications.check_stock_levels([item_type.type])
    return Response(reception_response(created, sealed_packs), status=status.HTTP_201_CREATED)


def receive_items(item_type: ItemType, data: dict[str, Any], identity: str, now: datetime,
                  order_line: StockOrder | None = None, order_label: str = '') -> tuple[list[Items], list[SealedPacks]]:
    """Cree les items d'une reception (`count` items, ou `packs` paquets fermes de `count` items avec "sealed_pack"),
    "peremption", "location" (rangement du stock), et l'inscrit au journal. `order_line` : ligne de commande
    fournisseur d'ou viennent les items (reception d'une commande)."""
    peremption = parse_date(data.get('peremption'), 'peremption')
    count = parse_int(data.get('count', 1), 'count', 1, 10000)
    sealed = bool(data.get('sealed_pack'))
    pack_count = parse_int(data.get('packs', 1), 'packs', 1, 500) if sealed else 1
    if count * pack_count > 10000:
        raise ApiError('10 000 items au plus par réception')
    location = storage_target(data.get('location'))
    sealed_packs, created = [], []
    for _ in range(pack_count):
        sealed_pack = None
        if sealed:
            sealed_pack = SealedPacks.objects.create(
                item_type=item_type,
                peremption=peremption if item_type.perissable else None,
                count=count,
                created_by=identity[:32],
            )
            sealed_packs.append(sealed_pack)
        created += ItemsPacks.objects.add_items(
            item_type, peremption, count, identity[:32], location=location, sealed_pack=sealed_pack, now=now,
            order_line=order_line,
        )
    summary = f'{count} × {item_type.name}' if pack_count == 1 else f'{pack_count} paquets de {count} × {item_type.name}'
    if item_type.perissable and peremption:
        summary += f", péremption {peremption.strftime('%d/%m/%Y')}"
    if len(sealed_packs) == 1:
        summary += f' (paquet fermé {sealed_packs[0].id})'
    elif sealed_packs:
        summary += f' (paquets fermés {sealed_packs[0].id} à {sealed_packs[-1].id})'
    if location is not None:
        summary += f', rangés dans {location.name}'
    if order_label:
        summary += f', commande {order_label}'
    journal.record(OperationKind.RECEPTION, identity, summary, lot=location, type=item_type.type,
                   count=count * pack_count, peremption=peremption.isoformat() if peremption else None,
                   sealed_pack=sealed_packs[0].id if sealed_packs else None,
                   **({'sealed_packs': [pack.id for pack in sealed_packs]} if len(sealed_packs) > 1 else {}),
                   **({'order_line': order_line.id} if order_line is not None else {}),
                   items=journal.item_ids(created))
    return created, sealed_packs


def reception_response(created: list[Items], sealed_packs: list[SealedPacks]) -> dict[str, Any]:
    today = timezone.localdate()
    response: dict[str, Any] = {'items': [ser.item_dict(item, today) for item in created]}
    if sealed_packs:
        response['sealed_pack'] = ser.sealed_pack_dict(sealed_packs[0])  # premier paquet (anciens postes)
        response['sealed_packs'] = [ser.sealed_pack_dict(pack) for pack in sealed_packs]
    return response


@api_view(['POST'])
@handle_errors
def item_delete(request: Request, iid: str) -> Response:
    """Marque un item comme supprime (ce n'est pas la voie normale : les verifs detectent les disparitions)."""
    require_front(request, GESTION)
    item = get_object_or_404(Items.objects.select_related('pack__item_type'), iid=iid)
    reason = str(request.data.get('reason', '')).strip()
    if not reason:
        raise ApiError("Une raison est obligatoire pour supprimer un item")
    services.mark_deleted(item, operation_identity(request)[:32], reason)
    return Response(ser.item_dict(item))


@api_view(['GET', 'POST'])
@handle_errors
def item_split(request: Request, iid: str) -> Response:
    """Decoupe d'un item en plus petits sous-types du meme type a unites (sachet de 5 serums phy → 5 dosettes).
    GET : sous-types possibles {"targets": [{"type", "name", "count"}]}. POST {"user", "type"} : decoupe, renvoie
    {"source": item, "items": [nouveaux items, a etiqueter]}."""
    require_front(request, GESTION)
    item = get_object_or_404(Items.objects.select_related('pack__item_type__group', 'location'), iid=iid)
    if request.method == 'GET':
        return Response({'targets': [{'type': target.type, 'name': target.name, 'count': count}
                                     for target, count in services.split_targets(item.pack.item_type)]})
    target = get_object_or_404(ItemType, type=str(request.data.get('type', '')))
    created = services.split_item(item, target, operation_identity(request))
    today = timezone.localdate()
    items = Items.objects.select_related('pack__item_type', 'location').filter(iid__in=[new.iid for new in created])
    return Response({'source': ser.item_dict(item, today),
                     'items': [ser.item_dict(new, today) for new in items.order_by('iid')]})


@api_view(['POST'])
@handle_errors
def items_disable(request: Request) -> Response:
    """Desactive ou reactive des items (rappel produit, non-conformite), au poste, par un admin (fenetre Inventaire) :
    {"user", "disabled": bool, "reason": texte (obligatoire pour desactiver), et les items vises : "items" (iids),
    "reception" (un iid : tous les items recus avec lui, meme paquet d'etiquettes et meme instant) et/ou "pack"
    (paquet ferme : tous ses items)}. Desactive, un item est invalide comme un perime."""
    require_front(request, ADMIN)
    data = request.data
    where = Q(iid__in=iid_list(data)) if data.get('items') else Q(pk__in=[])
    if data.get('reception'):
        first = get_object_or_404(Items, iid=str(data['reception']))
        where |= Q(pack_id=first.pack_id, added=first.added)
    if data.get('pack'):
        where |= Q(sealed_pack_id=get_object_or_404(SealedPacks, id=str(data['pack'])).id)
    targets = list(Items.objects.select_related('pack__item_type').filter(where))
    if not targets:
        raise ApiError('Aucun item à désactiver')
    changed = services.set_disabled(targets, operation_identity(request), bool(data.get('disabled', True)),
                                    data.get('reason', ''))
    return Response({'count': len(changed), 'total': len(targets)})


FRONT_NAME_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')


@api_view(['GET', 'POST'])
@handle_errors
def front_configs(request: Request) -> Response:
    """Connecter un autre ordinateur, depuis le poste (Utilisateurs > Ordinateurs connectes), par un admin : cree la cle d'un front
    distant et son fichier de configuration chiffre, telechargeable pendant 5 minutes par un lien (QR code).
    POST {"name", "password" (vide = genere et renvoye), "replace": bool, "poste": bool}. GET : fronts existants.
    Seulement depuis le poste principal (API locale, ou cle marquee poste) : la cle d'un simple front distant ne
    cree pas d'autres cles."""
    require_front(request, ADMIN)
    if not is_main_poste(request):
        raise ApiError("Seulement depuis le poste principal", status.HTTP_403_FORBIDDEN)
    now = timezone.now()
    FrontConfigDownload.objects.filter(expires__lte=now).delete()
    if request.method == 'GET':
        pending = set(FrontConfigDownload.objects.values_list('front_id', flat=True))
        return Response({'fronts': [
            {'name': front.name, 'revoked': front.revoked, 'created': front.created.isoformat(),
             'last_used': front.last_used.isoformat() if front.last_used else None,
             'last_address': front.last_address, 'download_pending': front.pk in pending, 'poste': front.poste,
             'self': front.name == getattr(request, 'qrprotec_front', '')}
            for front in FrontKey.objects.order_by('revoked', 'name')]})
    data = request.data
    name = str(data.get('name', '')).strip()
    if not FRONT_NAME_RE.match(name):
        raise ApiError("Nom de l'ordinateur : lettres sans accents, chiffres, . _ - (64 caractères au plus, sans espace)")
    password = str(data.get('password', ''))
    generated = not password
    if generated:
        password = front_config.generate_password()
    if len(password) < front_config.MIN_PASSWORD_LENGTH:
        raise ApiError(f"Mot de passe trop court ({front_config.MIN_PASSWORD_LENGTH} caractères au moins)")
    existing = FrontKey.objects.filter(name=name).first()
    if existing is not None and not data.get('replace'):
        raise ApiError(f"Un ordinateur s'appelle déjà « {name} » : choisissez un autre nom, ou remplacez sa clé "
                       "(l'ancien fichier ne marchera plus)", status.HTTP_409_CONFLICT, exists=True)
    base_url = qrprotec_setting('PUBLIC_BASE_URL').rstrip('/')
    ca_pem = front_config.server_ca_pem(base_url, qrprotec_setting('TLS'), qrprotec_setting('FRONT_CA_FILE'))
    with transaction.atomic():
        if existing is not None:
            existing.delete()
        front, key = FrontKey.create(name, poste=bool(data.get('poste')))
        payload = {'name': name, 'api_url': base_url, 'api_key': key,
                   'created': timezone.localtime(now).isoformat(timespec='seconds')}
        if ca_pem:
            payload['api_ca_pem'] = ca_pem
        download = FrontConfigDownload.objects.create(
            token=front_config.new_download_token(), front=front, content=front_config.encrypt(payload, password),
            created=now, expires=front_config.download_expiry(now))
    journal.record(OperationKind.SETTINGS, front_identity(request),
                   f"ordinateur « {name} » connecté au serveur" + (" (poste principal)" if front.poste else '')
                   + (" (clé remplacée)" if existing else ''),
                   front=name)
    return Response({
        'name': name,
        'url': f'{base_url}/front-config/{download.token}',
        'expires': download.expires.isoformat(),
        'seconds': int((download.expires - now).total_seconds()),
        'password': password if generated else '',
        'ca': bool(ca_pem),
        'replaced': existing is not None,
        'poste': front.poste,
    }, status=status.HTTP_201_CREATED)


@api_view(['POST'])
@handle_errors
def front_config_detail(request: Request, name: str) -> Response:
    """Ordinateurs connectes (fenetre Utilisateurs du poste), admin :
    {"action": "revoke" | "restore" | "delete" | "poste" | "unposte"}.
    Revoquer coupe l'acces tout de suite (la cle reste, reactivable) ; supprimer efface la cle et ses liens ;
    poste / unposte donne ou retire les droits de poste principal. Un poste principal distant ne peut ni se
    revoquer, ni se supprimer, ni se retirer ces droits : il se couperait lui-meme l'acces a cette fenetre."""
    require_front(request, ADMIN)
    if not is_main_poste(request):
        raise ApiError("Seulement depuis le poste principal", status.HTTP_403_FORBIDDEN)
    front = get_object_or_404(FrontKey, name=name)
    action = request.data.get('action')
    if action in ('revoke', 'delete', 'unposte') and name == getattr(request, 'qrprotec_front', ''):
        raise ApiError("Impossible depuis cet ordinateur lui-même : faites-le depuis un autre poste principal, ou "
                       "sur le serveur (sudo qrprotec-manage frontkey)", status.HTTP_409_CONFLICT)
    if action in ('poste', 'unposte'):
        front.poste = action == 'poste'
        front.save(update_fields=['poste'])
        summary = f"ordinateur « {name} » " + ("devenu poste principal" if front.poste else "n'est plus poste principal")
    elif action in ('revoke', 'restore'):
        front.revoked = action == 'revoke'
        front.save(update_fields=['revoked'])
        if front.revoked:
            FrontConfigDownload.objects.filter(front=front).delete()  # lien encore valable : coupe aussi
        summary = f"ordinateur « {name} » " + ("révoqué" if front.revoked else "réactivé")
    elif action == 'delete':
        front.delete()
        summary = f"ordinateur « {name} » supprimé"
    else:
        raise ApiError("Action inconnue (revoke, restore, delete, poste ou unposte)")
    journal.record(OperationKind.SETTINGS, front_identity(request), summary, front=name)
    return Response({'name': name, 'action': action})


def front_config_download(request: HttpRequest, token: str) -> HttpResponse:
    """Lien du QR code affiche au poste : le fichier chiffre, en piece jointe, tant que le lien est valable."""
    if request.method != 'GET':
        return HttpResponseNotAllowed(['GET'])
    FrontConfigDownload.objects.filter(expires__lte=timezone.now()).delete()
    download = FrontConfigDownload.objects.select_related('front').filter(
        token=token, expires__gt=timezone.now(), front__revoked=False).first()
    if download is None:
        response = HttpResponse("Lien expiré ou inconnu : demandez un nouveau QR code au poste "
                                "(Utilisateurs > Ordinateurs connectés).\n",
                                content_type='text/plain; charset=utf-8', status=404)
        response['Cache-Control'] = 'no-store'
        return response
    FrontConfigDownload.objects.filter(pk=download.pk).update(downloads=download.downloads + 1)
    response = HttpResponse(download.content, content_type='application/octet-stream')
    response['Content-Disposition'] = f'attachment; filename="{download.front.name}{front_config.SUFFIX}"'
    response['Cache-Control'] = 'no-store'
    response['X-Content-Type-Options'] = 'nosniff'
    return response


@api_view(['GET'])
@handle_errors
def receptions(request: Request) -> Response:
    """Receptions passees (items crees ensemble : meme paquet d'etiquettes, meme instant), les plus recentes
    d'abord : ?type=CODE, ?item=IID (la reception de cet item), ?limit=N. Sert a desactiver d'un coup tout ce qui a
    ete recu ensemble (paquet ouvert a la reception, rappel produit)."""
    require_front(request, GESTION)
    params = request.query_params
    queryset = Items.objects.all()
    if params.get('type'):
        queryset = queryset.filter(pack__item_type_id=params['type'])
    if params.get('item'):
        item = get_object_or_404(Items, iid=params['item'])
        queryset = queryset.filter(pack_id=item.pack_id, added=item.added)
    limit = parse_int(params.get('limit', 100), 'limit', 1, 1000)
    rows = (queryset.values('pack_id', 'pack__item_type_id', 'pack__item_type__name', 'pack__peremption', 'added',
                            'added_by')
            .annotate(count=Count('iid'), first=Min('iid'), last=Max('iid'), sealed_pack=Max('sealed_pack_id'),
                      sealed_packs=Count('sealed_pack_id', distinct=True),
                      active=Count('iid', filter=Q(status=ItemStatus.ACTIVE)),
                      in_stock=Count('iid', filter=services.in_stock_q() & Q(status=ItemStatus.ACTIVE)),
                      disabled=Count('iid', filter=Q(disabled=True)))
            .order_by('-added')[:limit])
    return Response([{
        'item': row['first'],
        'last': row['last'],
        'type': row['pack__item_type_id'],
        'type_name': row['pack__item_type__name'],
        'peremption': row['pack__peremption'].isoformat() if row['pack__peremption'] else None,
        'added': timezone.localtime(row['added']).isoformat(),
        'added_by': identity_name(row['added_by']),
        'count': row['count'],
        'active': row['active'],
        'in_stock': row['in_stock'],
        'disabled': row['disabled'],
        'sealed_pack': row['sealed_pack'],
        'sealed_packs': row['sealed_packs'],
    } for row in rows])


@api_view(['GET'])
@handle_errors
def reception_storage(request: Request) -> Response:
    """Rangement du stock propose a la reception d'un type (?type=CODE) : le dernier ou ce type a ete range."""
    require_front(request, GESTION)
    item_type = get_object_or_404(ItemType, type=str(request.query_params.get('type', '')))
    return Response({'type': item_type.type, 'location': services.last_storage(item_type)})


@api_view(['POST'])
@handle_errors
def reception_store(request: Request) -> Response:
    """Range d'un coup toute une reception dans un rangement du stock (ou en vrac dans le stock si "location" est
    vide) : {"user", "reception": un iid de la reception, "location"}. Seuls les items encore en stock bougent : ceux
    deja mis dans un lot, utilises ou supprimes restent ou ils sont."""
    require_front(request, GESTION)
    data = request.data
    first = get_object_or_404(Items, iid=str(data.get('reception', '')))
    location = storage_target(data.get('location'))
    identity = operation_identity(request, data)
    received = Items.objects.filter(pack_id=first.pack_id, added=first.added)
    iids = list(received.filter(services.in_stock_q(), status=ItemStatus.ACTIVE).order_by('iid')
                .values_list('iid', flat=True))
    if not iids:
        raise ApiError('Aucun item de cette réception en stock')
    if location is None:
        result = services.move_items(iids, None, identity, SeenWhile.REMOVE)
    else:
        result = services.move_items(iids, location, identity, SeenWhile.ADD)
    return Response({'moved': result['moved'], 'count': len(result['moved']), 'total': received.count(),
                     'location': location.id if location else None})


@api_view(['POST'])
@handle_errors
def item_restore(request: Request, iid: str) -> Response:
    require_front(request, GESTION)
    item = get_object_or_404(Items.objects.select_related('pack__item_type'), iid=iid)
    services.restore(item, operation_identity(request))
    return Response(ser.item_dict(item))


@api_view(['POST'])
@handle_errors
def items_to_stock(request: Request) -> Response:
    require_front(request, GESTION)
    identity = operation_identity(request)
    return Response(services.move_items(iid_list(request.data), None, identity, SeenWhile.REMOVE,
                                        debug=debug_options(request)))


@api_view(['GET'])
@handle_errors
def stock(request: Request) -> Response:
    require_front(request, GESTION)
    soon = parse_int(request.query_params.get('soon_days', 30), 'soon_days', 0, 3650)
    # les peremptions font baisser le stock sans evenement : on verifie les seuils a chaque consultation
    notifications.check_stock_levels()
    return Response(services.stock_status(soon))


@api_view(['GET'])
@handle_errors
def stock_forecast(request: Request) -> Response:
    return Response(forecast.forecast(**forecast_params(request.query_params)))


@api_view(['POST'])
@handle_errors
def stock_verif(request: Request) -> Response:
    require_front(request, GESTION)
    identity = operation_identity(request)
    return Response(services.perform_verif(None, iid_list(request.data), identity))


@api_view(['GET'])
@handle_errors
def sealed_packs(request: Request) -> Response:
    require_front(request, GESTION)
    queryset = SealedPacks.objects.select_related('item_type').order_by('-created')
    if request.query_params.get('opened') == '0':
        queryset = queryset.filter(opened__isnull=True)
    return Response([ser.sealed_pack_dict(pack) for pack in queryset[:500]])


@api_view(['POST'])
@handle_errors
def sealed_pack_open(request: Request, pack_id: str) -> Response:
    require_front(request, GESTION)
    sealed_pack = get_object_or_404(SealedPacks.objects.select_related('item_type'), id=pack_id)
    identity = operation_identity(request)
    now = timezone.now()
    # un paquet deja ouvert garde sa date d'ouverture : l'appel sert alors a reimprimer les etiquettes
    packs.mark_opened(sealed_pack, identity, now)
    today = timezone.localdate()
    data = ser.sealed_pack_dict(sealed_pack, with_items=False)
    data['items'] = [
        ser.item_dict(item, today)
        for item in sealed_pack.items.select_related('pack__item_type', 'location').order_by('iid')
    ]
    return Response(data)


@api_view(['POST'])
@handle_errors
def sealed_pack_printed(request: Request, pack_id: str) -> Response:
    """Poste : les etiquettes des items du paquet ouvert sont parties a l'imprimante (apres l'apercu). Le contenu
    peut alors etre pre-etiquete (packs.py)."""
    require_front(request, GESTION)
    sealed_pack = get_object_or_404(SealedPacks.objects.select_related('item_type'), id=pack_id)
    packs.mark_labels_printed(sealed_pack)
    return Response(ser.sealed_pack_dict(sealed_pack))


@api_view(['POST'])
@handle_errors
def sealed_pack_close(request: Request, pack_id: str) -> Response:
    """Annule une ouverture faite par erreur : le paquet redevient ferme (les etiquettes deja imprimees
    restent valables, les items sont les memes)."""
    require_front(request, GESTION)
    sealed_pack = get_object_or_404(SealedPacks.objects.select_related('item_type'), id=pack_id)
    was_opened = sealed_pack.opened is not None
    sealed_pack.opened = None
    sealed_pack.opened_by = ''
    sealed_pack.save(update_fields=['opened', 'opened_by'])
    if was_opened:
        journal.record(OperationKind.PACK_CLOSE, front_identity(request),
                       f'{sealed_pack.count} × {sealed_pack.item_type.name} (paquet {sealed_pack.id}, ouverture annulée)',
                       pack=sealed_pack.id, type=sealed_pack.item_type_id)
    return Response(ser.sealed_pack_dict(sealed_pack))


@api_view(['GET', 'POST'])
@handle_errors
def lot_types(request: Request) -> Response:
    """Types de lots, archives compris (champ archived).

    Un type « lot unique » (ex : un VPS, une armoire du VPS) est cree avec son lot, du meme nom (nom court compris,
    tronque a 16 caracteres) : `parent` (lot global) s'applique alors a ce lot, renvoye dans `created_lot`. Un rangement du stock est
    toujours un lot unique.
    """
    if request.method != 'GET':
        require_front(request, GESTION)
    if request.method == 'GET':
        return Response([ser.lot_type_dict(lot_type) for lot_type in LotType.objects.order_by('name')])
    data = request.data
    code = str(data.get('type', '')).strip()
    if not CODE_RE.match(code):
        raise ApiError(f"Le code du type de lot doit faire exactement {TYPE_LENGTH} caracteres alphanumeriques")
    if LotType.objects.filter(type=code).exists():
        raise ApiError(f"Le type de lot {code} existe deja", status.HTTP_409_CONFLICT)
    name = str(data.get('name', '')).strip()[:64]
    if not name:
        raise ApiError("Nom obligatoire")
    ensure_name_free(LotType.objects.all(), name, 'Le type de lot')
    storage = bool(data.get('storage', False))
    unique = storage or bool(data.get('unique', False))
    identity = operation_identity(request, data)[:32]
    with transaction.atomic():
        lot_type = LotType(
            type='', name=name, description=str(data.get('description', '')), created_by=identity,
            storage=storage, unique=unique, seal_required=bool(data.get('seal_required', False)),
        )
        apply_referentiel(lot_type, data)
        lot_type.type = code
        lot_type.save(force_insert=True)
        lot = None
        if unique:
            ensure_name_free(Lots.objects.filter(active=True), name, 'Le lot')
            lot = Lots(lot_type=lot_type, name=name, name_short=name[:16], created_by=identity)
            if storage:
                lot.storage_kind = storage_kind_value(data.get('storage_kind'))
            lot.parent = parse_parent(lot, data.get('parent'))
            lot.save()
        journal.record(OperationKind.CATALOG, identity, f'type de lot créé : {lot_type}' + (' (lot unique)' if unique else ''),
                       lot=lot, type=code)
    response = ser.lot_type_dict(lot_type)
    if lot is not None:
        response['created_lot'] = ser.lot_dict(lot, local=True, with_items=True)
    return Response(response, status=status.HTTP_201_CREATED)


@api_view(['GET', 'PATCH'])
@handle_errors
def lot_type_detail(request: Request, type_code: str) -> Response:
    if request.method != 'GET':
        require_front(request, GESTION)
    lot_type = get_object_or_404(LotType, type=type_code)
    if request.method == 'PATCH':
        data = request.data
        lots = list(lot_type.lots_set.select_related('parent__lot_type'))  # lot.lot_type : ce type (modifie ici)
        if lot_type.type == STOCK_LOT_TYPE and (
                ('storage' in data and not data['storage']) or ('unique' in data and not data['unique'])
                or data.get('archived')):
            raise ApiError("Le stock reste un rangement unique, toujours actif : seuls son nom et sa description "
                           "se modifient")
        if 'name' in data:
            name = str(data['name']).strip()[:64]
            if not name:
                raise ApiError("Nom obligatoire")
            ensure_name_free(LotType.objects.exclude(type=lot_type.type), name, 'Le type de lot')
            if lot_type.unique:
                # le lot d'un type unique porte le nom du type, nom court compris
                for lot in lots:
                    if lot.name == lot_type.name:
                        ensure_name_free(Lots.objects.filter(active=True).exclude(id=lot.id), name, 'Le lot')
                        lot.name = name
                        if lot.name_short == lot_type.name[:16]:
                            lot.name_short = name[:16]
                        lot.save(update_fields=['name', 'name_short'])
            lot_type.name = name
        if 'description' in data:
            lot_type.description = str(data['description'])
        if 'seal_required' in data:
            lot_type.seal_required = bool(data['seal_required'])
        if 'referentiel' in data or 'part_of' in data:
            apply_referentiel(lot_type, data)
        storage = bool(data.get('storage', lot_type.storage))
        # un rangement du stock est un lot unique (sauf un ancien type de rangement qui a deja plusieurs lots)
        unique = bool(data.get('unique', lot_type.unique)) or (storage and len(lots) <= 1)
        if storage and not lot_type.storage and (lot_type.referentiel or lot_type.part_of_id):
            raise ApiError("Un référentiel ou le rangement d'un référentiel ne peut pas devenir un rangement du stock")
        if storage and not lot_type.storage and len(lots) > 1:
            raise ApiError(f"{len(lots)} lots de ce type existent : un rangement du stock n'en a qu'un")
        if unique and not lot_type.unique and len(lots) > 1:
            raise ApiError(f"{len(lots)} lots de ce type existent : un lot unique n'en a qu'un")
        lot_type.unique = unique
        if storage != lot_type.storage:
            # un rangement fait partie du stock, un lot ordinaire non : ni l'un ni l'autre ne contient l'autre
            for lot in lots:
                if lot.children.filter(active=True).exists():
                    raise ApiError(f"Le lot {lot.name} contient des sous-lots : retirez-les d'abord")
            lot_type.storage = storage
            # rangement : jamais de contenu attendu, etiquette privee sans expiration
            if storage:
                LotContent.objects.filter(lot__lot_type=lot_type).delete()
                Lots.objects.filter(lot_type=lot_type).update(locations=[])
            if storage and lot_type.requirements.exists():
                lot_type.requirements.all().delete()
                lot_type.locations = []
                services.bump_lot_type_version(lot_type)
            for lot in lots:
                lot.verif_key_expires = lot.key_expiration()
                lot.key_expiry_stage = KeyExpiry.VALID
                if storage and (lot.parent is None or not lot.parent.lot_type.storage):
                    lot.parent = Lots.stock()  # un rangement est range dans le stock
                elif not storage and lot.parent is not None and lot.parent.lot_type.storage:
                    lot.parent = None  # n'est plus un rangement : sort du stock
                lot.save(update_fields=['verif_key_expires', 'key_expiry_stage', 'parent'])
        if 'archived' in data:
            archived = bool(data['archived'])
            active = [lot for lot in lots if lot.active]
            if archived and active and not lot_type.unique:
                raise ApiError("Des lots actifs sont de ce type (" + ', '.join(lot.name for lot in active)
                               + ") : archivez-les d'abord")
            with transaction.atomic():
                if lot_type.unique:
                    # le lot d'un type unique est archive et desarchive avec son type
                    for lot in lots:
                        if archived and lot.active and lot.children.filter(active=True).exists():
                            raise ApiError(f"Le lot {lot.name} contient des sous-lots actifs : archivez-les ou "
                                           "retirez-les du lot d'abord")
                        if not archived and not lot.active:
                            ensure_name_free(Lots.objects.filter(active=True).exclude(id=lot.id), lot.name, 'Le lot')
                        if not archived and lot.parent is not None and not lot.parent.active:
                            lot.parent = default_parent(lot)
                        lot.active = not archived
                        lot.save(update_fields=['active', 'parent'])
                lot_type.archived = archived
                lot_type.save()
        else:
            lot_type.save()
        journal.record(OperationKind.CATALOG, front_identity(request),
                       f'type de lot modifié : {lot_type} ({changed_fields(data, LOT_TYPE_FIELDS)})',
                       type=lot_type.type, fields=sorted(data.keys()))
    return Response(ser.lot_type_dict(lot_type))


@api_view(['PUT'])
@handle_errors
def lot_type_slots(request: Request, type_code: str) -> Response:
    """Remplace les lots exiges du type : [{"name": "B+", "accepted": ["bplusx"], "quantity": 1, "mode": "fixed",
    "optional": false}, ...] dans l'ordre. mode : 'fixed' (range pour de bon) ou 'assembly' (n'importe quel lot libre,
    choisi a l'assemblage). Les sous-lots deja ranges ne bougent pas : seule leur place change."""
    require_front(request, GESTION)
    lot_type = get_object_or_404(LotType, type=type_code)
    rows = request.data.get('slots', [])
    if not isinstance(rows, list):
        raise ApiError("slots : liste attendue")
    if rows and lot_type.storage:
        raise ApiError(f"{lot_type.name} est un rangement du stock : il n'exige pas de lots")
    clean = []
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ApiError("slots : liste d'objets attendue")
        accepted = row.get('accepted', [])
        if not isinstance(accepted, list) or not accepted:
            raise ApiError("Chaque lot exigé accepte au moins un type de lot")
        codes = []
        for code in accepted:
            other = get_object_or_404(LotType, type=str(code))
            if other.storage:
                raise ApiError(f"{other.name} est un rangement du stock : il ne peut pas être exigé")
            if other.type == lot_type.type:
                raise ApiError("Un type de lot ne peut pas s'exiger lui-même")
            if other.type not in codes:
                codes.append(other.type)
        mode = str(row.get('mode', SlotMode.FIXED))
        if mode not in SlotMode.values:
            raise ApiError("mode : 'fixed' ou 'assembly'")
        names = dict(LotType.objects.filter(type__in=codes).values_list('type', 'name'))
        name = str(row.get('name', '')).strip()[:64] or ' ou '.join(names[code] for code in codes)[:64]
        clean.append(LotTypeSlot(lot_type=lot_type, name=name, accepted=codes, mode=mode, order=index,
                                 quantity=parse_int(row.get('quantity', 1), 'quantity', 1),
                                 optional=bool(row.get('optional', False))))
    with transaction.atomic():
        lot_type.slots.all().delete()
        LotTypeSlot.objects.bulk_create(clean)
    journal.record(OperationKind.LOT, front_identity(request), f'lots exigés du type {lot_type.name} modifiés',
                   lot_type=lot_type.type, slots=[{'name': slot.name, 'accepted': slot.accepted,
                                                   'quantity': slot.quantity, 'mode': slot.mode} for slot in clean])
    return Response(ser.lot_type_dict(lot_type))


def slot_of(lot: Lots, slot_id: int) -> LotTypeSlot:
    slot = get_object_or_404(LotTypeSlot, id=parse_int(slot_id, 'slot', 1))
    if slot.lot_type_id != lot.lot_type_id:
        raise ApiError(f"Cette place n'est pas une place de {lot.name}")
    return slot


def slot_lot(lot_id: str) -> Lots:
    other = get_object_or_404(Lots.objects.select_related('lot_type', 'parent'), id=str(lot_id or ''))
    if not other.active:
        raise ApiError(f"Le lot {other.name} est archivé")
    return other


@api_view(['POST'])
@handle_errors
def lot_slot_action(request: Request, lot_id: str, action: str) -> Response:
    """Places des lots exiges d'un lot (Gestion des lots) :

    - assemble {slot, lot} : range un lot libre d'un type accepte dans une place a l'assemblage ;
    - release {lot} : libere un lot assemble (il redevient independant) ;
    - replace {slot, original, lot} : un lot non fixe prend la place fixe d'un sous-lot qui a un probleme ; l'original
      sort du lot (independant), le remplacant garde la trace du lot d'ou il vient ;
    - restore {lot} : l'original reprend sa place, le remplacant retourne dans le lot d'ou il venait."""
    require_front(request, GESTION)
    lot = slot_lot(lot_id)
    data = request.data
    who = operation_identity(request, data)[:32]
    with transaction.atomic():
        if action == 'assemble':
            slot = slot_of(lot, data.get('slot'))
            if slot.mode != SlotMode.ASSEMBLY:
                raise ApiError(f"{slot.name} est une place fixe : utilisez Remplacer")
            other = slot_lot(data.get('lot'))
            if other.id not in {candidate.id for candidate in services.slot_candidates(lot, slot)}:
                raise ApiError(f"{other.name} ne peut pas aller dans cette place : il faut un lot libre "
                               f"({', '.join(slot.accepted)}), pas un lot rangé dans un autre")
            taken = services.assign_slots([slot], list(lot.children.filter(active=True)))[0][0][1]
            if len(taken) >= slot.quantity:
                raise ApiError(f"La place {slot.name} est déjà occupée : libérez d'abord un lot")
            other.parent = lot
            other.assembled = True
            other.save(update_fields=['parent', 'assembled'])
            journal.record(OperationKind.LOT, who, f'assemblé dans {lot.name} ({slot.name})', lot=other)
        elif action == 'release':
            other = slot_lot(data.get('lot'))
            if other.parent_id != lot.id or not other.assembled:
                raise ApiError(f"{other.name} n'est pas assemblé dans {lot.name}")
            other.parent = None
            other.assembled = False
            free_rangement(other)
            other.save(update_fields=['parent', 'assembled', 'spare'])
            journal.record(OperationKind.LOT, who, f'libéré de {lot.name}', lot=other)
        elif action == 'replace':
            slot = slot_of(lot, data.get('slot'))
            if slot.mode != SlotMode.FIXED:
                raise ApiError(f"{slot.name} est une place à l'assemblage : libérez le lot puis assemblez-en un autre")
            original = slot_lot(data.get('original'))
            taken = services.assign_slots([slot], list(lot.children.filter(active=True)))[0][0][1]
            if original.id not in {child.id for child in taken}:
                raise ApiError(f"{original.name} n'occupe pas la place {slot.name} de {lot.name}")
            other = slot_lot(data.get('lot'))
            if other.id not in {candidate.id for candidate in services.slot_candidates(lot, slot)}:
                raise ApiError(f"{other.name} ne peut pas remplacer {original.name} : il faut un lot d'un type "
                               "accepté qui ne tient pas lui-même une place fixe")
            other.borrowed_from = other.parent
            other.parent = lot
            other.assembled = False
            other.replaces = original
            other.save(update_fields=['parent', 'assembled', 'replaces', 'borrowed_from'])
            original.parent = None
            original.assembled = False
            free_rangement(original)
            original.save(update_fields=['parent', 'assembled', 'spare'])
            journal.record(OperationKind.LOT, who, f'remplace {original.name} dans {lot.name}', lot=other)
            journal.record(OperationKind.LOT, who, f'remplacé par {other.name}, sorti de {lot.name}', lot=original)
        elif action == 'restore':
            other = slot_lot(data.get('lot'))
            if other.parent_id != lot.id or other.replaces_id is None:
                raise ApiError(f"{other.name} ne remplace aucun lot dans {lot.name}")
            original = slot_lot(other.replaces_id)
            if original.parent_id is not None:
                raise ApiError(f"{original.name} est rangé dans {original.parent.name} : sortez-le d'abord")
            back = other.borrowed_from if other.borrowed_from_id and other.borrowed_from.active else None
            original.parent = lot
            original.save(update_fields=['parent'])
            other.parent = back
            other.replaces = None
            other.borrowed_from = None
            if back is None:
                free_rangement(other)
            other.save(update_fields=['parent', 'replaces', 'borrowed_from', 'spare'])
            journal.record(OperationKind.LOT, who, f'reprend sa place dans {lot.name}', lot=original)
            journal.record(OperationKind.LOT, who, 'rendu' + (f' à {back.name}' if back else ''), lot=other)
        else:
            raise Http404
    lot.refresh_from_db()
    return Response(ser.lot_dict(lot, local=True, with_items=True))


@api_view(['PUT'])
@handle_errors
def lot_type_requirements(request: Request, type_code: str) -> Response:
    """Remplace la liste des exigences : [{"type": "serphy", "quantity": 4, "location": "pochette bleue",
    "optional": false}, ...], et l'ordre des emplacements (locations, facultatif : ordre actuel, les nouveaux a la
    suite). optional : facultatif a la verif, seulement pour du materiel non consommable sans taches."""
    require_front(request, GESTION)
    lot_type = get_object_or_404(LotType, type=type_code)
    rows = request.data.get('requirements', [])
    if not isinstance(rows, list):
        raise ApiError("requirements : liste attendue")
    if rows and lot_type.storage:
        raise ApiError(f"{lot_type.name} est un rangement du stock : il n'a pas de contenu attendu")
    order = request.data.get('locations', lot_type.locations)
    if not isinstance(order, list):
        raise ApiError("locations : liste attendue")
    locations = services.location_order([row for row in rows if isinstance(row, dict)], [str(name) for name in order],
                                        lot_type.locations or [])
    with transaction.atomic():
        before = services.lot_type_content_snapshot(lot_type)
        LotRequirements.objects.filter(lot_type=lot_type).delete()
        for row in rows:
            item_type = get_object_or_404(ItemType, type=str(row.get('type', '')))
            quantity = parse_int(row.get('quantity', 1), 'quantity', 1)
            location = locations.get(name_key(services.clean_location(row.get('location'))), '')
            if lot_type.referentiel:
                location = ''  # liste obligatoire : les emplacements sont ceux de ses rangements
            optional = bool(row.get('optional', False))
            if optional and not item_type.can_be_optional:
                raise ApiError(f"{item_type.name} ne peut pas être facultatif : seul le matériel non consommable sans "
                               "tâches peut l'être")
            try:
                LotRequirements.objects.create(
                    lot_type=lot_type, item_type=item_type, quantity=quantity, location=location, optional=optional
                )
            except IntegrityError:
                raise ApiError(f"Le type {item_type.type} apparait plusieurs fois")
        lot_type.locations = [] if lot_type.referentiel else list(locations.values())
        # rien de change (« Enregistrer » sans modification) : pas de nouvelle version ; modifie sans verif faite sur
        # la version en cours : corrigee en place
        changed = services.lot_type_content_snapshot(lot_type) != before
        new_version = changed and services.bump_lot_type_version(lot_type)
        lot_type.save(update_fields=['version', 'valid_version', 'locations'])
        if not changed:
            return Response({**ser.lot_type_dict(lot_type), 'new_version': False})
        journal.record(OperationKind.CATALOG, front_identity(request), f'contenu attendu modifié : {lot_type}',
                       type=lot_type.type, requirements=[
                           {'type': str(row.get('type', '')), 'quantity': row.get('quantity', 1),
                            'optional': bool(row.get('optional', False))} for row in rows
                       ], version=lot_type.version)
    return Response({**ser.lot_type_dict(lot_type), 'new_version': new_version})


@api_view(['GET'])
@handle_errors
def lot_type_coverage(request: Request, type_code: str) -> Response:
    """Couverture d'un referentiel : pour chaque lot qui doit le couvrir (ses lots, ou un lot d'un de ses rangements
    hors d'un lot du referentiel), la liste obligatoire face au contenu attendu de ses sacs (voir
    services.lot_coverage)."""
    require_front(request, GESTION)
    lot_type = get_object_or_404(LotType, type=type_code)
    if not lot_type.referentiel:
        raise ApiError(f"{lot_type.name} n'est pas un référentiel")
    return Response({'type': lot_type.type, 'name': lot_type.name,
                     'lots': [services.lot_coverage(lot, lot_type) for lot in services.coverage_lots(lot_type)]})


@api_view(['PUT'])
@handle_errors
def lot_content(request: Request, lot_id: str) -> Response:
    """Remplace le contenu propre d'un lot, par-dessus celui de son type : [{"type": "serphy", "extra": 2,
    "location": "poche avant"}, ...] (extra : en plus de la quantite du type ; location absent ou null : l'emplacement
    du type), et l'ordre des emplacements du lot (locations, facultatif ; celui du type s'il revient au meme)."""
    require_front(request, GESTION)
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    lot_type = lot.lot_type
    rows = request.data.get('content', [])
    if not isinstance(rows, list) or not all(isinstance(row, dict) for row in rows):
        raise ApiError("content : liste attendue")
    if rows and lot_type.storage:
        raise ApiError(f"{lot.name} est un rangement du stock : il n'a pas de contenu attendu")
    order = request.data.get('locations', lot.locations)
    if not isinstance(order, list):
        raise ApiError("locations : liste attendue")
    type_rows = {row.item_type_id: row for row in LotRequirements.objects.filter(lot_type=lot_type)}
    wanted = {}
    for row in rows:
        item_type = get_object_or_404(ItemType, type=str(row.get('type', '')))
        if item_type.type in wanted:
            raise ApiError(f"Le type {item_type.type} apparait plusieurs fois")
        extra = parse_int(row.get('extra', 0), 'extra', 0)
        location = row.get('location')
        location = None if location is None else services.clean_location(location)
        requirement = type_rows.get(item_type.type)
        if requirement is not None and location is not None and name_key(location) == name_key(requirement.location):
            location = None  # meme emplacement que le type : le suit s'il change
        if requirement is None and not extra:
            continue  # ni attendu par le type ni en plus : rien a retenir
        if requirement is not None and not extra and location is None:
            continue
        wanted[item_type.type] = (item_type, extra, location)
    # emplacements du lot (ceux du type que le lot garde et les siens), une seule orthographe par emplacement
    effective = [{'location': type_rows[code].location} for code in type_rows if code not in wanted]
    effective += [{'location': (type_rows[code].location if location is None and code in type_rows else location or '')}
                  for code, (_, _, location) in wanted.items()]
    known = [*(lot.locations or []), *(lot_type.locations or [])]
    locations = services.location_order(effective, [str(name) for name in order], known)
    inherited = services.location_order(effective, lot_type.locations or [], known)
    with transaction.atomic():
        before = services.lot_content_snapshot(lot)
        # repartition du referentiel (onglet Repartition du lot global) : gardee telle quelle
        removed = {row.item_type_id: row for row in LotContent.objects.filter(lot=lot, removed__gt=0)}
        LotContent.objects.filter(lot=lot).delete()
        for item_type, extra, location in wanted.values():
            if location is not None:
                location = locations.get(name_key(location), '')
            kept = removed.pop(item_type.type, None)
            LotContent.objects.create(lot=lot, item_type=item_type, extra=extra, location=location,
                                      removed=kept.removed if kept else 0)
        for kept in removed.values():
            LotContent.objects.create(lot=lot, item_type_id=kept.item_type_id, removed=kept.removed,
                                      location=kept.location)
        # meme ordre que le type : le lot suit celui du type (nouveaux emplacements, ordre change par la TECH)
        lot.locations = [] if list(locations) == list(inherited) else list(locations.values())
        changed = services.lot_content_snapshot(lot) != before
        if changed:
            services.bump_lot_version(lot)  # nouvelle version seulement si une verif a ete faite sur la version en cours
        lot.save(update_fields=['locations', 'version'])
        if not changed:
            return Response(ser.lot_dict(lot, local=True, with_items=True))
        journal.record(OperationKind.LOT, front_identity(request), f'contenu propre du lot modifié : {lot.name}',
                       lot=lot, content=[
                           {'type': code, 'extra': extra, 'location': location}
                           for code, (_, extra, location) in wanted.items()
                       ])
    return Response(ser.lot_dict(lot, local=True, with_items=True))


@api_view(['PUT'])
@handle_errors
def lot_distribution(request: Request, lot_id: str) -> Response:
    """Repartit la liste d'un referentiel entre les sacs d'un lot global (ses sous-lots dont le type est un rangement
    de ce referentiel) : {"referentiel": "lotBpl", "rows": [{"lot": <sac>, "type": "cardio", "quantity": 1,
    "location": "poche avant"}, ...]}. Le type de chaque sac reste la repartition par defaut : seul ce qui en differe
    est garde dans le contenu propre du sac (LotContent extra / removed / location). Une ligne absente ne change pas."""
    require_front(request, GESTION)
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    referentiel = get_object_or_404(LotType, type=str(request.data.get('referentiel', '')))
    sacs = {sac.id: sac for sac in services.distribution_sacs(lot).get(referentiel, [])}
    if not sacs:
        raise ApiError(f"{lot.name} ne contient aucun sac rangement de {referentiel.name}")
    rows = request.data.get('rows', [])
    if not isinstance(rows, list) or not all(isinstance(row, dict) for row in rows):
        raise ApiError("rows : liste attendue")
    wanted_types = {row.item_type_id for row in LotRequirements.objects.filter(lot_type=referentiel)}
    changes = {}
    for row in rows:
        sac = sacs.get(str(row.get('lot', '')))
        if sac is None:
            raise ApiError(f"{row.get('lot')} n'est pas un sac de {lot.name}")
        code = str(row.get('type', ''))
        if code not in wanted_types:
            raise ApiError(f"{code} n'est pas dans la liste de {referentiel.name}")
        if (sac.id, code) in changes:
            raise ApiError(f"{code} est donné deux fois pour {sac.name}")
        changes[(sac.id, code)] = (parse_int(row.get('quantity', 0), 'quantity', 0),
                                   services.clean_location(row.get('location')))
    item_types = {item_type.type: item_type for item_type in ItemType.objects.filter(type__in=wanted_types)}
    changed_sacs = []
    with transaction.atomic():
        for sac in sacs.values():
            before = services.lot_content_snapshot(sac)
            base = {row.item_type_id: row for row in LotRequirements.objects.filter(lot_type_id=sac.lot_type_id)}
            for (sac_id, code), (quantity, location) in changes.items():
                if sac_id != sac.id:
                    continue
                requirement = base.get(code)
                default = requirement.quantity if requirement else 0
                content = LotContent.objects.filter(lot=sac, item_type_id=code).first()
                content = content or LotContent(lot=sac, item_type=item_types[code])
                content.extra = max(0, quantity - default)
                content.removed = max(0, default - quantity)
                if quantity == 0 or (requirement is not None and name_key(location) == name_key(requirement.location)):
                    content.location = None  # absent de ce sac, ou au meme emplacement que son type : le suit
                else:
                    content.location = location
                if not content.extra and not content.removed and content.location is None:
                    if content.pk:
                        content.delete()
                    continue
                content.save()
            if services.lot_content_snapshot(sac) != before:
                services.bump_lot_version(sac)
                sac.save(update_fields=['version'])
                changed_sacs.append(sac.name)
        if changed_sacs:
            journal.record(OperationKind.LOT, front_identity(request),
                           f'répartition de {referentiel.name} modifiée : ' + ', '.join(changed_sacs), lot=lot,
                           distribution=[{'lot': sac_id, 'type': code, 'quantity': quantity, 'location': location}
                                         for (sac_id, code), (quantity, location) in changes.items()])
    lot.refresh_from_db()
    return Response(ser.lot_dict(lot, local=True, with_items=True))


@api_view(['GET', 'POST'])
@handle_errors
def lots(request: Request) -> Response:
    if request.method != 'GET':
        require_front(request, GESTION)
    if request.method == 'GET':
        queryset = Lots.objects.select_related('lot_type').order_by('lot_type__name', 'name')
        everything = request.query_params.get('all') == '1'
        if not everything:
            queryset = queryset.filter(active=True)
        # cles des lots (etiquettes privees) : roles gestion et admin seulement
        local = is_privileged(request)
        return Response(datacache.cached(('lots', everything, local, timezone.localdate()),
                                         lambda: _with_out(ser.lot_list(queryset, local=local))))
    data = request.data
    lot_type = get_object_or_404(LotType, type=str(data.get('lot_type', '')))
    if lot_type.archived:
        raise ApiError(f"Le type de lot {lot_type.name} est archivé")
    if (lot_type.unique or lot_type.storage) and lot_type.lots_set.exists():
        raise ApiError(f"{lot_type.name} est un " + ("rangement du stock" if lot_type.storage else "lot unique")
                       + " : son lot existe déjà", status.HTTP_409_CONFLICT)
    name = str(data.get('name', '')).strip()[:64]
    if not name:
        raise ApiError("Nom obligatoire")
    ensure_name_free(Lots.objects.filter(active=True), name, 'Le lot')
    lot = Lots(
        lot_type=lot_type,
        name=name,
        name_short=str(data.get('name_short', '') or name)[:16],
        key_location=str(data.get('key_location', '')).strip()[:128],
        created_by=operation_identity(request, data)[:32],
    )
    lot.parent = parse_parent(lot, data.get('parent'))
    lot.spare = bool(data.get('spare')) and lot_type.part_of_id is not None
    check_rangement_parent(lot)
    lot.save()
    journal.record(OperationKind.LOT, lot.created_by, f'lot créé ({lot_type.name})'
                   + (f', rangé dans {lot.parent.name}' if lot.parent else ''), lot=lot)
    return Response(ser.lot_dict(lot, local=True, with_items=True), status=status.HTTP_201_CREATED)


BATCH_MAX = 30  # lots crees d'un coup (sous-lots non compris)


def batch_sub_types(lot_type: LotType) -> list[dict[str, Any]]:
    """Sous-lots proposes a la creation de lots de `lot_type`, avec leur nombre par defaut.

    Les types de sous-lots deja ranges dans les lots de ce type, au nombre le plus courant, et les rangements du
    referentiel : 1 par defaut, 0 si tous leurs lots sont ranges dans des lots d'un autre type (le sac de soin du
    VPS est un rangement du B+, mais pas un sac des lots B+)."""
    lots = list(lot_type.lots_set.filter(active=True).prefetch_related('children__lot_type'))
    counts: defaultdict[str, Counter[int]] = defaultdict(Counter)  # type de sous-lot -> {nombre dans un lot: nombre de lots}
    names = {}
    for lot in lots:
        per_lot = Counter(child.lot_type_id for child in lot.children.all() if child.active)
        for child in lot.children.all():
            if child.active:
                names[child.lot_type_id] = child.lot_type
        for code in names:
            counts[code][per_lot.get(code, 0)] += 1
    for code in names:  # types apparus dans un lot apres les premiers : 0 pour les lots qui ne l'ont pas
        counts[code][0] += len(lots) - sum(counts[code].values())
    rows = {}
    for code, counter in counts.items():
        rows[code] = (names[code], max(counter.items(), key=lambda pair: (pair[1], pair[0]))[0])
    for rangement in lot_type.rangements.filter(archived=False).order_by('name'):
        if rangement.type in rows:
            continue
        parents = set(rangement.lots_set.filter(active=True).values_list('parent__lot_type_id', flat=True))
        rows[rangement.type] = (rangement, 0 if parents and lot_type.type not in parents else 1)
    return [
        {'lot_type': sub_type.type, 'name': sub_type.name, 'count': count,
         'rangement': sub_type.part_of_id == lot_type.type}
        for code, (sub_type, count) in sorted(rows.items(), key=lambda pair: pair[1][0].name.lower())
        if not (sub_type.archived or sub_type.storage or sub_type.unique)
    ]


def usual_key_location(lot_type: LotType) -> str:
    """Emplacement de l'etiquette cle le plus courant parmi les lots du type (vide s'il n'y en a pas)."""
    places = Counter(lot_type.lots_set.filter(active=True).exclude(key_location='')
                     .values_list('key_location', flat=True))
    return places.most_common(1)[0][0] if places else ''


def batch_plan(lot_type: LotType, count: int, base_name: str, base_short: str, sub_counts: list[tuple[str, int]]
              ) -> list[dict[str, Any]]:
    """Noms proposes pour `count` lots de `lot_type` et leurs sous-lots : « B+ 3 », « Sac de soin B+ 3 »…

    La numerotation reprend apres le plus grand numero deja pris par un lot actif nomme « <base> <n> »."""
    pattern = re.compile(re.escape(name_key(base_name)) + r' (\d+)$')
    taken = [int(match.group(1)) for lot in Lots.objects.filter(active=True).only('name')
             if (match := pattern.match(name_key(lot.name)))]
    start = max(taken, default=0) + 1
    sub_types = {sub.type: sub for sub in LotType.objects.filter(type__in=[code for code, n in sub_counts if n > 0])}
    key_locations = {code: usual_key_location(sub) for code, sub in sub_types.items()}
    plan = []
    for index in range(start, start + count):
        name = f'{base_name} {index}'
        short = f'{base_short} {index}'[:16]
        sub_lots = []
        for code, number in sub_counts:
            sub = sub_types.get(code)
            for rank in range(1, number + 1 if sub else 1):
                label = sub.name if number == 1 else f'{sub.name} {rank}'
                sub_lots.append({'lot_type': code, 'lot_type_name': sub.name, 'name': f'{label} {name}',
                                 'name_short': f'{label} {short}'[:16], 'key_location': key_locations[code]})
        plan.append({'name': name, 'name_short': short, 'key_location': usual_key_location(lot_type),
                     'sub_lots': sub_lots})
    return plan


def batch_lot_type(code: str) -> LotType:
    """Type de lot d'une creation groupee : ni archive, ni lot unique, ni rangement du stock."""
    lot_type = get_object_or_404(LotType, type=str(code or ''))
    if lot_type.archived:
        raise ApiError(f"Le type de lot {lot_type.name} est archivé")
    if lot_type.unique or lot_type.storage:
        raise ApiError(f"{lot_type.name} est un " + ("rangement du stock" if lot_type.storage else "lot unique")
                       + " : il n'existe qu'en un exemplaire")
    return lot_type


@api_view(['POST'])
@handle_errors
def lots_batch(request: Request) -> Response:
    """Creation de plusieurs lots d'un type, avec leurs sous-lots, en une fois.

    `preview` : renvoie les sous-lots proposes (`sub_types`) et les noms prevus (`lots`) sans rien creer, a partir
    de `count`, `name` (base des noms, le nom du type par defaut), `name_short` et `sub_lots` ([{lot_type, count}],
    les sous-lots proposes par defaut si absent). Sans `preview` : cree les `lots` donnes (noms eventuellement
    corriges a la main), tous ou aucun."""
    require_front(request, GESTION)
    data = request.data
    lot_type = batch_lot_type(data.get('lot_type'))
    if data.get('preview'):
        sub_types = batch_sub_types(lot_type)
        try:
            count = int(data.get('count', 1))
        except (TypeError, ValueError):
            raise ApiError("Nombre de lots invalide")
        if not 1 <= count <= BATCH_MAX:
            raise ApiError(f"Entre 1 et {BATCH_MAX} lots à la fois")
        base_name = str(data.get('name', '') or lot_type.name).strip()[:56] or lot_type.name
        base_short = str(data.get('name_short', '') or base_name).strip()[:12]
        wanted = data.get('sub_lots')
        if isinstance(wanted, list):
            sub_counts = []
            for row in wanted:
                if not isinstance(row, dict):
                    raise ApiError("Sous-lots invalides")
                try:
                    number = max(0, min(int(row.get('count', 0)), 9))
                except (TypeError, ValueError):
                    raise ApiError("Nombre de sous-lots invalide")
                sub_counts.append((str(row.get('lot_type', '')), number))
        else:
            sub_counts = [(row['lot_type'], row['count']) for row in sub_types]
        for code, number in sub_counts:
            if number:
                batch_lot_type(code)
        return Response({'sub_types': sub_types, 'lots': batch_plan(lot_type, count, base_name, base_short,
                                                                    sub_counts)})
    wanted = data.get('lots')
    if not isinstance(wanted, list) or not wanted:
        raise ApiError("Aucun lot à créer")
    if len(wanted) > BATCH_MAX:
        raise ApiError(f"Entre 1 et {BATCH_MAX} lots à la fois")
    who = operation_identity(request, data)[:32]
    sub_type_cache = {}
    seen = set()
    existing = list(Lots.objects.filter(active=True).only('name', 'active'))

    def clean(row: Any, row_type: LotType) -> Lots:
        if not isinstance(row, dict):
            raise ApiError("Lots invalides")
        name = str(row.get('name', '')).strip()[:64]
        if not name:
            raise ApiError("Nom obligatoire")
        if name_key(name) in seen:
            raise ApiError(f"Le nom « {name} » est donné deux fois", status.HTTP_409_CONFLICT)
        seen.add(name_key(name))
        ensure_name_free(existing, name, 'Le lot')
        return Lots(lot_type=row_type, name=name, name_short=str(row.get('name_short', '') or name).strip()[:16],
                    key_location=str(row.get('key_location', '')).strip()[:128], created_by=who)

    planned = []
    for row in wanted:
        lot = clean(row, lot_type)
        children = []
        for sub_row in (row.get('sub_lots') or []) if isinstance(row, dict) else []:
            code = str(sub_row.get('lot_type', '')) if isinstance(sub_row, dict) else ''
            if code not in sub_type_cache:
                sub_type_cache[code] = batch_lot_type(code)
            children.append(clean(sub_row, sub_type_cache[code]))
        planned.append((lot, children))
    parent = parse_parent(planned[0][0], data.get('parent'))
    spare = bool(data.get('spare')) and lot_type.part_of_id is not None
    for lot, children in planned:
        lot.parent = parent
        lot.spare = spare
        check_rangement_parent(lot)
    created = []
    with transaction.atomic():
        for lot, children in planned:
            lot.parent = parent
            lot.save()
            journal.record(OperationKind.LOT, who, f'lot créé ({lot_type.name})'
                           + (f', rangé dans {parent.name}' if parent else ''), lot=lot)
            created.append(lot)
            for child in children:
                child.parent = lot
                child.save()
                journal.record(OperationKind.LOT, who, f'lot créé ({child.lot_type.name}), rangé dans {lot.name}',
                               lot=child)
                created.append(child)
    return Response({'lots': [ser.lot_dict(lot, local=True, with_items=True) for lot in created]},
                    status=status.HTTP_201_CREATED)


def parse_parent(lot: Lots, value: str | None) -> Lots | None:
    """Lot parent demande (id, ou vide pour un lot independant). Refuse un lot archive et les boucles.

    Le stock est le lot global de tous les rangements du stock : un rangement est range dans le stock (par defaut)
    ou dans un autre rangement, un lot ordinaire jamais dans un rangement, et le stock lui-meme dans aucun lot."""
    if value in (None, ''):
        return default_parent(lot)
    if lot.is_stock:
        raise ApiError("Le stock ne peut pas être rangé dans un autre lot")
    parent = get_object_or_404(Lots.objects.select_related('lot_type'), id=str(value))
    if not parent.active:
        raise ApiError(f"Le lot {parent.name} est archivé")
    if lot.id and (parent.id == lot.id or any(sub.id == parent.id for sub in lot.descendants())):
        raise ApiError("Un lot ne peut pas être rangé dans lui-même ou dans un de ses sous-lots")
    if lot.lot_type.storage and not parent.lot_type.storage:
        raise ApiError("Un rangement du stock est rangé dans le stock ou dans un autre rangement")
    if not lot.lot_type.storage and parent.lot_type.storage:
        raise ApiError(f"{parent.name} est un rangement du stock : seuls des rangements peuvent y être rangés")
    return parent


def check_rangement_parent(lot: Lots) -> None:
    """Un lot d'un rangement (ex : un sac de soin du B+) se range dans un lot, sauf un sac de rechange."""
    rangement_of = lot.lot_type.part_of
    if rangement_of is not None and lot.parent_id is None and not lot.spare:
        raise ApiError(f"Un {lot.lot_type.name} se range dans un lot ({rangement_of.name} ou autre) : choisissez "
                       "« Rangé dans le lot », ou cochez « Sac de rechange » pour un sac libre")


def free_rangement(lot: Lots) -> None:
    """Lot sorti de son lot (libere, remplace) : un rangement devient un sac de rechange."""
    if lot.lot_type.part_of_id:
        lot.spare = True


def check_optional_lot(lot: Lots) -> None:
    """Un sous-lot facultatif (tout son contenu l'est) : range dans un lot, pas un rangement du stock."""
    if lot.lot_type.storage:
        raise ApiError("Un rangement du stock ne peut pas être facultatif")
    if lot.parent is None:
        raise ApiError("Seul un sous-lot peut être facultatif : rangez ce lot dans un autre lot d'abord")


def default_parent(lot: Lots) -> Lots | None:
    """Lot parent d'un lot independant : le stock pour un rangement du stock, aucun sinon."""
    return Lots.stock() if lot.lot_type.storage and not lot.is_stock else None


@api_view(['PATCH'])
@handle_errors
def lot_update(request: Request, lot_id: str) -> Response:
    require_front(request, GESTION)
    lot = get_object_or_404(Lots.objects.select_related('lot_type', 'parent'), id=lot_id)
    data = request.data
    if 'name' in data:
        name = str(data['name']).strip()[:64]
        if not name:
            raise ApiError("Nom obligatoire")
        if name_key(name) != name_key(lot.name):
            ensure_name_free(Lots.objects.filter(active=True).exclude(id=lot.id), name, 'Le lot')
        lot.name = name
    if 'name_short' in data:
        lot.name_short = str(data['name_short'])[:16]
    if 'parent' in data:
        lot.parent = parse_parent(lot, data['parent'])
    if 'key_location' in data:
        lot.key_location = str(data['key_location']).strip()[:128]
    if 'storage_kind' in data and lot.lot_type.storage and not lot.is_stock:
        lot.storage_kind = storage_kind_value(data['storage_kind'])
    if 'spare' in data:
        lot.spare = bool(data['spare']) and lot.lot_type.part_of_id is not None
    if 'parent' in data or 'spare' in data:
        check_rangement_parent(lot)
    if 'optional' in data:
        lot.optional = bool(data['optional'])
    if 'hidden_in_verif' in data:
        lot.hidden_in_verif = bool(data['hidden_in_verif'])
    if lot.optional and 'optional' in data or lot.optional and 'parent' in data:
        check_optional_lot(lot)
    if 'active' in data:
        active = bool(data['active'])
        if not active and lot.children.filter(active=True).exists():
            raise ApiError("Ce lot contient des sous-lots actifs : archivez-les ou retirez-les du lot d'abord")
        if active and not lot.active:
            if lot.lot_type.archived:
                raise ApiError(f"Le type de lot {lot.lot_type.name} est archivé : désarchivez-le d'abord")
            ensure_name_free(Lots.objects.filter(active=True).exclude(id=lot.id), lot.name, 'Le lot')
        if not active and lot.is_stock:
            raise ApiError("Le stock ne peut pas être archivé")
        if active and lot.parent is not None and not lot.parent.active:
            lot.parent = default_parent(lot)  # le lot parent a ete archive entre-temps : le lot redevient independant
            free_rangement(lot)
        lot.active = active
    old_name = Lots.objects.filter(id=lot.id).values_list('name', flat=True).first()
    lot.save()
    changes = []
    if 'name' in data and old_name != lot.name:
        changes.append(f'renommé (ancien nom : {old_name})')
    if 'name_short' in data:
        changes.append('nom court')
    if 'parent' in data:
        changes.append(f'rangé dans {lot.parent.name}' if lot.parent else 'lot indépendant')
    if 'key_location' in data:
        changes.append("emplacement de l'étiquette clé")
    if 'storage_kind' in data and lot.lot_type.storage and not lot.is_stock:
        changes.append(f'genre : {lot.storage_kind}')
    if 'optional' in data:
        changes.append('facultatif à la vérif' if lot.optional else 'obligatoire à la vérif')
    if 'hidden_in_verif' in data:
        changes.append('caché dans la vérif en cascade' if lot.hidden_in_verif else 'affiché dans la vérif en cascade')
    if 'spare' in data:
        changes.append('sac de rechange' if lot.spare else 'pas un sac de rechange')
    if 'active' in data:
        changes.append('actif' if lot.active else 'archivé')
    journal.record(OperationKind.LOT, front_identity(request), 'lot modifié : ' + (', '.join(changes) or 'aucun changement'),
                   lot=lot, fields=sorted(data.keys()))
    return Response(ser.lot_dict(lot, local=True, with_items=True))


@api_view(['POST'])
@handle_errors
def lot_seal(request: Request, lot_id: str) -> Response:
    require_front(request, GESTION)
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    identity = operation_identity(request)
    services.seal_lot(lot, identity, str(request.data.get('seal_number', '')).strip(), bool(request.data.get('force')))
    return Response(ser.lot_dict(lot, local=True, with_items=True))


@api_view(['POST'])
@handle_errors
def lot_rotate_key(request: Request, lot_id: str) -> Response:
    require_front(request, GESTION)
    lot = get_object_or_404(Lots.objects.select_related('lot_type'), id=lot_id)
    lot.rotate_key()
    lot.save(update_fields=['verif_key', 'verif_key_expires', 'key_expiry_stage'])
    journal.record(OperationKind.LOT_KEY, front_identity(request), 'étiquette clé renouvelée (ancienne invalidée)',
                   lot=lot)
    notifications.notify_admins('lot_key_renewed', 'étiquette de lot renouvelée',
                                f'étiquette clé du lot {lot.name} renouvelée par {short_name(front_user(request).matricule)}')
    return Response(ser.lot_dict(lot, local=True, with_items=True))


@api_view(['GET'])
@handle_errors
def lot_verifs(request: Request, lot_id: str) -> Response:
    require_front(request, GESTION)
    lot = get_object_or_404(Lots, id=lot_id)
    return Response([ser.verif_dict(verif) for verif in Verifs.objects.filter(lot=lot).order_by('-datetime')[:100]])


@api_view(['GET'])
@handle_errors
def operations(request: Request) -> Response:
    """Journal des operations, du plus recent au plus ancien (fenetre Journal du poste, roles gestion et admin).

    Filtres : kind (types separes par des virgules), by (identite stockee, ex M:1234), lot (avec ses sous-lots, archives
    compris, sauf sub=0), since / until (dates AAAA-MM-JJ incluses), q (texte cherche dans le resume, le nom du lot,
    le nom ou le matricule de la personne). Pages : before (id de la derniere ligne recue), limit.
    facets=1 ajoute les personnes presentes dans le journal et les types d'operations (listes des filtres)."""
    require_front(request, GESTION)
    params = request.query_params
    queryset = Operation.objects.select_related('cancelled_by').order_by('-at', '-id')
    if params.get('kind'):
        kinds = [kind for kind in params['kind'].split(',') if kind]
        unknown = sorted(set(kinds) - set(OperationKind.values))
        if unknown:
            raise ApiError("Type d'opération inconnu : " + ', '.join(unknown))
        queryset = queryset.filter(kind__in=kinds)
    if params.get('by'):
        queryset = queryset.filter(by=params['by'])
    if params.get('lot'):
        lot = get_object_or_404(Lots, id=params['lot'])
        queryset = queryset.filter(lot_id__in=[lot.id] if params.get('sub') == '0' else lot_and_sub_lot_ids(lot))
    since = parse_date(params.get('since'), 'since')
    until = parse_date(params.get('until'), 'until')
    if since:
        queryset = queryset.filter(at__date__gte=since)
    if until:
        queryset = queryset.filter(at__date__lte=until)
    text = str(params.get('q', '')).strip()
    if text:
        people = Secouristes.objects.all()
        for word in text.split():
            people = people.filter(Q(nom__icontains=word) | Q(prenom__icontains=word) | Q(matricule__icontains=word))
        identities = [f'{SOURCE_VERIFIED}:{matricule}' for matricule in people.values_list('matricule', flat=True)]
        queryset = queryset.filter(Q(summary__icontains=text) | Q(lot_name__icontains=text) | Q(by__icontains=text)
                                   | Q(by__in=identities))
    if params.get('before'):
        last = Operation.objects.filter(id=parse_int(params['before'], 'before')).first()
        if last is not None:
            queryset = queryset.filter(Q(at__lt=last.at) | Q(at=last.at, id__lt=last.id))
    limit = parse_int(params.get('limit', 200), 'limit', 1, 1000)
    rows = list(queryset[:limit + 1])
    names = journal.names([row.by for row in rows] + [row.cancelled_by.by for row in rows if row.cancelled_by])
    batches = set(OperationChange.objects.filter(batch__in={row.batch for row in rows if row.batch})
                  .values_list('batch', flat=True).distinct())
    response = {'operations': [ser.operation_dict(row, names, row.batch in batches) for row in rows[:limit]],
                'more': len(rows) > limit}
    if params.get('facets') == '1':
        identities = set(Operation.objects.exclude(by='').values_list('by', flat=True).distinct())
        everyone = journal.names(identities)
        response['people'] = sorted(
            ({'by': identity, 'name': everyone[identity], 'verified': identity.startswith(SOURCE_VERIFIED + ':')}
             for identity in identities),
            key=lambda person: (not person['verified'], person['name'].lower()),
        )
        response['kinds'] = [{'kind': value, 'label': label} for value, label in OperationKind.choices]
    return Response(response)


@api_view(['GET', 'POST'])
@handle_errors
def operation_cancel(request: Request, operation_id: int) -> Response:
    """Annuler une operation du journal (admins, page Journal du poste), voir undo.py.

    GET : ce qui sera annule avec elle {"operation": ligne, "together": [operations de la meme requete],
    "dependents": [operations posterieures qui en dependent, annulees aussi]} ou une erreur si elle ne peut pas
    l'etre. POST {"reason"} (motif obligatoire) : annule, renvoie la ligne d'annulation du journal. Une annulation
    s'annule de la meme facon (l'operation annulee est retablie)."""
    user = require_front(request, ADMIN)
    operation = get_object_or_404(Operation.objects.select_related('cancelled_by'), id=operation_id)
    if request.method == 'GET':
        own, others = undo.preview(operation)
        rows = own + others
        names = journal.names([row.by for row in rows])
        return Response({
            'operation': ser.operation_dict(operation, names, True),
            'together': [ser.operation_dict(row, names, True) for row in own if row.id != operation.id],
            'dependents': [ser.operation_dict(row, names, True) for row in others],
        })
    line = undo.cancel(operation, format_identity_verified(user.matricule), request.data.get('reason', ''))
    line = Operation.objects.select_related('cancelled_by').get(id=line.id)
    return Response(ser.operation_dict(line, journal.names([line.by]), True))


def lot_and_sub_lot_ids(lot: Lots) -> list[str]:
    """Le lot et tous ses sous-lots, archives compris (l'historique d'un sous-lot archive reste celui du lot)."""
    by_parent: dict[str, list[str]] = {}
    for lot_id, parent_id in Lots.objects.filter(parent__isnull=False).values_list('id', 'parent_id'):
        by_parent.setdefault(parent_id, []).append(lot_id)
    result, pending = [], [lot.id]
    while pending:
        current = pending.pop()
        if current not in result:
            result.append(current)
            pending.extend(by_parent.get(current, []))
    return result


def admin_count() -> int:
    return Secouristes.objects.filter(role=Role.ADMIN, active=True, key_expires__gte=timezone.localdate()).count()


def admin_exists() -> bool:
    return admin_count() > 0


@api_view(['GET'])
def setup(request: Request) -> Response:
    """Etat de premiere configuration : le front propose de creer un responsable s'il n'y en a aucun
    avec un badge valide (premiere installation, ou tous les badges responsables expires)."""
    admins = admin_count()
    return Response({
        'users': Secouristes.objects.count(),
        'admins': admins,
        'needs_admin': admins == 0,
    })


@api_view(['GET', 'POST'])
@handle_errors
def users(request: Request) -> Response:
    # premier administrateur (aucun admin avec un badge valide) : le poste le cree sans etre connecte
    first_admin = request.method == 'POST' and not admin_exists()
    if not first_admin:
        require_front(request, ADMIN)
    if request.method == 'GET':
        users_list = (Secouristes.objects.filter(guest=False).select_related('pin_contact').annotate(push_device_count=Count('push_subscriptions', distinct=True),
                                passkey_count=Count('passkeys', distinct=True))
                      .order_by('nom', 'prenom'))
        settings_row = ServerSettings.get()  # lu une fois pour toute la liste
        for user in users_list:
            user.server_pin_required_roles = settings_row.pin_required_roles
            user.server_pin_login_roles = settings_row.pin_login_roles
        return Response([ser.user_dict(user, local=True) for user in users_list])
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
    role = Role.ADMIN if first_admin else parse_role(data) or Role.NORMAL
    user = Secouristes(matricule=matricule, nom=nom[:32], prenom=prenom[:32], role=role)
    if 'fonction' in data:
        user.fonction = parse_fonction(data['fonction'])
    if data.get('pin_mode'):
        user.set_pin_mode(str(data['pin_mode']))
    if data.get('pin'):
        user.set_pin(data['pin'])  # sinon, un admin choisira son PIN a sa premiere connexion
    if 'pin_at_login' in data:
        user.set_pin_at_login(data['pin_at_login'])
    if data.get('pin_contact'):
        user.pin_contact = parse_pin_contact(user, data['pin_contact'])
    user.renew_key()
    with transaction.atomic():
        apply_default_contact(user, data)
        user.save()
        by = f'{SOURCE_VERIFIED}:{user.matricule}' if first_admin else front_identity(request)
        journal.record(OperationKind.USER, by, f'compte créé : {user} ({user.matricule}), rôle {user.get_role_display()}',
                       matricule=user.matricule, role=user.role)
    data = ser.user_dict(user, local=True)
    if first_admin:
        data['session'] = session_token(user, method='setup')  # le poste connecte directement le nouvel administrateur
    return Response(data, status=status.HTTP_201_CREATED)


@api_view(['GET', 'PATCH'])
@handle_errors
def user_detail(request: Request, matricule: str) -> Response:
    require_front(request, ADMIN)
    user = get_object_or_404(Secouristes, matricule=matricule)
    if request.method == 'PATCH' and user.guest:
        return guest_card_patch(request, user)
    if request.method == 'PATCH':
        data = request.data
        for field in ('nom', 'prenom'):
            if field in data:
                setattr(user, field, str(data[field]).strip()[:32])
        if 'fonction' in data:
            user.fonction = parse_fonction(data['fonction'])
        role = parse_role(data)
        if role:
            user.role = role
            if role == Role.ADMIN and user.pin_mode == PinMode.DISABLED:
                user.pin_mode = PinMode.DEFAULT  # PIN toujours obligatoire pour un admin
        if 'pin_mode' in data:
            user.set_pin_mode(str(data['pin_mode'] or ''))  # 'disabled' supprime le PIN (refuse pour un admin)
        if 'active' in data:
            user.active = bool(data['active'])
        if 'pin' in data:
            user.set_pin(data['pin'])  # '' supprime le PIN (refuse pour un admin)
        if 'pin_at_login' in data:
            user.set_pin_at_login(data['pin_at_login'])  # « Demander a chaque connexion » (impose pour un admin)
        if data.get('pin_reset'):
            user.reset_pin()  # deblocage : nouveau PIN choisi a la prochaine connexion
        if 'pin_contact' in data:
            user.pin_contact = parse_pin_contact(user, data['pin_contact'])
        if 'push_disabled' in data:
            user.push_disabled = parse_push_disabled(data['push_disabled'])
        if 'web_prefs' in data:
            user.web_prefs = {**(user.web_prefs or {}), **parse_web_prefs(data['web_prefs'])}
        if 'scanner_volume' in data:  # vide : volume par defaut du serveur
            value = data['scanner_volume']
            user.scanner_volume = None if value in (None, '') else parse_scanner_volume(value)
        with transaction.atomic():
            apply_default_contact(user, data)
            user.save()
            if data.get('delete_passkeys'):  # la connexion par nom et PIN redevient possible
                user.passkeys.all().delete()
            journal.record(OperationKind.USER, front_identity(request),
                           f'compte modifié : {user} ({user.matricule}) : {user_changes(data, user)}',
                           matricule=user.matricule, fields=sorted(data.keys()))
    return Response(ser.user_dict(user, local=True))


def guest_card_patch(request: Request, card: Secouristes) -> Response:
    """Carte d'invite : seulement active (desactivee, activee ou non : plus de connexion, sessions coupees), duree de
    vie (heures apres l'activation) et nom/prenom corriges. Toujours role secouriste, jamais de PIN."""
    data = request.data
    changes = []
    if 'active' in data:
        card.active = bool(data['active'])
        changes.append('réactivée' if card.active else 'désactivée')
    if 'guest_hours' in data:
        try:
            card.guest_hours = guests.parse_guest_hours(data['guest_hours'])
        except ValueError as exc:
            raise ApiError(str(exc))
        changes.append(f'durée {card.guest_hours}h')
    for field, length, label in (('nom', 32, 'nom'), ('prenom', 32, 'prénom'), ('guest_antenna', 64, 'antenne')):
        if field in data and card.guest_activated is not None:
            value = ' '.join(str(data[field] or '').split())[:length]
            if not value:
                raise ApiError('Nom, prénom et antenne obligatoires')
            setattr(card, field, value)
            changes.append(label)
    unknown = set(data) - {'active', 'guest_hours', 'nom', 'prenom', 'guest_antenna'}
    if unknown:
        raise ApiError("Carte d'invité : seuls l'état, la durée et le nom se modifient")
    card.save()
    journal.record(OperationKind.USER, front_identity(request),
                   f"carte d'invité {card.matricule} ({card}) modifiée : {', '.join(changes) or 'aucun changement'}",
                   matricule=card.matricule, fields=sorted(data.keys()))
    return Response(guests.card_row(card))


@api_view(['POST'])
@handle_errors
def guest_card_actions(request: Request, matricule: str) -> Response:
    """Carte d'invite (poste, admins) : {"action": "reuse"} reprend le numero d'une carte jamais activee (nouvelle
    carte a imprimer), {"action": "reactivate", "guest_hours", "new_card"?} rouvre une periode pour un invite qui
    revient (carte a son nom a imprimer si new_card). La cle en clair (badge_url) n'est que dans cette reponse."""
    require_front(request, ADMIN)
    card = get_object_or_404(Secouristes, matricule=matricule, guest=True)
    action = str(request.data.get('action') or '')
    if action not in ('reuse', 'reactivate'):
        raise ApiError('action : reuse ou reactivate')
    guest_card_action(card, action, request.data, front_user(request), front_identity(request))
    row = guests.card_row(card)
    if getattr(card, 'new_key', None):
        row['badge_url'] = ser.public_url('badge', m=card.matricule, key=card.new_key)
    return Response(row)


@api_view(['GET', 'POST'])
@handle_errors
def guest_cards(request: Request) -> Response:
    """Cartes d'invites (poste, admins). GET : toutes les cartes. POST {"count", "guest_hours"?} : nouvelles cartes a
    imprimer, avec leur cle en clair (badge_url) dans cette reponse seulement."""
    require_front(request, ADMIN)
    if request.method == 'GET':
        return Response({'cards': guests.cards(), 'hours': ServerSettings.get().guest_hours})
    count = parse_int(request.data.get('count', 1), 'count', 1, guests.GUEST_MAX_CARDS)
    try:
        hours = (guests.parse_guest_hours(request.data['guest_hours'])
                 if request.data.get('guest_hours') not in (None, '') else None)
    except ValueError as exc:
        raise ApiError(str(exc))
    admin = front_user(request)
    rows = []
    with transaction.atomic():
        for _ in range(count):
            card = guests.create_guest_card(admin, hours)
            journal.record(OperationKind.USER, front_identity(request),
                           f"carte d'invité {card.matricule} créée par {admin}, valable {card.guest_hours}h "
                           "après sa première connexion", matricule=card.matricule)
            row = guests.card_row(card)
            row['badge_url'] = ser.public_url('badge', m=card.matricule, key=card.new_key)
            rows.append(row)
    return Response({'cards': rows}, status=status.HTTP_201_CREATED)


def parse_fonction(value: str) -> str:
    """Role dans l'equipe (texte libre, 32 caracteres) : vide, c'est le role par defaut (Secouriste)."""
    return ' '.join(str(value or '').split())[:32] or DEFAULT_FONCTION


USER_FIELDS = {'nom': 'nom', 'prenom': 'prénom', 'fonction': 'rôle', 'pin': 'PIN', 'pin_reset': 'PIN réinitialisé', 'pin_mode': 'mode du PIN', 'pin_at_login': 'PIN demandé à chaque connexion',
               'pin_contact': 'admin à contacter', 'push_disabled': 'notifications', 'default_contact': 'contact par défaut',
               'web_prefs': 'réglages du web', 'delete_passkeys': 'passkeys supprimées (nom + PIN rétabli)',
               'scanner_volume': 'volume de la douchette'}


def user_changes(data: dict[str, Any], user: Secouristes) -> str:
    """Changements d'un compte pour le journal (jamais la valeur du PIN)."""
    changes = []
    if parse_role(data):
        changes.append(f'rôle {user.get_role_display()}')
    if 'active' in data:
        changes.append('actif' if user.active else 'désactivé')
    fields = changed_fields(data, USER_FIELDS)
    if fields:
        changes.append(fields)
    return ', '.join(changes) or 'aucun changement'


def parse_push_disabled(value: list[str]) -> list[str]:
    """Types de notifications web coupes pour un utilisateur (liste de noms de PUSH_TYPES)."""
    if not isinstance(value, list) or not all(isinstance(name, str) and name in PUSH_TYPES for name in value):
        raise ApiError("push_disabled : liste de types de notifications attendue (" + ', '.join(PUSH_TYPES) + ")")
    return sorted(set(value))


def parse_pin_contact(user: Secouristes, value: str) -> Secouristes | None:
    """Admin a contacter si le PIN est bloque (matricule d'un administrateur actif, vide = aucun)."""
    if value in (None, ''):
        return None
    contact = Secouristes.objects.filter(matricule=str(value)).first()
    if contact is None or contact.role != Role.ADMIN or not contact.active:
        raise ApiError("L'admin à contacter doit être un administrateur actif")
    return contact


@api_view(['POST'])
@handle_errors
def user_renew_key(request: Request, matricule: str) -> Response:
    require_front(request, ADMIN)
    user = get_object_or_404(Secouristes, matricule=matricule)
    user.renew_key()
    user.save(update_fields=['key_hash', 'key_expires', 'key_expiry_stage'])
    journal.record(OperationKind.USER, front_identity(request), f'badge renouvelé : {user} ({user.matricule})',
                   matricule=user.matricule)
    notifications.notify_admins('badge_renewed', 'badge renouvelé',
                                f'badge de {short_name(user.matricule)} renouvelé par {short_name(front_user(request).matricule)}')
    return Response(ser.user_dict(user, local=True))
