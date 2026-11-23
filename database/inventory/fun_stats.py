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
"""

from __future__ import annotations

from collections import Counter, defaultdict
from datetime import datetime, time, timedelta
from itertools import combinations
from typing import TYPE_CHECKING, Any
from collections.abc import Callable

from django.db.models import Q
from django.utils import timezone

from .idendity import SOURCE_VERIFIED, anonymized, identity_name
from .models import (Checklist, ItemMovement, Lots, MovementKind, Operation, OperationKind, ProblemReport,
                     Secouristes, Verifs, VerifItem, VerifResult)

if TYPE_CHECKING:
    from datetime import date

CASCADE_GAP = timedelta(minutes=30)  # comme views.public.CASCADE_GAP (Vérifs récentes)
PODIUM = 3
WEEKDAYS = ['lundi', 'mardi', 'mercredi', 'jeudi', 'vendredi', 'samedi', 'dimanche']
MONTHS = ['janvier', 'février', 'mars', 'avril', 'mai', 'juin', 'juillet', 'août', 'septembre', 'octobre',
          'novembre', 'décembre']
# heure « de nuit » : une vérif a 1 h du matin est plus tardive qu'une a 23 h (journee comptee a partir de 5 h)
DAY_START = 5


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
    """Vérifs reunies en sessions : [{at, by, people, lots, root, present, expired, missing, complete, ids}]."""
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
                    'present': 0, 'missing': 0, 'complete': True, 'ids': [], 'stock': verif.lot_id is None}
            sessions.append(last)
            open_by[key] = last
        last['_latest'] = verif.datetime
        last['ids'].append(verif.id)
        if verif.lot_id:
            last['lots'].add(verif.lot_id)
        last['people'].update(filter(None, map(_person, [verif.by] + list(verif.participants or []))))
        last['present'] += verif.present_count
        last['missing'] += verif.missing_count
        last['complete'] = last['complete'] and verif.complete
    return sessions


def _podium(counter: Counter, unit: str, unit_plural: str | None = None, minimum: int = 1,
            name: Callable[[str], str] = identity_name) -> list[dict[str, Any]]:
    """Les PODIUM premiers d'un Counter (ex aequo au meme rang). name : cle -> nom affiche (nom court d'une
    identite stockee par defaut)."""
    rows = [(key, value) for key, value in counter.most_common() if value >= minimum]
    podium, rank, previous = [], 0, None
    for index, (key, value) in enumerate(rows):
        if value != previous:
            rank = index + 1
            previous = value
        if rank > PODIUM:
            break
        podium.append({'rank': rank, 'name': name(key),
                       'value': value, 'unit': unit if value <= 1 else (unit_plural or unit + 's')})
    return podium


def _when(dt: datetime) -> str:
    local = timezone.localtime(dt)
    return f"le {local.day} {MONTHS[local.month - 1]} {local.year} à {local.hour} h {local.minute:02d}"


def _night_minutes(dt: datetime) -> int:
    """Minutes depuis DAY_START (5 h) : plus c'est grand, plus c'est tard dans la nuit."""
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
    verifs = list(Verifs.objects.filter(datetime__gte=start, datetime__lt=end).order_by('datetime', 'id'))
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
            # nuit : apres 21 h ou avant 5 h ; matin : de 5 h a 8 h
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
            'end': timezone.localtime(end).date().isoformat(), 'years': years(), 'totals': totals, 'awards': awards}


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
