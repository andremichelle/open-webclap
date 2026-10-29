#!/usr/bin/env bash
# Build the ZLEqualizer WebCLAP bundle into build/zl-equalizer/web/zl-equalizer.wclap. Needs Emscripten (emsdk)
# and the deps from fetch-deps.py.
set -euo pipefail
PORT=zl-equalizer
source "$(dirname "$0")/zl-build.sh"
zl_build "$@"
zl_bundle "$ROOT/external/zl-equalizer/zldsp_fft/LICENSE.md" Apache-2.0-zldsp_fft.txt \
    "$ROOT/external/zl-highway/LICENSE" Apache-2.0-Highway.txt \
    "$ROOT/external/zl-nlopt/COPYING" NLopt-COPYING.txt \
    "$ROOT/external/zl-nlopt/src/algs/luksan/COPYRIGHT" NLopt-luksan-COPYRIGHT.txt
