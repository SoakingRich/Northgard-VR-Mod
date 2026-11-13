# Changelog

## v0.9.1 — 2026-11-13

- Hold the non-pointer controller's trigger while selecting to add or remove units without
  clearing the current selection; use the same modifier on a Unit Tally to add every unit
  of that type, while pointer-trigger orders keep their normal behavior
- Dragging the map also moves the game's camera to the same area, keeping camera-tied effects
  aligned with the tabletop view
- Fame/military, selection and territory HUD panels use fixed slots so they do not move or
  overlap as the selection changes
- X and right stick click open the game's Escape / pause action; the VR options menu calls out
  the shortcut and includes a full controls guide
- The first tabletop frame after launch shows a VR notification pointing players to the VR menu
  and its Controls guide
- HUD distance and hand-menu aim controls in the VR options menu
- When the game shows its "a problem occurred" screen, the error is written to
  `NorthgardVR_log.txt`, and Continue returns to a working tabletop (it used to draw the map in
  a corner)

## v0.9.0 — 2026-11-04

First public beta. Primary hardware setup: Quest 3 through Virtual Desktop. Development and
regression testing also use the Meta XR Simulator.

- Tabletop VR view of the map with HUD panels around the table and on the left hand
- Controller laser: A selects / drags a selection box, trigger gives orders (or clicks
  buildings, panels and menus), B cancels a building placement
- Selection box drawn at the ground height where it starts; laser hits the real terrain
- Pan offset cleared when the game moves its own camera (unit list, alerts)
- Map fades out with distance instead of a hard far-plane edge
- Menus, pause menu and its confirmation dialogs on a flat screen in VR
- VR options menu: background, mixed reality (Virtual Desktop chroma key or OpenXR
  passthrough), key colour, map style, weather particles, pointer hand, table height,
  panel size, log folder; version shown at the top
- Multiplayer warning; Ranked matches play on a flat screen
- Bindings for Quest Touch, Valve Index, HTC Vive, WMR and the Khronos simple controller
  (only Quest Touch tested on hardware)
- Installer with desktop / Start menu shortcuts, launcher that re-patches after game updates,
  uninstaller that keeps your settings
