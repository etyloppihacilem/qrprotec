#!/usr/bin/env python3
"""Ecrit un requirements.txt (versions figees + empreintes) a partir de poetry.lock.

Permet d'embarquer exactement les dependances verrouillees par poetry dans le RPM, sans poetry.

    python3 packaging/lock2requirements.py poetry.lock > requirements.txt
"""

import sys
import tomllib


def main(path):
    with open(path, 'rb') as handle:
        lock = tomllib.load(handle)
    for package in lock.get('package', []):
        if 'main' not in package.get('groups', ['main']):
            continue
        line = f"{package['name']}=={package['version']}"
        if package.get('markers'):
            line += f" ; {package['markers']}"
        hashes = [entry['hash'] for entry in package.get('files', []) if entry.get('hash')]
        line += ''.join(f" \\\n    --hash={value}" for value in hashes)
        print(line)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'poetry.lock')
