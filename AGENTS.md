# AGENTS.md – Northgard VR (handoff)

Handoff for coding agents (Codex) taking over from Claude. Written 2026-11-05 from the
repo at `C:\Users\alexa\source\repos\Northgard-VR-Mod` (GitHub: SoakingRich/Northgard-VR-Mod).

## What this is

An unofficial OpenXR VR mod for **Northgard** (Steam, Shiro Games). It shows the map as a
**tabletop diorama**, with a controller laser for input and the game's HUD cut into panels
around the table and on the left hand. It is single-player focused, has been tested on a
Quest 3 through Virtual Desktop (VDXR) and on the Meta XR Simulator, and is in **public release
prep** (`v0.9.0` tag exists locally).

Owner: Alex (GitHub `SoakingRich`). Alex tests on the headset and makes the decisions. The
agent writes code and tests in the simulator.

## How it works (read this before touching anything)

Northgard runs on the **Heaps** engine as **HashLink bytecode** (`hlboot.dat` in the game
folder). No game file is modified:

1. **`tools/hlpatch`** (Rust) reads `hlboot.dat` and writes **`vrboot.dat`** with calls to
   our natives (`openxr@xr_*`) injected at a few points. Everything is resolved **by
   name**, so it survives game updates. `hlpatch patch --phase1 hlboot.dat vrboot.dat`
   runs these passes (see `main.rs`):
   - `patch_camtest`: wraps `h3d.Camera.update` so native code can re-aim the main camera.
     Pos is restored before return, so gameplay state is never changed.
   - `patch_xr`: in `hxd.App.mainLoop`, renders the frame twice (left eye, then right) via
     `xr_stereo_begin` / `xr_eye_done`, and calls `xr_frame` before present.
   - `patch_rt`: render-target size, mouse coordinate remap, vsync off (`hxd.Window`).
   - `patch_ui_alpha`: UI rendered with alpha so it can be cut into panels
     (`AppBase.render`).
   - `patch_ui_windows`: `ui.BaseUI.update` → `xr_ui_windows` (HUD window rects).
   - `patch_game_events`: `EventManager.dispatchEvent` → `xr_game_event`
     (notifications).
   - `patch_weather`: `map.Weather.update` → `xr_weather`.
   - `patch_cam_move`: `cam.CameraController.camUpdate/move` → `xr_cam_ctl*` (map drag
     moves the real game camera).
   - `patch_loading`: `map.LoadMap.start`.
   - `patch_game_hook`: `Game.onUpdate` → `xr_game` (match / Ranked detection).
   - `patch_exit`: `hxd.$System.exit` → `xr_exiting` (clean VR shutdown on quit).

   It also has recon subcommands: `find`, `methods`, `callers`, `sig`, `fields`,
   `field-uses`, `field-writes`, `static-writes`, `strings`, `enum`, `types`, `consts`,
   `rawops`, and `roundtrip` (byte-identical check). `tools/hlbc` is a vendored hlbc 0.7
   (MIT) with serializer fixes.

2. **`native/openxr_hdll`** (C) builds **`openxr.hdll`**, which HashLink auto-loads:
   - `openxr_hdll.c`: camera math, diorama controls, controller → game mouse, hotkeys,
     self-restart/quit flags, logging.
   - `xr_theater.c` (~180 KB, the core): loads the **OpenXR runtime directly** (active
     runtime JSON → library → `xrNegotiateLoaderRuntimeInterface`, no loader DLL), creates
     the D3D11 session on the game's device, handles swapchains, eye/projection and quad
     layers, the VR options menu, chroma-key/passthrough, table and sky, panel hit tests and
     haptics.
   - `xr_uiwin.c`: reads HUD window rects straight from the game's UI objects in memory
     (via libhl's field tables), and handles game events and weather.

3. **`native/launcher`**:
   - `launcher.c` builds `NorthgardVR.exe`. It re-patches with `hlpatch.exe` when
     `hlboot.dat` changed or `vrboot.dat` is missing, then starts
     `Northgard.exe vrboot.dat`. Steam's Play button stays vanilla.
   - `setup.c` builds `NorthgardVR_Setup.exe`, the installer and uninstaller (`/uninstall`).
     It finds the game through Steam's registry entry and `libraryfolders.vdf`.

4. **`config/`**:
   - `NorthgardVR_ui_layout.txt`: HUD panel placement, reloaded live while the game runs.
   - `NorthgardVR_notify_rules.txt`: game event → VR notification card.

HashLink gotchas found during recon (details in `docs/RECON.md`):
- In this HL build `ToDyn` boxes objects, so pointer identity on the native side is
  meaningless. Compare in bytecode instead.
- The runtime's by-name field lookups throw on some fields. Do field access in bytecode
  with the game's own field indices, or through libhl field tables (as `xr_uiwin.c` does).
- Keep injected bytecode tiny and do the logic natively.

## Paths on Alex's PC

| What | Where |
|---|---|
| Repo | `C:\Users\alexa\source\repos\Northgard-VR-Mod` |
| Game | `C:\Program Files (x86)\Steam\steamapps\common\Northgard` |
| Meta XR Simulator | `C:\Program Files\MetaXRSimulator` (the mod also finds it via the OpenXR registry) |
| Meta XR Operator standalone v207.0.1 | `C:\Users\alexa\Downloads\meta-xr-operator-standalone-public-v207.0.1\meta-xr-operator-standalone-public-v207.0.1` |
| Mod log | `<game>\NorthgardVR_log.txt` (shared-read, readable while the game runs) |

Files the mod reads or writes in the game folder:
- `NorthgardVR_settings.txt`: VR options. `debug 1` enables test tooling.
- `NorthgardVR_runtime.txt`: force a runtime: `auto`, `headset`, `simulator`, or a manifest
  path.
- `NorthgardVR_ui_layout.txt`, `NorthgardVR_notify_rules.txt`, `NorthgardVR_notify.txt`.
- `NorthgardVR_patched.txt`: hlboot stamp. Change it to force the launcher to re-patch.
- `NorthgardVR_lib.txt`: DLL name without `.hdll`, default `openxr`.
- Logs: `NorthgardVR_patch_log.txt`, `NorthgardVR_launcher_log.txt`,
  `NorthgardVR_setup_log.txt`.
- `steam_appid.txt`: lets a directly started copy log in to Steam.

## Build

Release builds are **cross-compiled on Linux** with MinGW + Rust (`scripts/make_release.sh
<version>` → `dist/NorthgardVR-<version>.zip`). GitHub Actions runs the same script on
push, and publishes a GitHub release for `v*` tags.

On Windows:
- **openxr.hdll:** use the CMake project in `native/openxr_hdll` (VS 2022/2026, x64
  Release), or MinGW with the command in `docs/DEVELOPMENT.md`. Unverified: the CMake file
  links `advapi32 user32 gdi32 uuid` but not `shell32`, which the MinGW build adds. Add it
  if the link fails. Pass `NGVR_VERSION` to set the version string.
- **hlpatch:** `cargo build --release` in `tools/hlpatch`. Rust may not be installed on the
  PC. If it isn't, use `hlpatch.exe` from a release zip or from the game folder. The
  launcher runs it automatically when `vrboot.dat` is missing.
- **Launcher and setup:** see `docs/DEVELOPMENT.md`.

## Dev / test loop (the simulator, not Alex's headset)

Alex's standing rules:
- **Self-test in the Meta XR Simulator** via Meta XR Operator. Don't wait on Alex's
  headset for things the simulator can show.
- **Don't ask Alex to close the game or apps.** Automate deploys and restarts. If the game
  is stuck and can't be closed, it's fine to kill and restart Steam.
- **Nothing counts as fixed until it has been tested and shown to work.** Don't call
  untested changes fixes. Say exactly what was and wasn't verified (e.g. "simulator only",
  "not tested on hardware").

Loop:
1. Build `openxr.hdll`, plus `vrboot.dat` if `hlpatch` changed.
2. Copy both into the game folder.
3. With `debug 1` set, create `NorthgardVR_restart.flag` in the game folder. The running
   game starts a fresh copy of itself with the same command line and exits. The new copy
   also closes leftover instances. A flag older than the running process is ignored.
   `NorthgardVR_quit.flag` closes the game and the simulator. If the game isn't running,
   start `NorthgardVR.exe`, or `Northgard.exe vrboot.dat` from the game folder.
4. Read `NorthgardVR_log.txt` and check what you changed with the Operator tools.

Runtime selection: by default the mod uses the system's active runtime (Virtual Desktop on
Alex's PC). It falls back to the simulator only in debug mode, or when
`NorthgardVR_runtime.txt` says `simulator`.

**Meta XR Operator:** the mod **does not use the OpenXR loader**, so the usual
`XR_API_LAYER_PATH` / `XR_ENABLE_API_LAYERS` env vars do nothing. Instead, in debug mode,
`NorthgardVR_operator.txt` in the game folder holds the Operator layer's folder, and
`xr_theater.c` chains the layer in front of the runtime itself. The layer then serves MCP
on `localhost:8720` from inside the game process. To use it from Codex, register the
proxy as a stdio MCP server in `~/.codex/config.toml`:

```toml
[mcp_servers.meta-xr-operator]
command = 'C:\Users\alexa\Downloads\meta-xr-operator-standalone-public-v207.0.1\meta-xr-operator-standalone-public-v207.0.1\windows\meta-xr-operator-mcp-proxy.exe'
```

Check that path against the unzipped folder. The tools used so far include:
`openxr_capture_composited_image` (screenshot), `openxr_set_head_pose`,
`openxr_set_controller_pose`, `openxr_set_controller_input`,
`openxr_release_input_devices`, `openxr_get_session_info`,
`openxr_get_active_interaction_profile`, and `openxr_get_frame_info`.

Debug-only remote-control files (in the game folder, `debug 1` only):

| File | Effect |
|---|---|
| `NorthgardVR_click.txt` | `x y` clicks at UI px; `x y r` right-clicks; `h` hovers; `w` hovers a world ground point (game units) |
| `NorthgardVR_find.txt` | `x y` logs the UI objects at that point |
| `NorthgardVR_dump.flag` | dumps the UI image |
| `NorthgardVR_mode.txt` | overrides the game mode (`Standard` / `Ranked` / empty) to test multiplayer rules |
| `NorthgardVR_restart.flag` / `NorthgardVR_quit.flag` | see the dev loop above |

Debug mode also enables these hotkeys: F7 stereo/flat, F8 UI panel, F9 camera override, F10
auto-orbit, F5/F6 world scale, and numpad yaw/pitch/zoom.

Simulator quirk: pressing A in the simulator also pulls the trigger. The mod ignores a
trigger press while A is down.

The release regression pass is `docs/RELEASE_CHECKLIST.md`.

## Current state and open work

The source of truth is **`docs/Tasks.md`**. Open items as of this handoff:
- **(C) Bug:** snow and weather particles show in winter even with Weather = Off
  (`xr_weather`, `map.Weather.update`).
- **(C) In progress:** controller bindings for Index, Vive and WMR are untested on real
  hardware. The active profile is logged as `controller profile:`.
- **(C) In progress:** the GitHub Actions release workflow hasn't run on GitHub yet.
- **(A) Headset and Alex checks:** quit-to-desktop on VD, HUD row / distance / hand-menu
  aim feel, weather fog follows drag, Steam login when started from the launcher,
  runtimes (VD, SteamVR, Link), GPU coverage, long sessions, and publishing the first public release.
- **Parked (`[-]`, only if seen again):** idle/sleep freeze, missing building preview,
  convert-cost label, HUD panels disappearing with VD passthrough.
- **Known issues** (see README): chroma key can tint transparent panels.

**Working tree note:** `openxr_hdll.c`, `hlpatch/src/main.rs` and `docs/Tasks.md` were
changed after the last commit ("Make release script runnable"). Check `git status` and
`git diff` first, and don't discard them.

## Decisions already made (don't relitigate)

- OpenXR only, using whatever runtime is active. Primary target: Quest 3 via Virtual
  Desktop.
- Tabletop diorama view, no first-person view. Motion controllers, not hand tracking.
- Mixed reality uses Virtual Desktop's chroma-key passthrough, with OpenXR passthrough as
  an option where the runtime offers it. It's toggled from the in-VR options menu.
- Standalone launcher plus installer, so playing from Steam stays vanilla. No Steam launch
  options, and no flat-screen fallback.
- Multiplayer: warn but never block. Ranked plays on a flat screen in VR.
- Public release, provided as-is. Releases stay **unsigned**. Shiro Games won't be contacted.

## Docs and conventions

- `docs/DEVELOPMENT.md`: build. `docs/ADVANCED.md`: settings, layout and notifications.
  `docs/RECON.md`: bytecode findings and hook evidence.
- `docs/PLAN.md` and `native/openxr_hdll/README.md` are early (Phase 0b/1) and partly
  stale. Read them for intent; the code is authoritative.
- **`docs/Tasks.md` style:** a clean task list, not a conversation. Fold Alex's comments
  into the task lines, don't reply inline. Don't add a "Decided against" section. Status
  markers are `[ ] [~] [x] [?] [-]`, owners are (A) and (C). A `[x]` line says how it was
  tested.
- User-facing changes go in `CHANGELOG.md` under "Unreleased".
- Commit or push only when Alex asks.
