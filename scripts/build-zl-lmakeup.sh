#!/usr/bin/env bash
# Build the ZLLMakeup WebCLAP bundle into build/zl-lmakeup/web/zl-lmakeup.wclap. Needs Emscripten (emsdk) and
# the deps from fetch-deps.py.
set -euo pipefail
PORT=zl-lmakeup
source "$(dirname "$0")/zl-build.sh"
zl_build "$@"
# LUFSMeter's MIT notice is in the header of the filter ZLLMakeup took from it
sed -n '/The MIT License/,/SOFTWARE\./p' "$ROOT/external/zl-lmakeup/source/DSP/Tracker/second_order_IIR_filter.h" \
    | tr -d '\r' > "$BUILD/MIT-LUFSMeter.txt"
zl_bundle "$ROOT/external/zl-lmakeup/resources/font/LICENSE.txt" Apache-2.0-OpenSans.txt \
    "$BUILD/MIT-LUFSMeter.txt" MIT-LUFSMeter.txt
