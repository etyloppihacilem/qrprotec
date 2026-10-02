#!/bin/bash
# Prepare les sources du RPM dans dist/sources :
#   qrprotec-VERSION.tar.gz         code (front + back + packaging) avec les sous-modules utiles
#   qrprotec-vendor-VERSION.tar.gz  dependances Python figees par poetry.lock (Django, DRF...)
#   qrprotec*.sysusers              utilisateurs (pour le %pre du spec)
# Le spec se construit ensuite sans reseau (rpmbuild, mock, COPR).
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

version=$(packaging/version.sh)
# Nom de fichier : le ^ des instantanes est accepte par rpm mais on l'evite dans les tarballs
tar_version=${version//^/_}
out=${1:-$root/dist/sources}
mkdir -p "$out"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Sous-modules necessaires a la compilation du front (le SDK C++ Inateck n'est pas utilise)
git submodule update --init app/imgui app/implot app/scanner_lib

# Fichiers suivis ou nouveaux (non ignores) du depot, plus ceux des sous-modules : un `make rpm`
# fonctionne aussi avant de committer.
list_files() {
    local submodules path
    submodules=$(git config --file .gitmodules --get-regexp '\.path$' | awk '{print $2}')
    git ls-files -z --cached --others --exclude-standard | while IFS= read -r -d '' path; do
        grep -qxF "$path" <<<"$submodules" && continue
        [[ $path == .github/* || ! -f $path ]] && continue
        printf '%s\0' "$path"
    done
    for path in app/imgui app/implot app/scanner_lib; do
        # binaires macOS / Windows du SDK Inateck inutiles sous Fedora
        git -C "$path" ls-files -z | sed -z "s|^|$path/|" | grep -zEv '^app/scanner_lib/ble/(mac|windows)/' || :
    done
}

prefix=qrprotec-$tar_version
list_files | tar --create --gzip --null --files-from=- \
    --transform "s|^|$prefix/|" --owner=0 --group=0 --numeric-owner \
    --file "$out/$prefix.tar.gz"

python3 packaging/lock2requirements.py poetry.lock >"$work/requirements.txt"
# Roues pures Python (py3-none-any) : --python-version fixe la cible quel que soit l'hote (la
# compatibilite avec Python >= 3.13 est garantie par poetry.lock)
python3 -m pip install --quiet --disable-pip-version-check --no-compile --no-deps \
    --require-hashes --only-binary=:all: --python-version 3.13 --ignore-requires-python \
    --target "$work/vendor" -r "$work/requirements.txt"
rm -rf "$work/vendor/bin"
find "$work/vendor" -name __pycache__ -type d -prune -exec rm -rf {} +
tar --create --gzip --owner=0 --group=0 --numeric-owner -C "$work" \
    --file "$out/qrprotec-vendor-$tar_version.tar.gz" vendor

cp packaging/files/qrprotec.sysusers packaging/files/qrprotec-kiosk.sysusers "$out/"

echo "$version"
