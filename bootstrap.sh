#!/bin/bash
# Premiere installation de QRProtec sur Fedora :
#
#   curl -fsSL https://etyloppihacilem.github.io/qrprotec/bootstrap.sh | sudo bash                                   # borne complete
#   curl -fsSL https://etyloppihacilem.github.io/qrprotec/bootstrap.sh | sudo QRPROTEC_PACKAGE=qrprotec-server bash  # back seul
#
# Ajoute le depot dnf de QRProtec puis installe le paquet (QRPROTEC_PACKAGE : qrprotec, defaut,
# qrprotec-server, qrprotec-kiosk ou qrprotec-front). Ensuite : `sudo dnf upgrade` pour les mises a
# jour. QRPROTEC_REPO_URL permet d'utiliser un autre depot.
set -euo pipefail

repo_url=${QRPROTEC_REPO_URL:-https://etyloppihacilem.github.io/qrprotec}
case $repo_url in
    @*) repo_url=https://etyloppihacilem.github.io/qrprotec ;;  # script lance depuis le depot git
esac
repo_file=/etc/yum.repos.d/qrprotec.repo
package=${QRPROTEC_PACKAGE:-qrprotec}

die() { echo "bootstrap : $*" >&2; exit 1; }

case $package in
    qrprotec | qrprotec-server | qrprotec-kiosk | qrprotec-front) ;;
    *) die "QRPROTEC_PACKAGE inconnu : $package (qrprotec, qrprotec-server, qrprotec-kiosk, qrprotec-front)" ;;
esac
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

echo "== Installation de $package"
dnf -y makecache --repo qrprotec
dnf -y install "$package"

echo
echo "$package est installe. Etapes suivantes :"
case $package in
    qrprotec)
        echo "  1. sudo qrprotec-setup     # nom d'hote, certificat HTTPS, clavier, pare-feu, premier admin"
        echo "  2. sudo systemctl reboot   # demarre directement sur le kiosk (tty1)" ;;
    qrprotec-server)
        echo "  1. sudo qrprotec-setup                   # nom d'hote, certificat HTTPS, pare-feu, premier admin"
        echo "  2. sudo qrprotec-setup --front-key NOM   # cle API de chaque front d'une autre machine" ;;
    qrprotec-kiosk)
        echo "  1. sudo qrprotec-setup --api-url https://SERVEUR --api-key CLE   # back distant"
        echo "  2. sudo systemctl reboot   # demarre directement sur le kiosk (tty1)" ;;
    qrprotec-front)
        echo "  Lancer QRProtec depuis le menu (ou qrprotec-front), puis regler le serveur :"
        echo "  clic sur « API hors ligne » (URL https://SERVEUR, cle du front)" ;;
esac
echo "Mises a jour : sudo dnf upgrade"

# Assistant tout de suite si un terminal est disponible (curl | bash : stdin n'est pas le terminal)
if [[ $package != qrprotec-front ]] && [[ -r /dev/tty && -w /dev/tty ]] && { : </dev/tty; } 2>/dev/null; then
    read -r -p "Lancer qrprotec-setup maintenant ? [O/n] " answer </dev/tty || answer=n
    if [[ ${answer:-o} =~ ^[oOyY] ]]; then
        qrprotec-setup </dev/tty
    fi
fi
