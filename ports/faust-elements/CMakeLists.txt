# Elements (faust-elements) as a WebCLAP bundle: module.wasm (the synth as a CLAP instrument) and ui/ (its editor,
# the presets and the atlas, copied by the build script). Plain CLAP, no JUCE: gen.py runs the pinned faust-rs
# compiler on this port's src/voice.dsp / src/body.dsp (single precision, 32 kHz) into ${GEN_DIR}, and
# elements_clap.cpp is the polyphonic host around the two generated classes (the generated code is self-contained;
# it only needs the architecture types the wrapper defines). No upstream file is modified.
# Configure with emcmake, see scripts/build-faust-elements.sh. The bundle lands in web/faust-elements.wclap.
# GEN_DIR (gen.py's output: voice_gen.cpp, body_gen.cpp, params.inc) comes from the build script.

cmake_minimum_required(VERSION 3.20)
project(faust-elements-webclap LANGUAGES CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake (Emscripten toolchain)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED GEN_DIR)
    message(FATAL_ERROR "Pass -DGEN_DIR=<gen.py output>")
endif()

get_filename_component(KIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(BUNDLE "${CMAKE_BINARY_DIR}/web/faust-elements.wclap")

# The flags and link options of the kit's DSP modules (modules/juce_webclap/juce_webclap.cmake), without JUCE: a
# standalone wasm module exporting clap_entry, malloc and the function table, importing only WASI. The generated
# Faust code needs no exceptions or RTTI; wasi_shim keeps the imports WASI-only. -ffp-contract=off keeps the
# float operations the model was checked with (an FMA here and there drifts against the firmware). The reverb's
# and strings' delay lines and the strike's 128k-sample table need room, hence the initial memory.
set(WEBCLAP_FLAGS -msimd128)

add_executable(module
    elements_clap.cpp
    "${KIT_ROOT}/modules/webclap/wasi_shim.cpp")

target_include_directories(module PRIVATE
    "${KIT_ROOT}/external/clap/include"
    "${GEN_DIR}")
target_compile_options(module PRIVATE ${WEBCLAP_FLAGS} -O3 -ffp-contract=off -fno-exceptions -fno-rtti)
target_link_options(module PRIVATE
    ${WEBCLAP_FLAGS}
    -O3
    -sSTANDALONE_WASM=1
    --no-entry
    -sEXPORTED_FUNCTIONS=_malloc,_free
    -Wl,--export=clap_entry
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=48MB
    -sSTACK_SIZE=1MB
    -sALLOW_TABLE_GROWTH=1
    -sERROR_ON_UNDEFINED_SYMBOLS=1)
set_target_properties(module PROPERTIES
    SUFFIX ".wasm"
    RUNTIME_OUTPUT_DIRECTORY "${BUNDLE}")
