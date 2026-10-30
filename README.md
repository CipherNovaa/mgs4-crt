# MGS4CRT

An ASI plugin that makes Metal Gear Solid 4 (Master Collection, on Linux through Proton) fill
a 5:4 CRT at 1280x1024, with the 3D shown whole and the 16:9 HUD kept at its true shape.

**Status:** working, version 2.30, in use on the author's 1280x1024 CRT (HUD, radar, item menu and pause screens confirmed on the tube). It supports
one executable build only (PE timestamp `0x6A8CFC47`) and stays off on any other build. Not yet
checked in play: `CutsceneFovCRT = 0` during a real-time cutscene, and the Act 4 MGS1 flashback
hand-off. `PanoramicStretch` and `FreeMouse` work but are off by default (see below).

<!-- MEDIA: side-by-side photo of the CRT, same in-game moment (gameplay with life bar + radar
     visible). Left: stock (16:9 letterbox with black bars top and bottom). Right: MGS4CRT
     (full tube, HUD at the corners). Save as docs/media/before-after-gameplay.jpg -->

<!-- MEDIA: photo of the CRT with the item/weapon carousel open, to show the HUD grows out of
     its box at the bottom edge. docs/media/item-menu.jpg -->

<!-- MEDIA: photo of the iPod screen or the pause menu on the CRT (clip rectangle moved,
     panels reaching the tube edge). docs/media/ipod.jpg -->

## The problem

On a screen narrower than 16:9, MGS4 draws a 16:9 picture with black bars above and below.
On a 1280x1024 tube that throws away 304 of its 1024 lines. Forcing the game to fill the screen
is not enough either: everything 2D is laid out for 16:9, so stretched to 5:4 the HUD, menus and
text come out 1.42 times too tall.

## What it does

- **Fills the tube with the real 3D.** No stretch and no crop: the game already renders the 3D
  at the screen's 5:4 shape, so the plugin shows all of it.
- **Gives every 2D element its 16:9 shape back**, anchored to the edges of the tube: the life
  bar and radar at the top, item and weapon boxes at the bottom, text in the middle at its own
  place. Opening a menu never makes the HUD jump.
- **Fits menus and overlays.** Panels that touch the top or bottom of the 16:9 layout keep
  touching the tube's edge, so there are no seams. Full-screen fades are left alone. The pause
  screens can be centred or stretched.
- **Moves clip rectangles** such as the iPod screen along with the HUD around them.
- **Field of view hotkeys** (F10 / F11 by default, both together to reset), saved per screen,
  with an option to keep real-time cutscenes at their own framing.
- **Graphics extras**, per screen: motion blur on/off (switches live), one shadow map
  resolution for all shadows, 16x anisotropic filtering.
- **Does nothing on 16:9 or wider screens**, so the same install works on a main monitor too.
- **Live config:** edits to `MGS4CRT.ini` are picked up within about a second.
- **An install kit** for the whole Proton setup: Sunny Side Up and its ASI loader, the Wine DLL
  override, the plugin, a vkBasalt colour preset and Hyprland window rules (`kit/`).

## How it works

The full design notes are the header comment of [`src/mgs4crt.c`](src/mgs4crt.c). In short:

**The 3D was never the problem.** The camera takes its aspect from the render buffer
(2560x2048 on the CRT with Sunny Side Up's 2x render scale), so the game already renders a
correct native 5:4 view. Only the last step, the blit of that buffer to the screen, uses a
16:9 letterbox rectangle. The plugin hooks `SetViewport` and rewrites that rectangle to the
whole screen. (Sunny Side Up's `StretchToFill` did not change it in borderless mode.)

**The 2D is fixed one draw at a time.** Every 2D sprite goes through one `DrawArrays`
function. Its vertex positions are int16 (1/16 pixel, scale in shader register 9) and its
transform sits in three vertex shader constants: the x row (register 12), the y row (13) and the
position (15), in normalised device coordinates. The hook:

1. reads the vertex attribute descriptor and buffer table the game keeps in memory, finds the
   sprite's CPU-side vertex copy and computes its true on-screen box in the 16:9 layout;
2. picks a fit `y' = a*y + b` that scales by k = 0.703 (the 16:9 height over the tube's height)
   around a pivot: the top edge, the bottom edge, the centre, or the group's own origin;
3. uploads the adjusted rows through the original `SetVertexConstant`, replaces the same
   registers if the draw's own flush re-sends them, and restores the game's values afterwards,
   so every other draw sees exactly what the game set.

**Groups, learned in gameplay.** A HUD element is several sprites drawn by one renderer view,
identified by its first sprite's position and scale. During gameplay each group is classified
(high: top edge, low: bottom edge, otherwise keep its place) and the fit is trusted after 30
steady frames. When a menu opens, trusted groups keep their gameplay fit and only the menu's own
groups get menu rules. That is what stops the radar or the life bar jumping.

**Sprites placed absolutely** (origin at the screen centre or the top-left corner: menu panels,
the selection frame, description text) carry their own screen position, so each one is fitted
alone. Those reaching the 16:9 top or bottom edge are stretched to the tube's edge.

**Sub-viewports** like the iPod screen are moved and resized like the HUD, and whatever is drawn
inside them is left alone.

### Walls hit on the way

- **Per-view viewport remapping failed.** The first versions remapped the 2D views' viewports to
  a 16:9 band. View ids are sequential per frame and shift whenever an optional layer appears
  (enemy labels, checkpoint text), so no fixed mapping held. The next version scaled the y row in
  `SetVertexConstant` and anchored by position; it still could not tell a panel from a label.
  Reading each sprite's real vertices in `DrawArrays` is what finally made the fit exact.
- **Things jumped when menus opened**, because a menu changes what is on screen and so how a
  group would be classified. Hence the learned, trusted fits above.
- **The aiming reticle broke apart**: its centre sways by about 0.0002 each frame, enough to flip
  rules. "Keep" groups now scale around their first sprite so they stay in one piece.
- **Loading screens draw a hidden HUD** in early views (4 to 5), which flipped the layout to HUD
  mode. HUD mode now counts only views from 40 on, where the real gameplay HUD is drawn, and is
  held for about a second after the HUD was last seen.
- **Enemy markers** (Solid Eye labels) are drawn at projected 3D positions. The 3D already has
  the tube's shape, so these are recognised by their transform and each keeps its exact place.
- **Shadow resolution picked the wrong screen.** The game reads its shadow settings at startup,
  while the buffer-size slot still holds the exe's 1920x1080 default. That default now counts as
  "not sized yet" and the plugin asks Wine for the primary display instead (fixed in 2.26, made
  right in 2.28).
- **Steam's DRM decrypts the code after load.** The plugin checks the PE timestamp, then waits
  up to 60 seconds for the expected function bytes before hooking, and patches nothing if they
  never match.
- **The final downscale is a single tap**, not an average (tested by shrinking the blit
  rectangle live). So an exact 2x render scale on the CRT gives a clean 2x2 average and is the
  best choice; 3x would lose all anti-aliasing.
- **Tried and parked:** a panoramic stretch for menus (`PanoramicStretch`) improved text but
  distorted the pause map (drawn in a clip window) and the compass (the only rotated sprite), so
  it ships at 0. `FreeMouse` freed the desktop cursor but clicks still went to the game, so it
  ships off.

The graphics extras are ported from MGSPatriotFix (MIT) and rewritten without its hooking
library: motion blur returns early from its setup function, shadows are set in the parsed shadow
settings, and anisotropic filtering goes through a small stub at the `CreateSamplerState` call.
All hooks are hand-written 14-byte absolute-jump detours with trampolines, each guarded by a
byte check of the function head.

## Workflow for this Project

The plugin was reverse-engineered and written with Cloud LLMs working on the live game, by hooking into memory directly:

- The agent attached gdb to the running game under Proton with hardware breakpoints to trace
  the renderer, and poked memory live through
  `/proc/<pid>/mem` to test ideas (the full-screen blit rectangle, the single-tap downscale)
  before writing any hook.
- It captured the CRT output with `grim` to check each change on the real frame, alongside the
  author's eyes on the tube.
- The plugin's log is written for verification: hooks installed, which screen it decided it is
  on, shadow sizes before and after, so a run can be checked without guessing.
- When vkBasalt's Deband smeared the title art, a numpy re-implementation of the shader did not
  reproduce it, so the agent bisected with test configs and relaunches instead, which found it.

## Requirements

- METAL GEAR SOLID 4 from the Steam Master Collection, the build with PE timestamp `0x6A8CFC47`.
- [Sunny Side Up](#credits) 1.0.7 (it provides Ultimate ASI Loader, which loads this plugin), in
  **Borderless** mode (its `RenderScale` only works there).
- A MinGW-w64 cross compiler (`x86_64-w64-mingw32-gcc`).
- Proton. Optional: vkBasalt, MangoHud, Hyprland.

## Build and install

```sh
src/build.sh                 # builds build/MGS4CRT.asi
src/build.sh --install       # also copies it (and the ini if missing) into MGS4/scripts
src/build.sh --install --ini # and overwrites the ini
```

Set `MGS4_GAME_DIR` if the game is not in the default Steam library.

### The install kit

`kit/install.sh` puts the whole setup back together and never overwrites tuned files unless
given `--force`; `--check` only reports. It needs:

| Setting | What |
|---|---|
| `MGS4_APPID` | The game's Steam AppId (for a non-Steam shortcut, the shortcut's id), used to find the Proton prefix |
| `MGS4_GAME_DIR` | Game folder holding `MGS4/` and `Launcher/` (default: the standard Steam library) |
| `MGS4_PREFIX` | Proton prefix, if not `compatdata/$MGS4_APPID/pfx` |
| `SSU_ZIP` | The Sunny Side Up 1.0.7 zip (default `kit/third_party/SunnySideUp-1.0.7.zip`) |
| `HYPR_CONFIG` | Hyprland Lua config to check for the window rules |

What it does:

- extracts Sunny Side Up and its `winmm.dll` into `MGS4/` and `Launcher/`;
- places `SunnySideUp.ini` (game and launcher) from `kit/ini/` if you put tuned copies there,
  and `MGS4CRT.ini`;
- builds and installs the plugin;
- adds the per-app Wine override `"winmm"="native,builtin"` for `mgs4.exe` and `launcher.exe`
  (`kit/wine/add-winmm-override.sh`, which backs up `user.reg` first);
- bakes the vkBasalt look (`kit/vkbasalt/`, see its README for fetching the shaders and preset);
- checks the Hyprland rules in `kit/hypr/mgs4-rules.lua` (force fullscreen, pick the monitor,
  make the main display the XWayland primary, ignore the game's activation requests).

Sunny Side Up settings this setup uses, CRT / main monitor: `RenderScale` 2.0 / 1.5,
`FpsLimit` 75 (the CRT runs at 75 Hz) / 165, `StretchToFill = 1`, `Width = Height = 0`,
`SkipLauncher = 1`. The game's own window mode lives in its encrypted save and is set once to
Borderless from the Master Collection launcher.

Steam launch options used:

```
ENABLE_VKBASALT=1 VKBASALT_CONFIG_FILE=$HOME/.config/vkBasalt/mgs4.conf MANGOHUD=1 MANGOHUD_CONFIG=no_display,output_folder=$HOME/mgs4-perf %command%
```

## Configuration

`MGS4CRT.ini` sits next to the plugin in `MGS4/scripts/`; every option is commented there. The
log is `MGS4CRT.log` in the same folder.

| Section | Option | Default | What |
|---|---|---|---|
| HUD | `Enabled` | 1 | 0 turns the whole plugin off |
| HUD | `FillScreen` | 1 | Show the whole 5:4 render instead of the 16:9 letterbox |
| HUD | `AnchorHUD` | 1 | Re-fit the 2D to its 16:9 shape; 0 leaves it stretched |
| HUD | `FullScreenMenus` | Stretch | Screens without the gameplay HUD: `Center` (stock layout in the middle, backgrounds fill) or `Stretch` (the Load screen always stays centred) |
| HUD | `SaveScreen` | Stretch | The pause menu's Save screens: `Center` (frame fills, window keeps its shape) or `Stretch` |
| HUD | `PanoramicStretch` | 0 | 0 to 100: bend the menu stretch into the top and bottom edges, like a TV's panoramic mode |
| HUD | `AnchorThreshold` | 0.5 | How far from the middle (0 to 1) an element sits before it is fitted to an edge |
| HUD | `FreeMouse` | 0 | Stop the game re-centring and clipping the desktop cursor (next launch) |
| FOV | `NarrowerHotkey` / `WiderHotkey` | F10 / F11 | F1 to F24, or blank. Both together reset |
| FOV | `FovStep` | 1 | Percent per tap (1% is about half a degree) |
| FOV | `CutsceneFovCRT` / `CutsceneFovMain` | 0 / 1 | 1 applies the FOV to real-time cutscenes too |
| FOV | `FovCRT` / `FovMain` | 100 / 100 | Percent of the stock FOV (about 58 degrees), 50 to 200; written by the hotkeys |
| Graphics | `AnisotropicFiltering` | 16 | Replaces the game's 8x; 0 or 8 leaves it (next launch) |
| Graphics | `MotionBlurCRT` / `MotionBlurMain` | 1 / 0 | Motion blur per screen, live |
| Graphics | `ShadowResolutionCRT` / `ShadowResolutionMain` | 4096 / 2048 | One size for all shadows (512 to 4096, 0 = stock); applies when a level loads |

"CRT" means any screen narrower than 16:9; "Main" means 16:9 or wider.

## Credits

- **MGSPatriotFix** by ShizCalev/Afevis (MIT): motion blur switch (cipherxof), shadow resolution
  (Emoose) and the anisotropic filtering override, ported into this plugin. See
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- **Sunny Side Up**: the widescreen and frame rate mod this plugin runs beside; its cutscene
  field of view option showed how to recognise the cutscene camera. Not included.
- **Ultimate ASI Loader** by ThirteenAG (MIT), bundled with Sunny Side Up.
- **SweetFX** shaders by CeeJay.dk and others, and the "MGS4 Clean Modern" ReShade preset, used
  by the vkBasalt look. Not included.

METAL GEAR SOLID is a trademark of Konami. This project contains no game code or assets.

## Licence

MIT, see [LICENSE](LICENSE).
