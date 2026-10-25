#!/usr/bin/env bash
# Build the OpenSpatialDelay WebCLAP bundle into build/open-spatial-delay/web/open-spatial-delay.wclap. Needs Emscripten
# (emsdk) and the deps from fetch-deps.py.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/open-spatial-delay"
WEB="$BUILD/web"
BUNDLE="$WEB/open-spatial-delay.wclap"

if ! command -v emcmake >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

python3 "$ROOT/scripts/fetch-deps.py"

emcmake cmake -S "$ROOT/ports/open-spatial-delay" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# The bundle's page (ui/)
cp "$ROOT/modules/juce_webclap/js/"*.js "$ROOT/ports/open-spatial-delay/ui/"* "$BUNDLE/ui/"

# License and notices (LICENSE, NOTICES.md, licenses/)
source "$ROOT/scripts/bundle-license.sh"
bundle_license "$BUNDLE" "$ROOT/ports/open-spatial-delay/NOTICES.md.in" \
    "$ROOT/external/open-spatial-delay/LICENSE" GPL-3.0.txt \
    "$ROOT/external/libmysofa/LICENSE" BSD-3-Clause-libmysofa.txt \
    "$ROOT/licenses/OFL-1.1.txt" OFL-1.1.txt \
    "$ROOT/external/fonts/DejaVu-LICENSE" DejaVu.txt \
    "${JUCE_CORE_LICENSES[@]}" "${JUCE_GRAPHICS_LICENSES[@]}" "${JUCE_FLAC_LICENSES[@]}"

# The archive hosts import (openDAW: Import WebCLAP...)
python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/open-spatial-delay.wclap.tar.gz"
echo "Built $BUNDLE — try it in the test host: python3 $ROOT/scripts/serve.py $ROOT, then"
echo "http://127.0.0.1:8123/test-host/?bundle=/build/open-spatial-delay/web/open-spatial-delay.wclap"
