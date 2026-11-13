# Northgard VR

VR mod for Northgard: the map on a table in front of you, controller laser input, HUD panels around you.

![Tabletop view](docs/images/tabletop.jpg)

Unofficial, not affiliated with Shiro Games. Requires Northgard on Steam. No game files are included or changed.

**[Download](https://github.com/SoakingRich/Northgard-VR-Mod/releases/latest)** · [Changelog](CHANGELOG.md) · [Issues](https://github.com/SoakingRich/Northgard-VR-Mod/issues)

Released as-is. Tested: Quest 3 via Virtual Desktop. Other OpenXR headsets untested.

## System requirements

| | Minimum | Recommended |
|---|---|---|
| Target | Stable 72 Hz, reduced/medium runtime quality | Stable 90 Hz, high runtime quality |
| CPU | Ryzen 5 3600 / Core i5-9600K | Ryzen 5 5600 / Core i5-12400 |
| GPU | RTX 2070 8 GB / RX 6600 8 GB | RTX 3070 / RX 6700 XT or better |

## Install

1. Extract the release zip.
2. Run `NorthgardVR_Setup.exe` (unsigned: *More info → Run anyway*).
3. Start your VR runtime, then the **Northgard VR** shortcut. Steam must be running.

Steam's Play button still starts the normal game. Uninstall: `NorthgardVR_Setup.exe /uninstall`.

## Controls

| Input | Action |
|---|---|
| Trigger | Click / give orders to selected units |
| A | Select, hold to drag a box |
| Other trigger + Select | Add/remove a unit without clearing the current selection |
| Other trigger + Unit Tally click | Add every unit of that type to the current selection |
| B | Cancel building placement |
| Grip | Drag map (both grips: zoom, rotate) |
| Right stick | Rotate, zoom |
| Left stick X | Table size |
| Y | Reset view |
| X / right stick click (left ≡ when exposed by the runtime) | Escape |
| Left stick click | VR options |

Index: same, Escape = right stick click. Vive: trackpad = stick, right menu = B. WMR: trackpad click = A.

“Other trigger” means the trigger on the controller without the laser (left by default, right
in left-handed mode). Select is A by default and X in left-handed mode. The modifier does not
change orders from the pointer trigger.

The VR options menu repeats the pause shortcut at the top and has a **Controls** guide. The first
time a tabletop match appears after launch, a VR notification points you to that menu.

## Notes

- Ranked matches show on a flat screen in VR.
- Problems: VR options → Log → Open folder, attach `NorthgardVR_log.txt` to an issue.

## Mixed reality (see your room around the table)

Virtual Desktop can blend your real room into VR. The mod paints everything around the table one
colour, and Virtual Desktop replaces that colour with the passthrough camera view.

1. On your PC, open the **Virtual Desktop** app, go to the dashboard → **Streaming**.
2. Turn on **VR Passthrough** and set its colour to **Green** (keep the default Red/Green/Blue values).
3. Set the **frame rate to 90 Hz or lower**. Passthrough does not work at 120 Hz.
4. In the game, click the left stick to open **VR options**:
   - **Mixed reality** → *Chroma key (Virtual Desktop)*
   - **Key colour** → *VD default green*

The bottom line of VR options shows the R/G/B values the mod uses; they must match Virtual
Desktop's exactly. If you see a flat green background instead of your room, they don't match.

## Known issues

- Snow can appear in winter with Weather off.
- Chroma key can tint transparent panels.

[Advanced settings](docs/ADVANCED.md) · [Building from source](docs/DEVELOPMENT.md) · MIT licence
