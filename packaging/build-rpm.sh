#!/bin/bash
# Construit le RPM (et le SRPM) de QRProtec dans dist/. A lancer sur Fedora, ou via `make rpm`
# qui l'execute dans un conteneur Fedora pour ne rien installer sur la machine de developpement.
#
#   packaging/build-rpm.sh                 # installe les dependances de build si root, puis construit
#   RPMBUILD_ARGS="--without kiosk" packaging/build-rpm.sh
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
dist=$root/dist
topdir=$dist/rpmbuild
spec=packaging/qrprotec.spec
read -r -a extra_args <<<"${RPMBUILD_ARGS:-}"

rm -rf "$topdir"
mkdir -p "$topdir"/{SOURCES,SPECS,BUILD,RPMS,SRPMS}

version=$(packaging/make-sources.sh "$topdir/SOURCES" | tail -n 1)
echo "== QRProtec $version"

if [[ $(id -u) -eq 0 ]] && command -v dnf >/dev/null; then
    dnf -y --setopt=install_weak_deps=False builddep \
        --define "qrprotec_version $version" "${extra_args[@]}" "$spec"
fi

rpmbuild -ba "$spec" \
    --define "_topdir $topdir" \
    --define "qrprotec_version $version" \
    "${extra_args[@]}"

find "$topdir/RPMS" "$topdir/SRPMS" -name '*.rpm' -exec cp -t "$dist" {} +

if command -v rpmlint >/dev/null; then
    rpmlint -c packaging/rpmlint.toml "$spec" "$dist"/*.rpm || echo "rpmlint : voir les avertissements ci-dessus" >&2
fi

ls -1 "$dist"/*.rpm
