# ###################################646f75627420796f7572206f776e206578697374656e6365###################################
#
#              """          idendity.py
#       -\-    _|__
#        |\___/  . \        Created on 23 Sep. 2026 at 18:37
#        \     /(((/        by hmelica
#         \___/)))/         hmelica@student.42.fr
#
# ######################################################################################################################

# utils.py ou identity.py

import unicodedata
from contextlib import contextmanager
from contextvars import ContextVar

SOURCE_VERIFIED = 'M'
SOURCE_DECLARED = 'D'
# Identite declaree d'un scelle ouvert par l'etiquette interieure, sans badge (pas un nom de personne)
ANONYMOUS_OPENING = 'Anonyme (étiquette intérieure)'


def format_identity_verified(matricule: str) -> str:
    """À utiliser quand l'utilisateur s'est authentifié via QR code (matricule)."""
    from .models import Secouristes
    if not Secouristes.objects.filter(matricule=matricule).exists():
        raise ValueError(f"Matricule inconnu : {matricule}")
    return f"{SOURCE_VERIFIED}:{matricule}"


def format_identity_declared(name: str) -> str:
    """À utiliser quand l'utilisateur entre juste son nom, sans vérification."""
    cleaned = name.strip()
    if not cleaned:
        raise ValueError("Nom vide")
    return f"{SOURCE_DECLARED}:{cleaned[:30]}"


def parse_identity(value: str) -> dict:
    """Décode une identité stockée pour affichage/traitement interne."""
    source, _, payload = value.partition(':')
    if source == SOURCE_VERIFIED:
        from .models import Secouristes
        secouriste = Secouristes.objects.filter(matricule=payload).first()
        return {
            'verified': True,
            'matricule': payload,
            'display_name': str(secouriste) if secouriste else f"Matricule inconnu ({payload})",
        }
    return {
        'verified': False,
        'display_name': payload,
    }


def identity_from_request(data, front_user=None, allow_declared=False):
    """Determine l'identite a associer a une operation, None si personne n'est identifie.

    - Poste (API locale ou distante) : l'utilisateur connecte, reconnu par son jeton de session ; le
      matricule envoye dans `user` n'est pas cru sur parole.
    - API publique : `user` doit etre {"matricule": ..., "key": ...} et la cle est verifiee, ou
      {"matricule": ..., "session": ...} (jeton de connexion, apres une connexion par nom et PIN).
    - A defaut, et seulement si `allow_declared` (reglage du serveur), `name` donne une identite declaree.
    """
    from .sessions import credentials_user
    if front_user is not None:
        return f"{SOURCE_VERIFIED}:{front_user.matricule}"
    secouriste = credentials_user(data.get('user'))
    if secouriste is not None:
        return f"{SOURCE_VERIFIED}:{secouriste.matricule}"
    name = data.get('name')
    if allow_declared and isinstance(name, str) and name.strip():
        return format_identity_declared(name)
    return None


# Noms anonymises de l'API publique : prenom seul, plus le debut du nom quand le prenom est partage
# (Michel / Julien D. / Julien S. ; Julien De. et Julien Du. si un Julien Deraine s'ajoute a Julien Dubois).
# Actif pendant une requete de l'API publique (voir ApiRoleMiddleware) : None hors de ce contexte, sinon le
# dictionnaire matricule -> nom court, calcule a la premiere utilisation.
_public_names = ContextVar('qrprotec_public_names', default=None)


def _fold(text: str) -> str:
    """Comparaison sans casse ni accents (Émile = emile)."""
    return ''.join(c for c in unicodedata.normalize('NFKD', text) if not unicodedata.combining(c)).casefold().strip()


def short_names(people) -> dict:
    """{cle: nom court} pour des (cle, prenom, nom) : prenom, suivi du plus court debut de nom qui le distingue
    des homonymes de meme prenom (« Julien D. »), ou du nom entier s'il n'y en a pas (« Julien Dubois »)."""
    groups = {}
    for key, prenom, nom in people:
        groups.setdefault(_fold(prenom), []).append((key, prenom.strip(), nom.strip()))
    names = {}
    for group in groups.values():
        for key, prenom, nom in group:
            others = [_fold(other) for other_key, _, other in group if other_key != key]
            if not others or not nom:
                names[key] = prenom
                continue
            length = next((n for n in range(1, len(nom) + 1)
                           if not any(other.startswith(_fold(nom[:n])) for other in others)), len(nom))
            prefix = nom[:length].rstrip(' -')
            # homonyme complet, ou nom qui commence un autre nom (Dubois / Duboisier) : nom entier
            names[key] = f"{prenom} {nom}" if len(prefix) >= len(nom) else f"{prenom} {prefix}."
    return names


def public_names() -> dict:
    """{matricule: nom court} de tous les secouristes, anciens compris (ils restent dans l'historique)."""
    from .models import Secouristes
    return short_names(Secouristes.objects.values_list('matricule', 'prenom', 'nom'))


@contextmanager
def anonymized():
    """Les noms renvoyes par identity_name sont anonymises dans ce bloc."""
    token = _public_names.set({})
    try:
        yield
    finally:
        _public_names.reset(token)


def is_anonymized() -> bool:
    return _public_names.get() is not None


def _names() -> dict:
    cache = _public_names.get()
    if not cache:
        cache.update(public_names() or {'': ''})  # jamais vide une fois calcule
    return cache


def declared_short_name(name: str) -> str:
    """Identite declaree (texte libre, « Jean Martin ») : premier mot, plus l'initiale de la suite."""
    words = name.split()
    if len(words) <= 1 or name == ANONYMOUS_OPENING:
        return name.strip()
    return f"{words[0]} {words[1][0].upper()}."


def identity_name(value: str) -> str:
    """Nom affiche d'une identite stockee (M:matricule ou D:nom) : complet, ou court sur l'API publique."""
    if not value:
        return ''
    if not is_anonymized():
        return parse_identity(value)['display_name']
    source, _, payload = value.partition(':')
    if source == SOURCE_VERIFIED:
        return _names().get(payload, 'Inconnu')
    return declared_short_name(payload)
