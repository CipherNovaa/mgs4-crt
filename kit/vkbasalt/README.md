# vkBasalt look ("MGS4 Clean Modern")

`gen_vkb.py` bakes a ReShade preset into plain vkBasalt shaders: it copies each shader the
preset uses, writes the preset's values in as the uniforms' defaults (vkBasalt has no
ReShade UI), and writes one vkBasalt config per preset (`~/.config/vkBasalt/mgs4-{lite,normal,strong}.conf`).
Deband is left out on purpose (in vkBasalt it smeared dark flat art into grainy smudges), and
CAS uses vkBasalt's built-in version (the `.fx` needs a newer ReShade).

The shaders and presets are other people's work and are not shipped here. Put them in place
before running `install.sh`:

- `shaders/`: `Tonemap.fx`, `LiftGammaGain.fx`, `Curves.fx`, `Vibrance.fx`, `FakeHDR.fx`
  (CeeJay.dk's SweetFX shaders, https://github.com/CeeJayDK/SweetFX) and `ReShade.fxh`,
  `ReShadeUI.fxh` (https://github.com/crosire/reshade-shaders, `slim` branch).
- `presets/`: `MGS4_Clean_Modern.ini`, `MGS4_Clean_Modern_Lite.ini`,
  `MGS4_Clean_Modern_Strong.ini` from the "MGS4 Clean Modern" ReShade preset.

Then `../install.sh` copies everything to `~/.local/share/vkBasalt/mgs4/` and runs the
generator there; `~/.config/vkBasalt/mgs4.conf` is a link to the active config (normal).
