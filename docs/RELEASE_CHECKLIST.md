# Release checklist (Meta XR Simulator)

Run with the release zip installed by `NorthgardVR_Setup.exe` (plus `debug 1` in
`NorthgardVR_settings.txt` for the test hooks), starting from the **Northgard VR** shortcut.
Tick each line in the release's issue / PR.

## Install and start
- [ ] Setup finds Northgard, copies the files, creates both shortcuts
- [ ] First start patches (`NorthgardVR_patch_log.txt` says OK) and VR starts; second start skips patching
- [ ] VR options menu shows the release version at the top right

## Menus
- [ ] Main menu on the flat screen; laser hover and click work
- [ ] Load a save from the menu with the laser

## Match
- [ ] Tabletop shows; grip pan, two-hand zoom/rotate, right stick rotate/zoom, Y reset view
- [ ] A click selects a unit; A drag draws the box at ground height and selects units
- [ ] Trigger with units selected gives an order; trigger with nothing selected selects a building
- [ ] Build menu on the left hand; pick a building, preview follows the laser, trigger places it
- [ ] B cancels a building preview
- [ ] Unit list click centres the view on the unit (pan offset cleared)
- [ ] Tooltips appear on hover over HUD panels
- [ ] Map edge fades smoothly when turning the head

## Pause and dialogs
- [ ] Escape (left menu button) opens the pause menu
- [ ] Quit to Title shows the confirmation dialog; OK returns to the flat title menu (no "SORRY" error)
- [ ] Save and load from the pause menu

## VR options (left thumbstick click)
- [ ] Every row changes and is saved (restart and check `NorthgardVR_settings.txt`)
- [ ] Log → Open folder opens Explorer

## Multiplayer rules (debug: `NorthgardVR_mode.txt`)
- [ ] `Standard`: multiplayer warning
- [ ] `Ranked`: flat 16:9 screen, laser clicks on it; empty file: tabletop back

## Quit
- [ ] Main menu Quit Game closes the game and VR cleanly; Steam goes back to PLAY

## Update path
- [ ] Change the stamp in `NorthgardVR_patched.txt`: the launcher re-patches on the next start
- [ ] `NorthgardVR_Setup.exe /uninstall` removes the files and shortcuts, keeps the settings
