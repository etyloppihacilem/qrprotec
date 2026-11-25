#!/bin/bash
# Pousse la copie de travail de la branche gh-pages (depot dnf + depot Flatpak) en UN SEUL commit, sans
# historique : la branche ne contient que le dernier etat publie. Les RPM et objets Flatpak sont des binaires
# qui ne se compressent pas ; garder chaque publication dans l'historique faisait grossir le depot git de
# ~65 Mo par release (1,2 Go en novembre 2026) pour rien, les versions utiles etant deja dans l'arbre.
#
#   packaging/push-pages.sh PAGES_DIR MESSAGE
#
# PAGES_DIR : clone de gh-pages (remote origin) ou depot cree par `git init -b gh-pages` (premiere publication)
# Pousse en --force-with-lease sur le commit clone : si une autre publication est passee entre-temps, le push
# echoue (code 1) et l'appelant recommence depuis un nouveau clone, sans ecraser cette publication.
set -euo pipefail

pages=${1:?PAGES_DIR}
message=${2:?MESSAGE}
cd "$pages"

# commit clone (vide a la premiere publication : la branche ne doit alors pas exister)
lease=$(git rev-parse -q --verify HEAD || true)

git checkout -q --orphan qrprotec-pages-tmp
git add -A
git -c user.name="github-actions[bot]" -c user.email="41898282+github-actions[bot]@users.noreply.github.com" \
    commit -q -m "$message"
git push --force-with-lease="gh-pages:$lease" origin HEAD:refs/heads/gh-pages
