#!/usr/bin/env bash
# Build the Radar WebCLAP bundle into build/radar/web/radar.wclap. Needs Emscripten (emsdk)
# and the deps from fetch-deps.py.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/radar"
WEB="$BUILD/web"
BUNDLE="$WEB/radar.wclap"

if ! command -v emcmake >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

python3 "$ROOT/scripts/fetch-deps.py" # JUCE, CLAP and the fonts: the port is a rewrite, nothing of radar is fetched

emcmake cmake -S "$ROOT/ports/radar" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# The bundle's pages (ui/: the window's, the device panel's), the panel's shared controls beside them (ui/opendaw-page)
cp "$ROOT/modules/juce_webclap/js/"*.js "$ROOT/ports/radar/ui/"* "$BUNDLE/ui/"
mkdir -p "$BUNDLE/ui/opendaw-page"
cp "$ROOT/modules/opendaw-page/panel.css" "$ROOT/modules/opendaw-page/panel.js" "$BUNDLE/ui/opendaw-page/"

# License and notices (LICENSE, NOTICES.md, licenses/)
source "$ROOT/scripts/bundle-license.sh"
bundle_license "$BUNDLE" "$ROOT/ports/radar/NOTICES.md.in" \
    "$ROOT/ports/radar/LICENSE" MIT-radar.txt \
    "$ROOT/external/fonts/Inter-LICENSE" OFL-1.1-Inter.txt \
    "${JUCE_CORE_LICENSES[@]}" "${JUCE_GRAPHICS_LICENSES[@]}"

# The archive hosts import (openDAW: Import WebCLAP...)
python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/radar.wclap.tar.gz"
echo "Built $BUNDLE — try it on the site (npm run dev in site/): http://localhost:8130/radar?local"
