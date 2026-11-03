#!/bin/sh
# Copies the Vita build to the console, as it is installed there:
#
#   ux0:app/IGNT00001/           eboot.bin, sce_sys/, shaders/ (the package)
#   ux0:data/ignition/ignition.vpk   the package, for VitaShell
#   ux0:data/ignition/           the game's files (the GOG or Steam release),
#                                or disc/ with an extracted disc, if not there
#
#   tools/vita_deploy.sh pull    shaders the Vita compiled ->
#                                platforms/vita/shaders/
#   tools/vita_deploy.sh push    build-vita/ignition.vpk -> the Vita
#
# The Vita is either a mounted ux0 (its memory card in a reader, or
# VitaShell's USB mode): VITA_UX0, or the first volume in /Volumes with
# app/, data/ and id.dat; or VitaShell's FTP server (Select in VitaShell):
# VITA_IP, port VITA_PORT (1337). DATA is the extracted disc (default the
# one `make` runs). VITA_MUSIC=1 also copies the .cue and .bin files for
# the CD music.
#
# A new install has no LiveArea bubble until VitaShell installs
# ux0:data/ignition/ignition.vpk once (or "Refresh LiveArea"); after
# that a push replaces the files in place.

set -e

TITLE=IGNT00001
VPK=build-vita/ignition.vpk
SHADERS=platforms/vita/shaders
# The first image folder under data/ by name, as the game picks it.
first_disc() {
    for d in data/*/disc data/*/*/disc; do
        [ -d "$d" ] && echo "${#d} $d"
    done | LC_ALL=C sort -n | head -n 1 | cut -d' ' -f2-
}
: "${DATA:=$(first_disc)}"
: "${VITA_PORT:=1337}"
# macOS's cp -X leaves out extended attributes, so no "._" files land on
# the card.
NOX=
[ "$(uname)" = Darwin ] && NOX=-X

find_ux0() {
    if [ -n "$VITA_UX0" ]; then
        echo "$VITA_UX0"
        return
    fi
    for v in /Volumes/*; do
        if [ -d "$v/app" ] && [ -d "$v/data" ] && [ -f "$v/id.dat" ]; then
            echo "$v"
            return
        fi
    done
}

UX0=$(find_ux0)
if [ -z "$UX0" ] && [ -z "$VITA_IP" ]; then
    echo "vita_deploy: no Vita: mount its ux0 (memory card, or VitaShell's USB mode) or" >&2
    echo "  start VitaShell's FTP server and set VITA_IP (and VITA_PORT, default 1337)" >&2
    exit 1
fi
FTP="ftp://$VITA_IP:$VITA_PORT"

pull() {
    mkdir -p "$SHADERS"
    if [ -n "$UX0" ]; then
        for g in "$UX0"/data/ignition/shaders/*.gxp; do
            [ -f "$g" ] || continue
            n=$(basename "$g")
            if [ ! -f "$SHADERS/$n" ]; then
                cp "$g" "$SHADERS/$n"
                echo "vita_deploy: shader $n"
            fi
        done
    else
        for n in $(curl -s --list-only "$FTP/ux0:/data/ignition/shaders/" | grep '\.gxp$' || true); do
            if [ ! -f "$SHADERS/$n" ]; then
                curl -s -o "$SHADERS/$n" "$FTP/ux0:/data/ignition/shaders/$n"
                echo "vita_deploy: shader $n"
            fi
        done
    fi
}

# put FILE PATH: FILE to ux0:PATH.
put() {
    if [ -n "$UX0" ]; then
        mkdir -p "$(dirname "$UX0/$2")"
        cp $NOX "$1" "$UX0/$2"
    else
        curl -s --ftp-create-dirs -T "$1" "$FTP/ux0:/$2"
    fi
}

# The first image folder (the data's own first) whose .cue has more than
# one track.
music_image() {
    for d in "$(dirname "$DATA")" data/* data/*/*; do
        c=$(ls "$d"/*.cue 2>/dev/null | grep -v '/\._' | head -n 1)
        if [ -n "$c" ] && [ "$(grep -c TRACK "$c")" -gt 1 ]; then
            echo "$d"
            return
        fi
    done
}

push() {
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    unzip -q "$VPK" -d "$tmp"
    (cd "$tmp" && find . -type f | sed 's|^\./||') | while read -r f; do
        put "$tmp/$f" "app/$TITLE/$f"
    done
    put "$VPK" data/ignition/ignition.vpk
    echo "vita_deploy: $VPK -> ux0:app/$TITLE${UX0:+ on $UX0}${VITA_IP:+ at $VITA_IP}"

    if [ -n "$UX0" ] && [ -d "$DATA" ] && [ ! -d "$UX0/data/ignition/disc" ]; then
        echo "vita_deploy: copying the disc to ux0:data/ignition/disc"
        mkdir -p "$UX0/data/ignition"
        cp -R $NOX "$DATA" "$UX0/data/ignition/disc"
    elif [ -z "$UX0" ]; then
        echo "vita_deploy: over FTP the disc is not copied; copy $DATA to ux0:data/ignition/disc once"
    fi
    if [ "$VITA_MUSIC" = 1 ]; then
        # The music is on the image's audio tracks: an image of the data
        # track alone has none, so take the first image that has some.
        img=$(music_image)
        if [ -f data/game.ins ] && [ -d data/MUSIC ]; then
            # The GOG and Steam releases: game.ins and Ogg Vorbis files.
            put data/game.ins data/ignition/game.ins
            for f in data/MUSIC/*.ogg; do
                [ -f "$f" ] && put "$f" "data/ignition/MUSIC/$(basename "$f")"
            done
            echo "vita_deploy: music from data/MUSIC"
        elif [ -z "$img" ]; then
            echo "vita_deploy: no disc image under data/ has audio tracks" >&2
        else
            for f in "$img"/*.cue "$img"/*.bin; do
                [ -f "$f" ] && put "$f" "data/ignition/$(basename "$f")"
            done
            echo "vita_deploy: CD music from $img"
        fi
    fi
    if [ -n "$UX0" ]; then
        sync
    fi
}

case "$1" in
pull) pull ;;
push) push ;;
*) echo "usage: $0 pull|push" >&2; exit 2 ;;
esac
