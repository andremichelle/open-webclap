#!/usr/bin/env bash
# Build the Glitchfest WebCLAP bundle from opendaw-commons (wclaps/glitchfest) into build/glitchfest/web/glitchfest.wclap.
# Needs Emscripten (emsdk), the CLAP headers from fetch-deps.py and opendaw-commons next to this repository (or $COMMONS).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMMONS="${COMMONS:-$ROOT/../opendaw-commons}"
BUILD="$ROOT/build/glitchfest"
BUNDLE="$BUILD/web/glitchfest.wclap"

if ! command -v em++ >/dev/null 2>&1; then
    for env in "${EMSDK:-}/emsdk_env.sh" "$HOME/emsdk/emsdk_env.sh" "$HOME/Development/emsdk/emsdk_env.sh"; do
        if [ -f "$env" ]; then source "$env" >/dev/null 2>&1; break; fi
    done
fi

python3 "$ROOT/scripts/fetch-deps.py"

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
OUT="$BUNDLE" CLAP_INCLUDE="$ROOT/external/clap/include" "$COMMONS/wclaps/glitchfest/source/build.sh"

python3 "$ROOT/scripts/pack-wclap.py" "$BUNDLE" "$BUILD/glitchfest.wclap.tar.gz"
echo "Built $BUILD/glitchfest.wclap.tar.gz, open /glitchfest?local on the site (cd site && npm run dev)"
