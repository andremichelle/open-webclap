#!/usr/bin/env bash
# Builds module.wasm and the page into $OUT. Needs emcc, the CLAP headers in $CLAP_INCLUDE and Mutable Instruments'
# eurorack sources (Beads, stmlib) in $EURORACK.
set -euo pipefail
cd "$(dirname "$0")"
cp page/index.html "$OUT/index.html"
cp LICENSE "$OUT/LICENSE"
em++ plugin/pearls.cpp \
    "$EURORACK/beads/dsp/processor.cc" "$EURORACK/beads/dsp/delay_controller.cc" \
    "$EURORACK/beads/dsp/delay_clock.cc" "$EURORACK/beads/dsp/trigger_generator.cc" "$EURORACK/beads/dsp/mu_law.cc" \
    "$EURORACK/beads/resources.cc" "$EURORACK/stmlib/dsp/units.cc" "$EURORACK/stmlib/utils/random.cc" \
    -std=c++17 -O3 -fno-exceptions -fno-rtti -Wno-vla-cxx-extension -I"$CLAP_INCLUDE" -I"$EURORACK" \
    -o "$OUT/module.wasm" -sSTANDALONE_WASM=1 --no-entry \
    -sEXPORTED_FUNCTIONS=_malloc,_free -Wl,--export=clap_entry \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=8MB -sSTACK_SIZE=256KB -sALLOW_TABLE_GROWTH=1 \
    -sERROR_ON_UNDEFINED_SYMBOLS=1
