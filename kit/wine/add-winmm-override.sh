#!/bin/bash
# Tell Wine to load the mod's winmm.dll (Ultimate ASI Loader) for MGS4 and its launcher.
#   MGS4_PREFIX   the game's Proton prefix (the folder holding user.reg), e.g.
#                 ~/.local/share/Steam/steamapps/compatdata/<appid>/pfx
set -e
: "${MGS4_PREFIX:?set MGS4_PREFIX to the Proton prefix of the game, .../compatdata/APPID/pfx}"
REG="$MGS4_PREFIX/user.reg"
if pgrep -x wineserver >/dev/null; then echo "wineserver still running, aborting"; exit 1; fi
if grep -q 'AppDefaults\\\\mgs4.exe\\\\DllOverrides' "$REG"; then echo "override already present"; exit 0; fi
cp "$REG" "$REG.bak-before-sunnysideup"
now=$(date +%s)
cat >> "$REG" <<EOT

[Software\\\\Wine\\\\AppDefaults\\\\mgs4.exe\\\\DllOverrides] $now
"winmm"="native,builtin"

[Software\\\\Wine\\\\AppDefaults\\\\launcher.exe\\\\DllOverrides] $now
"winmm"="native,builtin"
EOT
grep -A1 'AppDefaults\\\\\(mgs4\|launcher\).exe\\\\DllOverrides' "$REG"
