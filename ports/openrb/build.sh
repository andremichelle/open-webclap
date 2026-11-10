#!/usr/bin/env bash
# Builds one device's module.wasm and the page into $OUT: KIND is the device (0 303, 1 distortion, 2 PCF,
# 3 compressor, 4 delay, 5 808, 6 909; plugin/openrb.cpp's Kind). Needs emcc, cargo with the wasm32-unknown-emscripten target, the CLAP
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

# a drum machine's factory kit (kits/README.md) as a header
KIT_DIR="$TARGET/kit-$KIND"
mkdir -p "$KIT_DIR"
case "$KIND" in
    5) python3 kits/embed.py kits 808 "$KIT_DIR/factory_kit.h" ;;
    6) python3 kits/embed.py kits 909 "$KIT_DIR/factory_kit.h" ;;
esac

cp page/index.html "$OUT/index.html"
# the shared device panel controls (modules/opendaw-page), next to the page
mkdir -p "$OUT/opendaw-page"
cp ../../modules/opendaw-page/panel.css ../../modules/opendaw-page/panel.js "$OUT/opendaw-page/"
cp LICENSE "$OUT/LICENSE"
em++ plugin/openrb.cpp "$ENGINE" -std=c++17 -O3 -fno-rtti -I"$CLAP_INCLUDE" -I../../modules/opendaw-clap -I"$KIT_DIR" -DOPENRB_KIND="$KIND" \
    -mbulk-memory -mnontrapping-fptoint -fwasm-exceptions \
    -o "$OUT/module.wasm" -sSTANDALONE_WASM=1 --no-entry \
    -sEXPORTED_FUNCTIONS=_malloc,_free -Wl,--export=clap_entry \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=8MB -sSTACK_SIZE=256KB -sALLOW_TABLE_GROWTH=1 \
    -sERROR_ON_UNDEFINED_SYMBOLS=1
