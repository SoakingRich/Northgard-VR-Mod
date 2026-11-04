#!/usr/bin/env bash
# Builds the NorthgardVR release zip (Windows files, cross-compiled on Linux).
# Needs: x86_64-w64-mingw32-gcc, Rust with the x86_64-pc-windows-gnu target, python3 + numpy + pillow, zip.
# Usage: scripts/make_release.sh [version]   -> dist/NorthgardVR-<version>.zip
set -euo pipefail
cd "$(dirname "$0")/.."
VER="${1:-dev}"
OUT="dist/NorthgardVR-$VER"
rm -rf "$OUT" && mkdir -p "$OUT"
CC=x86_64-w64-mingw32-gcc
( cd native/openxr_hdll && $CC -O2 -shared -DNGVR_VERSION="\"$VER\"" -Ithird_party/hashlink -Ithird_party/openxr -o "../../$OUT/openxr.hdll" \
    src/openxr_hdll.c src/xr_theater.c src/xr_uiwin.c -ladvapi32 -luser32 -lgdi32 -luuid -lshell32 -static-libgcc )
$CC -O2 -mwindows -o "$OUT/NorthgardVR.exe" native/launcher/launcher.c -static-libgcc
$CC -O2 -mwindows -o "$OUT/NorthgardVR_Setup.exe" native/launcher/setup.c -lole32 -luuid -lshell32 -static-libgcc
( cd tools/hlpatch && cargo build --release --target x86_64-pc-windows-gnu -q )
cp tools/hlpatch/target/x86_64-pc-windows-gnu/release/hlpatch.exe "$OUT/"
cp config/NorthgardVR_ui_layout.txt config/NorthgardVR_notify_rules.txt CHANGELOG.md "$OUT/"
( cd "$OUT" && python3 ../../tools/gen_sky.py >/dev/null && rm -f ./*.png )
cat > "$OUT/README.txt" <<TXT
NorthgardVR $VER - VR mod for Northgard (Steam)

Install:   extract this zip anywhere, run NorthgardVR_Setup.exe.
Play:      "Northgard VR" shortcut (desktop / Start menu). Steam's Play button stays the normal game.
Uninstall: NorthgardVR_Setup.exe /uninstall  (your NorthgardVR settings are kept)
Problems:  VR options (left stick click) > Log > Open folder, and send NorthgardVR_log.txt.
TXT
( cd dist && rm -f "NorthgardVR-$VER.zip" && zip -qr "NorthgardVR-$VER.zip" "NorthgardVR-$VER" )
ls -la "dist/NorthgardVR-$VER" "dist/NorthgardVR-$VER.zip"
