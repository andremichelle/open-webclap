#!/usr/bin/env bash
# Build the seven OpenRB WebCLAP bundles, one per device, into build/openrb/web/<id>.wclap and
# build/openrb/<id>.wclap.tar.gz, and check them in node. Needs Emscripten (emsdk), Rust with the
# wasm32-unknown-emscripten target (rustup target add wasm32-unknown-emscripten), the CLAP headers and OpenRB-338
# (external/openrb-338) from fetch-deps.py.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/openrb"

if ! command -v em++ >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

python3 "$ROOT/scripts/fetch-deps.py"

# the devices in the order of plugin/openrb.cpp's Kind: the bundle's id (as the site's page) and its name
IDS=(openrb-bass openrb-distortion openrb-pcf openrb-compressor openrb-delay openrb-808 openrb-909)
NAMES=("OpenRB 303" "OpenRB Distortion" "OpenRB PCF" "OpenRB Compressor" "OpenRB Delay" "OpenRB 808" "OpenRB 909")

source "$ROOT/scripts/bundle-license.sh"
LIBM="$(ls -d "${CARGO_HOME:-$HOME/.cargo}"/registry/src/*/libm-0.2.16 | head -1)"
RUST_DOC="$(rustc --print sysroot)/share/doc/rust"
rm -rf "$BUILD/web" "$BUILD"/*.wclap.tar.gz
mkdir -p "$ROOT/site/public/bundled"
rm -f "$ROOT/site/public/bundled/openrb.wclap.tar.gz"

for KIND in "${!IDS[@]}"; do
    ID="${IDS[$KIND]}"
    BUNDLE="$BUILD/web/$ID.wclap"
    mkdir -p "$BUNDLE"
    OUT="$BUNDLE" KIND="$KIND" CLAP_INCLUDE="$ROOT/external/clap/include" CARGO_TARGET_DIR="$BUILD/cargo" \
        "$ROOT/ports/openrb/build.sh"

    # License and notices (LICENSE, NOTICES.md, licenses/): GPL-3.0, no JUCE; libm and Rust's standard library MIT or
    # Apache-2.0; the 808's kit CC0 and MIT (FluidR3Mono), the 909's CC BY 4.0
    KIT_LICENSES=()
    case "$KIND" in
        5) KIT_LICENSES=("$ROOT/ports/openrb/kits/FluidR3Mono_License.md" FluidR3Mono_License.md) ;;
        6) KIT_LICENSES=("$ROOT/licenses/CC-BY-4.0.txt" CC-BY-4.0.txt) ;;
    esac
    BUNDLE_LICENSE="$ROOT/ports/openrb/LICENSE" bundle_license "$BUNDLE" "$ROOT/ports/openrb/NOTICES.md.in" \
        "$LIBM/LICENSE.txt" libm-LICENSE.txt \
        "$RUST_DOC/COPYRIGHT-library.html" Rust-library.html \
        ${KIT_LICENSES[@]+"${KIT_LICENSES[@]}"}
    mkdir -p "$BUNDLE/licenses/rust"
    cp "$RUST_DOC/licenses/"*.txt "$BUNDLE/licenses/rust/"
    # the device's name, its own kit's section (<!-- 808 --> .. <!-- /808 -->), the others' sections out
    python3 - "$BUNDLE/NOTICES.md" "${NAMES[$KIND]}" "$KIND" <<'EOF'
import re, sys
path, name, kind = sys.argv[1], sys.argv[2], int(sys.argv[3])
text = open(path).read().replace("@DEVICE@", name)
for machine, own in (("808", kind == 5), ("909", kind == 6)):
    pattern = re.compile(rf"<!-- {machine} -->\n(.*?)<!-- /{machine} -->\n", re.S)
    text = pattern.sub((lambda m: m.group(1)) if own else "", text)
open(path, "w").write(text)
EOF

    python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/$ID.wclap.tar.gz"
    # not in openDAW's catalogue yet: the site serves them itself
    cp "$BUILD/$ID.wclap.tar.gz" "$ROOT/site/public/bundled/$ID.wclap.tar.gz"
done

node "$ROOT/ports/openrb/check.mjs" "$BUILD/web"
echo "Built ${#IDS[@]} bundles in $BUILD/web — try them on the site (cd site && npm run dev): http://localhost:8130/openrb-bass?local"
