# Building from source

## Native library (`native/openxr_hdll`)

CMake project for Visual Studio 2022 (x64 Release), or MinGW:

```
x86_64-w64-mingw32-gcc -O2 -shared -Ithird_party/hashlink -Ithird_party/openxr \
  -o openxr.hdll src/openxr_hdll.c src/xr_theater.c src/xr_uiwin.c \
  -ladvapi32 -luser32 -lgdi32 -luuid -lshell32 -static-libgcc
```

Add `-DNGVR_VERSION="\"x.y.z\""` to set the version shown in the VR options menu.

## Bytecode patcher (`tools/hlpatch`)

Rust: `cargo build --release` (Windows build: `--target x86_64-pc-windows-gnu`). It uses a
vendored copy of [hlbc](https://github.com/Gui-Yom/hlbc) (MIT) in `tools/hlbc`.

```
hlpatch patch --phase1 <Northgard>\hlboot.dat <Northgard>\vrboot.dat
```

Set `XR_LIB=name` to make the patched game load `name.hdll` instead of `openxr.hdll`.

## Launcher and setup (`native/launcher`)

```
x86_64-w64-mingw32-gcc -O2 -mwindows -o NorthgardVR.exe native/launcher/launcher.c -static-libgcc
x86_64-w64-mingw32-gcc -O2 -mwindows -o NorthgardVR_Setup.exe native/launcher/setup.c -lole32 -luuid -lshell32 -static-libgcc
```

`NorthgardVR.exe` re-patches when `hlboot.dat` changed (game update) or `vrboot.dat` is
missing, then starts `Northgard.exe vrboot.dat`. `NorthgardVR_lib.txt` names the mod DLL
(without `.hdll`, default `openxr`).

## Release

`scripts/make_release.sh <version>` builds `dist/NorthgardVR-<version>.zip`. GitHub Actions
(`.github/workflows/release.yml`) runs the same script on every push and publishes a
GitHub release for tags `v*`. Before tagging:

1. Run the simulator checklist in [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md).
2. Move the notes in [CHANGELOG.md](../CHANGELOG.md) from "Unreleased" to the new version.

## How it works

Northgard runs on the Heaps engine as HashLink bytecode (`hlboot.dat`). `hlpatch` injects calls
into the native library at a few points (frame loop, camera, render passes, UI update, mouse
input). The library talks to the OpenXR runtime directly (no loader needed), renders each eye
from the game's own renderer with an overhead camera, cuts the UI into quad layers and drives
the game's mouse from the controller ray. Findings are in [RECON.md](RECON.md), the plan in
[PLAN.md](PLAN.md) and the open work in [Tasks.md](Tasks.md).

Testing uses the Meta XR Simulator with `debug 1` in the settings, which enables
remote-control files (click, hover, UI dump, restart) for automated checks.
