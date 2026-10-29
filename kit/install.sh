#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
# Put the MGS4 setup back together (after a reinstall, a new prefix, a lost file):
# Sunny Side Up + ASI loader, the MGS4CRT plugin, the mod configs, the winmm
# override and the vkBasalt look. See ../README.md.
#
#   install.sh           install whatever is missing (never overwrites tuned files)
#   install.sh --force   also overwrite the ini files and vkBasalt configs from here
#   install.sh --check   only report what is in place
#
# Settings (environment):
#   MGS4_APPID     Steam AppId of the game (for a non-Steam shortcut: the shortcut's id).
#                  Used to find the Proton prefix.
#   MGS4_GAME_DIR  game folder holding MGS4/ and Launcher/ (default: standard Steam library)
#   MGS4_PREFIX    Proton prefix (default: <steam>/steamapps/compatdata/$MGS4_APPID/pfx)
#   SSU_ZIP        path to the Sunny Side Up 1.0.7 zip (default: third_party/SunnySideUp-1.0.7.zip)
#   HYPR_CONFIG    Hyprland Lua config to check (default: ~/.config/hypr/hyprland.lua)
# ─────────────────────────────────────────────────────────────────────────────
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
STEAM="$HOME/.local/share/Steam/steamapps"
APPID="${MGS4_APPID:-}"
GAME_DIR="${MGS4_GAME_DIR:-$STEAM/common/METAL GEAR SOLID 4 - Guns of the Patriots - Master Collection Edition}"
PREFIX="${MGS4_PREFIX:-${APPID:+$STEAM/compatdata/$APPID/pfx}}"
SSU_ZIP="${SSU_ZIP:-$HERE/third_party/SunnySideUp-1.0.7.zip}"
VKB="$HOME/.local/share/vkBasalt/mgs4"
VKB_CONF="$HOME/.config/vkBasalt"
HYPR="${HYPR_CONFIG:-$HOME/.config/hypr/hyprland.lua}"
MODE="${1:-install}"

ok()   { printf '  \e[32mok\e[0m    %s\n' "$1"; }
todo() { printf '  \e[33mneeds\e[0m %s\n' "$1"; MISSING=1; }
did()  { printf '  \e[36mdone\e[0m  %s\n' "$1"; }
MISSING=0
doit() { [ "$MODE" != --check ]; }

# copy <src> <dest> <label>: only when missing, or with --force
place() {
  if [ -e "$2" ] && [ "$MODE" != --force ]; then ok "$3"; return; fi
  if [ ! -e "$1" ]; then todo "$3 (no $(basename "$1") in the kit to copy)"; return; fi
  if doit; then mkdir -p "$(dirname "$2")" && cp "$1" "$2" && did "$3"; else todo "$3"; fi
}

[ -d "$GAME_DIR/MGS4" ] || { echo "Game not found at $GAME_DIR (set MGS4_GAME_DIR)"; exit 1; }
if doit && pgrep -f '^Z:.*(MGS4.mgs4|MGS1.mgs1|launcher)\.exe' >/dev/null; then
  echo "MGS4 is running — quit it first."; exit 1
fi

echo "Sunny Side Up 1.0.7 + Ultimate ASI Loader"
for f in MGS4/winmm.dll MGS4/scripts/SunnySideUp.asi Launcher/winmm.dll Launcher/scripts/SunnySideUp.asi; do
  if [ -e "$GAME_DIR/$f" ]; then ok "$f"
  elif doit && [ -f "$SSU_ZIP" ]; then (cd "$GAME_DIR" && unzip -oq "$SSU_ZIP" "$f") && did "$f"
  else todo "$f (get the Sunny Side Up 1.0.7 zip and set SSU_ZIP)"; fi
done

echo "Mod settings"
# Sunny Side Up's own ini files are not shipped here: drop your tuned copies into ini/
# (the README lists the settings this setup changes).
place "$HERE/ini/SunnySideUp.ini"          "$GAME_DIR/MGS4/scripts/SunnySideUp.ini"     "MGS4/scripts/SunnySideUp.ini"
place "$HERE/ini/SunnySideUp-launcher.ini" "$GAME_DIR/Launcher/scripts/SunnySideUp.ini" "Launcher/scripts/SunnySideUp.ini"
place "$HERE/../src/MGS4CRT.ini"           "$GAME_DIR/MGS4/scripts/MGS4CRT.ini"         "MGS4/scripts/MGS4CRT.ini"

echo "MGS4CRT plugin"
if [ -e "$GAME_DIR/MGS4/scripts/MGS4CRT.asi" ] && [ "$MODE" = --check ]; then ok "MGS4/scripts/MGS4CRT.asi"
elif doit; then MGS4_GAME_DIR="$GAME_DIR" "$HERE/../src/build.sh" --install >/dev/null && did "MGS4/scripts/MGS4CRT.asi (built from source)"
else todo "MGS4/scripts/MGS4CRT.asi"; fi

echo "Wine: load the mod's winmm.dll"
if [ -z "$PREFIX" ]; then todo "Proton prefix — set MGS4_APPID (or MGS4_PREFIX)"
elif [ ! -f "$PREFIX/user.reg" ]; then todo "Proton prefix — launch the game once from Steam first, then rerun"
elif grep -q 'AppDefaults\\\\mgs4.exe\\\\DllOverrides' "$PREFIX/user.reg"; then ok "winmm override in user.reg"
elif doit; then MGS4_PREFIX="$PREFIX" "$HERE/wine/add-winmm-override.sh" >/dev/null && did "winmm override in user.reg"
else todo "winmm override in user.reg"; fi

echo "vkBasalt look (Normal)"
if [ -f "$VKB_CONF/mgs4-normal.conf" ] && [ -f "$VKB/normal/Tonemap.fx" ] && [ "$MODE" != --force ]; then
  ok "presets in $VKB, configs in $VKB_CONF"
elif ! ls "$HERE"/vkbasalt/shaders/*.fx >/dev/null 2>&1 || ! ls "$HERE"/vkbasalt/presets/*.ini >/dev/null 2>&1; then
  todo "vkBasalt shaders/presets (fetch them first, see vkbasalt/README.md)"
elif doit; then
  mkdir -p "$VKB/textures" && cp -r "$HERE/vkbasalt/presets" "$HERE/vkbasalt/shaders" "$HERE/vkbasalt/gen_vkb.py" "$VKB/" \
    && (cd "$VKB" && python3 gen_vkb.py >/dev/null) && did "presets generated (lite, normal, strong)"
else todo "vkBasalt presets"; fi
if [ -e "$VKB_CONF/mgs4.conf" ]; then ok "mgs4.conf -> $(readlink "$VKB_CONF/mgs4.conf")"
elif doit && [ -f "$VKB_CONF/mgs4-normal.conf" ]; then ln -s mgs4-normal.conf "$VKB_CONF/mgs4.conf" && did "mgs4.conf -> mgs4-normal.conf"
else todo "mgs4.conf link"; fi

echo "Hyprland (checked only — see hypr/mgs4-rules.lua)"
if grep -q 'local mgs4_match' "$HYPR" 2>/dev/null; then ok "MGS4 window rules"; else todo "MGS4 window rules in $HYPR"; fi
if grep -q 'xrandr --output DP-3 --primary' "$HYPR" 2>/dev/null; then ok "DP-3 primary at startup"; else todo "xrandr DP-3 primary autostart in $HYPR"; fi

echo "Steam launch options (set by hand in Steam):"
echo "  ENABLE_VKBASALT=1 VKBASALT_CONFIG_FILE=$VKB_CONF/mgs4.conf MANGOHUD=1 MANGOHUD_CONFIG=no_display,output_folder=\$HOME/mgs4-perf %command%"
[ "$MODE" = --check ] && [ "$MISSING" = 1 ] && exit 2
exit 0
