#!/bin/bash
# Build MGS4CRT.asi with MinGW into ./build. With --install (or --install --ini) also copy it
# (plus the ini, if not already there) next to Sunny Side Up in the game's scripts folder,
# where Ultimate ASI Loader picks it up.
#
#   MGS4_GAME_DIR   the game folder that holds MGS4/ and Launcher/
#                   (default: the standard Steam library location)
set -eu
cd "$(dirname "$0")"
GAME_DIR="${MGS4_GAME_DIR:-$HOME/.local/share/Steam/steamapps/common/METAL GEAR SOLID 4 - Guns of the Patriots - Master Collection Edition}"
DEST="$GAME_DIR/MGS4/scripts"
mkdir -p ../build
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -shared -static-libgcc -s -o ../build/MGS4CRT.asi mgs4crt.c
echo "built ../build/MGS4CRT.asi"
[ "${1:-}" = "--install" ] || exit 0
[ -d "$DEST" ] || { echo "No scripts folder at $DEST (install Sunny Side Up first, or set MGS4_GAME_DIR)"; exit 1; }
# Replace via rename: a running game may have the old file mapped.
cp -f ../build/MGS4CRT.asi "$DEST/MGS4CRT.asi.new" && mv -f "$DEST/MGS4CRT.asi.new" "$DEST/MGS4CRT.asi"
[ -f "$DEST/MGS4CRT.ini" ] && [ "${2:-}" != "--ini" ] || cp MGS4CRT.ini "$DEST/"   # --ini overwrites
echo "installed to $DEST"
