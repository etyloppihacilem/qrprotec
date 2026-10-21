#!/usr/bin/env python3
"""Ecrit les macros rpm des dependances Python embarquees, a partir du dossier vendor/.

    python3 packaging/vendor-macros.py vendor > qrprotec-vendor.macros

Le spec les charge (Source4) pour ses Provides bundled() et les verifie dans %prep : une mise a jour
de poetry.lock (nouvelle version de Django, nouvelle dependance...) ne demande pas de retoucher le spec.
"""

import re
import sys
from email.parser import HeaderParser
from pathlib import Path


def main(vendor):
    dists = []
    for metadata in sorted(Path(vendor).glob('*.dist-info/METADATA')):
        headers = HeaderParser().parsestr(metadata.read_text(encoding='utf-8'))
        # Nom normalise comme python3dist() (PEP 503)
        name = re.sub(r'[-_.]+', '-', headers['Name']).lower()
        dists.append((name, headers['Version'], metadata.parent.name))
    if not dists:
        sys.exit(f'{vendor} : aucune dependance embarquee')
    provides = ', '.join(f'bundled(python3dist({name})) = {version}' for name, version, _ in dists)
    print(f'%vendor_provides {provides}')
    print(f"%vendor_dist_infos {' '.join(dist_info for _, _, dist_info in dists)}")


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'vendor')
