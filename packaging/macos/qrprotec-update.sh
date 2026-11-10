#!/bin/sh
# Mise a jour de QRProtec.app depuis le .dmg d'une release (lance par l'application, voir
# app/src/app/updater.cpp ; copie dans ~/Library/Caches/QRProtec/updates avant d'etre execute).
#
#   qrprotec-update.sh install PID DMG APP RELANCE ADMIN JOURNAL
#       attend la fin du processus PID, remplace APP (…/QRProtec.app) par celui de DMG, puis relance
#       l'application si RELANCE=1. ADMIN=1 : dossier de l'application non modifiable, le remplacement
#       passe par le mot de passe d'un administrateur (osascript).
#   qrprotec-update.sh replace DMG APP
#       remplacement seul (sous root si ADMIN=1).
#
# Rien n'est remplace si le .dmg ne contient pas un QRProtec.app intact (identifiant fr.qrprotec.front,
# signature valide). L'ancien .app est remis en place si le remplacement echoue.
set -u

bundle_id=fr.qrprotec.front

replace() {
    dmg=$1
    app=$2
    case $app in
        *.app) ;;
        *) echo "chemin inattendu : $app"; return 1 ;;
    esac
    mnt=$(mktemp -d "${TMPDIR:-/tmp}/qrprotec-update.XXXXXX") || return 1
    if ! hdiutil attach -quiet -nobrowse -readonly -noautoopen -mountpoint "$mnt" "$dmg"; then
        rmdir "$mnt"
        echo "image disque illisible : $dmg"
        return 1
    fi
    status=0
    new=$mnt/QRProtec.app
    dir=$(dirname "$app")
    staging=$dir/.QRProtec-update.app
    old=$dir/.QRProtec-old.app
    if [ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$new/Contents/Info.plist" 2>/dev/null)" != "$bundle_id" ]; then
        echo "QRProtec.app absent du .dmg ou identifiant inattendu"
        status=1
    elif ! codesign --verify --deep --strict "$new"; then
        echo "signature du nouveau QRProtec.app invalide"
        status=1
    else
        rm -rf "$staging" "$old"
        if ! ditto "$new" "$staging"; then
            echo "copie impossible dans $dir"
            status=1
        elif ! mv "$app" "$old"; then
            echo "ancien QRProtec.app impossible a deplacer"
            status=1
        elif ! mv "$staging" "$app"; then
            echo "nouveau QRProtec.app impossible a mettre en place : ancien remis"
            mv "$old" "$app"
            status=1
        fi
        rm -rf "$staging" "$old"
    fi
    hdiutil detach -quiet "$mnt" || hdiutil detach -quiet -force "$mnt"
    rmdir "$mnt" 2>/dev/null
    if [ $status -eq 0 ]; then
        # telecharge par l'application (curl) : normalement sans quarantaine, par precaution
        xattr -dr com.apple.quarantine "$app" 2>/dev/null
        echo "QRProtec.app mis a jour"
    fi
    return $status
}

case ${1:-} in
    replace)
        replace "$2" "$3"
        ;;
    install)
        pid=$2 dmg=$3 app=$4 relaunch=$5 admin=$6 log=$7
        exec >>"$log" 2>&1
        echo "$(date) : mise a jour de $app ($dmg)"
        # l'application se ferme
        while kill -0 "$pid" 2>/dev/null; do
            sleep 0.3
        done
        if [ "$admin" = 1 ]; then
            osascript - "$0" "$dmg" "$app" <<'APPLESCRIPT'
on run argv
    do shell script "/bin/sh " & quoted form of (item 1 of argv) & " replace " & quoted form of (item 2 of argv) & " " & quoted form of (item 3 of argv) with prompt "QRProtec installe sa mise à jour." with administrator privileges
end run
APPLESCRIPT
            status=$?
        else
            replace "$dmg" "$app"
            status=$?
        fi
        [ $status -eq 0 ] && rm -f "$dmg"
        # relance meme en cas d'echec (ancienne version), pour ne pas laisser l'utilisateur sans application
        [ "$relaunch" = 1 ] && open "$app"
        exit $status
        ;;
    *)
        echo "usage : $0 install PID DMG APP RELANCE ADMIN JOURNAL | replace DMG APP" >&2
        exit 2
        ;;
esac
