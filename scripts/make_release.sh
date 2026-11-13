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
mkdir -p "$OUT/licenses/rust"
cp LICENSE "$OUT/LICENSE.txt"
cp THIRD_PARTY_NOTICES.txt "$OUT/"
cp native/openxr_hdll/third_party/hashlink/LICENSE "$OUT/licenses/HashLink-MIT.txt"
cp native/openxr_hdll/third_party/openxr/LICENSE "$OUT/licenses/OpenXR-Apache-2.0.txt"
cp tools/hlbc/LICENSE "$OUT/licenses/hlbc-MIT.txt"

# Preserve the exact notices shipped by every registry crate linked into hlpatch.exe.
# Cargo has unpacked these sources by this point, so this adds no network dependency.
CARGO_REGISTRY_SRC="${CARGO_HOME:-$HOME/.cargo}/registry/src"
RUST_CRATES=(
    byteorder-1.5.0
    equivalent-1.0.3
    fixedbitset-0.4.2
    flexstr-0.9.2
    fmtools-0.1.2
    hashbrown-0.17.1
    hlbc-derive-0.7.0
    indexmap-2.14.2
    petgraph-0.6.5
    proc-macro2-1.0.107
    quote-1.0.47
    static_assertions-1.1.0
    syn-2.0.119
    thiserror-1.0.69
    thiserror-impl-1.0.69
    unicode-ident-1.0.27
)
for crate in "${RUST_CRATES[@]}"; do
    crate_src=""
    for candidate in "$CARGO_REGISTRY_SRC"/*/"$crate"; do
        if [[ -d "$candidate" ]]; then
            crate_src="$candidate"
            break
        fi
    done
    if [[ -z "$crate_src" ]]; then
        echo "Missing Cargo source directory for $crate" >&2
        exit 1
    fi

    crate_licenses="$OUT/licenses/rust/$crate"
    mkdir -p "$crate_licenses"
    copied=0
    for license_file in "$crate_src"/LICENSE* "$crate_src"/COPYING* \
                        "$crate_src"/UNLICENSE* "$crate_src"/COPYRIGHT*; do
        if [[ -f "$license_file" ]]; then
            cp "$license_file" "$crate_licenses/"
            copied=1
        fi
    done
    if [[ "$copied" -eq 0 ]]; then
        echo "No license file found for $crate" >&2
        exit 1
    fi
done

# The Rust standard library is also linked into hlpatch.exe. Rustup toolchains
# normally ship their own copyright and license files; include them when present.
RUST_SYSROOT="$(rustc --print sysroot)"
mkdir -p "$OUT/licenses/rust-toolchain"
for license_file in "$RUST_SYSROOT"/LICENSE* "$RUST_SYSROOT"/COPYRIGHT* \
                    "$RUST_SYSROOT"/share/doc/rust/LICENSE* \
                    "$RUST_SYSROOT"/share/doc/rust/COPYRIGHT*; do
    if [[ -f "$license_file" ]]; then
        cp "$license_file" "$OUT/licenses/rust-toolchain/"
    fi
done

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
