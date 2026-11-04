# NorthgardVR – Tasks & Milestones

Status: `[ ]` todo · `[~]` in progress · `[x]` done · `[?]` needs decision · `[-]` parked until confirmed real
Owner: **(A)** Alex (headset / decision) · **(C)** Claude (simulator / code)

---

## Milestone 0 – Playable personal build ✅

Done. Note: VD passthrough needs VD's Configure colour to match the mod's Key colour exactly. The hint line at the bottom of the VR options menu shows the R/G/B values. The current setting is "Green" (0, 255, 0), which is why VD needed Green at max. Switch the mod to "VD default green" to keep VD's default values.

## Milestone 1 – Known bugs

- [ ] (C) Snow / weather particles show in winter even with Weather = Off
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

- [~] (C) GitHub Actions (.github/workflows/release.yml): runs scripts/make_release.sh (tested here and from a clean clone); builds on every push, attaches the zip to a pre-release for tags v*. Not yet run on GitHub
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
- [ ] (A) Open beta: GitHub release marked "beta", bug reports via GitHub issues / Discord

## Milestone 5 – Public release

- [x] (C) README: install (Setup + shortcut), other controllers, multiplayer/Ranked, all settings, troubleshooting, known issues
- [x] (C) CHANGELOG.md (in the zip too); version from `make_release.sh <version>` shown in the VR options menu (top right) and the log (simulator: "0.9.0-test (Nov 4 2026)")
- [ ] (A) Publish on GitHub Releases (+ Nexus Mods?)
- [ ] (A) Gameplay video / screenshots for the release page

## Decided against

- Code signing: releases stay unsigned; the README explains the SmartScreen "More info → Run anyway" step

- Flat-screen fallback when VR fails to start
- Contacting Shiro Games before release
