#!/usr/bin/env bash
# Builds module.wasm and the page into $OUT. The CI provides emcc and the CLAP headers in $CLAP_INCLUDE.
set -euo pipefail
cd "$(dirname "$0")"
cp page/index.html "$OUT/index.html"
# the shared device panel controls (modules/opendaw-page), next to the page
mkdir -p "$OUT/opendaw-page"
cp ../../modules/opendaw-page/panel.css ../../modules/opendaw-page/panel.js "$OUT/opendaw-page/"
cp LICENSE "$OUT/LICENSE"
em++ plugin/hive.cpp -std=c++17 -O3 -fno-exceptions -fno-rtti -I"$CLAP_INCLUDE" -I../../modules/opendaw-clap \
    -o "$OUT/module.wasm" -sSTANDALONE_WASM=1 --no-entry \
    -sEXPORTED_FUNCTIONS=_malloc,_free -Wl,--export=clap_entry \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=4MB -sSTACK_SIZE=256KB -sALLOW_TABLE_GROWTH=1 \
    -sERROR_ON_UNDEFINED_SYMBOLS=1
