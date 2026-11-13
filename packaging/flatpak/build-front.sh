#!/bin/sh
# Construction, tests et installation du front dans le bac a sable de flatpak-builder (module qrprotec-front de
# io.github.etyloppihacilem.QRProtec.yml).
#
# Un seul script plutot qu'une liste de build-commands : flatpak-builder ouvre un nouveau bac a sable bwrap pour
# chaque commande, et chaque ouverture relit /etc/resolv.conf de la machine. Si ce fichier disparait un instant
# pendant le build (NetworkManager qui le reecrit, VPN, Wi-Fi qui se reconnecte, lien symbolique casse), la
# commande suivante echoue avant meme de demarrer : « bwrap: Can't bind mount /etc/resolv.conf ». Une seule
# ouverture rend ce cas tres improbable. Rien ici n'a besoin du reseau (les tests C++ non plus).
set -eu

: "${FLATPAK_ID:=io.github.etyloppihacilem.QRProtec}"
: "${FLATPAK_DEST:=/app}"
: "${FLATPAK_ARCH:=$(uname -m)}"
PREFIX=$FLATPAK_DEST

cmake -S app -B _build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_SKIP_BUILD_RPATH=ON
cmake --build _build
ctest --test-dir _build --output-on-failure

# Memes emplacements que le RPM, sous /app, pour reprendre le lanceur packaging/files/qrprotec-front
install -Dm755 _build/QRProtecApp "$PREFIX/libexec/qrprotec/qrprotec-front"
# polices a cote de l'executable (fonts.cpp les cherche la)
install -Dm644 -t "$PREFIX/libexec/qrprotec/fonts" app/third_party/fonts/*.ttf
install -Dm755 packaging/files/qrprotec-front "$PREFIX/bin/qrprotec-front"
sed -i -e "s|@LIBEXECDIR@|$PREFIX/libexec|g" -e "s|@LIBDIR@|$PREFIX/lib|g" -e "s|@DATADIR@|$PREFIX/share|g" \
    "$PREFIX/bin/qrprotec-front"

# SDK de la douchette : x86_64 seulement (sinon, front en mode clavier seul, comme le RPM)
if [ "$FLATPAK_ARCH" = x86_64 ]; then
    install -Dm755 app/scanner_lib/ble/linux/x86_64-unknown-linux-gnu/libinateck_scanner_ble.so \
        "$PREFIX/lib/qrprotec/libinateck_scanner_ble.so"
fi

# Modeles d'etiquettes fournis, copies au premier lancement dans ~/.var/app/<id>/data/qrprotec/templates
install -Dm644 -t "$PREFIX/share/qrprotec/templates" app/templates/*.qr
install -Dm644 -t "$PREFIX/share/qrprotec/templates/images" app/templates/images/*

# Menu et icone (celle du site web). StartupWMClass=qrprotec relie la fenetre X11 a l'entree.
install -Dm644 packaging/files/qrprotec.desktop "$PREFIX/share/applications/$FLATPAK_ID.desktop"
sed -i "s|^Icon=qrprotec$|Icon=$FLATPAK_ID|" "$PREFIX/share/applications/$FLATPAK_ID.desktop"
install -Dm644 database/inventory/web/icon-512.png "$PREFIX/share/icons/hicolor/512x512/apps/$FLATPAK_ID.png"
install -Dm644 database/inventory/web/icon-192.png "$PREFIX/share/icons/hicolor/192x192/apps/$FLATPAK_ID.png"
install -Dm644 database/inventory/web/favicon.png "$PREFIX/share/icons/hicolor/32x32/apps/$FLATPAK_ID.png"
