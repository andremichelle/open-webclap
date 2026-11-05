#!/usr/bin/env bash
# Build the Pearls WebCLAP bundle into build/pearls/web/pearls.wclap and check it in node. Needs Emscripten (emsdk),
# the CLAP headers and Beads with stmlib (external/eurorack) from fetch-deps.py.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/pearls"
BUNDLE="$BUILD/web/pearls.wclap"

if ! command -v em++ >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

python3 "$ROOT/scripts/fetch-deps.py"

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
OUT="$BUNDLE" CLAP_INCLUDE="$ROOT/external/clap/include" EURORACK="$ROOT/external/eurorack" "$ROOT/ports/pearls/build.sh"

python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/pearls.wclap.tar.gz"
node "$ROOT/ports/pearls/check.mjs" "$BUNDLE/module.wasm"
# not in openDAW's catalogue yet: the site serves this one itself
mkdir -p "$ROOT/site/public/bundled"
cp "$BUILD/pearls.wclap.tar.gz" "$ROOT/site/public/bundled/pearls.wclap.tar.gz"
echo "Built $BUNDLE — try it on the site (cd site && npm run dev): http://localhost:8130/pearls?local"
