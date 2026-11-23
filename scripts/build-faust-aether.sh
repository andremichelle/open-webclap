#!/usr/bin/env bash
# Build the Aether (faust-aether) WebCLAP bundle into build/faust-aether/web/faust-aether.wclap. Needs Emscripten
# (emsdk), the deps from fetch-deps.py, and the Faust compiler (2.88.0 at the time of writing): pass FAUST=/path/to/faust,
# or have it on PATH, or install the release to ~/faust (https://github.com/grame-cncm/faust/releases).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/faust-aether"
WEB="$BUILD/web"
BUNDLE="$WEB/faust-aether.wclap"
GEN="$BUILD/gen"

if ! command -v emcmake >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

FAUST="${FAUST:-}"
if [ -z "$FAUST" ]; then
    if command -v faust >/dev/null 2>&1; then FAUST="$(command -v faust)";
    elif [ -x "$HOME/faust/bin/faust" ]; then FAUST="$HOME/faust/bin/faust";
    else echo "faust not found: set FAUST=/path/to/faust or install the Faust 2.88.0 release to ~/faust" >&2; exit 1; fi
fi
FAUST_INCLUDE="$(cd "$(dirname "$FAUST")/../include" && pwd)"
echo "Using $FAUST ($("$FAUST" --version 2>&1 | head -1)), headers in $FAUST_INCLUDE"

python3 "$ROOT/scripts/fetch-deps.py"

# The generated engine (voice_gen.cpp, fx_gen.cpp), the parameter table (params.inc) and the page's patches.json
python3 "$ROOT/ports/faust-aether/gen.py" "$ROOT/external/faust-aether" "$GEN" "$FAUST"

emcmake cmake -S "$ROOT/ports/faust-aether" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DGEN_DIR="$GEN" -DFAUST_INCLUDE="$FAUST_INCLUDE" "$@"
cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# The bundle's page (ui/) and the presets as one patches.json the page loads from
mkdir -p "$BUNDLE/ui"
cp "$ROOT/ports/faust-aether/ui/"* "$BUNDLE/ui/"
cp "$GEN/patches.json" "$BUNDLE/ui/patches.json"

# License and notices (LICENSE, NOTICES.md): MIT upstream, no JUCE; the Faust standard libraries the generated
# code embeds are LGPL-2.1-or-later with the Faust exception (see NOTICES.md.in)
source "$ROOT/scripts/bundle-license.sh"
BUNDLE_LICENSE="$ROOT/external/faust-aether/LICENSE" bundle_license "$BUNDLE" "$ROOT/ports/faust-aether/NOTICES.md.in"

# The node check: parameters, state, notes, the page protocol, DSP timing
node "$ROOT/ports/faust-aether/check.mjs" "$BUNDLE/module.wasm"

# The archive hosts import (openDAW: Import WebCLAP...)
python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/faust-aether.wclap.tar.gz"

# The site serves this bundle itself (pages on forks run it without the openDAW catalogue; the asset host's
# CORS allows only the studio's own origin)
mkdir -p "$ROOT/site/public/bundled"
cp "$BUILD/faust-aether.wclap.tar.gz" "$ROOT/site/public/bundled/faust-aether.wclap.tar.gz"

echo "Built $BUNDLE — try it in the test host: python3 $ROOT/scripts/serve.py $ROOT, then"
echo "http://127.0.0.1:8123/test-host/?bundle=/build/faust-aether/web/faust-aether.wclap"
echo "or on the site: http://localhost:8130/faust-aether?local"
