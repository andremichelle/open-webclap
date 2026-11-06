#!/usr/bin/env bash
# Build the Oxo Synth2 WebCLAP bundle into build/oxo-synth2/web/oxo-synth2.wclap. Needs Emscripten (emsdk) and
# the deps from fetch-deps.py.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/oxo-synth2"
WEB="$BUILD/web"
BUNDLE="$WEB/oxo-synth2.wclap"

if ! command -v emcmake >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

python3 "$ROOT/scripts/fetch-deps.py"

emcmake cmake -S "$ROOT/ports/oxo-synth2" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# The bundle's page (ui/) and the factory bank as one patches.json the page loads from
mkdir -p "$BUNDLE/ui"
cp "$ROOT/ports/oxo-synth2/ui/"* "$BUNDLE/ui/"
python3 "$ROOT/ports/oxo-synth2/patches.py" "$ROOT/external/oxo-synth2" "$BUNDLE/ui/patches.json"

# License and notices (LICENSE, NOTICES.md): MIT throughout, no JUCE
source "$ROOT/scripts/bundle-license.sh"
BUNDLE_LICENSE="$ROOT/external/oxo-synth2/LICENSE" bundle_license "$BUNDLE" "$ROOT/ports/oxo-synth2/NOTICES.md.in"

# The node check: parameters, state bytes, notes, page protocol, DSP timing
node "$ROOT/ports/oxo-synth2/check.mjs" "$BUNDLE/module.wasm"

# The archive hosts import (openDAW: Import WebCLAP...)
python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/oxo-synth2.wclap.tar.gz"
echo "Built $BUNDLE — try it in the test host: python3 $ROOT/scripts/serve.py $ROOT, then"
echo "http://127.0.0.1:8123/test-host/?bundle=/build/oxo-synth2/web/oxo-synth2.wclap"
echo "or on the site: http://localhost:8130/oxo-synth2?local"
