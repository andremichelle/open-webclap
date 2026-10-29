#!/usr/bin/env bash
# Build the ZLSpectrumEqualizer WebCLAP bundle into build/zl-spectrum-equalizer/web/zl-spectrum-equalizer.wclap.
# Needs Emscripten (emsdk) and the deps from fetch-deps.py.
set -euo pipefail
PORT=zl-spectrum-equalizer
source "$(dirname "$0")/zl-build.sh"
zl_build "$@"
zl_bundle "$ROOT/external/zl-spectrum-equalizer/zldsp_fft/LICENSE.md" Apache-2.0-zldsp_fft.txt \
    "$ROOT/external/zl-highway/LICENSE" Apache-2.0-Highway.txt
