"""Cartes d'invites : secouristes d'une autre antenne, sans compte ni matricule.

Un admin les prepare a l'avance : imprimees au poste (fenetre Utilisateurs, onglet Cartes d'invites), ou montrees a
l'ecran depuis l'outil de debug du web (a activer dans les 24 h). Une carte n'a pas de nom : le nom et le prenom sont
demandes a la premiere connexion, qui l'active pour `guest_hours` heures (48 h par defaut, ServerSettings.guest_hours).
Son « matricule » est un numero de carte tire au hasard (INV...), qui ne sert qu'a la reconnaitre. Role secouriste,
jamais de PIN. Une carte desactivee (activee ou non) ne connecte plus personne et coupe les sessions ouvertes.

Une carte sert a un seul invite : l'historique (vérifs, journal) garde son nom par le numero de la carte.
"""

import secrets
from datetime import timedelta

from django.db import transaction
from django.utils import timezone

from .models import PinMode, Role, Secouristes, ServerSettings

GUEST_PREFIX = 'INV'
GUEST_CODE_ALPHABET = '23456789ABCDEFGHJKLMNPQRSTUVWXYZ'  # sans 0/O ni 1/I : numero lisible sur la carte
GUEST_CODE_LENGTH = 5
GUEST_HOURS_MAX = 24 * 30          # duree de vie : au plus 30 jours
GUEST_MAX_CARDS = 20               # cartes creees d'un coup
WEB_ACTIVATION_HOURS = 24          # carte montree a l'ecran (outil de debug) : a activer dans les 24 h

GUEST_STATE_LABELS = {
    'unused': 'Pas encore utilisée',
    'active': 'Activée',
    'expired': 'Expirée',
    'expired_unused': 'Pas activée à temps',
    'disabled': 'Désactivée',
}


def parse_guest_hours(value):
    """Duree de vie d'une carte d'invite en heures (1 a 720)."""
    try:
        hours = int(value)
    except (TypeError, ValueError):
        raise ValueError("Durée de la carte d'invité : un nombre d'heures") from None
    if not 1 <= hours <= GUEST_HOURS_MAX:
        raise ValueError(f"Durée de la carte d'invité : de 1 à {GUEST_HOURS_MAX} heures")
    return hours


def new_guest_matricule():
    for _ in range(50):
        matricule = GUEST_PREFIX + ''.join(secrets.choice(GUEST_CODE_ALPHABET) for _ in range(GUEST_CODE_LENGTH))
        if not Secouristes.objects.filter(matricule=matricule).exists():
            return matricule
    raise RuntimeError("Impossible de tirer un numéro de carte libre")


def create_guest_card(created_by, hours=None, activate_within_hours=None):
    """Nouvelle carte d'invite (non enregistree dans le journal : a l'appelant). Sa cle en clair est dans
    `new_key` (renew_key) le temps de la reponse, pour l'imprimer ou l'afficher."""
    card = Secouristes(matricule=new_guest_matricule(), nom='', prenom='', role=Role.NORMAL, fonction='Invité',
                       guest=True, pin_mode=PinMode.DISABLED, pin_at_login=False,
                       guest_hours=hours or ServerSettings.get().guest_hours, guest_created_by=created_by)
    card.renew_key()  # imprimee a l'avance : utilisable jusqu'a l'expiration normale d'un badge (un an)
    if activate_within_hours:
        card.guest_activate_before = timezone.now() + timedelta(hours=activate_within_hours)
    card.save()
    return card


def activate(card, nom, prenom):
    """Premiere connexion avec la carte : nom et prenom de l'invite, debut de sa duree de vie. Deux telephones qui
    scannent la meme carte en meme temps : seul le premier la nomme, le second se connecte sous ce nom. Retourne
    True si cette connexion a active la carte."""
    nom = ' '.join(str(nom or '').split())[:32]
    prenom = ' '.join(str(prenom or '').split())[:32]
    if not nom or not prenom:
        raise ValueError('Saisissez votre prénom et votre nom.')
    with transaction.atomic():
        done = Secouristes.objects.filter(pk=card.pk, guest=True, guest_activated__isnull=True).update(
            nom=nom, prenom=prenom, guest_activated=timezone.now())
    card.refresh_from_db()
    return bool(done)


def card_row(card, public=False):
    """Carte d'invite pour les listes : poste, ou outil de debug du web (`public` : jamais de nom de famille, ni celui
    de l'invite ni celui de l'admin qui a cree la carte)."""
    from .idendity import short_name
    from .serializers import _date
    expires = card.guest_expires
    creator = card.guest_created_by
    named = bool(card.prenom or card.nom)
    return {
        'matricule': card.matricule,
        'prenom': card.prenom,
        'nom': '' if public else card.nom,
        'name': (short_name(card.matricule) if public else f'{card.prenom} {card.nom}') if named else '',
        'state': card.guest_state(),
        'state_label': GUEST_STATE_LABELS[card.guest_state()],
        'hours': card.guest_hours,
        'created': _date(card.created),
        'created_by': (short_name(creator.matricule) if public else str(creator)) if creator else '',
        'created_by_matricule': creator.matricule if creator else '',
        'activated': _date(card.guest_activated),
        'expires': _date(expires),
        'activate_before': _date(card.guest_activate_before),
        'key_expires': _date(card.key_expires),
    }


STATE_ORDER = {'active': 0, 'unused': 1, 'expired': 2, 'expired_unused': 3, 'disabled': 4}


def cards(public=False):
    """Toutes les cartes d'invites : en cours d'abord, puis a utiliser, puis le reste, les plus recentes d'abord."""
    rows = [card_row(card, public) for card in Secouristes.objects.filter(guest=True).select_related('guest_created_by')]
    rows.sort(key=lambda row: row['created'], reverse=True)
    rows.sort(key=lambda row: STATE_ORDER[row['state']])
    return rows
