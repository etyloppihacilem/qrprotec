# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          print_counts.py
#       -\-    _|__
#        |\___/  . \        Created on 12 Nov. 2026 at 17:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Compteur d'impressions : etiquettes envoyees a l'imprimante par les fronts, par mois et par categorie d'etiquette
(voir models.PrintCount), pour suivre la consommation de bobines et de ruban.

Le front compte chaque etiquette acceptee par l'imprimante, par jour et par categorie, et envoie ses compteurs par
lots identifies (`batch`) : hors connexion, ils attendent sur le front ; un lot deja recu n'est pas recompte.
"""

from datetime import date, timedelta
from decimal import Decimal, InvalidOperation

from django.db import IntegrityError, transaction
from django.db.models import F, Sum
from django.db.models.functions import ExtractMonth, ExtractYear
from django.utils import timezone

from .models import LabelSupport, PrintCount, PrintCountBatch

MAX_ROWS = 400           # lignes par envoi (jour x categorie)
MAX_COUNT = 100000       # etiquettes par ligne
MIN_MM, MAX_MM = 5, 300  # dimensions d'une etiquette
MAX_AGE_DAYS = 400       # compteur reste sur un front hors connexion : au-dela, horloge douteuse


class CountError(Exception):
    pass


def _mm(value, field):
    try:
        number = Decimal(str(value)).quantize(Decimal('0.1'))
    except (InvalidOperation, ValueError, TypeError):
        raise CountError(f'{field} : nombre attendu')
    if not MIN_MM <= number <= MAX_MM:
        raise CountError(f'{field} : entre {MIN_MM} et {MAX_MM} mm')
    return number


def _row(row, today):
    if not isinstance(row, dict):
        raise CountError('counts : liste de compteurs attendue')
    try:
        day = date.fromisoformat(str(row.get('day', '')))
    except ValueError:
        raise CountError('day : date AAAA-MM-JJ attendue')
    # fuseau du front et heure du serveur peuvent differer d'un jour
    if day > today + timedelta(days=1) or day < today - timedelta(days=MAX_AGE_DAYS):
        raise CountError(f'day : {day.isoformat()} hors limites')
    support = row.get('support')
    if support not in LabelSupport.values:
        raise CountError('support : direct ou transfer attendu')
    try:
        count = int(row.get('count'))
    except (TypeError, ValueError):
        raise CountError('count : entier attendu')
    if not 0 < count <= MAX_COUNT:
        raise CountError('count : valeur hors limites')
    return day, _mm(row.get('width_mm'), 'width_mm'), _mm(row.get('height_mm'), 'height_mm'), support, count


def record(batch_id, front, rows):
    """Ajoute les compteurs d'un envoi. Renvoie (nombre d'etiquettes comptees, deja recu)."""
    batch_id = str(batch_id or '').strip()
    if not 8 <= len(batch_id) <= 64:
        raise CountError('batch : identifiant de l\'envoi attendu')
    if not isinstance(rows, list) or not rows or len(rows) > MAX_ROWS:
        raise CountError(f'counts : entre 1 et {MAX_ROWS} compteurs')
    today = timezone.localdate()
    parsed = [_row(row, today) for row in rows]
    front = (front or '')[:64]
    try:
        with transaction.atomic():
            PrintCountBatch.objects.create(id=batch_id, front=front)
            for day, width, height, support, count in parsed:
                keys = dict(day=day, width_mm=width, height_mm=height, support=support, front=front)
                if not PrintCount.objects.filter(**keys).update(count=F('count') + count):
                    PrintCount.objects.create(count=count, **keys)
    except IntegrityError:
        if PrintCountBatch.objects.filter(id=batch_id).exists():
            return 0, True
        raise
    return sum(row[4] for row in parsed), False


def _number(value):
    """40.0 -> « 40 », 40.5 -> « 40,5 »."""
    text = f'{Decimal(value).normalize():f}'
    return text.replace('.', ',')


def category_label(width, height, support):
    return f'{_number(width)} × {_number(height)} mm, {LabelSupport(support).label}'


def summary(year=None):
    """Tableau d'une annee : une ligne par categorie avec ses 12 mois et son total, plus les totaux par mois.
    `years` : annees qui ont des impressions (et l'annee en cours), pour choisir."""
    current = timezone.localdate().year
    year = year or current
    rows = (PrintCount.objects.filter(day__year=year)
            .annotate(month=ExtractMonth('day'))
            .values('width_mm', 'height_mm', 'support', 'month')
            .annotate(total=Sum('count')))
    categories = {}
    for row in rows:
        key = (row['width_mm'], row['height_mm'], row['support'])
        category = categories.setdefault(key, [0] * 12)
        category[row['month'] - 1] += row['total']
    result = []
    for (width, height, support), months in categories.items():
        result.append({
            'key': f'{_number(width)}x{_number(height)}-{support}',
            'label': category_label(width, height, support),
            'width_mm': float(width),
            'height_mm': float(height),
            'support': support,
            'months': months,
            'total': sum(months),
        })
    # les plus imprimees d'abord
    result.sort(key=lambda category: (-category['total'], category['label']))
    months_total = [sum(category['months'][month] for category in result) for month in range(12)]
    years = set(PrintCount.objects.annotate(year=ExtractYear('day')).values_list('year', flat=True).distinct())
    years.add(current)
    return {
        'year': year,
        'years': sorted(years, reverse=True),
        'current_month': timezone.localdate().month if year == current else 0,
        'categories': result,
        'months_total': months_total,
        'total': sum(months_total),
    }
