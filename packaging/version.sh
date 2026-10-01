#!/bin/bash
# Version RPM derivee du tag git :
#   v1.2.0                 -> 1.2.0
#   v1.2.0 + 3 commits     -> 1.2.0^3.gabc1234   (instantane, plus recent que 1.2.0, plus ancien que 1.2.1)
#   aucun tag              -> 0.0.0^<n>.g<sha>
# QRPROTEC_VERSION impose une valeur (ex : CI).
set -euo pipefail

if [[ -n ${QRPROTEC_VERSION:-} ]]; then
    echo "${QRPROTEC_VERSION#v}"
    exit 0
fi

cd "$(dirname "$0")/.."
if describe=$(git describe --tags --long --match 'v[0-9]*' 2>/dev/null); then
    # v1.2.0-3-gabc1234
    if [[ $describe =~ ^v(.+)-([0-9]+)-g([0-9a-f]+)$ ]]; then
        version=${BASH_REMATCH[1]//-/_}
        commits=${BASH_REMATCH[2]}
        sha=${BASH_REMATCH[3]}
    else
        echo "version.sh : sortie inattendue de git describe : $describe" >&2
        exit 1
    fi
else
    version=0.0.0
    commits=$(git rev-list --count HEAD 2>/dev/null || echo 0)
    sha=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
fi

if [[ $commits == 0 ]]; then
    echo "$version"
else
    echo "${version}^${commits}.g${sha}"
fi
