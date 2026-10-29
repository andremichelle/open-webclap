#!/usr/bin/env bash
# Build the ZLCompressor WebCLAP bundle into build/zl-compressor/web/zl-compressor.wclap. Needs Emscripten (emsdk)
# and the deps from fetch-deps.py.
set -euo pipefail
PORT=zl-compressor
source "$(dirname "$0")/zl-build.sh"
zl_build "$@"
zl_bundle "$ROOT/external/zl-compressor/zldsp_fft/LICENSE.md" Apache-2.0-zldsp_fft.txt \
    "$ROOT/external/zl-highway/LICENSE" Apache-2.0-Highway.txt
