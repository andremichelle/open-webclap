#!/usr/bin/env bash
# Builds module.wasm and the page into $OUT, as opendaw-commons' check runs it: emcc 3.1.51 on the path, the CLAP
# headers in $CLAP_INCLUDE (the folder holding clap/clap.h).
set -euo pipefail
cd "$(dirname "$0")"
cp -R page/. "$OUT/"
if [ -f LICENSE ]; then cp LICENSE "$OUT/LICENSE"; fi
em++ plugin.cpp -std=c++17 -O3 -fno-exceptions -fno-rtti -I"$CLAP_INCLUDE" -Ikit \
    -o "$OUT/module.wasm" -sSTANDALONE_WASM=1 --no-entry \
    -sEXPORTED_FUNCTIONS=_malloc,_free -Wl,--export=clap_entry \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=4MB -sSTACK_SIZE=256KB -sALLOW_TABLE_GROWTH=1 \
    -sERROR_ON_UNDEFINED_SYMBOLS=1
