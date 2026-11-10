#!/usr/bin/env bash
# Builds module.wasm and the page into $OUT. Needs emcc, cargo with the wasm32-unknown-emscripten target, the CLAP
# headers in $CLAP_INCLUDE and OpenRB-338 in external/openrb-338 (scripts/fetch-deps.py).
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"
TARGET="${CARGO_TARGET_DIR:-$ROOT/build/openrb/cargo}"

# the engine: upstream's crate behind the C API, a static library for emscripten
CARGO_TARGET_DIR="$TARGET" cargo build --manifest-path engine/Cargo.toml --release --locked \
    --target wasm32-unknown-emscripten
# Rust's std carries target features (bulk-memory-opt, call-indirect-overlong) newer than emsdk's wasm-opt knows;
# without the section the link names the features itself (bulk memory, saturating float to int, exceptions)
ENGINE="$TARGET/openrb-engine.a"
cp "$TARGET/wasm32-unknown-emscripten/release/libopenrb_webclap_engine.a" "$ENGINE"
OBJCOPY="$(dirname "$(command -v emcc)")/../bin/llvm-objcopy"
[ -x "$OBJCOPY" ] || OBJCOPY="${EMSDK:-}/upstream/bin/llvm-objcopy"
"$OBJCOPY" --remove-section=target_features "$ENGINE"

# the factory kits (kits/README.md) as a header
python3 kits/embed.py kits "$TARGET/factory_kits.h"

cp page/index.html "$OUT/index.html"
# the shared device panel controls (modules/opendaw-page), next to the page
mkdir -p "$OUT/opendaw-page"
cp ../../modules/opendaw-page/panel.css ../../modules/opendaw-page/panel.js "$OUT/opendaw-page/"
cp LICENSE "$OUT/LICENSE"
em++ plugin/openrb.cpp "$ENGINE" -std=c++17 -O3 -fno-rtti -I"$CLAP_INCLUDE" -I../../modules/opendaw-clap -I"$TARGET" \
    -mbulk-memory -mnontrapping-fptoint -fwasm-exceptions \
    -o "$OUT/module.wasm" -sSTANDALONE_WASM=1 --no-entry \
    -sEXPORTED_FUNCTIONS=_malloc,_free -Wl,--export=clap_entry \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=8MB -sSTACK_SIZE=256KB -sALLOW_TABLE_GROWTH=1 \
    -sERROR_ON_UNDEFINED_SYMBOLS=1
