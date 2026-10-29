#!/usr/bin/env bash
# Build the ZLLimiter WebCLAP bundle into build/zl-limiter/web/zl-limiter.wclap. Needs Emscripten (emsdk) and the
# deps from fetch-deps.py.
set -euo pipefail
PORT=zl-limiter
source "$(dirname "$0")/zl-build.sh"
zl_build "$@"
zl_bundle "$ROOT/external/zl-highway/LICENSE" Apache-2.0-Highway.txt
