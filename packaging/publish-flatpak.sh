#!/bin/bash
# Publie le Flatpak du front dans un depot Flatpak statique (dossier flatpak/ de la branche gh-pages servie par
# GitHub Pages, a cote du depot dnf). Les postes qui l'ont installe depuis ce depot (ou depuis le .flatpak de la
# release, qui pointe dessus) recoivent les nouvelles versions par `flatpak update`, et automatiquement par
# Logiciels (GNOME) ou Discover (KDE).
#
#   packaging/publish-flatpak.sh PAGES_DIR BASE_URL DEPOT_SOURCE [BUNDLE]
#
# PAGES_DIR    : copie de travail de la branche gh-pages
# BASE_URL     : URL publique de PAGES_DIR (ex : https://etyloppihacilem.github.io/qrprotec)
# DEPOT_SOURCE : depot OSTree construit par flatpak-builder (--repo)
# BUNDLE       : en plus, .flatpak a joindre a la release, lie au depot (mises a jour) et a Flathub (runtime)
#
# Depot signe obligatoire : GPG_KEY_ID (cle secrete deja importee dans gpg, la meme que celle du depot dnf, sans
# phrase de passe a saisir : gpg-agent prechargee). Les FLATPAK_KEEP (3) derniers commits sont gardes
# (`flatpak update --commit=...` pour revenir en arriere).
set -euo pipefail

pages=${1:?PAGES_DIR}
base_url=${2:?BASE_URL}
source_repo=${3:?DEPOT_SOURCE}
bundle=${4:-}
app_id=io.github.etyloppihacilem.QRProtec
branch=${FLATPAK_BRANCH:-master}
keep=${FLATPAK_KEEP:-3}
flathub=https://dl.flathub.org/repo/flathub.flatpakrepo

if [[ -z ${GPG_KEY_ID:-} ]]; then
    echo "publish-flatpak : GPG_KEY_ID manquant, depot non signe refuse (secret RPM_GPG_PRIVATE_KEY)" >&2
    exit 1
fi
sign=(--gpg-sign="$GPG_KEY_ID")
if [[ -n ${GNUPGHOME:-} ]]; then
    sign+=(--gpg-homedir="$GNUPGHOME")
fi

repo=$pages/flatpak
if [[ ! -f $repo/config ]]; then
    ostree init --mode=archive-z2 --repo="$repo"
fi

# L'application et ses extensions (.Debug, .Locale) construites par flatpak-builder
mapfile -t refs < <(ostree refs --repo="$source_repo" | grep -E "^(app|runtime)/${app_id//./\\.}([./]|$)")
if ((${#refs[@]} == 0)); then
    echo "publish-flatpak : $app_id absent de $source_repo" >&2
    exit 1
fi
flatpak build-commit-from --src-repo="$source_repo" "${sign[@]}" --no-update-summary "$repo" "${refs[@]}"
flatpak build-update-repo "${sign[@]}" --title=QRProtec --default-branch="$branch" \
    --generate-static-deltas --prune --prune-depth="$keep" "$repo"

gpg --export "$GPG_KEY_ID" >"$pages/qrprotec-flatpak.gpg"
key=$(base64 -w0 <"$pages/qrprotec-flatpak.gpg")

# Depot seul : flatpak remote-add --user qrprotec URL/qrprotec.flatpakrepo
cat >"$pages/qrprotec.flatpakrepo" <<REPO
[Flatpak Repo]
Title=QRProtec
Url=$base_url/flatpak/
Homepage=https://github.com/etyloppihacilem/qrprotec
Comment=Front de bureau QRProtec
GPGKey=$key
REPO

# Installation en une commande (ou un double-clic) : flatpak install --user URL/qrprotec.flatpakref
cat >"$pages/qrprotec.flatpakref" <<REF
[Flatpak Ref]
Name=$app_id
Branch=$branch
Title=QRProtec
Url=$base_url/flatpak/
SuggestRemoteName=qrprotec
RuntimeRepo=$flathub
IsRuntime=false
GPGKey=$key
REF

if [[ -n $bundle ]]; then
    flatpak build-bundle --repo-url="$base_url/flatpak/" --runtime-repo="$flathub" \
        --gpg-keys="$pages/qrprotec-flatpak.gpg" "$repo" "$bundle" "$app_id" "$branch"
fi
touch "$pages/.nojekyll"
