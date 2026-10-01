#!/bin/bash
# Premiere installation de la borne QRProtec sur Fedora Server :
#
#   curl -fsSL @REPO_URL@/bootstrap.sh | sudo bash
#
# Ajoute le depot dnf de QRProtec puis installe le paquet. Ensuite : `sudo dnf upgrade` pour les
# mises a jour. QRPROTEC_REPO_URL permet d'utiliser un autre depot.
set -euo pipefail

repo_url=${QRPROTEC_REPO_URL:-@REPO_URL@}
case $repo_url in
    @*) repo_url=https://etyloppihacilem.github.io/qrprotec ;;  # script lance depuis le depot git
esac
repo_file=/etc/yum.repos.d/qrprotec.repo

die() { echo "bootstrap : $*" >&2; exit 1; }

[[ $(id -u) -eq 0 ]] || die "a lancer en root : curl -fsSL $repo_url/bootstrap.sh | sudo bash"
# shellcheck source=/dev/null
. /etc/os-release
[[ ${ID:-} == fedora ]] || die "Fedora requis (systeme detecte : ${PRETTY_NAME:-inconnu})"
((${VERSION_ID:-0} >= 42)) || die "Fedora 42 ou plus recent requis (version ${VERSION_ID:-?})"

echo "== Ajout du depot $repo_url"
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
if curl -fsSL "$repo_url/qrprotec.repo" -o "$tmp"; then
    install -m 0644 "$tmp" "$repo_file"
else
    die "impossible de telecharger $repo_url/qrprotec.repo"
fi

echo "== Installation de qrprotec"
dnf -y makecache --repo qrprotec
dnf -y install qrprotec

cat <<DONE

QRProtec est installe. Etapes suivantes :
  1. sudo qrprotec-setup     # nom d'hote, certificat HTTPS, clavier, pare-feu, premier admin
  2. sudo systemctl reboot   # demarre directement sur le kiosk (tty1)
Mises a jour : sudo dnf upgrade
DONE

# Assistant tout de suite si un terminal est disponible (curl | bash : stdin n'est pas le terminal)
if [[ -r /dev/tty && -w /dev/tty ]] && { : </dev/tty; } 2>/dev/null; then
    read -r -p "Lancer qrprotec-setup maintenant ? [O/n] " answer </dev/tty || answer=n
    if [[ ${answer:-o} =~ ^[oOyY] ]]; then
        qrprotec-setup </dev/tty
    fi
fi
