# NorthgardVR – Tasks & Milestones

Status: `[ ]` todo · `[~]` in progress · `[x]` done · `[?]` needs decision · `[-]` parked until confirmed real
Owner: **(A)** Alex (headset / decision) · **(C)** Codex (simulator / code)

---

## Milestone 0 – Playable personal build ✅

Done. Note: VD passthrough needs VD's Configure colour to match the mod's Key colour exactly. The hint line at the bottom of the VR options menu shows the R/G/B values. The current setting is "Green" (0, 255, 0), which is why VD needed Green at max. Switch the mod to "VD default green" to keep VD's default values.

## Milestone 1 – Known bugs

- [ ] (C) Snow / weather particles show in winter even with Weather = Off
- [x] (C) Game error screen ("a problem occurred"): the exception message and stack now go to NorthgardVR_log.txt (`GAME ERROR:` lines), and VR keeps the match render size while the screen is up, so "Continue" returns to a working tabletop (before: the map drew in a corner with white around it). Simulator: test error in a match and in the main menu, Continue worked both times (debug tool: NorthgardVR_throw.flag)
- [ ] (A) Co-op conquest (VR host + desktop player): the joiner hung on the last loading step and got "Failed to connect" (Retry worked), and the host started paused. Try once more, and once with both players on the normal game, to see if VR is involved
- [ ] (C) Co-op conquest: error after "Return to Conquest Map" (2026-11-12) — cause unknown; next time the log has the error text
- [ ] (C) The VR pointer stayed off on the error screen in one simulator run (worked for Alex): check
- [x] (C) Quitting from the game menu now shuts VR down first (tested twice in the simulator: game ends, Steam back to PLAY after a Steam restart). Before, the menu quit skipped the VR shutdown and could freeze the desktop.
- [ ] (A) On the headset (VD): quit from the main menu and check Steam goes back to PLAY by itself
- [x] (C) Lore window closes with a laser click (simulator; the click landed outside the window, which also closes it – X itself not separately confirmed)
- [x] (C) Crash: a VR layer with an invalid pose failed every frame (XR_ERROR_POSE_INVALID) and the game died; layer poses are now validated before submit (tested with the laser starting at the eye: no failure)
- [x] (C) Minimised game window broke the laser (no client area): the mod restores it without taking focus
- [-] (C) Build-menu tooltips missing for some buttons – not reproduced: normal, locked, Mystic and Military buttons all show tooltips (simulator, 2026-11-03)
- [x] (C) Fog band between the map edge and the sky with passthrough + open map: dark fog far from the table now shows the room (simulator). Skybox variant: check
- [-] (C) Freeze when the headset goes idle / sleeps (ultra-low priority)
- [-] (C) Building preview sometimes missing (seen once)
- [-] (C) Convert-cost label only shown some of the time
- [-] (A) VD passthrough: HUD panels disappear

## Milestone 1b – Controls & view (Alex's list, 2026-11-03)

- [x] (C) A = select / drag a selection box; trigger = orders (right click) while units are selected, otherwise a click (buildings, HUD panels, menus, placing). Tested in the simulator: trigger with units → right click; trigger with nothing selected → selected the Town Hall; trigger in build preview → house placed; A drag → box + units selected
- [x] (C) Selection box drawn at the ground height where the drag starts (terrain height read from the game's height map); the laser also hits the real terrain for hover/clicks. Tested: drag started on the coast at height 1.8 instead of the table plane (8)
- [x] (C) B cancels a building placement (simulator: preview → B → nothing selected). B does nothing otherwise
- [x] (C) B no longer re-places the table (the headset's hold-Oculus does that); Y still resets the view
- [x] (C) When the game moves its camera (unit list click, alerts) your pan offset is cleared first. Tested: panned, clicked a villager in the unit list → landed centred on it
- [x] (C) Map fades out with distance in a circle around you instead of the game's straight far-plane edge (eye cameras get a 1.6× longer far plane; fade 55–90 % of it). Simulator: smooth edge when turning the head
- [x] (C) Pause menu "Are you sure?" dialog (Quit to Title) now shows in VR in place of the buttons, same size; OK/Cancel work with the laser (simulator)
- [x] (C) "Quit to Title" leaves the 3D view and goes back to the flat menu screen. Stereo now also needs the match itself running (Game.onUpdate / Game.render), not just its camera; the flat screen after a match also submits a cleared eye layer (else the simulator kept the old map frozen in view); the laser pauses ~2 s during the switch (a laser click carried over into the teardown made the game show "SORRY, a problem occurred"). Simulator: quit → title menu, laser works on it, loading again → 3D map
- [x] (C) HUD row under the top bar: fame + military/diplomacy, selection, territory in fixed slots (nothing moves when the selection changes, no overlap); minimap + calendar on their own to the right of everything; notifications stay on the left. Simulator: Town Hall and villager selections, panels stay put
- [x] (C) VR options "HUD distance" (Near 75 % / Normal 88 % default / Far 100 %): panels keep their size and move towards you. Simulator: all three
- [x] (C) Wrist panel aims from its middle (it looked over your head) at your eyes; VR options "Hand menu aim" offset (−20 … +10 cm). Simulator: setting cycles; the angle itself needs the headset
- [ ] (A) Headset: the new HUD row, HUD distance and Hand menu aim feel right
- [x] (C) Dragging the map in VR also moves the game camera there (same call as a minimap click), so camera-tied effects such as weather fog follow; the view doesn't jump. Simulator: drags move the game camera, pan offset goes to 0
- [x] (C) Controls are discoverable in VR: X and right stick click open the game pause menu; the VR options menu names the shortcut at the top and has a Controls page with Back; the first real tabletop frame after launch shows one VR notification pointing to left stick + Controls. Simulator: notification captured on first stereo entry, guide and Back exercised with the laser, X opened Pause, right stick still toggled Pause
- [~] (C) Off-hand trigger selection modifier: the logical non-pointer trigger latches the game's `Hotkeys.addUnit` (Shift) through a primary select click/drag; on `ui.win.units.AllUnits`, a modified click becomes Shift+right-click so the game adds the whole type. Pointer-trigger orders and other HUD clicks stay unchanged; mouse/Shift/Escape state clears on focus, tracking, menu, render, window and shutdown transitions, with held controls required to return neutral. Simulator (right- and left-handed): normal tally click cycled one unit, the modified tally path selected a group, a modified map primary click held Shift through mouse-up, pointer-trigger map orders stayed ordinary right-clicks, and opening the VR menu mid-gesture released mouse then Shift without a repeat click. Individual unit add/remove and additive drag-box outcomes remain to be verified
- [ ] (A) Headset, late game with weather: the fog clears where you drag to, without a minimap click. Needs an install whose hlpatch.exe has the camera hook (the game folder's hlpatch.exe is from 2026-11-04, so the launcher's vrboot.dat on 2026-11-05 lacked it)
- [ ] (A) Check on the headset: the new buttons feel right, the fade distance looks good when zoomed in/out, Quit to Title in VD
- Note: in the simulator, pressing A also pulls the trigger; the mod ignores a trigger press while A is down

## Milestone 2 – Release hardening

- [x] (C) Uses the system's active OpenXR runtime (VD, SteamVR, Link…); the simulator fallback only with `debug 1`. Testing on SteamVR / Link is under QA
- [~] (C) Controller bindings: Quest Touch, Valve Index (full), HTC Vive and WMR (basic), Khronos simple controller (fallback). All five accepted by the simulator runtime; Quest input still works (runtime picked meta/touch_controller_plus). Index/Vive/WMR not tested on real hardware – the active profile is logged ("controller profile:") so testers can report it
- [x] (C) Chroma-key passthrough only offered on Virtual Desktop (debug: also simulator); Mixed reality row says "Not available on this runtime" when nothing works (tested: non-debug on simulator → chroma off)
- [x] (C) Test-only code behind the `debug 1` setting: remote-control files (click/hover/find/dump/quit/restart), F7–F10 / numpad keys, closing other game instances and the simulator, `render_size`, the simulator fallback, diagnostic logs
- [x] (C) Multiplayer warning (Standard/Private) and a stronger Ranked warning, read from GameState.gameType (simulator, with a debug override – no real online match tested)
- [x] (C) Ranked matches play on the flat screen (16:9, laser still clicks on it) instead of the tabletop, with a notice; the tabletop comes back when the match ends. Simulator (debug mode override): Ranked from the start and mid-match → flat; leaving Ranked mid-match → tabletop; Quit to Title → next match 3D. No real online Ranked match tested
- [x] (C) "Log – Open folder" row in the VR options menu (tested in simulator)
- [x] (C) Left-handed mode: "Pointer hand" row in VR options mirrors the controllers (laser, trigger and stick on the left, hand menu on the right; Menu button unchanged). Tested in the simulator menus; hand-menu placement in a match not yet checked
- [x] (C) VR options: "Table height" (Auto below your head 0.55–1.10 m / Seated 0.70 m / Standing 1.00 m) and "Panel size" (75/100/125 %) for the HUD panels. Tested in the simulator in a match

## Milestone 3 – Install & update

- [x] (C) GitHub Actions (.github/workflows/release.yml): runs scripts/make_release.sh on main, `v*` tags, pull requests and manual dispatch; attaches the zip to a GitHub release for tags. Main run 41765514049 and `v0.9.0` tag run 41772237123 both succeeded on GitHub (2026-11-04); the public asset was created
- [x] (C) Standalone launcher `NorthgardVR.exe` (native/launcher): re-patches with `hlpatch.exe` when hlboot.dat changed (game update) or vrboot.dat is missing, then starts `Northgard.exe vrboot.dat`; if patching fails it offers the normal game. Tested on Alex's PC: first run patched (Windows hlpatch.exe, exit 0) and started VR; second run skipped patching. Failure dialog not yet tested
- [x] (C) Windows `hlpatch.exe` builds here (Rust windows-gnu target)
- [ ] (A) Test that the game connects to Steam (login, achievements) when started from the launcher instead of from Steam
- [x] (C) Installer `NorthgardVR_Setup.exe`: finds Northgard via Steam's registry entry + libraryfolders.vdf, copies the mod files, creates desktop + Start menu shortcuts "Northgard VR", offers to start. Tested on Alex's PC: refuses cleanly while the game runs, installs, shortcuts created, installed launcher patched and started VR
- [x] (C) Uninstaller: `NorthgardVR_Setup.exe /uninstall` removes the installed and generated files + shortcuts, keeps settings. Tested on Alex's PC

## Milestone 4 – QA

- [x] (C) Simulator release checklist written: docs/RELEASE_CHECKLIST.md (run before every tag). Install from a release zip (0.9.0-test) checked: Setup → shortcut → first start patched → VR runs, version in the log and options menu
- [ ] (A) Runtimes: Virtual Desktop, SteamVR, Quest Link
- [ ] (A) GPU coverage: NVIDIA plus at least one AMD tester
- [ ] (C/A) All clans, campaign / conquest, diplomacy, tech tree, trade, Lore, events (low priority)
- [ ] (A) 2-hour headset session including sleep/wake and alt-tab (low priority)
- [~] (A) Public release: `v0.9.0` is already public on GitHub; harden `v0.9.1`, smoke-test its public asset, then announce with bug reports via GitHub issues / Discord

## Milestone 5 – Public release

- [x] (C) README: install (Setup + shortcut), other controllers, multiplayer/Ranked, all settings, troubleshooting, known issues
- [x] (C) CHANGELOG.md (in the zip too); version from `make_release.sh <version>` shown in the VR options menu (top right) and the log (simulator: "0.9.0-test (Nov 4 2026)")
- [x] (A) `v0.9.0` published on GitHub Releases (2026-11-04); it remains the immutable baseline
- [ ] (A) Publish and announce the hardened `v0.9.1` release (+ Nexus Mods?)
- [ ] (A) Gameplay video / screenshots for the release page
