#!/usr/bin/env bash
# Builds module.wasm and the page into $OUT. Needs emcc, node, the CLAP headers in $CLAP_INCLUDE, Prism's source in
# $PRISM (patched) and a directory for the page's build in $UI.
set -euo pipefail
cd "$(dirname "$0")"
PORT="$(pwd)"
NATIVE="$PRISM/native/src"

# the module: the wrapper and Prism's DSP, unmodified (the reference analysis throws, hence wasm exceptions)
em++ plugin/prism.cpp "$NATIVE"/{dsp_utils,spectrum,oscilloscope,vumeter,lufsmeter,vectorscope,spectrogram,waveform,waterfall,multiband,reference_analysis}.cpp \
    ../../modules/webclap/wasi_shim.cpp \
    -std=c++17 -O3 -fwasm-exceptions -I"$CLAP_INCLUDE" -I../../modules/opendaw-clap -I"$NATIVE" \
    -o "$OUT/module.wasm" -sSTANDALONE_WASM=1 --no-entry \
    -sEXPORTED_FUNCTIONS=_malloc,_free -Wl,--export=clap_entry \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=16MB -sSTACK_SIZE=512KB -sALLOW_TABLE_GROWTH=1 \
    -sERROR_ON_UNDEFINED_SYMBOLS=1

# the page: Prism's plugin UI (src/) with the WebCLAP backend and entry (page/), upstream's dependency versions
mkdir -p "$UI"
rm -rf "$UI/src"
cp -R "$PRISM/src" "$UI/src"
cp page/backend.ts page/entry.ts page/webclap.css page/index.html "$UI/src/plugin-ui/"
cp page/vite.config.mjs "$UI/"
if ! cmp -s page/package-lock.json "$UI/package-lock.json" || [ ! -d "$UI/node_modules" ]; then
    cp page/package.json page/package-lock.json "$UI/"
    (cd "$UI" && npm ci --no-audit --no-fund --loglevel=error)
fi
(cd "$UI" && OUT="$OUT" npx vite build --config vite.config.mjs)
