#!/bin/bash
# Ajoute des RPM a un depot dnf statique (branche gh-pages servie par GitHub Pages).
#
#   packaging/publish-repo.sh PAGES_DIR BASE_URL RPM...
#
# PAGES_DIR : copie de travail de la branche gh-pages
# BASE_URL  : URL publique de ce depot (ex : https://etyloppihacilem.github.io/qrprotec)
# Les RPM vont dans PAGES_DIR/fedora/<version Fedora>/<arch>/ (version lue dans la release, ex
# 1.fc43). Les KEEP_VERSIONS (5) dernieres versions de chaque paquet sont gardees (dnf downgrade possible).
# Si GPG_KEY_ID est defini, la cle publique est publiee et gpgcheck active dans qrprotec.repo.
set -euo pipefail

pages=${1:?PAGES_DIR}
base_url=${2:?BASE_URL}
shift 2
keep=${KEEP_VERSIONS:-5}
root=$(cd "$(dirname "$0")/.." && pwd)

declare -A touched=()
for rpm_file in "$@"; do
    [[ $rpm_file == *.src.rpm ]] && continue
    release=$(rpm -qp --qf '%{RELEASE}' "$rpm_file")
    arch=$(rpm -qp --qf '%{ARCH}' "$rpm_file")
    if [[ ! $release =~ \.fc([0-9]+) ]]; then
        echo "publish-repo : $rpm_file n'est pas un RPM Fedora (release $release)" >&2
        exit 1
    fi
    dir=$pages/fedora/${BASH_REMATCH[1]}/$arch
    mkdir -p "$dir"
    cp -f "$rpm_file" "$dir/"
    touched[$dir]=1
done

for dir in "${!touched[@]}"; do
    # Garde, pour chaque paquet (nom), les $keep versions les plus recentes (date de construction)
    mapfile -t old < <(for f in "$dir"/*.rpm; do
        printf '%s %s %s\n' "$(rpm -qp --qf '%{NAME}' "$f")" "$(rpm -qp --qf '%{BUILDTIME}' "$f")" "$f"
    done | sort -k1,1 -k2,2nr | awk -v keep="$keep" '++seen[$1] > keep { print $3 }')
    if ((${#old[@]})); then
        rm -f "${old[@]}"
    fi
    createrepo_c --quiet --update "$dir"
done

gpgcheck=0
if [[ -n ${GPG_KEY_ID:-} ]]; then
    gpg --armor --export "$GPG_KEY_ID" >"$pages/RPM-GPG-KEY-qrprotec"
    gpgcheck=1
fi

cat >"$pages/qrprotec.repo" <<REPO
[qrprotec]
name=QRProtec - Fedora \$releasever - \$basearch
baseurl=$base_url/fedora/\$releasever/\$basearch/
enabled=1
gpgcheck=$gpgcheck
gpgkey=$base_url/RPM-GPG-KEY-qrprotec
metadata_expire=1h
skip_if_unavailable=False
REPO

sed "s|@REPO_URL@|$base_url|g" "$root/bootstrap.sh" >"$pages/bootstrap.sh"
touch "$pages/.nojekyll"
cat >"$pages/index.html" <<HTML
<!doctype html>
<html lang="fr"><head><meta charset="utf-8"><title>Depot dnf QRProtec</title></head>
<body>
<h1>Depot dnf QRProtec</h1>
<p>Installation sur Fedora Server :</p>
<pre>curl -fsSL $base_url/bootstrap.sh | sudo bash</pre>
<p>Fichier de depot : <a href="qrprotec.repo">qrprotec.repo</a></p>
</body></html>
HTML
