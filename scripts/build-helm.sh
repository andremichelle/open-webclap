#!/usr/bin/env bash
# Build the Helm WebCLAP bundle into build/helm/web/helm.wclap. Needs Emscripten (emsdk) and the deps from
# fetch-deps.py.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/helm"
WEB="$BUILD/web"
BUNDLE="$WEB/helm.wclap"

if ! command -v emcmake >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

python3 "$ROOT/scripts/fetch-deps.py"

emcmake cmake -S "$ROOT/ports/helm" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# The bundle's page (ui/) and the factory patches, converted to the current patch format in one file
mkdir -p "$BUNDLE/ui"
cp "$ROOT/ports/helm/ui/"* "$BUNDLE/ui/"
python3 "$ROOT/ports/helm/patches.py" "$ROOT/external/helm" "$BUNDLE/ui/patches.json"

# License and notices (LICENSE, NOTICES.md): GPL-3.0, no JUCE; the factory patches are CC BY 4.0
source "$ROOT/scripts/bundle-license.sh"
BUNDLE_LICENSE="$ROOT/ports/helm/LICENSE" bundle_license "$BUNDLE" "$ROOT/ports/helm/NOTICES.md.in" \
    "$ROOT/external/helm/patches/Factory Presets/LICENSE" CC-BY-4.0.txt

# The archive hosts import (openDAW: Import WebCLAP...)
python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/helm.wclap.tar.gz"
echo "Built $BUNDLE, try it on the site: http://localhost:8130/helm?local"
