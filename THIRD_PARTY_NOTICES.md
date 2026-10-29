# Third-party notices

## MGSPatriotFix (MIT)

The `[Graphics]` features in `src/mgs4crt.c` (motion blur switch, shadow resolution,
anisotropic filtering override) are ported from MGSPatriotFix by ShizCalev/Afevis, rewritten
for this executable without its hooking library. The motion blur switch is by cipherxof and
the shadow resolution by Emoose.

    MIT License

    Copyright (c) 2026 Afevis

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

## Sunny Side Up

The real-time cutscene camera detection (`CutsceneFovCRT` / `CutsceneFovMain`) follows the
approach Sunny Side Up uses for its `CutsceneFieldOfView` option: the same camera builder,
cutscene camera owner and rebuild call. No Sunny Side Up code or files are included.

## Not included, fetched by the user

- Sunny Side Up 1.0.7 (bundles Ultimate ASI Loader by ThirteenAG, MIT).
- SweetFX shaders by CeeJay.dk and others; ReShade's `ReShade.fxh` / `ReShadeUI.fxh`.
- The "MGS4 Clean Modern" ReShade preset.
