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
    from .models import Secouriste
    if not Secouriste.objects.filter(matricule=matricule).exists():
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
        from .models import Secouriste
        secouriste = Secouriste.objects.filter(matricule=payload).first()
        return {
            'verified': True,
            'matricule': payload,
            'display_name': str(secouriste) if secouriste else f"Matricule inconnu ({payload})",
        }
    return {
        'verified': False,
        'display_name': payload,
    }
