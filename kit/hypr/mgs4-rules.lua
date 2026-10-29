-- MGS4 window rules for a Hyprland Lua config (reference; install.sh checks they are present).
-- Needs `home` (your home directory) defined. Replace <APPID> with your Steam shortcut's AppId
-- (the window class is steam_app_<APPID>). Monitor names: DP-3 = the main LCD, HDMI-A-1 = the CRT.
-- Autostart (inside the exec-once block): DP-3 must be the XWayland primary.
	-- Make DP-3 the XWayland primary so Wine/Proton games fullscreen on it and read
	-- its 144Hz refresh (otherwise the CRT at 0x0 is treated as primary).
	hl.exec_cmd("sh -c 'sleep 3; xrandr --output DP-3 --primary'")

-- MGS4 (non-Steam shortcut steam_app_<APPID>; steam_proton when started via proton
-- directly). Runs borderless (Sunny Side Up's RenderScale only works there), which maps
-- as a tiled window — force fullscreen at map time. Also covers the launcher.
-- Target display follows ~/.cache/mgs4-display ("crt" → CRT ws11, else DP-3), written by
-- the author's rofi CRT/Main launcher script and reset to main when the game exits.
-- The title match also covers the Act 4 MGS1 flashback: mgs4.exe hands over to a separate
-- mgs1.exe ("METAL GEAR SOLID" window, same Proton app class), which restarts mgs4.exe when
-- it ends; both land on the same screen, fullscreen.
local mgs4_match = { class = "^(steam_app_<APPID>|steam_proton)$", title = "^(METAL GEAR SOLID.*)$" }
local mgs4_crt = false
do
	local f = io.open(home .. "/.cache/mgs4-display", "r")
	if f then
		mgs4_crt = (f:read("l") or "") == "crt"
		f:close()
	end
end
if mgs4_crt then
	hl.window_rule({ name = "mgs4-ws", match = mgs4_match, workspace = 11 })
	hl.window_rule({ name = "mgs4-mon", match = mgs4_match, monitor = "HDMI-A-1" })
else
	hl.window_rule({ name = "mgs4-mon", match = mgs4_match, monitor = "DP-3" })
end
hl.window_rule({ name = "mgs4-fs", match = mgs4_match, fullscreen = true })
-- MGS4 keeps requesting activation; with focus_on_activate that yanks focus back from
-- whatever the mouse is over (e.g. the terminal on the other monitor). Ignore its requests.
hl.window_rule({ name = "mgs4-noactivate", match = mgs4_match, suppress_event = "activate activatefocus" })
