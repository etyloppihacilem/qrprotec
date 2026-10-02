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

SOURCE_VERIFIED = 'M'
SOURCE_DECLARED = 'D'


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


def identity_from_request(data, front_user=None) -> str:
    """Determine l'identite a associer a une operation.

    - Poste (API locale ou distante) : l'utilisateur connecte, reconnu par son jeton de session ; le
      matricule envoye dans `user` n'est plus cru sur parole.
    - API publique : `user` doit etre {"matricule": ..., "key": ...} et la cle est verifiee.
    - A defaut, `name` est utilise comme identite declaree.
    """
    from .models import Secouristes
    if front_user is not None:
        return f"{SOURCE_VERIFIED}:{front_user.matricule}"
    user = data.get('user')
    if isinstance(user, dict):
        secouriste = Secouristes.objects.filter(matricule=str(user.get('matricule', ''))).first()
        if secouriste and secouriste.check_key(user.get('key')):
            return f"{SOURCE_VERIFIED}:{secouriste.matricule}"
    name = data.get('name')
    if isinstance(name, str) and name.strip():
        return format_identity_declared(name)
    return format_identity_declared('anonyme')
