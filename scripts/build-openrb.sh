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

python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/openrb.wclap.tar.gz"
node "$ROOT/ports/openrb/check.mjs" "$BUNDLE/module.wasm"
# not in openDAW's catalogue yet: the site serves this one itself
mkdir -p "$ROOT/site/public/bundled"
cp "$BUILD/openrb.wclap.tar.gz" "$ROOT/site/public/bundled/openrb.wclap.tar.gz"
echo "Built $BUNDLE — try it on the site (cd site && npm run dev): http://localhost:8130/openrb-bass?local"
