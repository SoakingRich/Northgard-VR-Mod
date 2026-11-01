# Recon findings (Phase 0)

Snapshot taken 2026-10-30 against the Steam install at
`C:\Program Files (x86)\Steam\steamapps\common\Northgard`.

## Build

| Item | Value |
|---|---|
| Game version | 4.3.29.43337 (`shiro.json`) |
| Steam app id | 466560 |
| Default launch | DirectX 11: `Northgard.exe` → loads `hlboot.dat` + `directx.hdll` |
| Alt launch | OpenGL: `Northgard.exe sdlboot.dat` → `sdl.hdll` |
| VM | HashLink **JIT** (64-bit in root; `hl32/` and `ng32/` are 32-bit fallbacks) |
| Bytecode | `hlboot.dat`, HL bytecode **v4**, **debug info present** |
| Size | 54,162 functions · 49,783 types · 834 natives · 74,945 strings |

**Key point:** `Northgard.exe` accepts the boot file as an argument (that's how the
OpenGL launch option works). We can write a patched **`vrboot.dat`** next to the
original and launch with `Northgard.exe vrboot.dat`. No original game file is modified,
and a Steam update or file verification can't break the flat game.

## Bytecode tooling

- `hlbc` 0.7 parses the file fully.
- The stock serializer had two bugs, both fixed in `tools/hlbc`:
  1. It used `CString`, which panics on HL strings containing an embedded NUL. The fix writes raw bytes.
  2. Object `bindings` were stored in a `HashMap`, which reordered them on write. The fix uses `IndexMap`.
- **Result:** `hlpatch roundtrip hlboot.dat out.dat` is **byte-identical** (16,363,512 bytes).
  We have a verified base for injecting code.

## Engine = stock Heaps.io (open source)

The main loop matches upstream `hxd/System.hl.hx`:
`processEvents → timeoutTick → loopFunc() → Engine.CURRENT.driver.present()`.

The relevant classes are present, with names:

| Area | Classes / functions |
|---|---|
| Frame | `hxd.App.mainLoop`, `hxd.App.render(engine)`, `h3d.Engine.render/begin/end` |
| Render targets | `h3d.Engine.pushTarget/pushTargets/popTarget`, `h3d.Engine.clear` |
| 3D scene | `h3d.scene.Scene.render(engine)`, `Scene.getRenderCamera`; renderers `h3d.scene.pbr.Renderer`, `h3d.scene.fwd.Renderer` |
| Camera | `h3d.Camera.update`, `makeCameraMatrix`, `makeFrustumMatrix`, `rayFromScreen`, `unproject`, `setTransform` |
| Game camera | `cam.CameraController` (`update`, `camUpdate`, `move`, `getGroundIntersect`, `getCamZoomCoefficient`, `setFollowUnit`, `dragUpdate`) |
| Game scene | `CustomScene` (`resize`, `checkResize`) |
| UI | `h2d.Scene`, `h2d.RenderContext`, `h2d.Camera` |
| Driver | `h3d.impl.DirectXDriver` → natives in `directx.hdll` |

`directx.hdll` natives of interest include `create`, `present`, `get_back_buffer`,
`create_texture_2d`, `create_render_target_view`, `om_set_render_targets`,
`copy_resource`, `rs_set_viewports`, and `resize`. No native returns the
`ID3D11Device` directly. Instead, our native lib calls `ID3D11Resource::GetDevice()`
on any `dx_resource` handed to it, such as a Heaps texture's handle. This avoids
replacing or proxying `directx.hdll`.

## Implications

- **Hooks by name.** We patch bytecode at known functions instead of pattern-scanning
  GPU memory.
- **Upstream source.** Heaps source on GitHub is a reference for each patched
  function's semantics, so we don't need to decompile game code beyond confirming that
  Shiro's version matches.
- **Game updates.** Each update changes findex numbers, so the patcher must resolve
  hooks **by name**, never by index.

## Phase 0c findings (2026-10-30)

- **Patching is proven in-game.** Injected natives load and run every frame. Instruction
  insertion with jump fix-ups (`tools/hlpatch/src/inject.rs`) works, including
  inserts before `Ret` and next to backward-jump `Label`s.
- **This HashLink build's quirks:**
  - **`ToDyn` boxes objects.** Each call returns a new pointer, so native-side
    pointer identity is useless. Compare identity in bytecode (`JEq`/`JNotEq`) instead.
  - **By-name field lookup fails on some fields.** `hl_dyn_getp`/`hl_dyn_getd` throw on
    some fields (e.g. `Game.s3d`, `VectorImpl.x`). Do all field access in bytecode
    with the game's own field indices (`hlpatch fields <Type>`). Native code does math only.
- **Main camera.** `Game.onUpdate` calls `this.s3d.camera.update()` (one call site).
  The patch stores that camera in a new global, `vr_main_camera`, and `h3d.Camera.update`
  compares `this` against it.
- **Field indices:** `h3d.Camera` has `pos`=10, `up`=11, `target`=12.
  `h3d.VectorImpl` has `x`/`y`/`z`=0..2 (all f64). `$Game.inst` is static field 21.
- **Testing pitfall:** Steam's "Stop" button does **not** reliably kill Northgard. The
  game can sit on its "Thank you!" exit screen. Leftover processes then run *old* builds
  and confuse results. Kill them with `taskkill /IM Northgard.exe /F` before each test.
  The DLL now logs all running Northgard.exe processes at startup.
- **Root cause of the "game hooks never fire" results:** the remote-control tool's
  "open application: Northgard" call **launches a new, vanilla `Northgard.exe`**
  (no `vrboot.dat` argument; parent = the Claude desktop app) instead of focusing the
  running one. Every test I played was in one of those unpatched instances, while the
  patched instance sat at the main menu behind it. Rule: launch only through Steam, and
  focus the game by clicking its window. The DLL's process list (patched instance has
  Steam as its parent) shows if this happens again.
- **Steam login:** Single Player is disabled ("Not connected") when duplicate instances
  are running. Story Mode works offline.

## Phase 0c result: PASS (2026-10-30 14:47)

- The camera override orbits the real game view around its target. Terrain, trees,
  coastline, fog of war, territory borders and lighting all render correctly from new
  yaw angles at the game's ~40 degree pitch, with no culling holes. Go for stereo.
- The scene is drawn with the **RenderContext camera**, which `RenderContext.setCamera`
  fills via `Camera.load(main)`. The patch tracks it in a second global,
  `vr_render_camera` (set in the `Camera.load` prologue when `src == main`), and
  `Camera.update` applies the override to both cameras.
- Main camera at the start of a skirmish: pos (422, 658, 56), target (383, 614, 8):
  ~76 units from the target, ~40 degree elevation.
- Still to check: steep and top-down pitch, and extreme zoom. The remote-control tool
  can't send numpad keys, so map pitch/zoom to F-keys next time.

## Phase 1 result: PASS (2026-10-31 11:33)

- Theater mode works on Quest 3 through Virtual Desktop (VDXR 1.1.0). The game shows as
  a 3.2 m floating screen.
- `xr_frame(directx.get_back_buffer())` is injected right before `driver.present()` in
  `hxd.System.mainLoop`. The native code takes the game's `ID3D11Device` from the back
  buffer, loads the runtime directly (manifest, then `xrNegotiateLoaderRuntimeInterface`)
  and `CopyResource`s the back buffer (3440x1440, R8G8B8A8_UNORM=28) into an
  R8G8B8A8_UNORM_SRGB (29) swapchain.
- Known issue for Phase 2: the game presents with vsync at the monitor's 60 Hz while the
  headset wants 72–120 Hz, so frames get paced by both. Turn off game vsync while VR is
  active.

## Phase 2 + 3 result: PASS (2026-10-31 12:08)

- **Stereo:** `hxd.App.mainLoop` renders the frame twice: `xr_stereo_begin()`, then
  `engine.render` (left eye), `xr_eye_done(bb)`, then the game's own `engine.render`
  (right eye), `xr_eye_done(bb)`. The `Camera.update` hook sets per-eye pos/target/up and
  the asymmetric projection through the game's own fields (fovY, screenRatio, zoom=1,
  viewX/viewY off-centre).
- **Diorama:** STAGE (floor) space; table at 0.85 m, 0.5 m ahead of the head. A
  table-local point v maps to the world as `C + S*M(v)`, where `C` = game camera target +
  pan offset and `S` = units/m (default 60). Controls: grip-drag pans, two-hand pinch
  zooms and twists to rotate, right stick rotates/zooms, left stick sets table height,
  B recentres, Y resets the view. Alex: "feels really nice".
- **UI:** `Game.onRender` draws underScene, s3d, then s2d. Eye passes hide s2d via
  `visible=false`. Do **not** skip `s2d.render`: AppBase's final composite uses s2d's
  RenderContext, and skipping it gave black eyes. A UI-only pass renders s2d after the
  eyes. The UI image is fully opaque (alpha 255), so the VR UI panel (F8) is off until
  alpha is fixed.
- **Deploy without user action:** versioned DLL names (`XR_LIB=openxr_N` for hlpatch)
  and `NorthgardVR_restart.flag`: the running game relaunches
  `Northgard.exe vrboot.dat` and exits. `steam_appid.txt` (466560) keeps direct launches
  logged into Steam.

## Self-testing with Meta XR Simulator + Operator (2026-10-31 12:40)

- **Runtime choice is automatic.** The DLL tries the active runtime (Virtual Desktop)
  first. If `xrGetSystem` finds no headset (`-35`, form factor unavailable), it falls
  back to the Meta XR Simulator. It finds the simulator through the `AvailableRuntimes`
  registry key, or by searching `C:\Program Files\MetaXRSimulator`
  (`v207.0\meta_openxr_simulator.json` → `SIMULATOR.dll`, emulates a Quest 3,
  recommended eye 1680x1760).
  - Optional `NorthgardVR_runtime.txt`: `auto` (default), `simulator`, `headset`, or a
    manifest path.
  - Caveat: the choice is made at startup, so connect the headset before launching.
- `NorthgardVR_operator.txt` = the Meta XR Operator `windows` folder: the DLL chains the
    `XR_APILAYER_METAX_operator` layer in front of the runtime. The layer serves MCP on
    `http://localhost:8720/mcp` from inside the game.
- **Agent access:** with `meta-xr-operator-mcp-proxy.exe` registered in the Claude
  desktop config, the operator tools reach the cloud session through the PC link.
  Fallback: the built-in browser pane can POST JSON-RPC to `localhost:8720/mcp`
  (protocol `2025-06-18`, `Mcp-Session-Id` header).
- **Tools that matter:** `openxr_capture_composited_image` (eye view),
  `openxr_set_head_pose`, `openxr_set_controller_pose` (`pose_type` aim/grip, stage
  space), `openxr_set_controller_input` (Trigger/Grip/A/B/sticks). On VDXR only the
  read-only tools are listed. Image capture and input automation need the simulator.
- **Driving the PC menu:** Steam Play (never `open_application("Northgard")`), then
  click Single Player (66,254) → New Game (196,262) → Play (64,490) in the 1568x656
  screenshot frame. Mouse Without Borders can steal the foreground. Bringing Steam to
  the front and clicking through to the game window works.
- A restart flag older than the process is now ignored (it used to cause one extra
  relaunch).

### Phase 5 pointer result: PASS (select)

- Simulator head at (0, 1.7, 0) pitched about 40 degrees down. The table sits at
  (0, 0.85, -0.5), and the stereo diorama renders correctly in the simulator.
- With the right controller's aim pose at (0.15, 1.25, -0.15) pointing at the table
  centre and the trigger touched, hover works: the zone tooltip follows the ray.
- **Trigger held at 1.0 → left click → Town Hall selected.** The HUD selection panel
  appears, and the headset shows the zone outline. The logged ray hits the ground at
  exactly the table centre, so the math checks out.
- Open items:
  - (a) A 0.3 s `auto_release` press did not register a click; a held press did. Check
    whether quick real presses get missed.
  - (b) With the pointer off, the game reads the real OS cursor. A cursor at the window
    edge edge-scrolls the camera target, which slides the map on the table. Disable
    edge-scroll in VR, or park the cursor at the centre.
  - (c) Above the map's far edge the eye view is black: the map is unbounded, not a
    table. Needs the table-edge work.
  - (d) Right-click orders (A) not tested yet.

## Windowed mirror, decoupled swapchains, pointer fixes (2026-10-31 12:50)

- **Northgard option set to Windowed** (Options → Graphics). In Fullscreen mode, the game
  re-applies fullscreen whenever its window is focused.
- At XR start the DLL resizes the game window to the headset's eye aspect, as large as
  the monitor's work area allows. It goes at the top-left, as a normal window: 1292x1353
  client for 1680x1760 eyes on the 3440x1440 monitor. The game renders the eyes at the
  window size, so this also sets eye resolution. Rendering eyes at full recommended size
  needs offscreen eye targets (Phase 6).
- Swapchains are `max(recommended eye, back buffer)` in size. Copies use
  `CopySubresourceRegion` into the top-left, and `imageRect` = the copied size, so the
  game window can be any size up to that.
- **Quick-tap fix.** A tap from rest used to turn the pointer on and click in the same
  frame, before the main camera was moved to the controller ray, so the click went to
  the old view. Presses are now latched and sent once the pointer has aimed for 3 frames.
  Even one-frame taps give a full down/up.
- **Edge-scroll fix.** While in stereo with the pointer off, the game's mouse is parked at
  the window centre unless the real cursor is over the game window.
- **Results (simulator):** a 0.15 s trigger tap selected a villager. A 0.15 s A tap with
  the villager selected issued a move order (the ray hit 12 units beside the town hall,
  and the villager went there). Phase 5 select + order: PASS.

## Pointer rework: overview camera (2026-10-31 13:04, openxr_10)

- **Headset bug in openxr_9 (Alex):** holding the trigger or A made all units and
  buildings vanish, and nothing could be selected. Cause: the pointer moved the game's
  **main camera** to the controller. The game culls dynamic objects, and picks, with the
  main camera, so with its narrow view from the controller everything else was culled.
  Short simulator taps hid this.
- **Fix:** while stereo runs, the main camera becomes a fixed **overview camera**. It
  looks straight down on the table centre, covers a 1.5 m table radius at 60 degrees
  fovY, and screen-up = away on the table. The controller ray is intersected with the
  ground plane (camera target z), projected through the overview with Heaps' conventions,
  and the game's mouse is posted to that pixel. The game's own picking and culling use
  the stable overview.
- **Verified in the simulator:**
  - Holding the trigger keeps all objects visible.
  - Held trigger at the table centre selects the Town Hall (mouse at the client centre).
  - Aiming left and nearer moves the mouse left and down (sign check OK).
- **Known limits:**
  - Picking uses a vertical ray at the ground hit, so tall objects are picked by their
    footprint.
  - The ground plane is flat at the camera target height.
  - Objects beyond 1.5 m of table radius are outside the overview (may be culled).
- The VD Quest 3 recommends a 3072x3264 eye; the eyes still render at the window size.

## Phase 6a: render size independent of the window (2026-10-31 17:20, openxr_11)

- `hlpatch patch_rt`:
  - `hxd.Window.get_width/get_height` return `xr_rt_w/h()` while XR runs. That is the
    recommended eye size, capped at 4096: sim 1680x1760, VD Quest 3 3072x3264.
    `Engine.onWindowResize` resizes the engine and back buffer to it, and Present
    stretches it into the window.
  - `hxd.Window.onEvent` rescales `dx.Event.mouseX/Y` (window px → engine px), so the PC
    mouse and the pointer's posted messages both line up.
  - Before `driver.present()`: `if (xr_vsync_off()) getInstance().set_vsync(false)`.
- **Simulator results:**
  - Back buffer 1680x1760 in a 1292x1353 window.
  - 72 fps (the sim's rate) with vsync off.
  - PC menu clicks land correctly.
  - Pointer select still works.
- Side effect: UI renders at the larger size and is scaled down in the window, so the
  menus and HUD on the PC look smaller. Raise Options → UI Scaling if needed.

## Table edge (2026-10-31 17:31, openxr_12)

- Eyes are copied into their swapchains by a fullscreen pixel shader (HLSL compiled at
  runtime with `d3dcompiler_47.dll`).
  - Per pixel, the eye ray (pose, fov tangents) is intersected with the table plane.
  - Inside radius R the game image shows, fading over 3 cm into a 3.5 cm wooden rim.
    Outside, or above the horizon, a dark gradient background.
- Colour path:
  - The back buffer is copied into an R8G8B8A8_TYPELESS texture, read through a
    UNORM_SRGB view (decode).
  - It is written to the sRGB swapchain RTV (encode), so colours are unchanged.
- The game's D3D11 state is saved and restored around the draw: RTV/DSV, viewports, VS/PS,
  input layout, topology, PS SRV/sampler/CB slot 0, blend/depth/raster.
- Table: round, `TABLE_RADIUS_M` 0.55, centre `TABLE_AHEAD_M` 0.65 in front of you. The
  pointer overview now covers R + 0.1 m.
- Known: tall objects near the far edge can be clipped where their view ray crosses the
  plane outside R. Using depth would fix that.
- Simulator: both eyes correct, 71 fps (sim rate), pointer select OK.

## Phase 4: VR HUD panels (2026-10-31 17:58, openxr_14)

- **Alpha.** AppBase.render draws s2d into an offscreen target cleared to `0` (alpha 0),
  then composites it opaquely. `hlpatch patch_ui_alpha` injects after `popTarget`:
  `if (xr_ui_capture()) xr_ui_target(tex.t.res)`. The target (premultiplied, real alpha;
  about 10% coverage in-game) is copied into the UI swapchain during the UI pass.
- **Panels.** `NorthgardVR_ui_layout.txt` in the game folder (repo copy:
  `config/NorthgardVR_ui_layout.txt`) lists rects cut from the UI image.
  - Each rect is anchored to a corner in UI pixels. Each panel has a pose in table
    space (x right, y up, z toward you), a yaw/pitch and a width in metres.
  - Each becomes an XR quad layer with that imageRect of the UI swapchain.
  - The file is re-read every 60 stereo frames when changed (no restart).
  - Without the file, the whole UI is one panel.
- **Measured at 1680x1760, UI scale 1x:**

  | Element | Rect |
  |---|---|
  | Top bar | 0,0,1680,34. Fame/diplomacy icons left 0..150; resources right 680..1680 |
  | Players + fame list | 0,40,170,150 |
  | Tip box | 600,110,480,140 |
  | Weather + minimap (+ unit icons) | bottom-left 275x425 |
  | Warband/Civilians tallies + Build | bottom-right 275x430 |
  | Selection summary | bottom-centre |

- **Layout v1:**
  - economy bar behind the far edge, tips above it, players far-left
  - left HUD (weather/minimap) and right HUD (tallies/build) near the sides, tilted 35
    degrees toward you
  - selection summary at the near edge, tilted 55 degrees
  - notifications far-right
  Verified in the simulator: all visible, and Town Hall selection shows.
- **Not yet:**
  - The pointer can't click HUD panels (they need ray → panel → UI pixel → mouse).
  - Hovered-territory and warning popups not placed yet (seen only when they appear).
  - Rects assume UI scale 1x.
- Debug: `NorthgardVR_ui.raw` (RGBA dump of the UI target at stereo frame 600).

## Clickable HUD panels (2026-10-31 18:12, openxr_15)

- The pointer ray is tested against the panels submitted last frame (`xrt_panel_hit`:
  plane hit, quad basis from the pose quaternion, u/v → imageRect pixel). On a hit the
  game mouse goes to that UI pixel (converted to window px; `onEvent` scales back).
  Otherwise the ray falls through to the table.
- Simulator: aiming at the floating BUILD button and pulling the trigger opened the
  game's build menu.
- The build menu lives in the right column above the unit tallies (UI rows ~720–1330 at
  1680x1760). Layout v2 gives it its own `build_menu` panel; notifications moved up and
  right.
- A click on the first Village card registered at the intended UI pixel (1463,801), but
  no placement ghost appeared. Not yet known whether that card was unavailable or the
  click didn't take. Check next.
- Ideas: laser + cursor visual, highlight the hit panel, haptics.

## Laser + cursor (2026-10-31 19:06, openxr_16/17)

- A 128x64 premultiplied texture (left: dot with a white core and cyan ring; right: beam
  cross-section). Copied each frame into a small swapchain; two quad layers are submitted
  last, so they draw on top of everything.
- Beam: quad from the right aim pose along the ray, its normal turned toward the head.
  It ends at the HUD panel hit (`xrt_panel_hit`) or the table plane hit (within R +
  4 cm); otherwise it is 1.5 m long.
- Dot at the hit point, facing the head, scaled with distance.
- Thicker and brighter while aiming (trigger touched or half-pulled, or A held).
- Simulator: beam and dot visible on the table. Aiming at BUILD highlights it (game
  hover works through the panel).

## Building from VR works (2026-10-31 19:27, openxr_18)

- **Click slop.** `post_mouse` no longer re-sends unchanged positions. While a button is
  held, the mouse stays at the press point until the ray moves more than 10 px (then it
  becomes a drag). The release uses the press point if no drag started.
- The earlier "card click does nothing" was my test aim, not the click. The build menu
  is bottom-anchored and its height varies with its contents, so a fixed UI y hits
  different rows each match. With the aim on the actual card, the card started
  placement (green preview following the laser on the table) and the trigger placed it.
- Simulator flow: BUILD panel → Scout Camp card → aim at the table → trigger. The
  Scout Camp was placed and started construction ("15% built").
- Notifications appear top-left under the players list (e.g. "You have enough Lore…"),
  so the layout moved them to the left (`lt 0 195 430 520`).

## Haptics, watchdog, test notes (2026-10-31 21:50, openxr_20)

- Haptics: vibration output action on both hands (`/output/haptic`).
  - Light tick (0.15, 15 ms) when the laser enters a HUD panel.
  - 0.35/25 ms on select (trigger click).
  - 0.6/45 ms on order (A).
  - The simulator can't show vibration; needs a check on the Quest.
- `build_menu` panel lowered (centre 0.42 m above the table, further right/back).
- Watchdog thread: if `xr_smoke` (once per frame) stops for 45 s, it logs `WATCHDOG` and
  terminates the process.
- **Steam stuck on "Stopping" (cause found):** the Meta XR Simulator app is started by
  the game process, so Steam counts it as part of the game. After the game exits or is
  killed, Steam waits until the simulator window is closed too. Close the simulator →
  Play is back.
- Northgard has no box/drag selection; it selects by clicking, and double-click selects
  all units with the same job. A drag test just selected the unit under the release
  point. Click slop keeps double-clicks intact.
- The game window may open offset (the game restores its own windowed position). Menu
  clicks must use the window's actual position.

## Closing the game without a shell (2026-10-31 21:49, openxr_21)

- `NorthgardVR_quit.flag` in the game folder: the running game closes the
  MetaXRSimulator.exe processes and ends itself. Tested: Steam went straight back to
  PLAY within seconds.
- The simulator is also closed on a restart flag, when the watchdog fires (45 s with no
  frame), and at startup (leftovers). Steam can no longer get stuck on "Stopping".
- The only case still needing a shell or the user: a process that can't even run its
  watchdog thread (stuck in the driver). Not seen so far.

## Territory panel (2026-10-31 21:55, openxr_22)

- `NorthgardVR_dump.flag` in the game folder makes the game dump the UI image
  (`NorthgardVR_ui.raw`) within half a second, for measuring popups while they're open.
- Zone/territory info appears when a zone is **clicked**, not hovered. It sits bottom-right,
  left of the unit tallies, at UI (1100,1615) 305x135 for an unexplored zone (taller for
  explored zones). It is also the second card seen next to a building selection.
  - Layout: `territory rb -590 -450 320 450`, placed front-right of the table.
  - `selection` trimmed to 640 px wide so the two don't duplicate.
- Weather warnings ("You should prepare for winter", "food production reduced") are in
  the weather strip of `left_hud`. Lore/year notifications are in the left
  `notifications` panel.

## Alex's headset feedback fixes (2026-10-31 22:20, openxr_24)

- **Feedback:**
  - the table was too small
  - no building preview
  - drag box invisible except over HUD
  - selection summary clipped
  - selection/territory should sit under the economy bar
  - units vanish when zoomed out
- **Root cause of the preview/drag issues:** the overview mapped the table to most of the
  game screen. Aiming near the table edge put the mouse over (hidden-in-VR) HUD
  elements, so the game hid the building preview and the clicks went to the UI. The
  drag rectangle is drawn in the UI layer, so it only showed where a panel cut-out
  covered it.
- **Fixes:**
  - The overview now maps the table (R + 0.1 m) to a central disc of 0.46 × the smaller
    screen half-size, inside the area the HUD never covers.
  - The camera height is fixed at 70 game units (the field of view widens instead, up to
    150°), so zooming out doesn't move it far away.
  - Drag box: drawn by us as four flat beam quads on the table (play space), between the
    table-plane hit at press and the current hit. Aligned with the table, since the
    overview's screen axes are.
  - Table radius 0.75 m (was 0.55), centre 0.85 m ahead (was 0.65). Layout v3 moves the
    side panels out to about ±1.0 m. Selection (680x330, wider) and territory now sit
    under the economy bar on the far side.
  - Zoom-out capped at 130 units/m (default 60). Beyond about 2.5× the camera far plane
    cuts the map, and at about 7× the game's distance fog covers everything.
    TODO: write zFar/zNear in the camera hook.
- **Simulator checks:**
  - Building preview visible at the laser tip near the territory edge.
  - Drag box drawn on the table.
  - Units/buildings visible at 2×.

## Adjustable table + VR menus (2026-10-31 22:40, openxr_27)

- **Table size:** left stick X resizes it (0.35–1.6 m radius, exponential). The near
  edge stays 10 cm in front of you; the centre moves along the table's forward
  direction. Saved to `NorthgardVR_settings.txt` (`table_radius`) 1 s after you stop,
  and loaded when the table is first placed. Layout panel positions scale with
  radius / 0.75. Verified: grew to 1.59 and back to 0.76, saved both.
- **Main menus in VR:** the theater screen (menus outside a match) is now 2 m tall,
  sized by height for the near-square render, at 2.2 m.
  - It becomes the single hit target for the same pointer code: laser + dot, trigger =
    click, A = right click. `aim_mouse` is skipped when the overview isn't active.
  - Fix: `read_input` locates controllers at `g_fs.predictedDisplayTime`, which was
    stale in theater frames (aim invalid). Theater frames now set it.
  - Verified in the simulator, all by VR clicks: Single Player → New Game → Play started
    a match.
- **In-match menus (pause/options):** they render in the centre of the UI, which isn't
  cut out. Added a `center cm -450 -450 900 900` panel high above the far side
  (transparent when nothing is there). NOT verified yet. Escape went to the simulator
  window; the ≡ button on the left HUD is the in-VR way to open the menu.

## Quest session findings + fixes (2026-11-01 21:30, openxr_29, untested)

- **Drag hitch + no box: PROVEN cause.** The log shows `stereo xrEndFrame failed (-24)`,
  which is `XR_ERROR_LAYER_LIMIT_EXCEEDED`. Projection + 11 panels + laser 2 + drag box 4
  quads = 18 layers, over VD's limit, so those frames are rejected.
  - Fix: the drag box is drawn in the eye mask shader (table-local rect from
    `xr_drag_rect`, 2.5 mm cyan border plus a faint fill) and uses no layers.
  - The panel count is capped at `maxLayerCount` (now logged) − projection − laser.
- **UI dump at the Quest's 3072x3264:** UI scale is still 1x (same pixel sizes).
  - The Lore window opens top-centre (969,103 1134x759).
  - Right column at the bottom: build menu 514 px (y −898), unit tallies 310 px (−374),
    build button (−54). In the simulator match the tallies were about 430 px. **Element
    sizes vary per match/faction/selection**, so fixed rects clip or catch neighbours.
  - Fix: **auto-sized panels.** Every 15 UI frames, 1-px lines of the UI image are
    copied to double-buffered staging textures and read a round later (no stall):
    - a column at W−140: right column runs → button / tallies / build menu
    - a row at H−40: selection cards and territory
    - columns through those: their tops
    Layout anchors `@build @tally @sel @terr`; the position is the bottom centre,
    with metres per pixel = width_m / w.
  - Layout v4:
    - build menu directly beside the tallies
    - selection/territory auto-sized under the economy bar
    - one tall `center` panel (ct, top 80 px to H−560) for Lore/pause windows,
      replacing `tips`
    - h < 0 in a rect means "down to |h| px above the bottom"
- **Building preview on the Quest: still unexplained.** Northgard does show one (green
  ghost seen in the simulator at 1680x1760).
  - Added `render_size W H` in `NorthgardVR_settings.txt` to make the simulator render
    at the Quest's 3072x3264 and reproduce it.
- A quit flag older than the process is now ignored. Otherwise a quit sent while the
  game was already closed would kill the next launch.

## Quest feedback round 2 (2026-11-01 22:25, openxr_32, simulator at 3072x3264)

All checked in the Meta XR Simulator with `render_size 3072 3264` (the Quest's eye size),
driven by the operator, unless marked otherwise.
- **Desktop mirror:** the left eye is copied before the UI pass and put back before
  present. **Verified:** the mirror window shows the 3D view in a match (no table mask,
  no HUD panels). Menus showed before too.
- **Hover sound spam (main menu):** the pointer now stays on 0.5 s after the last
  trigger touch, no longer jumps the mouse to the screen centre when it goes off, and
  owns the game mouse for 2 s after use (real mouse events over the mirror are ignored).
  Not verifiable in the simulator (no capacitive touch flicker); needs the Quest.
- **Duplicate unit tallies / map:** auto-box probes now ignore runs touching the left or
  right 40 px. **Verified:** one tally panel, the selection panel holds only the card.
- **Second drag box:** the `center` panel now covers UI y 80–900 only, above the area
  the game draws its own drag rect in. **Verified:** cyan box only on the table.
- **Panel high in the sky / Lore on the desktop:** the `center` panel (Lore, tips, pause)
  is lower and nearer (`0.00 0.87 -1.05`, 1.10 m wide), drawn just above the economy bar.
  **Verified:** clicking the Lore counter in the economy bar opens Lore in VR, fully
  visible. The UI dump puts the Lore window at UI x 984–2088, y 152–848.
- **Escape from VR:** the left controller's menu button (≡) or the right thumbstick
  click sends Escape (key down on press, up on release; down+up in one frame is missed
  by the game's key polling). **Verified:** it closes the main-menu submenu. A real Esc
  closes Lore.
- **Lore's X with the laser:** the click lands on the X (log: UI ≈ 2066,175, the X is
  2056–2080 × 161–185) but the window doesn't close. Cause unknown; use Escape.
- **Building preview: shows at 3072x3264.** Build → card → hover the table: green ghost
  at the laser tip, red over the Town Hall. Earlier "card does nothing" was again my aim
  (between card rows). The renderer is therefore not the cause on the Quest. Remaining
  suspects are input-side (fixed in openxr_30): the trigger-touch flicker turning the
  pointer off, the mouse jumping to the screen centre, and the real mouse over the
  mirror window taking over. Needs a Quest check.
- **Freeze when the headset went idle (21:51, openxr_29 on VD):** the log stopped at
  `session state -> 4` (VISIBLE), the watchdog never fired (the whole process stalled),
  and Steam's Stop hung on "Stopping"; restarting Steam cleared it. Cause not found yet.
- Mouse coordinates in the log are window-client px; the client size changes when the
  mirror window is restored/resized, so convert with the current client size.

## Open map option (2026-11-01 22:30, openxr_33)

- `open_map 1` (default) in `NorthgardVR_settings.txt` turns the round table mask off:
  the map runs on past the table edge; `open_map 0` brings back the round wooden table.
  Implemented in the mask shader (`drag2.z`), so the cyan drag box still works.
- `save_settings` now keeps every other line of the settings file (it used to drop
  `render_size`, and would drop `open_map`).
- **Verified in the simulator** (`6_open_map_no_mask`): the whole view is map.
- What the mask was hiding, visible now:
  - a pale fog band and dark sky some way past the table (the game's distance fog /
    far plane);
  - fog-of-war darkness;
  - units and buildings far outside the overview camera's view may be culled;
  - the laser can't click there: mouse positions only exist inside the overview's
    screen.
- **Steam stays on "Running"/"Stopping" after the quit flag** (`TerminateProcess` on
  ourselves), even for a game Steam launched directly. Exiting and reopening Steam clears
  it. Not yet known what keeps Steam waiting (overlay helper? process stuck in GPU
  teardown?). The 21:51 Quest freeze may be related.

## HUD panels from the game's own windows (2026-11-01 23:05, openxr_37, layout v5)

Alex asked whether the hand-measured cut-outs would break on other setups. The monitor
doesn't matter (the UI renders at the headset's eye size), but fixed rects do break with
another headset/eye size, the game's UI scale, language, clan or patches. So:
- **Hook:** hlpatch `patch_ui_windows` injects `xr_ui_windows(this)` at the start of
  `ui.BaseUI.update` (runs in menus and matches).
  - Lesson: `ToDyn` on an object register allocates a dynamic wrapper. The native read
    fields from the wrapper, hit an access violation, and HL showed the game's "SORRY — A
    problem occurred" dialog. Fixed with `UnsafeCast` (plain pointer).
- **Native (`xr_uiwin.c`):**
  - Every 4th call it walks `BaseUI.windows` (`Array<ui.Window>`) and takes each
    window's class name from its HL type.
  - Rect = union of the root and its visible descendant flows: `absX/absY`,
    `calculatedWidth/Height × matA/matD`, clipped to the screen.
  - It also reads `BaseUI.tip` as a pseudo-window `tip`.
  - Fields are read straight from memory through libhl's per-class lookup tables
    (`hl_get_obj_rt` + `hl_lookup_find`), so a missing field reads as 0 and never
    throws.
  - The visible set is logged (`UI windows: …`) when it changes.
- **Window classes seen in a match:**
  - `ui.sel.Resources` — the top bar, full width
  - `ui.sel.Players`
  - `ui.win.Notifications` — the list; its root is 50x50, so descendants are needed
  - `ui.win.Notify` / `WarNotify` — pop-ups
  - `ui.win.MiniMap`, `ui.win.Calendar`, `ui.win.CalendarTimers`
  - `ui.win.units.AllUnits` — the tallies
  - `ui.win.BuildButton`, `ui.win.BuildWindow`
  - `ui.sel.BuildingInfos` (and other `ui.sel.*Infos`) — selection
  - `ui.sel.ZoneInfos` — territory
  - `ui.win.Tuto` — tips
  - `ui.win.tech.TechTree` — Lore
  - `ui.win.CooldownInfo` — empty
- **Layout v5 anchors:**
  - `@win:<classes>` (comma list, `*` prefix wildcard; union): exact names claim
    windows before wildcards, and each window goes to one panel.
  - `@rest` gets every unclaimed visible window: Lore, tips, pause, dialogs, and things
    we haven't met.
  - `@win:tip` is placed just above where the laser meets a panel.
  - `claim <classes>` hides windows (Resources, CooldownInfo).
  - The top bar stays as fixed corner rects (it spans the full width).
  - The old `@build/@tally/@sel/@terr` probes and fixed rects still work.
- **Verified in the simulator (3072x3264):**
  - all panels appear
  - Lore (TechTree) appears as its own exactly sized panel above the economy bar
  - the notification pop-up goes to the notifications panel
  - Town Hall selection → selection + territory panels
  - build menu opens next to the tallies
  - Escape (left ≡) closes Lore in a match
- **Not working: hover tooltips.** The game shows none for the laser, even in its own
  UI image (economy bar, build cards). The tip panel is ready, but the game isn't making
  tooltips from our mouse input. Investigate (hover state / mouse-move events /
  `TipElement.onOver`).

## Hover tooltips + hover-sound spam: PROVEN cause and fix (2026-11-02 09:40, openxr_38)

- **Cause:** the game's window uses `TrackMouseEvent`, so Windows sends WM_MOUSELEAVE when
  the cursor leaves it. The VR laser posts mouse moves while the real cursor is elsewhere,
  so Windows answers each move with a leave.
  - Log: first laser move at 09:35:07.843, WM_MOUSELEAVE arrived 4 ms later.
  - The game saw hover → leave → hover…, which replays hover sounds and closes tooltips
    before they open.
- **Fix:** the mod subclasses the game window's procedure (`hook_game_wndproc`, installed
  in `xr_window_mirror`) and drops WM_MOUSELEAVE / WM_NCMOUSELEAVE while the VR pointer was
  used in the last 2 s. The real mouse behaves normally otherwise.
- **Verified in the simulator** (`9_tooltip_above_laser`):
  - hovering BUILD highlights it
  - hovering a unit icon in the tallies shows the game's tooltip ("Merchant")
  - the `@win:tip` panel floats it just above the laser dot, facing the same way as
    the panel
- Earlier "no tooltip" on the economy bar was my aim passing just over the bar (the log
  had no `pointer on HUD panel`).

## Mixed reality / passthrough (2026-11-02 10:15, openxr_39)

- **Setting:** `mixed_reality 1` in `NorthgardVR_settings.txt`. The mod uses whichever
  route the runtime offers:
  1. `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND`: the runtime blends the camera view behind
     transparent pixels.
  2. An `XR_FB_passthrough` layer submitted under everything.
  - With MR on, the eye mask makes everything outside the table rim transparent (alpha 0)
    and forces the round table (open map off).
  - The projection layer gets `BLEND_TEXTURE_SOURCE_ALPHA`.
  - Menus float as an opaque screen in the room.
- **Every launch now logs** the runtime's instance extensions, whether
  `XR_FB_passthrough` is offered, and (once a headset is found) its environment blend
  modes.
- **Virtual Desktop (VDXR 1.1.0 on Alex's PC), PROVEN:**
  - It offers 33 extensions, and `XR_FB_passthrough` is **not** among them, so the
    passthrough-layer route is impossible on VDXR.
  - Its blend modes can only be read with the headset connected (`xrGetSystem` fails
    with -35 otherwise). They'll be logged on the next Quest launch. If they include
    `alpha_blend`, MR switches on automatically with `mixed_reality 1`.
- **Meta XR Simulator:**
  - offers `XR_FB_passthrough`; blend modes: opaque only
  - The mod created the passthrough + layer ("mixed reality ON via XR_FB_passthrough
    layer"), and the simulator's runtime log shows it receiving the layer each frame.
  - But the simulator reports `updateColorSwapchain: hasValidTexture[0] is false` (no
    synthetic-room camera feed, even after choosing LivingRoom and restarting it), so
    the background stays black. Not visually verified.
- **SteamVR:** not tried. Its OpenXR runtime on Quest (via Link/VD) is generally
  opaque-only; to research.

## VR options menu + chroma-key mixed reality for Virtual Desktop (2026-11-02 10:40, openxr_40)

- Virtual Desktop has no OpenXR passthrough. Its "VR Passthrough" (Streaming tab, since
  1.29.0) chroma-keys a chosen colour in the final image into the camera view.
- **Mixed reality mode 3 (chroma key):**
  - the eye mask draws everything outside the table rim (and the sky) as flat key colour
    (default magenta 255,0,255; sRGB-exact)
  - in menus, a 60 m key-coloured quad sits behind the menu screen
  - settings: `mixed_reality 2`, `mixed_reality_key_r/g/b`
- **VR options menu:** the left thumbstick click toggles it (it starts closed). It's
  0.6 m in front of you; laser + trigger to change rows:
  - Mixed reality: Off → Chroma key → Passthrough, where the runtime has one
  - Key colour: Magenta / Green / Blue
  - Map: open map / round table
  - Close
  - Drawn with GDI into its own swapchain. While the laser is on it, the game gets no
    mouse.
  - Changes apply at once and are saved (`settings_set` rewrites one key, keeps the
    rest).
- **Verified in the simulator** (`10_vr_options_menu`, `11_chroma_key_table`): the menu
  opens and closes, hover highlights, and the key colour cycles. Chroma key ON gives a
  magenta background in menus and in a match a round wooden table on magenta.
- **Known issue:** the HUD panels' semi-transparent backgrounds blend with the key
  colour (pinkish), so VD may half-key them or leave a pink tint. Fix if seen on the
  Quest: draw the panels with opaque backgrounds in chroma mode.
