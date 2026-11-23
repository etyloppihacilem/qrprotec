"""Version de release du serveur, affichee dans les Reglages (web et poste, via /api/health/).

Le RPM ecrit la version de son tag (packaging/version.sh : 1.2.0, ou 1.2.0^3.gabc1234 entre deux tags) dans le
fichier VERSION a cote de ce module. Sans ce fichier (copie de travail, tests) : chaine vide, « version de
developpement » cote affichage."""

from __future__ import annotations

from functools import lru_cache
from pathlib import Path

VERSION_FILE = Path(__file__).with_name('VERSION')


@lru_cache(maxsize=1)
def release_version() -> str:
    try:
        return VERSION_FILE.read_text(encoding='utf-8').strip()
    except OSError:
        return ''
