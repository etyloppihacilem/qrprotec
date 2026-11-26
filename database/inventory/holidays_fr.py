# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          holidays_fr.py
#       -\-    _|__
#        |\___/  . \        Created on 26 Nov. 2026 at 18:00
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

"""Jours ouvres et jours feries francais (metropole), calcules ici sans service externe : delais de livraison des
commandes fournisseurs (purchases.py). Un jour ouvre est un jour de semaine (lundi a vendredi) qui n'est pas ferie.

Feries : 1er janvier, lundi de Paques, 1er mai, 8 mai, Ascension, lundi de Pentecote, 14 juillet, 15 aout,
1er novembre, 11 novembre, 25 decembre. Paques par l'algorithme de Meeus/Jones/Butcher (calendrier gregorien).
Pas le vendredi saint ni le 26 decembre d'Alsace-Moselle."""

from __future__ import annotations

from datetime import date, timedelta
from functools import lru_cache


def easter(year: int) -> date:
    """Dimanche de Paques (calendrier gregorien, algorithme anonyme de Meeus/Jones/Butcher)."""
    a = year % 19
    b, c = divmod(year, 100)
    d, e = divmod(b, 4)
    f = (b + 8) // 25
    g = (b - f + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i, k = divmod(c, 4)
    l = (32 + 2 * e + 2 * i - h - k) % 7  # noqa: E741
    m = (a + 11 * h + 22 * l) // 451
    month, day = divmod(h + l - 7 * m + 114, 31)
    return date(year, month, day + 1)


@lru_cache(maxsize=64)
def holidays(year: int) -> dict[date, str]:
    """Jours feries de l'annee, avec leur nom."""
    sunday = easter(year)
    return {
        date(year, 1, 1): "Jour de l'an",
        sunday + timedelta(days=1): 'Lundi de Pâques',
        date(year, 5, 1): 'Fête du travail',
        date(year, 5, 8): 'Victoire 1945',
        sunday + timedelta(days=39): 'Ascension',
        sunday + timedelta(days=50): 'Lundi de Pentecôte',
        date(year, 7, 14): 'Fête nationale',
        date(year, 8, 15): 'Assomption',
        date(year, 11, 1): 'Toussaint',
        date(year, 11, 11): 'Armistice 1918',
        date(year, 12, 25): 'Noël',
    }


def is_holiday(day: date) -> bool:
    return day in holidays(day.year)


def is_business_day(day: date) -> bool:
    return day.weekday() < 5 and not is_holiday(day)


def business_days_between(start: date, end: date) -> int:
    """Jours ouvres apres `start` jusqu'a `end` compris : commande un lundi, livree le mardi = 1 jour ouvre ; le
    vendredi, livree le lundi = 1 (le week-end ne compte pas). 0 si `end` n'est pas apres `start`."""
    if end <= start:
        return 0
    count = 0
    day = start
    while day < end:
        day += timedelta(days=1)
        count += is_business_day(day)
    return count


def add_business_days(start: date, days: int) -> date:
    """Date `days` jours ouvres apres `start` (livraison prevue d'une commande passee ce jour-la). 0 : `start`."""
    day = start
    left = max(0, days)
    while left > 0:
        day += timedelta(days=1)
        if is_business_day(day):
            left -= 1
    return day
