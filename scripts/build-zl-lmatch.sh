#!/usr/bin/env bash
# Build the ZLLMatch WebCLAP bundle into build/zl-lmatch/web/zl-lmatch.wclap. Needs Emscripten (emsdk) and
# the deps from fetch-deps.py.
set -euo pipefail
PORT=zl-lmatch
source "$(dirname "$0")/zl-build.sh"
zl_build "$@"
zl_bundle "$ROOT/external/zl-lmatch/resources/font/LICENSE.txt" Apache-2.0-OpenSans.txt \
    "$ROOT/external/zl-lmatch/source/DSP/LUFSMeter/README.rst" MIT-LUFSMeter.txt
