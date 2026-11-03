#!/usr/bin/env bash
# Build the Signalsmith Basics WebCLAP bundle (seven effects) into build/signalsmith-basics/web/signalsmith-basics.wclap. Needs Emscripten (emsdk) and the
# CLAP headers from fetch-deps.py.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/signalsmith-basics"
BUNDLE="$BUILD/web/signalsmith-basics.wclap"

if ! command -v em++ >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

python3 "$ROOT/scripts/fetch-deps.py"

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
OUT="$BUNDLE" CLAP_INCLUDE="$ROOT/external/clap/include" SIGNALSMITH="$ROOT/external" "$ROOT/ports/signalsmith-basics/build.sh"

python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/signalsmith-basics.wclap.tar.gz"
node "$ROOT/ports/signalsmith-basics/check.mjs" "$BUNDLE/module.wasm"
echo "Built $BUNDLE — try it on the site (cd site && npm run dev): http://localhost:8130/signalsmith-chorus?local"
