#!/usr/bin/env bash
# Builds module.wasm and the page into $OUT. Needs emcc, the CLAP headers in $CLAP_INCLUDE and the Signalsmith
# libraries (Basics, DSP, Linear, Hilbert IIR) in $SIGNALSMITH, each in its signalsmith-<name>/include.
set -euo pipefail
cd "$(dirname "$0")"
cp page/index.html "$OUT/index.html"
# the shared device panel controls (modules/opendaw-page), next to the page
mkdir -p "$OUT/opendaw-page"
cp ../../modules/opendaw-page/panel.css ../../modules/opendaw-page/panel.js "$OUT/opendaw-page/"
cp LICENSE "$OUT/LICENSE"
em++ plugin/basics.cpp -std=c++17 -O3 -fno-exceptions -fno-rtti -I"$CLAP_INCLUDE" -I../../modules/opendaw-clap \
    -I"$SIGNALSMITH/signalsmith-basics/include" -I"$SIGNALSMITH/signalsmith-dsp/include" \
    -I"$SIGNALSMITH/signalsmith-linear/include" -I"$SIGNALSMITH/signalsmith-hilbert/include" \
    -o "$OUT/module.wasm" -sSTANDALONE_WASM=1 --no-entry \
    -sEXPORTED_FUNCTIONS=_malloc,_free -Wl,--export=clap_entry \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=8MB -sSTACK_SIZE=256KB -sALLOW_TABLE_GROWTH=1 \
    -sERROR_ON_UNDEFINED_SYMBOLS=1
