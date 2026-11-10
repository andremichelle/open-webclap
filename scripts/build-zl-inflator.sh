#!/usr/bin/env bash
# Build the ZLInflator WebCLAP bundle into build/zl-inflator/web/zl-inflator.wclap. Needs Emscripten (emsdk) and
# the deps from fetch-deps.py.
set -euo pipefail
PORT=zl-inflator
source "$(dirname "$0")/zl-build.sh"
zl_build "$@"
# the device panel alone, no window
ZL_WINDOW=0
zl_bundle
