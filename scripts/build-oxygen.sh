#!/usr/bin/env bash
# Build the oXygen WebCLAP bundle into build/oxygen/web/oxygen.wclap. Needs Emscripten (emsdk)
# and the deps from fetch-deps.py.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/oxygen"
WEB="$BUILD/web"
BUNDLE="$WEB/oxygen.wclap"

if ! command -v emcmake >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

python3 "$ROOT/scripts/fetch-deps.py"

emcmake cmake -S "$ROOT/ports/oxygen" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# The bundle's page (ui/)
cp "$ROOT/modules/juce_webclap/js/"*.js "$ROOT/ports/oxygen/ui/"* "$BUNDLE/ui/"

# License and notices (LICENSE, NOTICES.md, licenses/)
source "$ROOT/scripts/bundle-license.sh"
bundle_license "$BUNDLE" "$ROOT/ports/oxygen/NOTICES.md.in" \
    "$ROOT/external/oxygen/LICENSE" BSD-3-Clause-oXygen.txt \
    "$ROOT/external/fonts/Inter-LICENSE" OFL-1.1-Inter.txt \
    "${JUCE_CORE_LICENSES[@]}" "${JUCE_GRAPHICS_LICENSES[@]}"

# The archive hosts import (openDAW: Import WebCLAP...)
python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/oxygen.wclap.tar.gz"
echo "Built $BUNDLE — try it in the test host: python3 $ROOT/scripts/serve.py $ROOT, then"
echo "http://127.0.0.1:8123/test-host/?bundle=/build/oxygen/web/oxygen.wclap"
