# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          serializers.py
#       -\-    _|__
#        |\___/  . \        Created on 29 Sep. 2026 at 14:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Conversion des modeles en dictionnaires JSON pour les API."""

from datetime import datetime, timedelta
from urllib.parse import urlencode

from django.utils import timezone

from .idendity import SOURCE_VERIFIED, identity_name
from .journal import plural
from .models import PIN_OPTIONAL_ROLES, PUSH_TYPES, WEB_PREFS, ItemStatus, LotType, Role, compact_iid, custom_order_key, qrprotec_setting
from .reports import item_reports, lot_reports, pack_reports, summary as report_summary
from .services import restock_credit, LotPreload, fixed_slots_missing, optional_lot, slot_candidates, assign_slots, optional_types, ordered_requirements, lot_distribution, hidden_in_verif, virtual_lot, unplaced_lines, out_deadline, requirements_status


def public_url(path: str, **params) -> str:
    base = qrprotec_setting('PUBLIC_BASE_URL')
    if not base.endswith('/'):
        base += '/'
    return f"{base}{path}?{urlencode(params)}"


def _date(value):
    """Date seule (peremption, expiration) ou horodatage en heure locale (verifs, passages)."""
    if not value:
        return None
    if isinstance(value, datetime):
        return timezone.localtime(value).isoformat(timespec='seconds')
    return value.isoformat()


def _who(value):
    # nom court (prenom, initiale en cas d'homonyme) sur l'API publique, voir idendity.anonymized
    return identity_name(value)


def item_type_dict(item_type):
    return {
        'type': item_type.type,
        'name': item_type.name,
        'description': item_type.description,
        'min_quantity': item_type.min_quantity,
        'perissable': item_type.perissable,
        'default_pack_size': item_type.default_pack_size,
        'tear_off': item_type.tear_off,
        'label_style': item_type.label_style,
        'consumable': item_type.consumable,
        'physical': item_type.physical,  # faux : item a cocher, sans etiquette
        'tasks': list(item_type.tasks or []),
        # revision du materiel non consommable (0 mois : pas de revision)
        'revision_months': item_type.revision_months,
        'revision_tasks': list(item_type.revision_tasks or []),
        'revision_usable': item_type.revision_usable,
        'archived': item_type.archived,
        # attentes en quantite : type a unites (unit, ex : mL) rempli par ses sous-types, ou sous-type (group) qui en
        # remplit fills unites ; default_subtype : sous-type propose par defaut (reception, commande, reassort)
        'unit': item_type.unit,
        'group': item_type.group_id or '',
        'fills': item_type.fills,
        'default_subtype': item_type.default_subtype_id or '',
    }


def item_dict(item, today=None):
    today = today or timezone.localdate()
    return {
        'iid': item.iid,
        # forme courte imprimee sur les etiquettes (D sans date, N pour les zeros du compteur)
        'iid_short': compact_iid(item.iid),
        # contenu du QR code de l'etiquette : ouvre le front web comme un scan de l'item
        'url': public_url('item', id=compact_iid(item.iid)),
        'type': item.pack.item_type_id,
        'type_name': item.pack.item_type.name,
        # etiquette a dechirer avant utilisation : un item non scanne a une verif est utilise
        'tear_off': item.pack.item_type.tear_off,
        # style d'etiquette : QR de l'URL, petit QR ou code-barres de l'iid seul
        'label_style': item.pack.item_type.label_style,
        # non consommable : absent a la verif d'un lot, il passe hors lot (ni utilise, ni disparu)
        'consumable': item.pack.item_type.consumable,
        # taches a cocher a la verif (etiquette de taches) : l'item ne compte qu'une fois toutes validees
        'tasks': list(item.pack.item_type.tasks or []),
        'task_url': public_url('task', id=compact_iid(item.iid)),
        'peremption': _date(item.pack.peremption),
        # materiel a reviser : prochaine revision (en retard : a remplacer si le type ne le permet pas, sinon
        # avertissement), taches de la revision, derniere revision
        'revision': item.pack.item_type.has_revision,
        'revision_due': _date(item.revision_due) if item.pack.item_type.has_revision else None,
        'revision_late': item.revision_late(today),
        'revision_usable': item.pack.item_type.revision_usable,
        'revision_months': item.pack.item_type.revision_months,
        'revision_tasks': list(item.pack.item_type.revision_tasks or []) if item.pack.item_type.has_revision else [],
        'revised_at': _date(item.revised_at),
        'revised_by': _who(item.revised_by),
        # a remplacer : perime, signale inutilisable ou desactive (ne compte plus dans son lot, voir reports.py)
        'expired': item.needs_replacement(today),
        'outdated': item.is_expired(today),  # perime seulement
        'unusable': item.unusable,
        'reported': item.reported,  # signalement ouvert : jamais marque disparu par une verif
        # signalements ouverts (de l'item ou de son paquet ferme) : popup et bip quand il est scanne
        'reports': item_reports(item),
        # desactive (rappel produit, non-conformite) : invalide comme un perime, a remplacer
        'disabled': item.disabled,
        'disabled_reason': item.disabled_reason,
        'disabled_at': _date(item.disabled_at),
        'disabled_by': _who(item.disabled_by),
        'status': item.status,
        'location': item.location_id,
        'location_name': item.location.name if item.location_id else '',
        # en stock : hors lot, ou range dans un rangement du stock (armoire, tiroir...)
        'in_stock': item.status not in (ItemStatus.OUT, ItemStatus.LOOSE) and (item.location_id is None or item.location.lot_type.storage),
        # sorti du stock sans lot : compte comme utilise a cette date s'il n'est pas revu a une verif
        'out_until': _date(out_deadline(item)) if item.status == ItemStatus.OUT else None,
        'sealed_pack': item.sealed_pack_id,
        'missed_verifs': item.missed_verifs,
        'last_seen': _date(item.last_seen),
        'last_seen_by': _who(item.last_seen_by),
        'last_seen_while': item.last_seen_while,
        'added': _date(item.added),
        'added_by': _who(item.added_by),
        'deleted_reason': item.deleted_reason,
    }


def revision_dict(revision):
    return {'at': _date(revision.at), 'by': _who(revision.by), 'tasks': list(revision.tasks or []),
            'next_due': _date(revision.next_due), 'note': revision.note}


def item_last_location(item):
    """Dernier emplacement connu d'un item (fiche d'item du front web) : son lot actuel, ou pour un item qui n'est
    plus actif (sorti, disparu, utilise...) le lot qu'il a quitte en dernier. None : en stock, ou jamais range."""
    lot, at = None, None
    if item.location_id:
        lot, at = item.location, item.last_seen
    elif item.status != ItemStatus.ACTIVE:
        movement = (item.movements.select_related('from_lot__lot_type').filter(from_lot__isnull=False)
                    .order_by('-at', '-id').first())
        if movement:
            lot, at = movement.from_lot, movement.at
    if lot is None or not lot.active:
        return None
    return {
        'id': lot.id,
        'name': lot.name,
        'lot_type_name': lot.lot_type.name,
        'storage': lot.lot_type.storage,
        'path': [{'id': parent.id, 'name': parent.name} for parent in reversed(lot.ancestors())],
        'current': item.location_id == lot.id,
        'at': _date(at),
    }


def sealed_pack_dict(sealed_pack, with_items=True):
    data = {
        'id': sealed_pack.id,
        'type': sealed_pack.item_type_id,
        'type_name': sealed_pack.item_type.name,
        'peremption': _date(sealed_pack.peremption),
        'count': sealed_pack.count,
        'created': _date(sealed_pack.created),
        'created_by': _who(sealed_pack.created_by),
        'opened': _date(sealed_pack.opened),
        'opened_by': _who(sealed_pack.opened_by),
        'url': public_url('pack', id=sealed_pack.id),
        # items desactives (rappel produit, non-conformite) et signalements ouverts du paquet
        'disabled': sealed_pack.items.filter(disabled=True).count(),
        'reported': sealed_pack.reports.exclude(status='resolved').count(),
        'reports': pack_reports(sealed_pack),
    }
    if with_items:
        data['items'] = [item.iid for item in sealed_pack.items.order_by('iid')]
    return data


def suggested_part_of(lot_type):
    """Referentiel propose pour un type qui n'en a pas encore (transition) : celui de tous ses lots s'ils sont tous
    des sous-lots de lots d'un meme type (ex : les sacs de soin, tous dans des B+). '' sinon."""
    if lot_type.referentiel or lot_type.part_of_id or lot_type.storage:
        return ''
    parents = set(lot_type.lots_set.filter(active=True).values_list('parent__lot_type', flat=True))
    if len(parents) != 1 or None in parents:
        return ''
    parent_type = parents.pop()
    return '' if parent_type == lot_type.type else parent_type


def lot_type_dict(lot_type):
    # lot unique : son lot (actif de preference, le plus recent s'il y en a eu plusieurs)
    lot = lot_type.lots_set.order_by('-active', '-created').first() if lot_type.unique else None
    return {
        'type': lot_type.type,
        'name': lot_type.name,
        'description': lot_type.description,
        'version': lot_type.version,
        'storage': lot_type.storage,
        'unique': lot_type.unique,
        'seal_required': lot_type.seal_required,
        'archived': lot_type.archived,
        'lot': lot.id if lot else '',
        'lot_count': lot_type.lots_set.count(),
        'locations': list(lot_type.locations or []),
        # referentiel (liste obligatoire, sans emplacements) et rangements (sa repartition entre des sacs)
        'referentiel': lot_type.referentiel,
        'part_of': lot_type.part_of_id or '',
        'part_of_name': lot_type.part_of.name if lot_type.part_of_id else '',
        'rangements': [{'type': row.type, 'name': row.name}
                       for row in lot_type.rangements.order_by('name')] if lot_type.referentiel else [],
        'suggested_part_of': suggested_part_of(lot_type),
        # lots exiges (fixes ou a l'assemblage), avec les types acceptes
        'slots': [
            {'id': slot.id, 'name': slot.name, 'accepted': list(slot.accepted), 'quantity': slot.quantity,
             'mode': slot.mode, 'optional': slot.optional}
            for slot in lot_type.slots.all()
        ],
        'requirements': [
            {
                'type': requirement.item_type_id,
                'type_name': requirement.item_type.name,
                'quantity': requirement.quantity,
                'location': requirement.location,
                # facultatif a la verif : seulement pour du materiel non consommable sans taches
                'optional': requirement.optional and requirement.item_type.can_be_optional,
                'can_be_optional': requirement.item_type.can_be_optional,
            }
            for requirement in ordered_requirements(lot_type)
        ],
    }


STATE_LABELS = {
    'unusable': '✘ Non utilisable (signalé)',
    'unplaced': '✘ Liste pas toute dans les sacs',
    'sealed': '✔ Scellé',
    'sealed_expired': '✘ Scellé, contient des périmés',
    'never': '✘ Jamais vérifié',
    'incomplete': '✘ Incomplet',
    'recommended': '⚠ Vérif recommandée, réassort',
    'opened': '⚠ Vérif recommandée, scellé ouvert',
    'list_back': '⚠ Vérif recommandée, sorti avec une liste',
    'used': '⚠ Vérif recommandée, lot utilisé',
    'seal_opened': '✘ Scellé ouvert, vérif complète puis scellé requis',
    'must_seal': '✘ Vérifié, doit être scellé',
    'verified': '✔ Vérifié, complet',
    'optional': '✔ Facultatif, jamais vérifié',
}
KIND_ORDER = {'ok': 0, 'warn': 1, 'bad': 2}


def lot_state(data):
    """Etat d'un lot seul (memes regles que les fronts) : kind 'ok' (vert), 'warn' (orange) ou 'bad' (rouge)."""
    if data.get('unusable'):
        # signale non utilisable (signalement ouvert sur le lot) : rouge, scelle ou verifie, jusqu'a sa resolution
        code = 'unusable'
    elif data.get('unplaced'):
        # lot d'un referentiel (B+) : des lignes de la liste ne sont dans aucun de ses sacs (onglet Repartition)
        code = 'unplaced'
    elif data['is_sealed']:
        code = 'sealed_expired' if data['expired_count'] else 'sealed'
    elif not data['last_verif']:
        # sous-lot facultatif : son lot parent se valide sans lui, jamais verifie n'est pas un probleme
        code = 'optional' if data.get('optional') else 'never'
    elif not data['complete']:
        code = 'incomplete'
    elif data.get('seal_required'):
        # type a scelle obligatoire : un lot ouvert est rouge, meme verifie, jusqu'a un nouveau scelle
        code = 'seal_opened' if data['verif_recommended'] else 'must_seal'
    elif data['verif_recommended']:
        # sans reassort depuis la derniere verif : recommandee par l'ouverture du scelle (etiquette d'ouverture)
        code = ('recommended' if data['restocked_count'] else 'list_back' if data.get('back_from_list')
                else 'used' if data.get('marked_used') else 'opened')
    else:
        code = 'verified'
    kind = 'ok' if code in ('sealed', 'verified', 'optional') else 'warn' if code in ('recommended', 'opened', 'list_back', 'used') else 'bad'
    return {'kind': kind, 'code': code, 'label': STATE_LABELS[code]}


def _credit_dict(credit):
    return None if credit is None else {'outings_left': credit['outings_left'], 'until': _date(credit['until'])}


def _lot_core(lot, today, items=None, preload=None):
    """Champs communs a toutes les vues d'un lot (sans cles ni contenu detaille)."""
    if items is None:
        items = list(lot.items.select_related('pack__item_type').filter(status=ItemStatus.ACTIVE).order_by('iid'))
    requirements = requirements_status(lot, today, items, preload)
    expired = [item for item in items if item.needs_replacement(today)]
    soon_limit = today + timedelta(days=30)
    soon = [item for item in items if item.pack.peremption and today <= item.pack.peremption <= soon_limit]
    revised = [item for item in items if item.pack.item_type.has_revision and item.revision_due]
    data = {
        'id': lot.id,
        'lot_type': lot.lot_type_id,
        'lot_type_name': lot.lot_type.name,
        'storage': lot.lot_type.storage,
        'seal_required': lot.lot_type.seal_required,
        'name': lot.name,
        'name_short': lot.name_short,
        'version': lot.version,
        'active': lot.active,
        'parent': lot.parent_id,
        'key_location': lot.key_location,
        # place a l'assemblage (temporaire) ; lot qui tient la place fixe d'un autre (remplacement)
        'assembled': lot.assembled,
        'replaces': lot.replaces_id or '',
        'replaces_name': lot.replaces.name if lot.replaces_id and preload is None else '',
        # sous-lot facultatif a la verif de son parent (services.optional_lot)
        'optional': optional_lot(lot),
        'spare': lot.spare,  # sac de rechange (rangement libre, voir Lots.spare)
        'hidden_in_verif': lot.hidden_in_verif,  # case de la gestion (effet reel : 'hidden' des descendants)
        'is_sealed': lot.is_sealed,
        'sealed': _date(lot.sealed) if lot.is_sealed else None,
        'sealed_by': _who(lot.sealed_by) if lot.is_sealed else '',
        'seal_number': lot.seal_number if lot.is_sealed else '',
        'unsealed': _date(lot.unsealed),
        'unsealed_by': _who(lot.unsealed_by),
        # un lot scelle reste valide jusqu'a la premiere peremption de son contenu
        'valid_until': _date(min((item.pack.peremption for item in items if item.pack.peremption), default=None)),
        'created': _date(lot.created),
        'last_verif': _date(lot.last_verif),
        'last_verif_by': _who(lot.last_verif_by),
        'verif_recommended': lot.verif_recommended,
        'restocked': _date(lot.restocked) if lot.verif_recommended else None,
        'restocked_by': _who(lot.restocked_by) if lot.verif_recommended else '',
        'restocked_count': lot.restocked_count if lot.verif_recommended else 0,
        'back_from_list': lot.back_from_list if lot.verif_recommended else '',
        # marque « utilise » par un secouriste (bouton de la fiche)
        'marked_used': _date(lot.marked_used) if lot.verif_recommended else None,
        'marked_used_by': _who(lot.marked_used_by) if lot.verif_recommended and lot.marked_used else '',
        # credit de confiance du reassort (reglage decoche) : sorties restantes et date limite, sinon None
        'restock_credit': _credit_dict(restock_credit(lot, preload.settings if preload else None)),
        'item_count': len(items),
        'expired_count': len(expired),
        'expiring_soon_count': len(soon),
        # materiel a reviser : prochaine revision, revisions en retard et dans les 30 jours
        'next_revision': _date(min((item.revision_due for item in revised), default=None)),
        'revision_late_count': sum(1 for item in revised if item.revision_due < today),
        'revision_soon_count': sum(1 for item in revised if today <= item.revision_due <= soon_limit),
        'requirements': requirements,
        # complet des que le minimum d'items frais est la : un perime en trop ne le rend pas incomplet
        # places fixes (lots exiges, LotTypeSlot) vides : le lot est incomplet
        'slots_missing': fixed_slots_missing(lot, preload),
        'public_url': public_url('verif', lot=lot.id),
        # signalements ouverts du lot lui-meme (ou d'un type d'item du lot) : toujours affiches sur sa fiche ; un
        # seul inutilisable rend le lot non utilisable
        'reports': ([report_summary(report) for report in preload.reports[lot.id]] if preload is not None
                    else lot_reports(lot)),
    }
    data['unusable'] = any(not report['usable'] for report in data['reports'])
    # lot d'un referentiel reparti entre ses sacs : lignes de la liste placees dans aucun sac (attendues nulle part)
    data['unplaced'] = unplaced_lines(lot)
    data['complete'] = (all(row['present'] >= row['required'] for row in requirements) and not data['slots_missing']
                        and not data['unplaced'])
    data['state'] = lot_state(data)
    return data


def aggregate_tree(rows):
    """Etat d'un lot global a partir de ses lots (lignes de _lot_core avec 'depth', en profondeur d'abord).

    - un lot qui ne contient rien et n'attend rien mais a des sous-lots (simple regroupement) ne compte pas ;
    - un sous-lot d'un lot scelle intact est valide tant qu'il ne contient pas de perimes ;
    - un sous-lot facultatif (services.optional_lot) ne compte pas ;
    - le lot global est valide si tous ses lots le sont, et sa derniere verif est la plus ancienne des
      dernieres verifs de ses lots (aucune si l'un d'eux n'a jamais ete verifie).
    Ajoute a chaque ligne 'counted' et 'effective' (etat dans le lot global).
    """
    parents = {row['id']: row['parent'] for row in rows}
    by_id = {row['id']: row for row in rows}
    has_children = {row['parent'] for row in rows if row['parent']}
    kind = 'ok'
    oldest = None
    never = False
    counted = 0
    problems = 0
    for row in rows:
        covered = False
        parent = parents.get(row['id'])
        while parent in by_id:
            if by_id[parent]['state']['code'] == 'sealed':
                covered = True
                break
            parent = parents.get(parent)
        # un sous-lot facultatif ne compte pas : le lot global est valide sans lui
        row['counted'] = (not row.get('optional')
                          and bool(row['requirements'] or row['item_count'] or row.get('slots_missing') or row.get('unplaced')
                                   or row['id'] not in has_children))
        if covered and not row['expired_count'] and not row.get('unusable'):
            row['effective'] = {'kind': 'ok', 'code': 'covered', 'label': '✔ Dans un lot scellé'}
        else:
            row['effective'] = row['state']
        if not row['counted']:
            continue
        counted += 1
        effective = row['effective']
        if KIND_ORDER[effective['kind']] > KIND_ORDER[kind]:
            kind = effective['kind']
        if effective['kind'] != 'ok':
            problems += 1
        if effective['code'] == 'covered':
            continue
        if row['is_sealed'] and not row['last_verif']:
            stamp = row['sealed']
        else:
            stamp = row['last_verif']
        if not stamp:
            never = True
        elif oldest is None or stamp < oldest:
            oldest = stamp
    if kind == 'ok':
        label = '✔ Tous les lots sont valides'
    elif kind == 'warn':
        label = f'⚠ Vérif recommandée ({plural(problems, "lot")})'
    else:
        label = f'✘ {plural(problems, "lot")} à traiter'
    return {
        'kind': kind,
        'label': label,
        'complete': kind == 'ok',
        'last_verif': None if never else oldest,
        'never_verified': never,
        'lot_count': counted,
    }


def attention_rank(row):
    """Ce qu'un lot demande comme attention, pour le ranger : 3 rouge (a traiter), 2 orange (verif recommandee),
    1 valide mais une peremption ou une revision approche, 0 rien a faire, -1 lot archive. Un simple regroupement
    (rien d'attendu, rien dedans) ne compte que par ses sous-lots."""
    if not row.get('active', True):
        return -1
    if row.get('counted') is False:
        return 0
    kind = (row.get('effective') or row['state'])['kind']
    if kind != 'ok':
        return 3 if kind == 'bad' else 2
    soon = row.get('expiring_soon_count', 0) + row.get('revision_late_count', 0) + row.get('revision_soon_count', 0)
    return 1 if soon else 0


def attention_order(rows):
    """Lignes de lots en profondeur d'abord ('id', 'parent') reordonnees pour que, a chaque niveau, les branches a
    traiter passent devant les autres : l'arborescence reste intacte (chaque lot suivi de ses sous-lots) et l'ordre
    d'origine departage les egalites. Une branche vaut le lot le plus a traiter qu'elle contient."""
    ids = {row['id'] for row in rows}
    children = {}
    roots = []
    for row in rows:
        if row.get('parent') in ids and row['parent'] != row['id']:
            children.setdefault(row['parent'], []).append(row)
        else:
            roots.append(row)
    ranks = {}

    def rank(row):
        if row['id'] not in ranks:
            ranks[row['id']] = attention_rank(row)  # garde-fou contre un cycle
            ranks[row['id']] = max([ranks[row['id']]] + [rank(child) for child in children.get(row['id'], [])])
        return ranks[row['id']]

    ordered = []
    done = set()

    def walk(level):
        for row in sorted(level, key=lambda row: -rank(row)):  # tri stable : ordre d'origine a egalite
            if row['id'] in done:
                continue
            done.add(row['id'])
            ordered.append(row)
            walk(children.get(row['id'], []))

    walk(roots)
    walk(rows)  # cycle eventuel (ne devrait pas exister)
    return ordered


def _tree_row(lot, today, depth):
    row = _lot_core(lot, today)
    row['depth'] = depth
    return row


def lot_global(lot, today):
    """Lot global (racine) du lot et etat de chacun de ses lots ; None si le lot n'a ni parent ni sous-lot."""
    root = lot.root()
    tree = root.descendants()
    if len(tree) <= 1:
        return None
    rows = [_tree_row(sub_lot, today, sub_lot.depth) for sub_lot in tree]
    for sub_lot, row in zip(tree, rows):
        # cache dans la hierarchie (B+ du VPS) : les fronts l'enlevent de l'arbre, hors gestion des lots du poste
        row['hidden'] = hidden_in_verif(sub_lot, row)
    summary = aggregate_tree(rows)
    rows = attention_order(rows)  # sous-lots a traiter en haut, sous leur parent
    keep = ('id', 'name', 'name_short', 'lot_type_name', 'storage', 'seal_required', 'parent', 'depth', 'is_sealed', 'sealed',
            'last_verif', 'last_verif_by', 'item_count', 'expired_count', 'revision_late_count', 'complete', 'verif_recommended', 'state', 'effective',
            'counted', 'optional', 'unusable', 'slots_missing', 'assembled', 'replaces_name', 'hidden', 'unplaced')
    summary.update({
        'id': root.id,
        'name': root.name,
        'lots': [{key: row[key] for key in keep} for row in rows],
    })
    return summary


def lot_dict(lot, local=False, with_items=False, today=None, with_global=True, preload=None):
    """`preload` (services.LotPreload) : donnees de toute une liste de lots deja lues (lot_list)."""
    today = today or timezone.localdate()
    if preload is not None:
        items = preload.items[lot.id]
        ancestors = preload.ancestors(lot)
        children = preload.children[lot.id]
    else:
        items = list(lot.items.select_related('pack__item_type').filter(status=ItemStatus.ACTIVE).order_by('iid'))
        ancestors = lot.ancestors()
        children = lot.children.filter(active=True)
    data = _lot_core(lot, today, items, preload)
    data['parent_name'] = ancestors[0].name if ancestors else ''
    # hidden : lot cache dans la hierarchie (B+ du VPS), saute par le fil « Dans : » du web
    data['path'] = [{'id': parent.id, 'name': parent.name, 'hidden': hidden_in_verif(parent)}
                    for parent in reversed(ancestors)]
    data['children'] = [
        {'id': child.id, 'name': child.name}
        for child in sorted(children, key=lambda child: (child.name.lower(), child.id))
    ]
    if with_global:
        data['global'] = lot_global(lot, today)
    if preload is None:
        data['slots'] = lot_slots(lot, children, today)
        # lot d'un referentiel (B+) : pas d'etiquette, verifie par ses sacs (services.virtual_lot)
        data['virtual'] = virtual_lot(lot)
    if with_items:
        _add_items(data, lot, items, today)
        # sous-lots (en profondeur d'abord) : une verif du lot les couvre aussi
        data['descendants'] = []
        for sub_lot in lot.descendants(include_self=False):
            sub_items = list(sub_lot.items.select_related('pack__item_type').filter(status=ItemStatus.ACTIVE)
                             .order_by('iid'))
            row = _lot_core(sub_lot, today, sub_items)
            row['depth'] = sub_lot.depth
            row['parent'] = sub_lot.parent_id  # repartition des items par lot (services.assign_items)
            # cache dans la vérif en cascade : les fronts remontent ses sous-lots dans son parent (parentIdOf)
            row['hidden'] = hidden_in_verif(sub_lot, row)
            row['virtual'] = virtual_lot(sub_lot)
            _add_items(row, sub_lot, sub_items, today)
            data['descendants'].append(row)
    if local:
        data['verif_key'] = lot.verif_key
        data['verif_key_expires'] = _date(lot.verif_key_expires)
        data['private_url'] = public_url('verif', lot=lot.id, key=lot.verif_key)
        data['seal_url'] = public_url('seal', lot=lot.id, s=lot.seal_code) if lot.is_sealed else ''
        # etiquette d'ouverture, a ranger dans le lot scelle : la scanner ouvre le scelle
        data['seal_open_url'] = (public_url('unseal', lot=lot.id, c=lot.seal_open_code)
                                 if lot.is_sealed and lot.seal_open_code else '')
        data['own_content'] = lot_own_content(lot, preload)
        if preload is None:
            data['distribution'] = lot_distribution(lot)  # repartition du referentiel entre ses sacs
    return data


def _slot_lot(lot, today):
    core = _lot_core(lot, today)
    return {'id': lot.id, 'name': lot.name, 'lot_type': lot.lot_type_id, 'lot_type_name': lot.lot_type.name,
            'state': core['state'], 'parent_name': lot.parent.name if lot.parent_id else '',
            'replaces': lot.replaces_id or '', 'replaces_name': lot.replaces.name if lot.replaces_id else ''}


def lot_slots(lot, children, today):
    """Lots exiges du type du lot (LotTypeSlot) et ce qui les occupe : sous-lots ranges dans chaque place, places
    vides, lots disponibles pour une place a l'assemblage (valides en premier) et lots qui peuvent remplacer un lot
    fixe (Gestion des lots du poste)."""
    slots = list(lot.lot_type.slots.all())
    if not slots:
        return []
    names = dict(LotType.objects.filter(type__in={code for slot in slots for code in slot.accepted})
                 .values_list('type', 'name'))
    children = list(lot.children.select_related('lot_type', 'replaces').filter(active=True))
    rows = []
    for slot, taken in assign_slots(slots, children)[0]:
        candidates = [_slot_lot(other, today) for other in slot_candidates(lot, slot)]
        candidates.sort(key=lambda row: KIND_ORDER[row['state']['kind']])  # valides en premier (tri stable)
        rows.append({
            'id': slot.id, 'name': slot.name, 'mode': slot.mode, 'quantity': slot.quantity, 'optional': slot.optional,
            'accepted': [{'type': code, 'name': names.get(code, code)} for code in slot.accepted],
            'lots': [_slot_lot(child, today) for child in taken],
            'missing': slot.quantity - len(taken),
            'available': candidates if slot.mode == 'assembly' else [],
            'available_ok': sum(1 for row in candidates if row['state']['kind'] == 'ok'),
            'candidates': candidates if slot.mode == 'fixed' else [],
        })
    return rows


def lot_own_content(lot, preload=None):
    """Contenu propre d'un lot, tel que la TECH l'edite (Gestion des lots) : items en plus du type (extra) et
    emplacements de ce lot (location None : celui du type), ordre de ses emplacements (vide : celui du type)."""
    if preload is not None:
        content = sorted(preload.content[lot.id], key=lambda row: (row.item_type.name, row.item_type_id))
    else:
        content = lot.content.select_related('item_type').order_by('item_type__name', 'item_type_id')
    return {
        'locations': list(lot.locations or []),
        'rows': [
            {'type': row.item_type_id, 'type_name': row.item_type.name, 'extra': row.extra, 'removed': row.removed,
             'location': row.location}
            for row in content
        ],
    }


def seal_opening_dict(lot):
    """Scelle tel qu'il etait quand son etiquette d'ouverture a ete scannee (ecran d'ouverture des fronts) : pose
    quand, par qui, valide ou non a l'ouverture (aucun item perime, inutilisable ou desactive sous le scelle) et
    jusqu'a quand le contenu etait valide (premiere peremption, sous-lots compris)."""
    opened = timezone.localtime(lot.unsealed).date() if lot.unsealed else timezone.localdate()
    items = [item for sub_lot in lot.descendants()
             for item in sub_lot.items.select_related('pack__item_type').filter(status=ItemStatus.ACTIVE)]
    return {
        'sealed': _date(lot.sealed),
        'sealed_by': _who(lot.sealed_by),
        'seal_number': lot.seal_number,
        'opened': _date(lot.unsealed),
        'valid': lot.sealed is not None and not any(item.needs_replacement(opened) for item in items),
        'valid_until': _date(min((item.pack.peremption for item in items if item.pack.peremption), default=None)),
    }


def _add_items(data, lot, items, today):
    # facultatif dans ce lot (attendu facultatif ou sous-lot facultatif) : non scanne, il ne retient pas le lot
    _, optional = optional_types(lot)

    def row(item):
        result = item_dict(item, today)
        result['optional'] = optional(item)
        return result
    data['items'] = [row(item) for item in items]
    data['missing_items'] = [
        row(item)
        for item in lot.items.select_related('pack__item_type').filter(status=ItemStatus.MISSING).order_by('iid')
    ]


def lot_list(lots, local=False, today=None, attention=False):
    """Liste de lots dans l'ordre de l'arborescence (chaque lot global suivi de ses sous-lots), avec pour chaque
    lot sa profondeur et l'etat de son lot global. 'attention_order' donne la place du lot quand les lots a traiter
    passent en haut (attention_order) ; attention=True renvoie directement la liste dans cet ordre."""
    today = today or timezone.localdate()
    lots = list(lots)
    ids = {lot.id for lot in lots}
    children = {}
    for lot in lots:
        if lot.parent_id in ids:
            children.setdefault(lot.parent_id, []).append(lot)
    for siblings in children.values():
        siblings.sort(key=lambda child: (*custom_order_key(child), child.name.lower(), child.id))  # comme Lots.descendants
    ordered = []
    seen = set()

    def walk(lot, depth, root):
        if lot.id in seen:
            return
        seen.add(lot.id)
        ordered.append((lot, depth, root))
        for child in children.get(lot.id, []):
            walk(child, depth + 1, root)

    # lots globaux dans l'ordre choisi, puis dans celui de la requete (tri stable)
    for lot in sorted((lot for lot in lots if lot.parent_id not in ids), key=custom_order_key):
        walk(lot, 0, lot)
    for lot in lots:  # cycle eventuel (ne devrait pas exister)
        walk(lot, 0, lot)
    rows = []
    trees = {}
    preload = LotPreload(lots)  # quelques requetes pour toute la liste, au lieu de cinq par lot
    for lot, depth, root in ordered:
        row = lot_dict(lot, local=local, today=today, with_global=False, preload=preload)
        row['depth'] = depth
        row['root'] = root.id
        row['hidden'] = hidden_in_verif(lot, row)  # cache dans la hierarchie : saute par l'Etat des lots du web
        row['virtual'] = virtual_lot(lot)  # B+ de referentiel : un de ses sacs coche au « Départ » coche le B+
        row['description'] = lot.lot_type.description  # recherche de la page « Départ » (autres noms, marques)
        row['sort_order'] = lot.sort_order  # ordre choisi parmi ses freres (ecran « Ordre des lots »), None : pas range
        row['tree_order'] = len(rows)  # place dans l'arborescence sans le tri par couleur (ecran « Ordre des lots »)
        rows.append(row)
        trees.setdefault(root.id, []).append(row)
    for root_id, tree in trees.items():
        if len(tree) <= 1:
            continue
        summary = aggregate_tree(tree)
        summary.update({'id': root_id, 'name': tree[0]['name']})
        for row in tree:
            row['global'] = summary
    ordered = attention_order(rows)
    for index, row in enumerate(ordered):
        row['attention_order'] = index
    return ordered if attention else rows


def user_dict(user, local=False):
    data = {
        'matricule': user.matricule,
        'prenom': user.prenom,
        'role': user.role,
        'role_label': user.get_role_display(),
        'fonction': user.fonction,  # role dans l'equipe, texte libre (sans effet sur les droits)
        'privileged': user.privileged,  # gestion ou admin (mode privilegie du front)
        'has_pin': user.has_pin,
        'pin_required': user.pin_required,
        'pin_mode': user.pin_mode,  # '' : reglage par defaut du serveur, 'required' ou 'disabled'
        'pin_can_disable': user.pin_can_disable,  # PIN facultatif : l'utilisateur peut le supprimer (reglages web)
        'pin_mandatory': user.pin_mandatory,  # code obligatoire (admin, pin_mode required ou role du reglage serveur)
        'pin_at_login': user.pin_at_login or user.pin_at_login_locked,  # « Demander a chaque connexion »
        'pin_at_login_locked': user.pin_at_login_locked,  # code a la connexion obligatoire (admin, mode, role)
        'active': user.active,
        'key_expires': _date(user.key_expires),
        'pin_blocked': user.pin_blocked is not None,
        'pin_reset_required': user.pin_reset_required,
        'default_contact': user.default_contact,
        'web_prefs': user.web_prefs_full(),  # reglages du front web (models.WEB_PREFS), valeurs par defaut comprises
    }
    if local:
        data['nom'] = user.nom  # pas de nom de famille sur l'API publique (front web)
        contact = user.pin_contact
        data['pin_blocked_since'] = _date(user.pin_blocked)
        data['pin_failures'] = user.pin_failures_total
        data['pin_forgotten'] = user.pin_forgotten
        data['pin_contact'] = contact.matricule if contact else ''
        data['pin_contact_name'] = str(contact) if contact else ''
        data['scanner_volume'] = user.scanner_volume  # choisi par l'utilisateur, null : defaut du serveur
        # notifications web : types permis par le role, et ceux qu'un admin a coupes
        disabled = set(user.push_disabled or [])
        data['push_types'] = [{'type': name, 'label': PUSH_TYPES[name][0], 'enabled': name not in disabled}
                              for name in user.push_role_types()]
        count = getattr(user, 'push_device_count', None)
        data['push_devices'] = user.push_subscriptions.count() if count is None else count
        # reglages du front web avec leur libelle, pour les cases de la fenetre Utilisateurs du poste
        data['web_prefs_list'] = [{'name': name, 'label': label, 'value': user.web_pref(name)}
                                  for name, (label, _) in WEB_PREFS.items()]
        # La cle du badge n'est connue qu'a sa creation ou son renouvellement (seule son empreinte est en base) :
        # elle n'apparait que dans cette reponse-la, pour imprimer le badge.
        key = getattr(user, 'new_key', None)
        if key:
            data['key'] = key
            data['badge_url'] = public_url('badge', m=user.matricule, key=key)
    return data


def verif_dict(verif):
    return {
        'id': verif.id,
        'lot': verif.lot_id,
        'datetime': _date(verif.datetime),
        'by': _who(verif.by),
        'participants': [_who(value) for value in verif.participants],
        'complete': verif.complete,
        'present_count': verif.present_count,
        'missing_count': verif.missing_count,
        'expired_count': verif.expired_count,
        'replaced_count': verif.replaced_count,
    }


def operation_dict(operation, names):
    """Ligne du journal des operations. `names` : noms affiches des identites (journal.names)."""
    return {
        'id': operation.id,
        'at': _date(operation.at),
        'kind': operation.kind,
        'kind_label': operation.get_kind_display(),
        'by': operation.by,
        'by_name': names.get(operation.by, operation.by),
        'verified': operation.by.startswith(SOURCE_VERIFIED + ':'),
        'lot': operation.lot_id,
        'lot_name': operation.lot_name,
        'summary': operation.summary,
        'details': operation.details,
        'reconstructed': operation.reconstructed,
    }


# Destinataire SMS : API SMS plus maintenue (voir notifications.py), garde en commentaire.
# def recipient_dict(recipient):
#     # la cle d'identification n'est jamais renvoyee
#     return {
#         'id': recipient.id,
#         'name': recipient.name,
#         'user': recipient.user,
#         'has_password': bool(recipient.password),
#         'active': recipient.active,
#         'last_sent': _date(recipient.last_sent),
#         'last_status': recipient.last_status,
#     }


def server_settings_dict(settings_row):
    return {
        'declared_identity': settings_row.declared_identity,
        'pin_required_roles': list(settings_row.pin_required_roles or []),  # code obligatoire, en plus des admins
        'pin_login_roles': list(settings_row.pin_login_roles or []),  # code a la connexion obligatoire, idem
        'pin_role_choices': [{'value': role, 'label': Role(role).label} for role in PIN_OPTIONAL_ROLES],
        'scanner_volume_default': settings_row.scanner_volume_default,
        'web_barcodes': settings_row.web_barcodes,
        'antenna_name': settings_row.antenna_name,
        'departure_full_verif': settings_row.departure_full_verif,
        'restock_verif_recommended': settings_row.restock_verif_recommended,
        'restock_credit_outings': settings_row.restock_credit_outings,
        'restock_credit_days': settings_row.restock_credit_days,
        'storage_verif_outings': settings_row.storage_verif_outings,
    }


def notification_settings_dict(settings_row):
    # delai de l'alerte « expire bientot » des etiquettes privees de lot et des badges (notifications web) ; les
    # evenements et destinataires SMS ne sont plus exposes (API SMS plus maintenue, voir notifications.py)
    return {'key_expiry_warning_days': settings_row.expiry_warning_days}
