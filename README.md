# Northgard VR Mod

An OpenXR mod that plays **Northgard** in VR as a tabletop diorama: the map sits on a
table in front of you, the HUD floats around it as panels, and you play with a motion
controller laser. Built for Quest 3 through Virtual Desktop (VDXR); single-player.

Unofficial fan project, not affiliated with Shiro Games. You need your own copy of
Northgard. **No game files are included or modified**: the mod writes a patched copy of
the game's bytecode next to the original and the game is launched with it.

## Features

- Stereo diorama: the map on an adjustable round table (or an open map with no table edge)
- HUD as VR panels placed around the table, sized live from the game's own UI windows
  (economy bar, players, notifications, minimap/calendar, unit tallies, build menu,
  selection, territory, Lore/pause/dialogs, hover tooltips next to the laser)
- Controller laser for menus and the match: hover, click, drag-select (drawn on the table),
  building placement with preview, tooltips
- VR options menu (left thumbstick click): mixed reality, key colour, map style
- Mixed reality: chroma-key mode for Virtual Desktop's VR Passthrough, or real OpenXR
  passthrough on runtimes that offer it (Meta Quest Link with Passthrough over Link)
- Desktop window mirrors the left eye

## Controls (Touch controllers)

| Input | Action |
|---|---|
| Right trigger | Click (hold + move: drag-select) |
| Right A | Right click |
| Left thumbstick click | Open / close the VR options menu |
| Left ≡ menu button, or right thumbstick click | Escape (close windows, cancel) |
| Right stick | Rotate the map (X), zoom (Y) |
| Left stick | Resize the table (X), table height (Y) |
| One grip held + move | Drag the map |
| Both grips | Grab the map with two hands: move, rotate, scale |
| Right B | Re-place the table in front of you |
| Left Y | Reset the view |

The keyboard and mouse keep working as normal.

## Install

1. Build or download `openxr.hdll` (see Build) and copy it into the Northgard folder,
   e.g. `C:\Program Files (x86)\Steam\steamapps\common\Northgard`.
2. Generate the patched bytecode from your own copy of the game:
   ```powershell
   cd tools\hlpatch
   cargo build --release
   .\target\release\hlpatch.exe patch --phase1 "<Northgard>\hlboot.dat" "<Northgard>\vrboot.dat"
   ```
3. Copy `config\NorthgardVR_ui_layout.txt` into the Northgard folder.
4. Steam → Northgard → Properties → Launch Options: `%command% vrboot.dat`
5. Start Virtual Desktop (or another OpenXR runtime), then launch Northgard from Steam.

To go back to the normal game, clear the launch option. `hlboot.dat` is never touched.

## Settings

`NorthgardVR_settings.txt` in the game folder (the options menu writes it too):

| Key | Meaning |
|---|---|
| `table_radius 0.75` | Table radius in metres (0.35–1.6) |
| `open_map 1` | 1 = map without a table edge, 0 = round wooden table |
| `mixed_reality 0` | 0 off, 1 OpenXR passthrough if the runtime offers it, 2 chroma key |
| `mixed_reality_key_r/g/b` | Chroma-key colour (default 255 0 255). For Virtual Desktop, turn on Streaming → VR Passthrough with the same colour |

`NorthgardVR_runtime.txt`: `auto` (default: Virtual Desktop, else the Meta XR Simulator),
`headset` or `simulator`. `NorthgardVR_ui_layout.txt` places the HUD panels and is
reloaded while the game runs (format described at the top of the file).

The mod logs to `NorthgardVR_log.txt` in the game folder.

## Build

- **Native library** (`native/openxr_hdll`): CMake project for Visual Studio 2022 (x64
  Release), or MinGW:
  ```
  x86_64-w64-mingw32-gcc -O2 -shared -Ithird_party/hashlink -Ithird_party/openxr \
    -o openxr.hdll src/openxr_hdll.c src/xr_theater.c src/xr_uiwin.c \
    -ladvapi32 -luser32 -lgdi32 -luuid -static-libgcc
  ```
- **Bytecode patcher** (`tools/hlpatch`): Rust, `cargo build --release`. It uses a vendored
  copy of [hlbc](https://github.com/Gui-Yom/hlbc) (MIT) in `tools/hlbc`. Set `XR_LIB=name`
  to make the patched game load `name.hdll` instead of `openxr.hdll`.

## How it works

Northgard runs on the Heaps engine as HashLink bytecode (`hlboot.dat`). `hlpatch` injects
calls into a native library at a few points (frame loop, camera, render passes, UI update,
mouse input). The library talks to the OpenXR runtime directly (no loader needed), renders
each eye from the game's own renderer with an overhead camera, cuts the UI into quad layers
and drives the game's mouse from the controller ray. Details and findings are in
[docs/RECON.md](docs/RECON.md); the plan in [docs/PLAN.md](docs/PLAN.md).

## Known issues

- When the mod closes the game itself (quit/restart), Steam may keep showing it as running
  until Steam is restarted.
- Chroma-key mode: semi-transparent HUD panel backgrounds can pick up a tint of the key colour.
- Lore's close button doesn't react to the laser; use Escape (left ≡).

## Licence

Mod code: MIT (see `LICENSE`). Third-party: HashLink header (MIT), OpenXR SDK headers
(Apache-2.0), hlbc (MIT).
