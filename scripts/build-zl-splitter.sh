#!/usr/bin/env bash
# Build the ZLSplitter WebCLAP bundle into build/zl-splitter/web/zl-splitter.wclap. Needs Emscripten (emsdk)
# and the deps from fetch-deps.py.
set -euo pipefail
PORT=zl-splitter
source "$(dirname "$0")/zl-build.sh"
zl_build "$@"
zl_bundle "$ROOT/external/zl-kfr/LICENSE.txt" GPL-2.0-KFR.txt
