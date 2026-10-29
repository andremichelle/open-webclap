#!/usr/bin/env bash
# Build the ZLWarm WebCLAP bundle into build/zl-warm/web/zl-warm.wclap. Needs Emscripten (emsdk) and
# the deps from fetch-deps.py.
set -euo pipefail
PORT=zl-warm
source "$(dirname "$0")/zl-build.sh"
zl_build "$@"
zl_bundle "$ROOT/external/zl-friz/LICENSE" MIT-friz.txt
