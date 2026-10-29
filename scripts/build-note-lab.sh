#!/usr/bin/env bash
# Build the Note Lab WebCLAP bundle into build/note-lab/web/note-lab.wclap and check it in node. Needs Emscripten
# (emsdk) and the CLAP headers from fetch-deps.py.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/note-lab"
BUNDLE="$BUILD/web/note-lab.wclap"

if ! command -v em++ >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

python3 "$ROOT/scripts/fetch-deps.py"

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
OUT="$BUNDLE" CLAP_INCLUDE="$ROOT/external/clap/include" "$ROOT/ports/note-lab/build.sh"

python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/note-lab.wclap.tar.gz"
node "$ROOT/ports/note-lab/check.mjs" "$BUNDLE/module.wasm"
# not in openDAW's catalogue yet: the site serves this one itself
mkdir -p "$ROOT/site/public/bundled"
cp "$BUILD/note-lab.wclap.tar.gz" "$ROOT/site/public/bundled/note-lab.wclap.tar.gz"
echo "Built $BUNDLE — try it in the test host: python3 $ROOT/scripts/serve.py $ROOT, then"
echo "http://127.0.0.1:8123/test-host/?bundle=/build/note-lab/web/note-lab.wclap"
