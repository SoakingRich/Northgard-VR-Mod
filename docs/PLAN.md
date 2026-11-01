# NorthgardVR: plan

**Goal:** a personal-use OpenXR mod that makes Northgard a stereo **tabletop
diorama**. It is single-player, uses Quest 3 motion controllers, and targets a
5080 GPU and 9800X3D CPU.

## 0. Decisions so far

- **Headset:** Quest 3 with motion controllers (no hand tracking).
- **Stance:** standing. The default table height is about waist height (~0.9 m above
  the stage floor) and can be adjusted.
- **Keyboard:** stays usable for hotkeys alongside the controllers. VR input is
  additive: synthetic events are injected next to real ones, and real keyboard events
  are never blocked. Caveat: keyboard events need the game window to have focus.
- **Mode:** single-player only, personal use; git added later by Alex.
- **Toolchain on the PC:** Visual Studio 2026 Community (v18, MSVC 14.51, bundled CMake
  + Ninja) and VS 2022 Community. Rust is not installed yet. Until it is, `vrboot.dat`
  is generated in the Claude workspace and copied over.

## 1. Architecture

```
Northgard.exe vrboot.dat
 ├─ vrboot.dat ── hlboot.dat + injected hooks (built by tools/hlpatch)
 │    ├─ new natives: "openxr@xr_*"  → HashLink auto-loads openxr.hdll
 │    ├─ hook: frame start   → xr_wait_frame / xr_begin_frame
 │    ├─ hook: App.render    → render s3d once per eye into eye targets,
 │    │                         render s2d (UI) into an offscreen target
 │    ├─ hook: Camera.update → native overwrites view/proj with the eye's
 │    │                         (diorama transform × eye pose × eye FOV)
 │    └─ hook: input         → controller ray → synthetic mouse/keys
 └─ openxr.hdll (C++, native/openxr_hdll)
      ├─ OpenXR loader, D3D11 session (device from ID3D11Resource::GetDevice)
      ├─ eye swapchains; copy Heaps eye textures → swapchain images
      ├─ UI → XrCompositionLayerQuad (floating/wrist panel, free on the compositor)
      └─ action sets: poses, trigger, grip, sticks, buttons, haptics
```

### Why this design

- **No changes to game files.** We ship `vrboot.dat` + `openxr.hdll` and launch with
  the boot-file argument, which already exists for the OpenGL path.
- **Static patching over runtime hooking.** JIT'd HL code calls functions directly, so
  native hot-patching would be fragile. Bytecode injection is deterministic, verified by
  the byte-identical round-trip, and we can inspect the result with `hlbc`.
- **Logic split.** Native code handles all the math and XR state. Injected bytecode
  stays small: call native, call existing engine function, loop. This keeps
  hand-assembled opcodes to a minimum.
- **Escape hatch.** If hand-assembled stubs get unwieldy, we'll write the mod-side logic
  in Haxe, compile it against upstream Heaps, and have `hlpatch` **merge** that module
  into the game bytecode, remapping types and functions by name. More tooling, but it
  scales. We'll decide at the end of Phase 2.

### Diorama math (native side)

For each eye:
`view = inverse(eyePose_stage) × stageFromTable × tableFromWorld`

- `tableFromWorld` = uniform scale `s` (map metres → table cm), plus a pan offset and a
  yaw rotation, all driven by the controllers.
- `proj` = asymmetric frustum from `XrView.fov`, using Heaps' depth conventions
  (DX11, 0..1).
- **Culling:** Heaps culls against the camera frustum. We set the culling frustum to the
  union of both eyes (or disable it at small table scales). If the game hides or LODs
  terrain based on `CameraController` zoom, we pin those values to "fully zoomed out".

## 2. Phases & milestones

| # | Phase | Done when |
|---|---|---|
| 0 | **Recon & tooling** ✅ (mostly) | Byte-identical round-trip; hooks identified by name (`docs/RECON.md`) |
| 0b | Patch smoke test ✅ | `vrboot.dat` with one trivial injected native call (writes a log line) launches and plays normally |
| 0c | **Camera-freedom test (desktop)** ✅ | Native overrides the camera to orbit/tilt the map on the flat screen. Shows whether terrain, trees, fog of war, shadows and culling survive arbitrary angles and perspective. This is the decisive go/no-go before investing in stereo (from the earlier ChatGPT research) |
| 1 | **OpenXR bootstrap / "theater"** ✅ | `openxr.hdll` creates a D3D11 session on the game's device. The flat back buffer appears on a quad layer in the headset, and the frame loop is paced by `xrWaitFrame` |
| 2 | **True stereo** ✅ | `s3d` rendered per eye with XR pose/FOV, submitted as a projection layer; correct scale, no swimming, 90 Hz on Quest 3. Fix shadows, fog, post-FX, and any screen-space effects that break in stereo |
| 3 | **Diorama** ✅ (first version) | Map sits on a virtual table. Grip-drag pans, two-hand grip scales/rotates, a recenter button and table height adjust work. Culling/LOD fixed so the whole visible map renders |
| 4 | **UI in VR** | `s2d` rendered offscreen → quad layer (floating panel + optional wrist panel); resolution/curvature tuned for legibility |
| 5 | **Input** | Controller ray hits the table → game picking (select, drag-select box, right-click orders); ray on the UI panel → `h2d` mouse events; button map for hotkeys/speed/pause; haptics on select/order |
| 6 | **Polish & performance** | Settings file (scale, table height, IPD override, UI panel position); render-scale; optional MSAA; comfort options; no hitches |
| 7 | **Packaging (personal)** | `install.ps1` builds `vrboot.dat` from the current `hlboot.dat` and adds a Steam launch option; patcher refuses on unknown game versions |

Rough effort for one person: Phase 0b–1 takes a few evenings, Phase 2 is the hardest
(1–2 weeks), and Phases 3–5 take about 1–2 weeks each.

## 3. Hook points (resolved by name in `hlpatch`)

| Hook | Function | Injection |
|---|---|---|
| Frame begin | `hxd.App.mainLoop` (start) | `xr_frame_begin()` → returns shouldRender |
| Per-eye render | `hxd.App.render` | replace single `s3d.render(e)` with eye loop: `xr_eye_begin(i, tex)` → `pushTarget` → `s3d.render` → `popTarget` |
| Camera override | `h3d.Camera.update` (end) | `xr_camera_override(this.mcam, this.mproj, this.m)`, a no-op when no eye is active |
| UI capture | `hxd.App.render` (`s2d.render`) | wrap in `pushTarget(uiTex)` |
| Frame end | before `driver.present()` in `hxd.System.mainLoop` | `xr_frame_end(uiTex)` |
| Input | `hxd.Window` event dispatch / `CameraController.updateKeys` | inject synthetic events from `xr_poll_input()` |
| Camera controls | `cam.CameraController.camUpdate/move` | disable desktop camera movement in VR; diorama owns the view |

Each is confirmed against upstream Heaps source plus `hlbc decomp` before
implementation.

## 4. Runtime / headset

- The mod targets **OpenXR only**, so any OpenXR runtime works.
- **Recommended:** **Virtual Desktop + VDXR** (its native OpenXR runtime) for wireless,
  or **Meta Quest Link** (Meta's OpenXR runtime) for wired/Air Link debugging. Both
  avoid the SteamVR translation layer.
- SteamVR works as an OpenXR runtime too ("OpenVR" itself isn't used), but it adds
  latency and overhead.
- Development tip: Quest Link over cable gives the most stable debugging. Switch the
  active OpenXR runtime in the Meta app or VD settings.
- **Target:** 90 Hz, per-eye ~2064×2208 at 1.0 scale. Northgard is light, and a 5080
  should handle 1.3–1.5× supersampling.

## 5. Risks

| Risk | Mitigation |
|---|---|
| Game update shifts bytecode | Name-based hook resolution; version check from `shiro.json`; re-run `install.ps1` |
| Frustum/LOD/fog-of-war tied to the desktop camera | Pin zoom/LOD; union frustum; investigate `CameraController.getCamZoomCoefficient` |
| Screen-space effects (SSAO, outlines, fog) assume mono | Per-eye render targets give each eye a full pipeline; disable broken effects in VR |
| Double scene render cost | Plenty of GPU headroom; render-scale fallback |
| Picking relies on `rayFromScreen` with the desktop camera | Point a dedicated pick camera along the controller ray, mouse at centre |
| Online/anti-cheat | Single-player only; VR boot file never used for multiplayer |

## 6. Reference projects & fallbacks

From the earlier ChatGPT research, which I haven't independently verified. These are
worth studying for design and technique, though none targets Heaps/HashLink:

- **GeneralsVR / Generals: Zero Hour XR**: RTS tabletop interaction design (rays,
  world scaling, floating UI).
- **Diablo II: Resurrected VR**: camera conversion of an overhead game without engine
  source.
- **SnowRunner VR**: DX11 `dxgi.dll` proxy → OpenXR. This is our **fallback** if
  bytecode patching hits a wall (e.g. after a game update).
- ReShade + SuperDepth3D / Geo-11: depth-based fake stereo on a virtual screen. These
  are not pursued, since bytecode patching already gives true geometry stereo, but
  they're a quick sanity check of the depth buffer if needed.

## 7. Repo layout

```
NorthgardVR/
  docs/            PLAN.md, RECON.md
  tools/hlbc/      vendored hlbc 0.7 (MIT) with serializer fixes
  tools/hlpatch/   Rust CLI: roundtrip, find, (later) patch → vrboot.dat
  native/openxr_hdll/   C++ OpenXR + D3D11 native lib (Phase 1)
  scripts/         backup / build / launch helpers
```

## 8. Status & handoff (2026-10-31 12:52)

**Deployed build:** `vrboot.dat` (`XR_LIB=openxr_9`) + `openxr_9.hdll` in the game folder.
Steam launch option `%command% vrboot.dat`.

**Done:**
- 0, 0b, 0c, 1 (theater), 2 (stereo), 3 (diorama).
- 5 (pointer select + orders), verified in the Meta XR Simulator.
- Automatic runtime choice: VD headset first, simulator fallback.
- Windowed mirror sized to the eye aspect.
- Self-test loop: Steam launch, restart flag, operator screenshots and simulated input.

**Testing on the headset (Virtual Desktop):**
1. Connect the Quest in VD **before** launching. The runtime is chosen at startup; with no
   headset it falls back to the simulator.
2. Launch from Steam. Menus show on a floating screen; start a skirmish from the PC window.
3. Controls:
   - Grip: drag the map. Both grips: zoom/rotate.
   - Right stick: rotate/zoom. Left stick: table height.
   - B: recentre the table. Y: reset the view.
   - Touch or half-pull the right trigger to aim. Pull the trigger to select. A: give an
     order.
   - F7: stereo/theater. F8: UI panel (opaque, WIP).

**Next (in order):**
1. **Phase 6a: eye resolution independent of the window.** Hook `h3d.Engine.onWindowResize`
   (findex 661; reads `window.get_width/get_height`, then `resize(w,h)`) so that in VR the
   engine resizes to the recommended eye size (1680x1760 sim; VD will differ). Present
   stretches it into the window. Then scale mouse event coords (window px → engine px) so
   PC menu clicks still line up. `hxd.Window.set_vsync` is findex 723 (set false in VR).
2. Table edge: clip/fade the map outside a table radius (black void beyond the map
   today).
3. Phase 4: VR UI. Alex's list of HUD pieces to rebuild for VR:
   - unit tallies
   - hovered territory attributes
   - selection summary
   - minimap
   - weather track
   - notifications list
   - fame tallies
   - top economy HUD
   - warning notifications

   Approach to decide: per-element panels cut from the UI pass (it has alpha once
   cleared), or quads placed around the table/wrist. Also: laser/cursor visual for the pointer; haptics.
4. Check real-Quest quick taps and trigger-touch behaviour (the simulator reports no
   touch).

## Later (noted 2026-11-01)

- Passthrough: support it on Virtual Desktop's OpenXR runtime (VDXR) if possible (e.g. an
  alpha-blend environment blend mode or VD's passthrough extension), and research whether
  anything similar is possible through SteamVR.
- Lore's close X doesn't respond to a laser click (the click lands on it); investigate.
- Headset-idle freeze on VD (session VISIBLE): investigate; the watchdog can't help when
  the whole process stalls.
- Mirror: optionally show the masked eye plus panels (currently the raw left eye).
- Steam keeps showing the game as running after our quit/restart (TerminateProcess):
  find what Steam waits on, and exit cleanly instead (e.g. ask the game to quit).
- Open map: consider widening the overview camera (and so the clickable area) when the
  mask is off, and softening the far fog band.
- Retire the pixel-probe auto boxes (`@build/@tally/@sel/@terr`) now that panels come
  from the game's windows.
- Top bar: split `ui.sel.Resources` into its children (fame / resources) instead of fixed
  corner rects.
