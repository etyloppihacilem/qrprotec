# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          fun_stats.py
#       -\-    _|__
#        |\___/  . \        Created on 17 Nov. 2026 at 23:40
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Stats fun de l'annee (front web, gestion et admin) : page cachee derriere 5 appuis sur « Prévisions » de la Gestion
de la TECH, a presenter a l'AG. Podiums des secouristes (vérifs, périmés trouvés, duos...), records (vérif la plus
tardive, périmé le plus ancien...) et chiffres de l'annee.

Une « vérif » compte comme une seule session : les etapes d'une vérif en cascade (meme personne, meme lot racine, moins
de 30 min entre deux etapes) et une vérif groupee d'un lot et de ses sous-lots ne font qu'une, comme dans « Vérifs
récentes ». Chaque participant d'une vérif à plusieurs la compte. Seuls les secouristes connectes (badge, nom + PIN)
entrent dans les podiums : ni noms declares ni invites. Noms courts seulement (prenom, jamais le nom de famille).

Durees (Verifs.started et VerifItem.scanned_at, enregistres depuis la migration 0080) : temps passe en vérif, vitesse
de scan, vérifs les plus rapides et les plus lentes, par lot au choix dans la page. Seules les sessions dont chaque
etape a son heure de debut comptent (les vérifs d'avant ces champs n'ont pas de duree), et pas celles de plus de
MAX_DURATION (vérif oubliee ouverte).
"""

from __future__ import annotations

from collections import Counter, defaultdict
from datetime import datetime, time, timedelta
from itertools import combinations
from typing import TYPE_CHECKING, Any
from collections.abc import Callable, Mapping

from django.db.models import Q
from django.utils import timezone

from .idendity import SOURCE_VERIFIED, anonymized, identity_name
from .models import (Checklist, ItemMovement, Lots, MovementKind, Operation, OperationKind, ProblemReport,
                     Secouristes, Verifs, VerifItem, VerifResult)
from .services import VerifTitles

if TYPE_CHECKING:
    from datetime import date

CASCADE_GAP = timedelta(minutes=30)  # comme views.public.CASCADE_GAP (Vérifs récentes)
PODIUM = 3
WEEKDAYS = ['lundi', 'mardi', 'mercredi', 'jeudi', 'vendredi', 'samedi', 'dimanche']
MONTHS = ['janvier', 'février', 'mars', 'avril', 'mai', 'juin', 'juillet', 'août', 'septembre', 'octobre',
          'novembre', 'décembre']
# heure « de nuit » : une vérif a 1h du matin est plus tardive qu'une a 23h (journee comptee a partir de 5h)
DAY_START = 5
# durees : au-dela, une vérif restee ouverte (oubliee) plutot qu'une vraie duree, ecartee des stats de duree
MAX_DURATION = timedelta(hours=3)
# vitesse de scan d'un secouriste : sur ses vérifs seul (a plusieurs, les scans des autres comptent), pas avant
# SPEED_MIN_VERIFS vérifs et SPEED_MIN_ITEMS items
SPEED_MIN_VERIFS = 3
SPEED_MIN_ITEMS = 30
# vérif la plus rapide de l'annee (tous lots) : pas un lot de 2 items
QUICKEST_MIN_ITEMS = 10
BURST_WINDOW = timedelta(minutes=1)


def period(value: str | int | None = None, today: date | None = None) -> tuple[datetime, datetime, str]:
    """(debut, fin, libelle) : 12 derniers mois par defaut, ou une annee civile (« 2025 »)."""
    today = today or timezone.localdate()
    tz = timezone.get_current_timezone()
    if value not in (None, '', 'last12'):
        year = int(value)
        start = datetime(year, 1, 1, tzinfo=tz)
        end = datetime(year + 1, 1, 1, tzinfo=tz)
        return start, min(end, timezone.now()), str(year)
    start = datetime.combine(today - timedelta(days=364), time(), tzinfo=tz)
    return start, timezone.now(), '12 derniers mois'


def years() -> list[int]:
    """Annees civiles avec au moins une vérif, la plus recente d'abord."""
    first = Verifs.objects.order_by('datetime').values_list('datetime', flat=True).first()
    if first is None:
        return []
    return list(range(timezone.localdate().year, timezone.localtime(first).year - 1, -1))


def _person(identity: str) -> str | None:
    """Cle d'un secouriste connecte (M:matricule), None pour un nom declare ou un invite."""
    return identity if identity and identity.startswith(SOURCE_VERIFIED + ':') else None


def _sessions(verifs: list[Verifs], parents: dict[str, str | None]) -> list[dict[str, Any]]:
    """Vérifs reunies en sessions : [{at, by, people, lots, root, present, expired, missing, complete, ids, started,
    end, rows}]. started : debut de la premiere etape (None si une etape n'a pas d'heure de debut : pas de duree),
    end : fin de la derniere ; rows : lignes services.VerifTitles des lots verifies (nom de la vérif)."""
    def root(lot_id: str) -> str:
        seen = set()
        while parents.get(lot_id) and lot_id not in seen:
            seen.add(lot_id)
            lot_id = parents[lot_id]
        return lot_id

    sessions = []
    open_by: dict[tuple[str, str], dict[str, Any]] = {}  # (by, racine) -> derniere session, pour rattacher les etapes d'une cascade
    for verif in verifs:  # par date croissante
        key = (verif.by, root(verif.lot_id) if verif.lot_id else None)
        last = open_by.get(key)
        if (last is None or key[1] is None or verif.datetime - last['_latest'] > CASCADE_GAP
                or (verif.lot_id in last['lots'] and verif.datetime != last['_latest'])):
            last = {'at': verif.datetime, 'by': verif.by, 'people': set(), 'lots': set(), 'root': key[1],
                    'present': 0, 'missing': 0, 'complete': True, 'ids': [], 'stock': verif.lot_id is None,
                    'started': verif.started, 'end': verif.datetime, 'rows': {}}
            sessions.append(last)
            open_by[key] = last
        last['_latest'] = verif.datetime
        last['ids'].append(verif.id)
        # cascade : chaque etape commence a la fin de la precedente, la session du debut de la 1re a la fin de la
        # derniere ; une etape sans heure de debut (ancien front) et la session n'a pas de duree
        if last['started'] is not None:
            last['started'] = None if verif.started is None else min(last['started'], verif.started)
        last['end'] = verif.datetime
        if verif.lot_id:
            last['lots'].add(verif.lot_id)
            last['rows'].pop(verif.lot_id, None)
            last['rows'][verif.lot_id] = VerifTitles.row(verif)  # lot revu a plusieurs etapes : sa derniere verif
        last['people'].update(filter(None, map(_person, [verif.by] + list(verif.participants or []))))
        last['present'] += verif.present_count
        last['missing'] += verif.missing_count
        last['complete'] = last['complete'] and verif.complete
    return sessions


def _podium(counter: Mapping[Any, float], unit: str, unit_plural: str | None = None, minimum: float = 1,
            name: Callable[[Any], str] = identity_name, fmt: Callable[[float], str] | None = None,
            lowest: bool = False) -> list[dict[str, Any]]:
    """Les PODIUM premiers d'un compteur (ex aequo au meme rang). name : cle -> nom affiche (nom court d'une
    identite stockee par defaut). fmt : valeur -> texte affiche avec son unite (durees, vitesses), unit ignore ;
    lowest : les plus petites valeurs d'abord (temps les plus courts)."""
    rows = sorted(((key, value) for key, value in counter.items() if value >= minimum),
                  key=lambda row: row[1], reverse=not lowest)
    podium, rank, previous = [], 0, None
    for index, (key, value) in enumerate(rows):
        if value != previous:
            rank = index + 1
            previous = value
        if rank > PODIUM:
            break
        if fmt is not None:
            podium.append({'rank': rank, 'name': name(key), 'value': fmt(value), 'unit': ''})
            continue
        podium.append({'rank': rank, 'name': name(key),
                       'value': value, 'unit': unit if value <= 1 else (unit_plural or unit + 's')})
    return podium


def _when(dt: datetime) -> str:
    local = timezone.localtime(dt)
    return f"le {local.day} {MONTHS[local.month - 1]} {local.year} à {local.hour}h{local.minute:02d}"


def _night_minutes(dt: datetime) -> int:
    """Minutes depuis DAY_START (5h) : plus c'est grand, plus c'est tard dans la nuit."""
    local = timezone.localtime(dt)
    return ((local.hour - DAY_START) % 24) * 60 + local.minute


def compute(value: str | int | None = None) -> dict[str, Any]:
    """Stats fun de la periode : {period, years, totals, awards}. awards : cartes {key, emoji, title, text, podium?,
    record?} dans l'ordre de presentation, sans les cartes vides (rien a montrer cette annee)."""
    start, end, label = period(value)
    with anonymized():
        return _compute(start, end, label)


def _compute(start: datetime, end: datetime, label: str) -> dict[str, Any]:
    parents = dict(Lots.objects.values_list('id', 'parent_id'))
    names = dict(Lots.objects.values_list('id', 'name'))
    verifs = list(Verifs.objects.select_related('lot').filter(datetime__gte=start, datetime__lt=end)
                  .order_by('datetime', 'id'))
    sessions = _sessions(verifs, parents)
    session_of = {verif_id: session for session in sessions for verif_id in session['ids']}

    per_person: Counter[str] = Counter()
    duos: Counter[tuple[str, str]] = Counter()
    partners: defaultdict[str, set[str]] = defaultdict(set)
    scanned: Counter[str] = Counter()
    per_day: Counter[tuple[str, date]] = Counter()
    lot_counter: Counter[str] = Counter()
    weekday: Counter[int] = Counter()
    month: Counter[int] = Counter()
    night = None
    early = None
    for session in sessions:
        people = sorted(session['people'])
        local = timezone.localtime(session['at'])
        weekday[local.weekday()] += 1
        month[local.month] += 1
        if session['root']:
            lot_counter[names.get(session['root'], '?')] += 1
        for person in people:
            per_person[person] += 1
            scanned[person] += session['present']
            per_day[(person, local.date())] += 1
            partners[person].update(other for other in people if other != person)
        for pair in combinations(people, 2):
            duos[pair] += 1
        if people:
            minutes = _night_minutes(session['at'])
            # nuit : apres 21h ou avant 5h ; matin : de 5h a 8h
            if minutes >= 16 * 60 and (night is None or minutes > night[0]):
                night = (minutes, session)
            if minutes < 3 * 60 and (early is None or minutes < early[0]):
                early = (minutes, session)

    # perimes trouves : items perimes vus (ou remplaces) pendant les vérifs, comptes pour chaque participant
    expired_rows = list(VerifItem.objects.filter(verif__in=[v.id for v in verifs])
                        .filter(Q(expired=True) | Q(result=VerifResult.REPLACED))
                        .values_list('verif_id', 'item__pack__peremption', 'item__pack__item_type__name'))
    hunters: Counter[str] = Counter()
    oldest = None
    verif_at = {verif.id: verif.datetime for verif in verifs}
    for verif_id, peremption, type_name in expired_rows:
        session = session_of.get(verif_id)
        for person in session['people'] if session else ():
            hunters[person] += 1
        if peremption and session and session['people']:
            days = (timezone.localtime(verif_at[verif_id]).date() - peremption).days
            if oldest is None or days > oldest[0]:
                oldest = (days, type_name, session, verif_at[verif_id])

    awards = []

    def add(key: str, emoji: str, title: str, text: str, podium: list[dict[str, Any]] | None = None,
            record: dict[str, Any] | None = None) -> None:
        if podium or record:
            awards.append({'key': key, 'emoji': emoji, 'title': title, 'text': text, 'podium': podium or [],
                           'record': record})

    add('verifs', '🏆', 'Machine à vérifs', 'Le plus de vérifs de lots sur la période.',
        _podium(per_person, 'vérif'))
    add('expired', '🕵️', 'Flair à périmés', 'Le plus de périmés débusqués pendant les vérifs.',
        _podium(hunters, 'périmé'))
    duo_podium = _podium(Counter({f"{identity_name(a)} & {identity_name(b)}": n for (a, b), n in duos.items()}),
                         'vérif ensemble', 'vérifs ensemble', minimum=2, name=str)
    add('duos', '👯', 'Duo inséparable', 'Les binômes qui ont fait le plus de vérifs ensemble.', duo_podium)
    add('team', '🤝', 'Esprit d\'équipe', 'Le plus de coéquipiers différents en vérif.',
        _podium(Counter({person: len(others) for person, others in partners.items() if others}), 'coéquipier'))
    add('scanned', '📦', 'Scanner en feu', 'Le plus d\'items scannés présents.', _podium(scanned, 'item'))
    if per_day:
        (person, day), count = per_day.most_common(1)[0]
        if count >= 2:
            add('marathon', '🏃', 'Marathon du jour', 'Le plus de vérifs en une seule journée.', record={
                'name': identity_name(person), 'value': f"{count} vérifs",
                'detail': f"le {WEEKDAYS[day.weekday()]} {day.day} {MONTHS[day.month - 1]} {day.year}"})
    if night:
        session = night[1]
        add('night', '🦉', 'Oiseau de nuit', 'La vérif la plus tardive de l\'année.', record={
            'name': ' & '.join(identity_name(p) for p in sorted(session['people'])),
            'value': timezone.localtime(session['at']).strftime('%H h %M'),
            'detail': _when(session['at']) + (f" ({names[session['root']]})" if session['root'] in names else '')})
    if early:
        session = early[1]
        add('early', '🐓', 'Lève-tôt', 'La vérif la plus matinale de l\'année.', record={
            'name': ' & '.join(identity_name(p) for p in sorted(session['people'])),
            'value': timezone.localtime(session['at']).strftime('%H h %M'),
            'detail': _when(session['at']) + (f" ({names[session['root']]})" if session['root'] in names else '')})
    if oldest and oldest[0] > 0:
        days, type_name, session, at = oldest
        add('fossil', '🦖', 'Archéologue', 'Le périmé le plus ancien retrouvé dans un lot.', record={
            'name': ' & '.join(identity_name(p) for p in sorted(session['people'])),
            'value': f"{days} jours" if days > 1 else '1 jour',
            'detail': f"{type_name} périmé depuis {_duration(days)}, trouvé {_when(at)}"})

    timing = _timing(sessions, parents, add)

    # operations du journal : scellés, reassorts, signalements, listes
    operations = Operation.objects.filter(at__gte=start, at__lt=end)
    seals = Counter(op for op in operations.filter(kind=OperationKind.SEAL).values_list('by', flat=True) if _person(op))
    add('seals', '🔒', 'Pro du scellé', 'Le plus de lots scellés.', _podium(seals, 'scellé'))
    restocks: Counter[str] = Counter()
    for by, details in operations.filter(kind=OperationKind.RESTOCK).values_list('by', 'details'):
        if _person(by):
            restocks[by] += (details or {}).get('count') or 1
    add('restock', '🧺', 'Pro du réassort', 'Le plus d\'items remis dans les lots par réassort.',
        _podium(restocks, 'item'))
    reports = Counter(by for by in ProblemReport.objects.filter(created__gte=start, created__lt=end)
                      .values_list('by', flat=True) if _person(by))
    add('reports', '🚨', 'Œil de lynx', 'Le plus de problèmes signalés.', _podium(reports, 'signalement'))
    checklists = Counter(f"{SOURCE_VERIFIED}:{matricule}" for matricule in Checklist.objects.filter(
        created__gte=start, created__lt=end, is_template=False).values_list('owner_id', flat=True))
    add('lists', '📋', 'As de l\'organisation', 'Le plus de listes de matériel préparées.', _podium(checklists, 'liste'))

    # lots : le plus verifie, le plus delaisse
    add('pet', '🎒', 'Lot chouchou', 'Les lots les plus vérifiés.', _podium(lot_counter, 'vérif', name=str))
    forgotten = _forgotten_lot(verifs, parents, names, start, end)
    if forgotten:
        add('forgotten', '🦥', 'Lot oublié', 'Le plus long moment sans vérif.', record=forgotten)

    # nouvelles recrues : comptes crees sur la periode, classes par vérifs
    recruits = set(f"{SOURCE_VERIFIED}:{m}" for m in Secouristes.objects.filter(
        created__gte=start, created__lt=end).values_list('matricule', flat=True))
    add('rookie', '🌱', 'Recrue de l\'année', 'Les nouveaux qui ont déjà le plus de vérifs.',
        _podium(Counter({p: n for p, n in per_person.items() if p in recruits}), 'vérif'))

    if weekday:
        day, count = weekday.most_common(1)[0]
        add('weekday', '📅', 'Jour préféré', 'Le jour de la semaine où l\'on vérifie le plus.', record={
            'name': WEEKDAYS[day].capitalize(), 'value': f"{count} vérifs" if count > 1 else '1 vérif',
            'detail': 'et le plus calme : ' + WEEKDAYS[min(range(7), key=lambda d: weekday.get(d, 0))]})
    if month:
        busiest, count = month.most_common(1)[0]
        add('month', '🔥', 'Mois le plus chaud', 'Le mois qui a compté le plus de vérifs.', record={
            'name': MONTHS[busiest - 1].capitalize(), 'value': f"{count} vérifs" if count > 1 else '1 vérif',
            'detail': ''})

    used = ItemMovement.objects.filter(at__gte=start, at__lt=end, cancelled__isnull=True)
    totals = {
        'verifs': len(sessions),
        'lots': sum(len(session['lots']) for session in sessions),
        'scanned': sum(session['present'] for session in sessions),
        'expired': len(expired_rows),
        'people': len(per_person),
        'used': used.filter(kind=MovementKind.USED).count(),
        'discarded': used.filter(kind__in=[MovementKind.DISCARDED, MovementKind.REPLACED]).count(),
        'team_verifs': sum(1 for session in sessions if len(session['people']) >= 2),
    }
    return {'period': label, 'start': timezone.localtime(start).date().isoformat(),
            'end': timezone.localtime(end).date().isoformat(), 'years': years(), 'totals': totals,
            'time_totals': timing['totals'], 'lots': timing['lots'], 'awards': awards}


def clock(seconds: float, precise: bool = True) -> str:
    """« 45 s », « 12 min 05 s », « 1h07 min 05 s » ; precise faux : sans les secondes au-dela d'une minute
    (« 12h40 min », temps cumules)."""
    total = max(0, round(seconds))
    hours, rest = divmod(total, 3600)
    minutes, secs = divmod(rest, 60)
    if hours:
        return f"{hours}h{minutes:02d} min" + (f" {secs:02d} s" if precise else '')
    if minutes:
        return f"{minutes} min" + (f" {secs:02d} s" if precise else '')
    return f"{secs} s"


def _rate(value: float) -> str:
    """Vitesse de scan : « 4,2 items/min »."""
    return f"{value:.1f}".replace('.', ',') + ' items/min'


def _people(session: dict[str, Any]) -> str:
    return ' & '.join(identity_name(p) for p in sorted(session['people']))


AddCard = Callable[..., None]


def _timing(sessions: list[dict[str, Any]], parents: dict[str, str | None], add: AddCard) -> dict[str, Any]:
    """Stats de duree des sessions qui en ont une : cartes ajoutees par add (temps passe, vitesse, rafale, vérif
    express et la plus longue, vérifs a plusieurs), {'totals': tuiles [{value, label}], 'lots': par lot verifie en
    entier [{id, name, count, average, fastest, slowest, podium}], le plus verifie d'abord}."""
    timed: list[tuple[dict[str, Any], float]] = []
    for session in sessions:
        if session['started'] is None or session['stock']:
            continue
        span = session['end'] - session['started']
        if timedelta(0) < span <= MAX_DURATION:
            timed.append((session, span.total_seconds()))
    if not timed:
        return {'totals': [], 'lots': []}

    titles = VerifTitles(parents)
    per_person: Counter[str] = Counter()
    solo_items: Counter[str] = Counter()
    solo_seconds: Counter[str] = Counter()
    solo_count: Counter[str] = Counter()
    by_lot: defaultdict[str, list[tuple[dict[str, Any], float]]] = defaultdict(list)
    lot_names: dict[str, str] = {}
    complete: list[tuple[dict[str, Any], float]] = []
    for session, seconds in timed:
        people = session['people']
        for person in people:
            per_person[person] += seconds
        if len(people) == 1:
            (person,) = people
            solo_items[person] += session['present']
            solo_seconds[person] += seconds
            solo_count[person] += 1
        if session['complete'] and people:
            complete.append((session, seconds))
            # lot verifie en entier : la vérif ne se nomme que d'un lot (les 2 sacs du B+ → B+), comme Vérifs récentes
            named, _ = titles.names(list(session['rows'].values()))
            if len(named) == 1:
                session['name'] = named[0]['name']
                lot_names[named[0]['id']] = named[0]['name']
                by_lot[named[0]['id']].append((session, seconds))

    def when(session: dict[str, Any]) -> str:
        return _when(session['started'])

    def label(session: dict[str, Any]) -> str:
        return f" ({session['name']})" if session.get('name') else ''

    add('time', '⏳', 'Pilier des vérifs', 'Le plus de temps passé en vérif.',
        _podium(per_person, '', fmt=lambda value: clock(value, precise=False)))
    speeds = {person: solo_items[person] * 60 / solo_seconds[person] for person in solo_seconds
              if solo_count[person] >= SPEED_MIN_VERIFS and solo_items[person] >= SPEED_MIN_ITEMS}
    add('speed', '⚡', 'Scan éclair', f"Le plus d'items scannés par minute, en vérif seul (au moins "
        f"{SPEED_MIN_VERIFS} vérifs et {SPEED_MIN_ITEMS} items).", _podium(speeds, '', minimum=0, fmt=_rate))

    burst = _burst([session for session, _ in timed])
    if burst:
        count, session = burst
        add('burst', '🔫', 'Rafale', 'Le plus de scans en une minute pendant une vérif.', record={
            'name': _people(session) or '?', 'value': f"{count} scans en 1 min",
            'detail': when(session) + label(session)})

    quick = [(session, seconds) for session, seconds in complete if session['present'] >= QUICKEST_MIN_ITEMS]
    if quick:
        session, seconds = min(quick, key=lambda row: row[1])
        add('quickest', '🚀', 'Vérif express', f"La vérif complète la plus rapide de l'année (au moins "
            f"{QUICKEST_MIN_ITEMS} items).", record={
                'name': _people(session), 'value': clock(seconds),
                'detail': f"{session['present']} items, {when(session)}{label(session)}"})
    if complete:
        session, seconds = max(complete, key=lambda row: row[1])
        add('slowest', '🐢', 'Tout en finesse', 'La vérif complète la plus longue de l\'année.', record={
            'name': _people(session), 'value': clock(seconds),
            'detail': f"{session['present']} items, {when(session)}{label(session)}"})

    # a plusieurs ou seul, sur les memes lots : moyenne des deux sur chaque lot qui a les deux, ponderee
    solo_total = team_total = 0.0
    solo_n = team_n = 0
    for rows in by_lot.values():
        solo = [seconds for session, seconds in rows if len(session['people']) == 1]
        team = [seconds for session, seconds in rows if len(session['people']) >= 2]
        if solo and team:
            weight = min(len(solo), len(team))
            solo_total += sum(solo) / len(solo) * weight
            team_total += sum(team) / len(team) * weight
            solo_n += len(solo)
            team_n += len(team)
    if solo_n >= 2 and team_n >= 2 and solo_total > 0:
        change = round((team_total - solo_total) * 100 / solo_total)
        if change:
            add('together', '👥', 'Plus on est de fous…', 'À plusieurs ou seul, sur les mêmes lots.', record={
                'name': 'À plusieurs', 'value': f"{abs(change)} % plus " + ('vite' if change < 0 else 'lent'),
                'detail': f"sur {team_n} vérifs à plusieurs et {solo_n} seul"})

    lots = []
    for lot_id, rows in by_lot.items():
        fastest = min(rows, key=lambda row: row[1])
        slowest = max(rows, key=lambda row: row[1])
        best: dict[str, float] = {}
        for session, seconds in rows:
            for person in session['people']:
                best[person] = min(best.get(person, seconds), seconds)

        def record(row: tuple[dict[str, Any], float]) -> dict[str, Any]:
            session, seconds = row
            return {'name': _people(session), 'value': clock(seconds), 'seconds': round(seconds),
                    'detail': f"{session['present']} items, {when(session)}"}

        lots.append({'id': lot_id, 'name': lot_names[lot_id], 'count': len(rows),
                     'average': clock(sum(seconds for _, seconds in rows) / len(rows)),
                     'fastest': record(fastest), 'slowest': record(slowest),
                     'podium': _podium(best, '', minimum=0, fmt=clock, lowest=True)})
    lots.sort(key=lambda lot: (-lot['count'], lot['name']))

    total = sum(seconds for _, seconds in timed)
    people_total = sum(seconds * len(session['people']) for session, seconds in timed)
    items = sum(session['present'] for session, _ in timed)
    tiles = [{'value': clock(total, precise=False), 'label': 'passées en vérif'}]
    if people_total > total:
        tiles.append({'value': clock(people_total, precise=False), 'label': 'de temps cumulé des secouristes'})
    if complete:
        tiles.append({'value': clock(sum(seconds for _, seconds in complete) / len(complete)),
                      'label': 'par vérif complète en moyenne'})
    if items:
        tiles.append({'value': _rate(items * 60 / total), 'label': 'en moyenne'})
    tiles.append({'value': str(len(timed)), 'label': 'vérif chronométrée' if len(timed) == 1
                  else 'vérifs chronométrées'})
    return {'totals': tiles, 'lots': lots}


def _burst(sessions: list[dict[str, Any]]) -> tuple[int, dict[str, Any]] | None:
    """(scans, session) : le plus de scans d'items dans une fenetre de BURST_WINDOW, sur les heures de scan exactes
    (VerifItem.scanned_at) des sessions qui ont des participants connectes."""
    session_of = {verif_id: session for session in sessions if session['people'] for verif_id in session['ids']}
    times: defaultdict[int, list[datetime]] = defaultdict(list)
    for verif_id, at in VerifItem.objects.filter(verif__in=list(session_of), scanned_at__isnull=False).values_list(
            'verif_id', 'scanned_at'):
        times[id(session_of[verif_id])].append(at)
    by_key = {id(session): session for session in session_of.values()}
    best: tuple[int, dict[str, Any]] | None = None
    for key, moments in times.items():
        moments.sort()
        first = 0
        for last, at in enumerate(moments):
            while at - moments[first] >= BURST_WINDOW:
                first += 1
            count = last - first + 1
            if best is None or count > best[0]:
                best = (count, by_key[key])
    return best if best and best[0] >= 2 else None


def _duration(days: int) -> str:
    if days >= 730:
        return f"plus de {days // 365} ans"
    if days >= 365:
        return 'plus d\'un an'
    if days >= 60:
        return f"{days // 30} mois"
    return f"{days} jours" if days > 1 else '1 jour'


def _forgotten_lot(verifs: list[Verifs], parents: dict[str, str | None], names: dict[str, str], start: datetime,
                   end: datetime) -> dict[str, str] | None:
    """Lot actif (pas un rangement, pas un sous-lot) resté le plus longtemps sans vérif sur la periode."""
    tops = {lot.id: lot for lot in Lots.objects.select_related('lot_type').filter(
        active=True, parent__isnull=True, lot_type__storage=False)}
    if not tops:
        return None

    def root(lot_id: str) -> str:
        seen = set()
        while parents.get(lot_id) and lot_id not in seen:
            seen.add(lot_id)
            lot_id = parents[lot_id]
        return lot_id

    times = defaultdict(list)
    for verif in verifs:
        if verif.lot_id:
            times[root(verif.lot_id)].append(verif.datetime)
    best = None
    for lot_id, lot in tops.items():
        begin = max(start, lot.created)
        points = [begin] + times.get(lot_id, []) + [end]
        for before, after in zip(points, points[1:]):
            gap = after - before
            if best is None or gap > best[0]:
                best = (gap, lot_id, before, after)
    if best is None or best[0].days < 1:
        return None
    gap, lot_id, before, after = best
    still = after == end
    return {'name': names.get(lot_id, '?'), 'value': f"{gap.days} jours",
            'detail': ('toujours sans vérif depuis ' if still else 'sans vérif entre ')
            + timezone.localtime(before).strftime('%d/%m/%Y')
            + ('' if still else ' et ' + timezone.localtime(after).strftime('%d/%m/%Y'))}
