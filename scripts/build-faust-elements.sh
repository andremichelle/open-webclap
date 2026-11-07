#!/usr/bin/env bash
# Build the Elements (faust-elements) WebCLAP bundle into build/faust-elements/web/faust-elements.wclap. Needs
# Emscripten (emsdk), the deps from fetch-deps.py, a Rust toolchain (cargo) and the Faust 2.88.0 release installed
# to ~/faust (https://github.com/grame-cncm/faust/releases) for its standard libraries: elements.lib uses
# `ondemand`, which only faust-rs (grame-cncm/faust-rs, pinned below) compiles, so this script builds faust-rs
# from source once into external/faust-rs (cached; needs cargo >= 1.85, e.g. via rustup).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/faust-elements"
WEB="$BUILD/web"
BUNDLE="$WEB/faust-elements.wclap"
GEN="$BUILD/gen"

# faust-rs (grame-cncm/faust-rs, LGPL-2.1-or-later) at the commit faust-elements' results were measured with
# (its README: "faust-rs 22999fe5, faustprobe 0.9.2"). Only its CLI binary is built (no JIT backends needed).
FAUST_RS_REPO="https://github.com/grame-cncm/faust-rs.git"
FAUST_RS_COMMIT="22999fe53aaaa5f08542a34a304fb87a276336a2"
FAUST_RS_DIR="$ROOT/external/faust-rs"
FAUST_RS_BIN="$FAUST_RS_DIR/target/release/faust-rs"

if ! command -v emcmake >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

# The Faust standard libraries (basics.lib, maths.lib, ...) faust-rs resolves elements.lib's imports against:
# the official Faust 2.88.0 release, installed to ~/faust (its share/faust).
FAUST_HOME="${FAUST_HOME:-$HOME/faust}"
if [ ! -d "$FAUST_HOME/share/faust" ]; then
    echo "Faust's standard libraries not found in $FAUST_HOME/share/faust: install the Faust 2.88.0 release" >&2
    echo "(https://github.com/grame-cncm/faust/releases) to ~/faust, or set FAUST_HOME" >&2
    exit 1
fi

if [ ! -x "$FAUST_RS_BIN" ]; then
    # prefer rustup's cargo when present: the pinned faust-rs needs a recent rustc (>= 1.95), which Homebrew's
    # standalone toolchain may lag behind
    if [ -x "$HOME/.cargo/bin/cargo" ]; then
        export PATH="$HOME/.cargo/bin:$PATH"
    fi
    command -v cargo >/dev/null 2>&1 || { echo "cargo not found: install Rust (rustup) to build faust-rs" >&2; exit 1; }
    echo "Building faust-rs $FAUST_RS_COMMIT (one-time, cached in $FAUST_RS_DIR)"
    mkdir -p "$FAUST_RS_DIR"
    git -C "$FAUST_RS_DIR" init -q 2>/dev/null || true
    git -C "$FAUST_RS_DIR" remote add origin "$FAUST_RS_REPO" 2>/dev/null || true
    git -C "$FAUST_RS_DIR" fetch -q --depth 1 origin "$FAUST_RS_COMMIT"
    git -C "$FAUST_RS_DIR" checkout -q FETCH_HEAD
    cargo build --release --manifest-path "$FAUST_RS_DIR/Cargo.toml" -p compiler --bin faust-rs
fi
echo "Using $FAUST_RS_BIN ($("$FAUST_RS_BIN" --version 2>&1 | head -1)), Faust libraries in $FAUST_HOME/share/faust"

python3 "$ROOT/scripts/fetch-deps.py"

# The web presets (web/controls.js) as JSON for gen.py, then the generated engine (voice_gen.cpp, body_gen.cpp),
# the parameter table (params.inc) and the page's patches.json
mkdir -p "$GEN"
node "$ROOT/ports/faust-elements/extract-presets.mjs" "$ROOT/external/faust-elements" "$GEN/presets.json"
python3 "$ROOT/ports/faust-elements/gen.py" "$ROOT/external/faust-elements" "$GEN" "$FAUST_RS_BIN" \
    "$FAUST_HOME/share/faust" "$GEN/presets.json"

emcmake cmake -S "$ROOT/ports/faust-elements" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DGEN_DIR="$GEN" "$@"
cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# The bundle's page (ui/), the presets, the sound atlas and the orbit component (faust-orbit-ui, MIT, vendored
# upstream) with its ui descriptor, as the page loads them from
mkdir -p "$BUNDLE/ui/vendor"
cp "$ROOT/ports/faust-elements/ui/"* "$BUNDLE/ui/"
cp "$GEN/patches.json" "$BUNDLE/ui/patches.json"
cp "$GEN/orbit.json" "$BUNDLE/ui/orbit.json"
cp "$ROOT/external/faust-elements/web/atlas.json" "$BUNDLE/ui/atlas.json"
cp -r "$ROOT/external/faust-elements/web/vendor/faust-orbit-ui" "$BUNDLE/ui/vendor/"

# License and notices (LICENSE, NOTICES.md): MIT upstream (Elements' firmware and the model); the Faust standard
# libraries the generated code embeds are LGPL-2.1-or-later with the Faust exception (see NOTICES.md.in)
source "$ROOT/scripts/bundle-license.sh"
BUNDLE_LICENSE="$ROOT/external/faust-elements/LICENSE" bundle_license "$BUNDLE" "$ROOT/ports/faust-elements/NOTICES.md.in"

# The node check: parameters, state, notes, the page protocol, DSP timing
node "$ROOT/ports/faust-elements/check.mjs" "$BUNDLE/module.wasm"

# The archive hosts import (openDAW: Import WebCLAP...)
python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/faust-elements.wclap.tar.gz"

# The site serves this bundle itself (pages on forks run it without the openDAW catalogue; the asset host's
# CORS allows only the studio's own origin)
mkdir -p "$ROOT/site/public/bundled"
cp "$BUILD/faust-elements.wclap.tar.gz" "$ROOT/site/public/bundled/faust-elements.wclap.tar.gz"

echo "Built $BUNDLE — try it in the test host: python3 $ROOT/scripts/serve.py $ROOT, then"
echo "http://127.0.0.1:8123/test-host/?bundle=/build/faust-elements/web/faust-elements.wclap"
echo "or on the site: http://localhost:8130/faust-elements?local"
