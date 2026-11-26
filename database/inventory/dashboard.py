# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          dashboard.py
#       -\-    _|__
#        |\___/  . \        Created on 26 Nov. 2026 at 21:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Tableau de bord de la TECH (roles gestion et admin, web et poste) : en un seul endroit, comment va la TECH (lots,
vérifs, réassorts), ce qu'elle consomme et jette, ce qu'elle dépense (commandes fournisseurs), les signalements, les
listes et départs, les étiquettes et consommables, l'utilisation du logiciel et les stats fun de l'année.

Tout est calcule et ecrit ici (phrases comprises), les deux fronts affichent la meme chose avec le meme rendu
generique. Chaque section : {key, emoji, title, help, link, tiles, lines, charts, tables} avec
- tiles : [{value, label, detail, level}] (chiffres en grand) ;
- lines : [{text, level}] (phrases) ;
- charts : [{title, unit, labels, series: [{name, values, color}], stacked}] (barres par mois) ;
- tables : [{title, columns, rows: [{cells, level, lot}]}].
level : 'bad' (rouge), 'warn' (orange), 'ok' (vert) ou '' (neutre). En tete, `alerts` reprend les phrases rouges et
oranges de toutes les sections : ce qui merite l'attention d'abord.

Score de réassort : un réassort (items ranges dans un lot sans vérif complète) est « utile » si la vérif suivante du lot
le trouve complet. Une vérif suivante incomplète dit que le réassort n'a pas suffi (il manquait déjà des items, ou le
mauvais matériel a été mis) ; un item réassorti introuvable à la vérif suivante a été rangé ailleurs. Les items en plus
(au-delà de l'attendu) qui restent dans les lots montrent des réassorts trop généreux.
"""

from __future__ import annotations

import statistics
from collections import Counter, defaultdict
from datetime import datetime, time, timedelta
from typing import TYPE_CHECKING, Any

from django.db.models import Count
from django.utils import timezone

from . import consumables, fun_stats, print_counts, purchases
from . import serializers as ser
from .idendity import SOURCE_VERIFIED
from .journal import pl, plural
from .models import (
    Checklist, ChecklistStatus, ItemMovement, Items, ItemStatus, Lots, MovementKind, Operation, OperationKind,
    ProblemReport, ReportResolution, ReportStatus, Secouristes, Verifs, VerifItem, VerifResult, qrprotec_setting,
)
from .reports import target_name
from .tasks import LIST_RETURN_DAYS

if TYPE_CHECKING:
    from collections.abc import Iterable
    from datetime import date

MONTHS = 12
TOP = 8  # lignes des tableaux
RESTOCK_WARN = 70  # score de réassort (%) : orange en dessous
RESTOCK_BAD = 50   # rouge en dessous
VALID_WARN = 90    # lots valides (%)
VALID_BAD = 70
SCORE_WARN = 75  # scores globaux de fonctionnement : orange en dessous
SCORE_BAD = 50   # rouge en dessous
REPORT_ANSWER_HOURS = 48  # signalement : premiere reponse du TECH attendue dans les 48h
SHORT_MONTHS = ['janv.', 'févr.', 'mars', 'avr.', 'mai', 'juin', 'juil.', 'août', 'sept.', 'oct.', 'nov.', 'déc.']
# couleurs des series (memes teintes sur le web et au poste)
BLUE = '#2463b3'
GREEN = '#2e9e4f'
ORANGE = '#e8890c'
RED = '#c62828'
GREY = '#8a94a6'
ACTIVE_DAYS = 30
EXPIRY_SOON_DAYS = 30
EXPIRY_LATER_DAYS = 90


# Ce que recouvre chaque genre d'operation du journal (info-bulle du tableau « Opérations les plus fréquentes »)
OPERATION_HELP = {
    OperationKind.VERIF: 'Vérif d\'un lot : ses items scannés et comparés à ce qui est attendu.',
    OperationKind.STOCK_VERIF: 'Vérif d\'un rangement du stock (ou du stock sans rangement).',
    OperationKind.RESTOCK: 'Items rangés dans un lot sans vérif complète (le lot passe en « vérif recommandée »).',
    OperationKind.REMOVE: 'Items sortis d\'un lot et remis dans le stock.',
    OperationKind.SEAL: 'Scellé posé sur un lot vérifié complet.',
    OperationKind.UNSEAL: 'Scellé d\'un lot ouvert (étiquette d\'ouverture, ajout ou retrait de matériel).',
    OperationKind.RECEPTION: 'Matériel reçu et entré dans le stock (nouvelles étiquettes d\'items).',
    OperationKind.ITEM_DELETE: 'Item supprimé de l\'inventaire (perdu, jeté hors vérif).',
    OperationKind.ITEM_RESTORE: 'Item supprimé par erreur remis dans l\'inventaire.',
    OperationKind.ITEM_DISABLE: 'Item désactivé (non conforme) : à remplacer à la prochaine vérif.',
    OperationKind.ITEM_ENABLE: 'Item désactivé remis en service.',
    OperationKind.ITEM_SPLIT: 'Item découpé en plusieurs (ex : une boîte en unités).',
    OperationKind.ITEM_OUT: 'Matériel sorti du stock sans lot (liste de matériel, usage direct).',
    OperationKind.ITEM_OUT_USED: 'Matériel sorti du stock qui n\'est pas revenu : compté comme utilisé.',
    OperationKind.PACK_OPEN: 'Paquet fermé ouvert : ses items deviennent des items étiquetés un par un.',
    OperationKind.PACK_CLOSE: 'Paquet refermé.',
    OperationKind.PACK_PRELABEL: 'Paquet fermé étiqueté à l\'avance.',
    OperationKind.LOT: 'Gestion des lots : création, modification, archivage, contenu propre d\'un lot.',
    OperationKind.LOT_KEY: 'Étiquette clé d\'un lot renouvelée (l\'ancienne ne marche plus).',
    OperationKind.CATALOG: 'Types d\'items et types de lots créés ou modifiés (contenu attendu, minimums...).',
    OperationKind.USER: 'Comptes des secouristes : création, droits, badge, code PIN, passkey.',
    OperationKind.SETTINGS: 'Réglages du serveur modifiés.',
    OperationKind.REPORT: 'Signalement créé, pris en charge ou résolu.',
    OperationKind.REVISION: 'Révision périodique de matériel faite.',
    OperationKind.DISINFECTION: 'Désinfection de matériel ou d\'un lot faite.',
    OperationKind.CHECKLIST: 'Liste de matériel créée, validée (sortie) ou rentrée.',
    OperationKind.LOT_USED: 'Lot marqué « utilisé » depuis sa fiche : vérif recommandée.',
    OperationKind.LOT_REPORT: 'Rapport d\'état d\'un lot imprimé (PDF).',
    OperationKind.ORDER: 'Commande saisie, modifiée, reçue ou annulée.',
    OperationKind.CANCEL: 'Opération annulée depuis le journal.',
    OperationKind.LOGIN: 'Connexion d\'un utilisateur.',
}


# ---- Aides --------------------------------------------------------------------------------------------------------

def tile(value: str | int, label: str, detail: str = '', level: str = '') -> dict[str, Any]:
    return {'value': str(value), 'label': label, 'detail': detail, 'level': level}


def line(text: str, level: str = '', emoji: str = '') -> dict[str, Any]:
    """emoji : a part du texte, le web le met devant ; le poste ne l'affiche pas (sa police n'en a pas)."""
    return {'text': text, 'level': level, 'emoji': emoji}


def section(key: str, emoji: str, title: str, help_text: str, link: str = '') -> dict[str, Any]:
    """link : ou aller plus loin ('lots', 'reports', 'lists', 'forecast', 'purchases', 'stats', 'consumables'...)."""
    return {'key': key, 'emoji': emoji, 'title': title, 'help': help_text, 'link': link, 'tiles': [], 'lines': [],
            'charts': [], 'tables': [], 'score': None}


def score(part: dict[str, Any], title: str, value: int | None, detail: str, parts: list[str] | None = None) -> None:
    """Score global de fonctionnement d'une activite (0 a 100, plus haut vaut mieux), repris en tete du tableau de
    bord. value None : pas encore assez de donnees, le score est affiche gris."""
    part['score'] = {'key': part['key'], 'emoji': part['emoji'], 'title': title, 'value': value, 'detail': detail,
                     'parts': parts or [], 'level': level_for(value, SCORE_WARN, SCORE_BAD)}


def percent(part: int, total: int) -> int | None:
    return round(part * 100 / total) if total else None


def level_for(value: int | None, warn: int, bad: int) -> str:
    """Niveau d'un pourcentage ou plus haut vaut mieux."""
    if value is None:
        return ''
    return 'bad' if value < bad else 'warn' if value < warn else 'ok'


def month_keys(today: date, count: int = MONTHS) -> list[str]:
    keys = []
    year, month = today.year, today.month
    for _ in range(count):
        keys.append(f'{year:04d}-{month:02d}')
        month -= 1
        if month == 0:
            year, month = year - 1, 12
    return list(reversed(keys))


def month_label(key: str) -> str:
    return f"{SHORT_MONTHS[int(key[5:7]) - 1]} {key[2:4]}"


def month_of(moment: datetime | date) -> str:
    local = timezone.localtime(moment) if isinstance(moment, datetime) else moment
    return f'{local.year:04d}-{local.month:02d}'


def by_month(keys: list[str], moments: Iterable[datetime | date]) -> list[int]:
    counts = Counter(month_of(moment) for moment in moments)
    return [counts.get(key, 0) for key in keys]


def chart(title: str, keys: list[str], series: list[tuple[str, list[int] | list[float], str]], unit: str = '',
          stacked: bool = False) -> dict[str, Any]:
    return {'title': title, 'unit': unit, 'labels': [month_label(key) for key in keys], 'stacked': stacked,
            'series': [{'name': name, 'values': values, 'color': color} for name, values, color in series]}


def duration_text(delta: timedelta) -> str:
    """« 3h », « 2 jours », « 5 semaines » : un délai lisible d'un coup d'œil."""
    hours = delta.total_seconds() / 3600
    if hours < 1:
        return f"{max(1, round(hours * 60))} min"
    if hours < 48:
        return f"{round(hours)}h"
    days = round(hours / 24)
    if days < 60:
        return plural(days, 'jour')
    return plural(round(days / 30), 'mois', 'mois')


def euros(value: float | None) -> str:
    return '–' if value is None else purchases.euros(value)


def lot_path(row: dict[str, Any]) -> str:
    return ' › '.join([part['name'] for part in row.get('path', [])] + [row['name']])


# ---- Sections -----------------------------------------------------------------------------------------------------

def _lot_rows(today: date) -> list[dict[str, Any]]:
    """Lots ordinaires actifs (comme les taches du TECH : ni rangements du stock, ni simples regroupements)."""
    lots = Lots.objects.select_related('lot_type').filter(active=True).order_by('lot_type__name', 'name')
    rows = ser.lot_list(lots, today=today)
    has_children = {row['parent'] for row in rows if row['parent']}
    return [row for row in rows if not row['storage']
            and not (not row['requirements'] and not row['item_count'] and row['id'] in has_children)]


def _lots_section(rows: list[dict[str, Any]], sessions: list[dict[str, Any]], keys: list[str],
                  today: date) -> dict[str, Any]:
    part = section('lots', '🚑', 'Lots et vérifs', 'État des lots aujourd\'hui, vérifs faites et lots qui attendent '
                   'leur vérif depuis longtemps.', 'lots')
    counted = [row for row in rows if not (row.get('optional') and not row['last_verif'])]
    kinds = Counter((row.get('effective') or row['state'])['kind'] for row in counted)
    valid = percent(kinds['ok'], len(counted))
    part['tiles'].append(tile(f"{valid} %" if valid is not None else '–', 'des lots valides',
                              f"{kinds['ok']} sur {len(counted)}", level_for(valid, VALID_WARN, VALID_BAD)))
    if kinds['bad']:
        part['tiles'].append(tile(kinds['bad'], pl(kinds['bad'], 'lot rouge', 'lots rouges'), 'à traiter', 'bad'))
    if kinds['warn']:
        part['tiles'].append(tile(kinds['warn'], pl(kinds['warn'], 'lot orange', 'lots orange'),
                                  'vérif recommandée', 'warn'))
    month_ago = timezone.now() - timedelta(days=30)
    recent = sum(1 for session in sessions if session['at'] >= month_ago and not session['stock'])
    part['tiles'].append(tile(recent, pl(recent, 'vérif', 'vérifs') + ' en 30 jours',
                              f"{sum(1 for session in sessions if not session['stock'])} sur 12 mois"))
    timed = [(session['end'] - session['started']).total_seconds() for session in sessions
             if session['started'] is not None and session['complete'] and not session['stock']
             and timedelta(0) < session['end'] - session['started'] <= fun_stats.MAX_DURATION]
    if timed:
        part['tiles'].append(tile(fun_stats.clock(statistics.median(timed), precise=False),
                                  'pour une vérif complète', f"médiane sur {plural(len(timed), 'vérif chronométrée', 'vérifs chronométrées')}"))
    team = sum(1 for session in sessions if len(session['people']) >= 2)
    if sessions and team:
        part['tiles'].append(tile(f"{percent(team, len(sessions))} %", 'des vérifs à plusieurs', plural(team, 'vérif')))

    # couverture : lots vérifiés (ou scellés) dans les ROUTINE_VERIF_DAYS derniers jours
    routine = qrprotec_setting('ROUTINE_VERIF_DAYS') or 90
    limit = today - timedelta(days=routine)
    late = []
    for row in counted:
        if row['is_sealed']:
            continue
        last = datetime.fromisoformat(row['last_verif']).date() if row['last_verif'] else None
        if last is None or last < limit:
            late.append((last, row))
    covered = percent(len(counted) - len(late), len(counted))
    if counted:
        part['lines'].append(line(
            f"{covered} % des lots vérifiés (ou scellés) dans les {routine} derniers jours"
            + (f" : {plural(len(late), 'lot')} à revoir." if late else '.'),
            'warn' if late else 'ok'))
    never = [row for last, row in late if last is None]
    if never:
        part['lines'].append(line(f"{plural(len(never), 'lot')} jamais {pl(len(never), 'vérifié')}.", 'bad'))
    late.sort(key=lambda pair: (pair[0] is not None, pair[0] or today))
    if late:
        part['tables'].append({'title': 'Lots qui attendent leur vérif depuis le plus longtemps',
                               'columns': ['Lot', 'Dernière vérif', 'Il y a'], 'rows': [
            {'cells': [lot_path(row), last.strftime('%d/%m/%Y') if last else 'jamais',
                       plural((today - last).days, 'jour') if last else '–'],
             'level': 'bad' if last is None else 'warn', 'lot': row['id']} for last, row in late[:TOP]]})
    lot_sessions = [session for session in sessions if not session['stock']]
    complete = percent(sum(1 for session in lot_sessions if session['complete']), len(lot_sessions))
    known = [value for value in (valid, covered, complete) if value is not None]
    score(part, 'Vérifs', round(sum(known) / len(known)) if known else None,
          'moyenne de trois parts : lots valides, lots vérifiés à temps, vérifs complètes',
          [f"{valid} % des lots valides aujourd'hui" if valid is not None else 'pas encore de lot',
           f"{covered} % des lots vérifiés (ou scellés) dans les {routine} jours" if covered is not None else '',
           f"{complete} % des vérifs complètes (12 mois)" if complete is not None else 'pas encore de vérif'])
    part['score']['parts'] = [text for text in part['score']['parts'] if text]
    per_month = by_month(keys, [session['at'] for session in sessions if not session['stock']])
    part['charts'].append(chart('Vérifs par mois (une vérif en cascade compte pour une)', keys,
                                [('Vérifs', per_month, BLUE)]))
    return part


def _restock_section(rows: list[dict[str, Any]], start: datetime, keys: list[str]) -> dict[str, Any]:
    part = section('restock', '🔁', 'Réassorts', 'Un réassort est utile si la vérif suivante du lot le trouve complet. '
                   'Sinon il manquait encore du matériel, ou le mauvais a été mis. Les items en plus qui restent dans '
                   'les lots montrent des réassorts trop généreux.', 'lots')
    operations = list(Operation.objects.filter(kind=OperationKind.RESTOCK, at__gte=start, cancelled_by__isnull=True,
                                               lot__isnull=False, lot__lot_type__storage=False)
                      .order_by('at').values('id', 'at', 'lot_id', 'lot_name', 'details'))
    lot_ids = {row['lot_id'] for row in operations}
    verifs: defaultdict[str, list[Verifs]] = defaultdict(list)
    for verif in Verifs.objects.filter(lot_id__in=lot_ids, datetime__gte=start).order_by('datetime', 'id'):
        verifs[verif.lot_id].append(verif)
    pending = 0
    results: list[tuple[dict[str, Any], Verifs]] = []
    for operation in operations:
        following = next((verif for verif in verifs[operation['lot_id']] if verif.datetime > operation['at']), None)
        if following is None:
            pending += 1
        else:
            results.append((operation, following))
    # items reassortis introuvables a la vérif suivante
    lost: Counter[int] = Counter()
    wanted = {(following.id, iid) for operation, following in results for iid in operation['details'].get('items', [])}
    if wanted:
        missing = set(VerifItem.objects.filter(verif_id__in={verif for verif, _ in wanted},
                                               result=VerifResult.MISSING)
                      .values_list('verif_id', 'item__iid'))
        for operation, following in results:
            lost[operation['id']] = sum(1 for iid in operation['details'].get('items', [])
                                        if (following.id, iid) in missing)
    useful = [operation for operation, following in results if following.complete]
    restock_score = percent(len(useful), len(results))
    level = level_for(restock_score, RESTOCK_WARN, RESTOCK_BAD)
    part['tiles'].append(tile(f"{restock_score} %" if restock_score is not None else '–', 'score de réassort',
                              f"{len(useful)} sur {len(results)} complets à la vérif suivante" if results
                              else 'pas encore de réassort suivi d\'une vérif', level))
    part['tiles'].append(tile(len(operations), pl(len(operations), 'réassort') + ' sur 12 mois',
                              plural(sum(int(row['details'].get('count') or 0) for row in operations), 'item')
                              + (' rangés' if sum(int(row['details'].get('count') or 0) for row in operations) > 1
                                 else ' rangé')))
    if pending:
        part['tiles'].append(tile(pending, 'en attente de vérif', 'réassorts pas encore contrôlés'))
    lost_total = sum(lost.values())
    if lost_total:
        part['tiles'].append(tile(lost_total, pl(lost_total, 'item introuvable', 'items introuvables'),
                                  'réassortis puis absents à la vérif suivante', 'warn'))
    lost_share = percent(lost_total, sum(len(operation['details'].get('items', [])) for operation, _ in results))
    # score global : reassorts complets ensuite, moins la part d'items reassortis introuvables
    score(part, 'Réassorts', None if restock_score is None else max(0, restock_score - (lost_share or 0)),
          'réassorts qui laissent le lot complet à la vérif suivante, moins les items réassortis introuvables',
          [f"{restock_score} % des réassorts complets à la vérif suivante" if restock_score is not None
           else 'pas encore de réassort suivi d\'une vérif']
          + ([f"{lost_share} % des items réassortis introuvables ensuite"] if lost_share else []))
    if restock_score is not None:
        failed = len(results) - len(useful)
        if failed:
            part['lines'].append(line(
                f"{plural(failed, 'réassort')} sur {len(results)} {pl(failed, 'a', 'ont')} laissé le lot incomplet à "
                f"la vérif suivante : il manquait encore du matériel après le réassort.",
                'bad' if level == 'bad' else 'warn'))
        else:
            part['lines'].append(line('Tous les réassorts contrôlés ont laissé un lot complet à la vérif suivante.', 'ok'))
    if lost_total:
        part['lines'].append(line(f"{plural(lost_total, 'item réassorti', 'items réassortis')} {pl(lost_total, 'introuvable', 'introuvables')} "
                                  f"à la vérif suivante : rangés dans un autre lot, ou utilisés entre-temps.", 'warn'))

    # par mois : utiles / incomplets, du mois du reassort
    ok_month = by_month(keys, [operation['at'] for operation in useful])
    all_month = by_month(keys, [operation['at'] for operation, _ in results])
    part['charts'].append(chart('Réassorts contrôlés par la vérif suivante, par mois', keys, [
        ('Complets ensuite', ok_month, GREEN),
        ('Incomplets ensuite', [total - ok for total, ok in zip(all_month, ok_month)], ORANGE)], stacked=True))

    # lots ou les reassorts ratent le plus
    per_lot: defaultdict[str, list[int]] = defaultdict(lambda: [0, 0, 0])  # reassorts, incomplets, introuvables
    names: dict[str, str] = {}
    for operation, following in results:
        stats = per_lot[operation['lot_id']]
        stats[0] += 1
        stats[1] += 0 if following.complete else 1
        stats[2] += lost[operation['id']]
        names[operation['lot_id']] = operation['lot_name']
    worst = sorted(((lot, stats) for lot, stats in per_lot.items() if stats[1] or stats[2]),
                   key=lambda pair: (-pair[1][1], -pair[1][2], names[pair[0]]))
    if worst:
        part['tables'].append({'title': 'Lots où les réassorts suffisent le moins', 'columns': [
            'Lot', 'Réassorts', 'Incomplets ensuite', 'Items introuvables'], 'rows': [
            {'cells': [names[lot], stats[0], stats[1], stats[2]], 'level': 'warn', 'lot': lot}
            for lot, stats in worst[:TOP]]})

    # items en plus qui restent dans les lots
    extras = _extras(rows)
    total = sum(count for count, _, _ in extras)
    if total:
        part['tiles'].append(tile(total, pl(total, 'item en plus', 'items en plus'),
                                  f"dans {plural(len(extras), 'lot')}, au-delà de l'attendu", 'warn' if total >= 10 else ''))
        trend = _present_trend([row['id'] for _, row, _ in extras])
        part['tables'].append({'title': 'Items en plus qui s\'accumulent dans les lots', 'columns': [
            'Lot', 'En plus', 'Surtout', 'Présents aux vérifs (12 mois)'], 'rows': [
            {'cells': [lot_path(row), count, detail, trend.get(row['id'], '–')], 'level': '', 'lot': row['id']}
            for count, row, detail in extras[:TOP]]})
    return part


def _extras(rows: list[dict[str, Any]]) -> list[tuple[int, dict[str, Any], str]]:
    """Items au-delà de l'attendu dans chaque lot (comme le « + 2 » bleu des jauges) et types pas attendus du tout :
    [(nombre, ligne du lot, types les plus en trop)], le plus chargé d'abord."""
    expected_types: dict[str, set[str]] = {}
    over: dict[str, Counter[str]] = {}
    names: dict[str, str] = {}
    for row in rows:
        expected_types[row['id']] = set()
        over[row['id']] = Counter()
        for requirement in row['requirements']:
            expected_types[row['id']].add(requirement['type'])
            expected_types[row['id']].update(requirement.get('fills', {}))
            names[requirement['type']] = requirement['type_name']
            if requirement.get('check') or requirement.get('is_group') or requirement.get('within'):
                continue  # items a cocher, quantites en unites : pas comptes en items
            extra = requirement['present'] + requirement['expired'] - requirement['expected']
            if extra > 0:
                over[row['id']][requirement['type']] += extra
    strays = (Items.objects.filter(location_id__in=list(expected_types), status=ItemStatus.ACTIVE,
                                   deleted__isnull=True)
              .values('location_id', 'pack__item_type_id', 'pack__item_type__name')
              .annotate(count=Count('iid')))
    for stray in strays:
        if stray['pack__item_type_id'] not in expected_types[stray['location_id']]:
            over[stray['location_id']][stray['pack__item_type_id']] += stray['count']
            names[stray['pack__item_type_id']] = stray['pack__item_type__name']
    result = []
    by_id = {row['id']: row for row in rows}
    for lot_id, counter in over.items():
        if not counter:
            continue
        detail = ', '.join(f"{count} × {names.get(code, code)}" for code, count in counter.most_common(3))
        result.append((sum(counter.values()), by_id[lot_id], detail))
    result.sort(key=lambda entry: (-entry[0], entry[1]['name']))
    return result


def _present_trend(lot_ids: list[str]) -> dict[str, str]:
    """Items présents à la première et à la dernière vérif complète de l'année : « 42 → 47 »."""
    year_ago = timezone.now() - timedelta(days=365)
    first: dict[str, int] = {}
    last: dict[str, int] = {}
    for lot_id, present in (Verifs.objects.filter(lot_id__in=lot_ids, complete=True, datetime__gte=year_ago)
                            .order_by('datetime', 'id').values_list('lot_id', 'present_count')):
        first.setdefault(lot_id, present)
        last[lot_id] = present
    return {lot_id: f"{first[lot_id]} → {last[lot_id]}" for lot_id in first if first[lot_id] != last[lot_id]}


def _consumption_section(start: datetime, keys: list[str], today: date, stock: dict[str, Any] | None
                         ) -> dict[str, Any]:
    part = section('consumption', '📦', 'Consommation et péremptions', 'Ce qui sort des lots (utilisé) et ce qui est '
                   'jeté périmé : l\'objectif est de ne rien jeter.', 'forecast')
    moves = list(ItemMovement.objects.filter(at__gte=start, cancelled__isnull=True, kind__in=(
        MovementKind.USED, MovementKind.DISCARDED, MovementKind.REPLACED))
        .values_list('kind', 'at', 'item_type_id', 'item_type__name'))
    used = [row for row in moves if row[0] == MovementKind.USED]
    thrown = [row for row in moves if row[0] != MovementKind.USED]
    waste = percent(len(thrown), len(moves))
    score(part, 'Péremptions', None if waste is None else 100 - waste,
          'part de ce qui sort des lots qui est utilisé plutôt que jeté périmé',
          [f"{len(used)} utilisés, {len(thrown)} jetés périmés sur 12 mois"] if moves else ['pas encore de sortie'])
    part['tiles'].append(tile(len(used), pl(len(used), 'item utilisé', 'items utilisés'), 'sur 12 mois'))
    part['tiles'].append(tile(len(thrown), pl(len(thrown), 'item jeté périmé', 'items jetés périmés'),
                              f"{waste} % de ce qui est sorti" if waste is not None else '',
                              'warn' if thrown else 'ok'))
    if stock and stock.get('discarded_year'):
        part['tiles'].append(tile(euros(stock['discarded_year']), 'jetés périmés', 'au prix d\'achat, sur 12 mois',
                                  'warn'))
    items = Items.objects.filter(status=ItemStatus.ACTIVE, deleted__isnull=True, pack__peremption__isnull=False)
    soon = items.filter(pack__peremption__gte=today, pack__peremption__lte=today + timedelta(days=EXPIRY_SOON_DAYS)).count()
    later = items.filter(pack__peremption__gte=today,
                         pack__peremption__lte=today + timedelta(days=EXPIRY_LATER_DAYS)).count()
    expired = items.filter(pack__peremption__lt=today).count()
    part['tiles'].append(tile(soon, pl(soon, 'item périmé', 'items périmés') + f" dans {EXPIRY_SOON_DAYS} jours",
                              f"{later} dans {EXPIRY_LATER_DAYS} jours", 'warn' if soon else ''))
    if expired:
        part['lines'].append(line(f"{plural(expired, 'item')} déjà {pl(expired, 'périmé')} encore en place "
                                  f"(lots ou stock) : à jeter et remplacer.", 'bad'))
    if thrown:
        part['lines'].append(line(f"{plural(len(thrown), 'item')} {pl(len(thrown), 'jeté')} {pl(len(thrown), 'périmé')} "
                                  f"en 12 mois : les Prévisions proposent des échanges entre lots pour utiliser plus tôt "
                                  f"ce qui va périmer.", 'warn'))
    part['charts'].append(chart('Sorties par mois', keys, [
        ('Utilisés', by_month(keys, [row[1] for row in used]), BLUE),
        ('Jetés périmés', by_month(keys, [row[1] for row in thrown]), RED)], stacked=True))
    per_type: defaultdict[str, list[Any]] = defaultdict(lambda: ['', 0, 0])
    for kind, _, code, name in moves:
        per_type[code][0] = name
        per_type[code][1 if kind == MovementKind.USED else 2] += 1
    top = sorted(per_type.values(), key=lambda row: (-(row[1] + row[2]), row[0]))[:TOP]
    if top:
        part['tables'].append({'title': 'Matériel le plus consommé (12 mois)', 'columns': [
            'Type', 'Utilisés', 'Jetés périmés', 'Par mois'], 'rows': [
            {'cells': [name, used_count, thrown_count, str(round((used_count + thrown_count) / MONTHS, 1)).replace('.', ',')],
             'level': 'warn' if thrown_count > used_count else '', 'lot': ''} for name, used_count, thrown_count in top]})
    return part


def _spending_section(today: date) -> tuple[dict[str, Any], dict[str, Any] | None]:
    part = section('spending', '💶', 'Dépenses et commandes', 'Commandes fournisseurs saisies au poste (Réception > '
                   'Commandes fournisseurs) : dépenses, livraisons, fournisseurs, valeur du matériel.', 'purchases')
    stats = purchases.stats(today)
    year = stats['year']
    if not year['orders'] and not stats['suppliers']:
        part['lines'].append(line('Pas encore de commande saisie : saisissez-les au poste (Réception > Commandes '
                                  'fournisseurs) pour suivre les dépenses et les délais ici.'))
        return part, stats['stock']
    part['tiles'].append(tile(euros(year['spent']), 'dépensés sur 12 mois',
                              f"≈ {euros(year['monthly_average'])} / mois, {plural(year['orders'], 'commande')}"))
    if year['shipping']:
        part['tiles'].append(tile(euros(year['shipping']), 'de livraison',
                                  f"{str(year['shipping_share']).replace('.', ',')} % des dépenses"
                                  if year['shipping_share'] is not None else ''))
    open_rows = purchases.orders_list(only_open=True, today=today)
    late = sum(1 for row in open_rows if row['state'] == 'late')
    part['tiles'].append(tile(len(open_rows), pl(len(open_rows), 'commande en cours', 'commandes en cours'),
                              plural(late, 'en retard', 'en retard') if late else 'aucune en retard',
                              'warn' if late else ''))
    if late:
        part['lines'].append(line(f"{plural(late, 'commande')} en retard de livraison.", 'warn'))
    stock = stats['stock']
    if stock['value']:
        part['tiles'].append(tile(euros(stock['value']), 'de matériel en place',
                                  f"stock {euros(stock['value_stock'])}, lots {euros(stock['value_lots'])}"))
    delay = stats['delay']
    if delay['median'] is not None:
        part['tiles'].append(tile(plural(delay['median'], 'jour ouvré', 'jours ouvrés'), 'de délai de livraison',
                                  f"médian, sur {plural(delay['samples'], 'commande')}"))
    for key in ('next_text', 'yearly_text', 'savings_text'):
        if stats['estimates'][key]:
            part['lines'].append(line(stats['estimates'][key]))
    if stock.get('waste_text'):
        part['lines'].append(line(stock['waste_text'], 'warn'))
    keys = [row['month'] for row in stats['months']]
    part['charts'].append(chart('Dépenses par mois (date de commande)', keys, [
        ('Articles', [row['items'] or 0 for row in stats['months']], BLUE),
        ('Livraison', [row['shipping'] or 0 for row in stats['months']], ORANGE)], unit='€', stacked=True))
    if stats['suppliers']:
        part['tables'].append({'title': 'Fournisseurs', 'columns': [
            'Fournisseur', 'Dépensé (12 mois)', 'Commandes', 'Délai médian'], 'rows': [
            {'cells': [row['name'], euros(row['spent_year']), row['orders_year'],
                       plural(row['delay_median'], 'jour') if row['delay_median'] is not None else '–'],
             'level': '', 'lot': ''} for row in stats['suppliers'][:TOP]]})
    return part, stock


def _reports_section(start: datetime, keys: list[str]) -> dict[str, Any]:
    part = section('reports', '⚠️', 'Signalements', 'Problèmes signalés par les secouristes : combien, et en combien '
                   'de temps ils sont réglés.', 'reports')
    now = timezone.now()
    open_reports = list(ProblemReport.objects.exclude(status=ReportStatus.RESOLVED).order_by('created'))
    new = sum(1 for report in open_reports if report.status == ReportStatus.NEW)
    part['tiles'].append(tile(len(open_reports), pl(len(open_reports), 'signalement ouvert', 'signalements ouverts'),
                              f"{plural(new, 'nouveau', 'nouveaux')} à prendre en charge" if new else 'tous pris en charge',
                              'bad' if new else 'warn' if open_reports else 'ok'))
    year = list(ProblemReport.objects.select_related('item_type').filter(created__gte=start))
    resolved = [report for report in year if report.resolved]
    part['tiles'].append(tile(len(year), pl(len(year), 'signalement') + ' sur 12 mois',
                              plural(len(resolved), 'résolu', 'résolus')))
    if resolved:
        delays = [(report.resolved - report.created).total_seconds() for report in resolved]
        part['tiles'].append(tile(duration_text(timedelta(seconds=statistics.median(delays))), 'pour résoudre',
                                  'médiane'))
    answerable = [report for report in year if report.updated or now - report.created > timedelta(hours=REPORT_ANSWER_HOURS)]
    quick = sum(1 for report in answerable
                if report.updated and report.updated - report.created <= timedelta(hours=REPORT_ANSWER_HOURS))
    fast = percent(quick, len(answerable))
    score(part, 'Signalements', fast, f"signalements qui ont eu une réponse du TECH en moins de {REPORT_ANSWER_HOURS}h",
          [f"{quick} sur {len(answerable)} {pl(len(answerable), 'signalement')} en moins de {REPORT_ANSWER_HOURS}h"]
          if answerable else ['pas encore de signalement'])
    taken = [report for report in year if report.updated]
    if taken:
        waits = [(report.updated - report.created).total_seconds() for report in taken]
        part['tiles'].append(tile(duration_text(timedelta(seconds=statistics.median(waits))), 'avant une réponse',
                                  'médiane, première réponse du TECH'))
    if open_reports:
        oldest = open_reports[0]
        age = now - oldest.created
        part['lines'].append(line(f"Le plus ancien ouvert : {target_name(oldest)}, depuis {duration_text(age)}.",
                                  'bad' if age > timedelta(days=30) else 'warn'))
    part['charts'].append(chart('Signalements par mois', keys, [
        ('Signalés', by_month(keys, [report.created for report in year]), ORANGE),
        ('Résolus', by_month(keys, [report.resolved for report in resolved if report.resolved]), GREEN)]))
    # conclusion des signalements resolus : remis en service, desactive, retire de l'inventaire, ou sans suite (lot)
    if resolved:
        outcomes = Counter(report.resolution for report in resolved)
        names = {ReportResolution.OK: 'Remis en service (utilisable)', ReportResolution.DISABLED: 'Désactivé (à remplacer)',
                 ReportResolution.REMOVED: 'Retiré de l\'inventaire', ReportResolution.NONE: 'Réglé sans retrait (lot, autre)'}
        part['tables'].append({'title': 'Conclusion des signalements résolus (12 mois)', 'columns': [
            'Conclusion', 'Signalements', 'Part'], 'rows': [
            {'cells': [names[code], outcomes[code], f"{percent(outcomes[code], len(resolved))} %"],
             'level': 'warn' if code in (ReportResolution.DISABLED, ReportResolution.REMOVED) else '', 'lot': ''}
            for code in (ReportResolution.REMOVED, ReportResolution.DISABLED, ReportResolution.OK, ReportResolution.NONE)
            if outcomes[code]]})
        withdrawn = [report for report in resolved
                     if report.resolution in (ReportResolution.DISABLED, ReportResolution.REMOVED)]
        if withdrawn:
            part['tiles'].append(tile(len(withdrawn), pl(len(withdrawn), 'objet retiré', 'objets retirés'),
                                      f"désactivés ou retirés de l'inventaire, sur {plural(len(resolved), 'résolu', 'résolus')}"))
            types = Counter(report.item_type.name if report.item_type_id else (report.lot_name or 'Lot')
                            for report in withdrawn)
            part['tables'].append({'title': 'Matériel retiré après un signalement (12 mois)', 'columns': [
                'Matériel', 'Désactivé ou retiré'], 'rows': [
                {'cells': [name, count], 'level': '', 'lot': ''} for name, count in types.most_common(TOP)]})
    most = Counter(target_name(report) for report in year)
    repeated = [(name, count) for name, count in most.most_common(TOP) if count >= 2]
    if repeated:
        part['tables'].append({'title': 'Signalé plusieurs fois (12 mois)', 'columns': ['Objet', 'Signalements'],
                               'rows': [{'cells': [name, count], 'level': '', 'lot': ''} for name, count in repeated]})
    return part


def _lists_section(start: datetime, keys: list[str]) -> dict[str, Any]:
    part = section('lists', '🎒', 'Listes et départs', 'Listes de matériel sorties pour des postes de secours, lots '
                   'marqués utilisés.', 'lists')
    lists = list(Checklist.objects.filter(is_template=False, validated__gte=start).values_list(
        'validated', 'returned', 'status'))
    part['tiles'].append(tile(len(lists), pl(len(lists), 'liste sortie', 'listes sorties'), 'sur 12 mois'))
    since = timezone.now() - timedelta(days=LIST_RETURN_DAYS)
    out = Checklist.objects.filter(is_template=False, status=ChecklistStatus.VALIDATED, validated__lt=since).count()
    if out:
        part['tiles'].append(tile(out, pl(out, 'liste pas rentrée', 'listes pas rentrées'),
                                  f"sorties depuis plus de {plural(LIST_RETURN_DAYS, 'jour')}", 'warn'))
        part['lines'].append(line(f"{plural(out, 'liste')} {pl(out, 'sortie')} depuis plus de "
                                  f"{plural(LIST_RETURN_DAYS, 'jour')} sans retour : le matériel n'est pas revenu.", 'warn'))
    used = list(Operation.objects.filter(kind=OperationKind.LOT_USED, at__gte=start, cancelled_by__isnull=True)
                .values_list('at', flat=True))
    if used:
        part['tiles'].append(tile(len(used), pl(len(used), 'lot marqué utilisé', 'lots marqués utilisés'),
                                  'bouton de la fiche'))
    due = [row for row in lists if row[0] < since]
    back = sum(1 for row in due if row[1] is not None and row[1] - row[0] <= timedelta(days=LIST_RETURN_DAYS))
    returned = percent(back, len(due))
    score(part, 'Listes', returned, f"listes sorties rentrées (retour scanné) dans les {plural(LIST_RETURN_DAYS, 'jour')}",
          [f"{back} sur {len(due)} {pl(len(due), 'liste')} {pl(back, 'rentrée')} à temps"] if due
          else ['pas encore de liste sortie'])
    templates = Checklist.objects.filter(is_template=True).count()
    if templates:
        part['tiles'].append(tile(templates, pl(templates, 'modèle de liste', 'modèles de liste')))
    part['charts'].append(chart('Listes sorties par mois', keys, [
        ('Listes', by_month(keys, [row[0] for row in lists]), BLUE),
        ('Lots marqués utilisés', by_month(keys, used), ORANGE)]))
    return part


def _labels_section(today: date) -> dict[str, Any]:
    part = section('labels', '🏷️', 'Étiquettes et consommables', 'Étiquettes imprimées et stock de rouleaux et '
                   'rubans de l\'imprimante.', 'consumables')
    counts = print_counts.summary(today.year)
    month = counts['months_total'][today.month - 1]
    part['tiles'].append(tile(counts['total'], pl(counts['total'], 'étiquette imprimée', 'étiquettes imprimées')
                              + f" en {today.year}", f"{month} ce mois-ci"))
    if counts['lost_total']:
        part['tiles'].append(tile(counts['lost_total'], pl(counts['lost_total'], 'étiquette perdue', 'étiquettes perdues')
                                  + f" en {today.year}", 'calibrage, ratées : sorties du rouleau sans impression',
                                  'warn' if counts['lost_total'] * 10 > counts['total'] else ''))
    pools = consumables.pools(today)
    cost = sum(pool['year_cost'] or 0 for pool in pools)
    if cost:
        part['tiles'].append(tile(euros(cost), 'de consommables', f"en {today.year}"))
    level_of = {'bad': 'bad', 'warn': 'warn', 'ok': 'ok', 'unknown': ''}
    for pool in pools:
        part['lines'].append(line(f"{pool['label']} : {pool['state']}"
                                  + (f" ({pool['stock_text']})" if pool['stock_text'] and pool['level'] != 'ok' else ''),
                                  level_of[pool['level']]))
    if not pools:
        part['lines'].append(line('Aucun rouleau ni ruban enregistré : Étiquettes > Consommables au poste.'))
    if counts['categories']:
        part['tables'].append({'title': f"Étiquettes imprimées en {today.year}", 'columns': ['Étiquette', 'Imprimées'],
                               'rows': [{'cells': [row['label'], row['total']], 'level': '', 'lot': ''}
                                        for row in [row for row in counts['categories'] if row['total']][:TOP]]})
    return part


def _software_section(start: datetime, sessions: list[dict[str, Any]], keys: list[str]) -> dict[str, Any]:
    part = section('software', '💻', 'Utilisation du logiciel', 'Qui se sert de QRProtec, et comment : connexions, vérifs '
                   'hors connexion, annulations.')
    now = timezone.now()
    month_ago = now - timedelta(days=ACTIVE_DAYS)
    people = {by for by in Operation.objects.filter(at__gte=month_ago, by__startswith=SOURCE_VERIFIED + ':')
              .values_list('by', flat=True).distinct()}
    for session in sessions:
        if session['at'] >= month_ago:
            people.update(session['people'])
    team = Secouristes.objects.filter(active=True, guest=False).count()
    part['tiles'].append(tile(len(people), pl(len(people), 'secouriste actif', 'secouristes actifs'),
                              f"en {ACTIVE_DAYS} jours, sur {plural(team, 'compte')}"))
    year_people: set[str] = set()
    for session in sessions:
        year_people.update(session['people'])
    part['tiles'].append(tile(len(year_people), pl(len(year_people), 'secouriste', 'secouristes') + ' en vérif',
                              'sur 12 mois'))
    logins = list(Operation.objects.filter(kind=OperationKind.LOGIN, at__gte=start).values_list('at', flat=True))
    if logins:
        part['tiles'].append(tile(sum(1 for at in logins if at >= month_ago), 'connexions en 30 jours',
                                  f"{len(logins)} sur 12 mois"))
    verifs = list(Verifs.objects.filter(datetime__gte=start).only('datetime', 'received'))
    offline = sum(1 for verif in verifs if verif.sent_late())
    if offline:
        part['tiles'].append(tile(offline, pl(offline, 'vérif hors connexion', 'vérifs hors connexion'),
                                  'envoyées plus tard'))
    cancelled = Operation.objects.filter(kind=OperationKind.CANCEL, at__gte=start).count()
    total_ops = Operation.objects.filter(at__gte=start).exclude(kind__in=(OperationKind.CANCEL, OperationKind.LOGIN)).count()
    part['tiles'].append(tile(total_ops, pl(total_ops, 'opération', 'opérations') + ' au journal',
                              f"{plural(cancelled, 'annulée', 'annulées')}" if cancelled else 'sur 12 mois'))
    kinds = Counter(Operation.objects.filter(at__gte=start).exclude(kind__in=(OperationKind.CANCEL, OperationKind.LOGIN))
                    .values_list('kind', flat=True))
    labels = dict(OperationKind.choices)
    if kinds:
        part['tables'].append({'title': 'Opérations les plus fréquentes (12 mois)', 'columns': ['Opération', 'Nombre'],
                               'rows': [{'cells': [labels.get(kind, kind), count], 'level': '', 'lot': '',
                                         'help': OPERATION_HELP.get(kind, '')}
                                        for kind, count in kinds.most_common(TOP)]})
    part['charts'].append(chart('Opérations par mois (journal)', keys, [
        ('Opérations', by_month(keys, Operation.objects.filter(at__gte=start).exclude(
            kind__in=(OperationKind.CANCEL, OperationKind.LOGIN)).values_list('at', flat=True)), GREY)]))
    return part


def _fun_section() -> dict[str, Any]:
    part = section('fun', '🎉', 'Fun facts', 'Les chiffres et records de l\'équipe sur 12 mois. '
                   'La page complète les présente en grand, une carte à la fois.', 'stats')
    data = fun_stats.compute('last12')
    totals = data['totals']
    for value, one, many in ((totals['verifs'], 'vérif', 'vérifs'), (totals['scanned'], 'item scanné', 'items scannés'),
                             (totals['expired'], 'périmé débusqué', 'périmés débusqués'),
                             (totals['team_verifs'], 'vérif à plusieurs', 'vérifs à plusieurs')):
        if value:
            part['tiles'].append(tile(value, pl(value, one, many)))
    for row in data['time_totals'][:2]:
        part['tiles'].append(tile(row['value'], row['label']))
    for award in data['awards']:
        if award['podium']:
            first = award['podium'][0]
            value = f"{first['value']} {first['unit']}".strip()
            part['lines'].append(line(f"{award['title']} : {first['name']} ({value})", emoji=award['emoji']))
        elif award['record']:
            record = award['record']
            part['lines'].append(line(f"{award['title']} : {record['name']} ({record['value']})",
                                     emoji=award['emoji']))
    if not data['awards']:
        part['lines'].append(line('Pas encore assez de vérifs sur les 12 derniers mois.'))
    return part


def compute(today: date | None = None) -> dict[str, Any]:
    """Tableau de bord complet (12 derniers mois)."""
    today = today or timezone.localdate()
    tz = timezone.get_current_timezone()
    start = datetime.combine(today - timedelta(days=364), time(), tzinfo=tz)
    keys = month_keys(today)
    start = min(start, datetime.combine(datetime.fromisoformat(keys[0] + '-01').date(), time(), tzinfo=tz))
    parents = dict(Lots.objects.values_list('id', 'parent_id'))
    verifs = list(Verifs.objects.select_related('lot').filter(datetime__gte=start).order_by('datetime', 'id'))
    sessions = fun_stats._sessions(verifs, parents)
    rows = _lot_rows(today)
    spending, stock = _spending_section(today)
    sections = [
        _lots_section(rows, sessions, keys, today),
        _restock_section(rows, start, keys),
        _consumption_section(start, keys, today, stock),
        spending,
        _reports_section(start, keys),
        _lists_section(start, keys),
        _labels_section(today),
        _software_section(start, sessions, keys),
        _fun_section(),
    ]
    rank = {'bad': 0, 'warn': 1}
    alerts = [{'text': row['text'], 'level': row['level'], 'section': part['key'], 'emoji': part['emoji']}
              for part in sections for row in part['lines'] if row['level'] in rank]
    alerts.sort(key=lambda row: rank[row['level']])  # tri stable : l'ordre des sections ensuite
    scores = [part['score'] for part in sections if part['score'] is not None]
    known = [row['value'] for row in scores if row['value'] is not None]
    overall = round(sum(known) / len(known)) if known else None
    return {'today': today.isoformat(), 'start': timezone.localtime(start).date().isoformat(), 'alerts': alerts,
            'scores': scores, 'overall': {'value': overall, 'level': level_for(overall, SCORE_WARN, SCORE_BAD),
                                          'detail': 'moyenne des scores de chaque activité'},
            'sections': sections}
