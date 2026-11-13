# Advanced settings

## Settings file

The VR options menu stores its choices in `NorthgardVR_settings.txt` in the Northgard folder
(`…\Steam\steamapps\common\Northgard`). You can also edit it by hand while the game is closed:

| Key | Meaning |
|---|---|
| `background` | 0 dark, 1 stormy sky, 2 dusk sky |
| `mixed_reality` | 0 off, 1 OpenXR passthrough if the runtime offers it, 2 chroma key (Virtual Desktop) |
| `mixed_reality_key_r/g/b` | Chroma-key colour (0–255 each) |
| `open_map` | 1 = map without a table edge, 0 = round wooden table |
| `weather` | 1 = show the game's snow/rain particles |
| `left_handed` | 1 = laser and buttons on the left controller |
| `table_height` | 0 auto (below your head), 1 seated, 2 standing |
| `table_radius` | Table radius in metres (0.35–1.6; also left stick left/right) |
| `panel_size` | HUD panel size in % (75 / 100 / 125) |
| `hud_distance` | HUD distance in % of the layout file's positions (75 near / 88 normal / 100 far); panels keep their size |
| `hand_aim` | cm above (+) or below (−) your eyes that the wrist panel faces (−20 to +10) |
| `menu_size_w`, `menu_size_h` | Resolution the menus render at (default 1600 × 900) |
| `debug` | 1 = developer test tools (remote-control files, F-keys, simulator fallback). Leave off. |

## OpenXR runtime

By default the mod uses your system's active OpenXR runtime. To force one, create
`NorthgardVR_runtime.txt` containing `auto`, `headset`, `simulator` (Meta XR Simulator), or the
full path of a runtime's JSON manifest.

## HUD layout

`NorthgardVR_ui_layout.txt` places every HUD panel (position, angle, size) around the table or
on the left hand. It is reloaded while the game runs, so you can tune it live. The format is
described at the top of the file; `lhand` at the end of a line attaches a panel to the left
controller.

Lines ending in `row` get fixed slots side by side (in file order), centred on the `@row` line.
Each slot is the line's `width_m` wide and stays in place whether its panel is shown or not; a
panel too big for its slot is shrunk to fit. `rowup` stacks a panel on top of the one before it.

## VR notifications

Message cards appear to your front-right. Colours: info (cyan), success (green), warning
(amber), danger (red). Point the laser at one and pull the trigger to dismiss it.

- **From a script or another tool:** write lines to `NorthgardVR_notify.txt` in the game folder,
  `<info|success|warning|danger> <seconds> <text>` (plain text also works). The file is read
  and deleted within half a second.
- **From game events:** `NorthgardVR_notify_rules.txt`, lines like
  `on NewYear info 6 A new year begins`, with `{1}`… for the event's values. The log lists the
  events the game sends as they happen.
