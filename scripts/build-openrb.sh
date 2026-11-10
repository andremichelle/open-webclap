#!/usr/bin/env bash
# Build the OpenRB WebCLAP bundle (the 303 and the four effects in one module) into build/openrb/web/openrb.wclap and
# check it in node. Needs Emscripten (emsdk), Rust with the wasm32-unknown-emscripten target (rustup target add
# wasm32-unknown-emscripten), the CLAP headers and OpenRB-338 (external/openrb-338) from fetch-deps.py.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/openrb"
BUNDLE="$BUILD/web/openrb.wclap"

if ! command -v em++ >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

python3 "$ROOT/scripts/fetch-deps.py"

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
OUT="$BUNDLE" CLAP_INCLUDE="$ROOT/external/clap/include" CARGO_TARGET_DIR="$BUILD/cargo" "$ROOT/ports/openrb/build.sh"

# License and notices (LICENSE, NOTICES.md, licenses/): GPL-3.0, no JUCE; the factory kits' samples CC BY 4.0
# (909), CC0 and MIT (808); libm and Rust's standard library MIT or Apache-2.0
source "$ROOT/scripts/bundle-license.sh"
LIBM="$(ls -d "${CARGO_HOME:-$HOME/.cargo}"/registry/src/*/libm-0.2.16 | head -1)"
RUST_DOC="$(rustc --print sysroot)/share/doc/rust"
BUNDLE_LICENSE="$ROOT/ports/openrb/LICENSE" bundle_license "$BUNDLE" "$ROOT/ports/openrb/NOTICES.md.in" \
    "$ROOT/licenses/CC-BY-4.0.txt" CC-BY-4.0.txt \
    "$ROOT/ports/openrb/kits/FluidR3Mono_License.md" FluidR3Mono_License.md \
    "$LIBM/LICENSE.txt" libm-LICENSE.txt \
    "$RUST_DOC/COPYRIGHT-library.html" Rust-library.html
mkdir -p "$BUNDLE/licenses/rust"
cp "$RUST_DOC/licenses/"*.txt "$BUNDLE/licenses/rust/"

python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/openrb.wclap.tar.gz"
node "$ROOT/ports/openrb/check.mjs" "$BUNDLE/module.wasm"
# not in openDAW's catalogue yet: the site serves this one itself
mkdir -p "$ROOT/site/public/bundled"
cp "$BUILD/openrb.wclap.tar.gz" "$ROOT/site/public/bundled/openrb.wclap.tar.gz"
echo "Built $BUNDLE — try it on the site (cd site && npm run dev): http://localhost:8130/openrb-bass?local"
