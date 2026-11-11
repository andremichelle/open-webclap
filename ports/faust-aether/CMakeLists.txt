# Aether (faust) as a WebCLAP bundle: module.wasm (the synth as a CLAP instrument) and ui/ (its editor and the
# presets, copied by the build script). Plain CLAP, no JUCE: gen.py runs the pinned Faust compiler on upstream's
# aether.dsp / effects.dsp (single precision, like the published web build) into ${GEN_DIR}, and aether_clap.cpp
# is the polyphonic host around the two generated classes. No upstream file is modified.
# Configure with emcmake, see scripts/build-faust-aether.sh. The bundle lands in web/faust-aether.wclap.
# GEN_DIR (gen.py's output: voice_gen.cpp, fx_gen.cpp, params.inc) and FAUST_INCLUDE (the Faust headers) come
# from the build script.

cmake_minimum_required(VERSION 3.20)
project(faust-aether-webclap LANGUAGES CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake (Emscripten toolchain)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED GEN_DIR OR NOT DEFINED FAUST_INCLUDE)
    message(FATAL_ERROR "Pass -DGEN_DIR=<gen.py output> -DFAUST_INCLUDE=<faust headers, e.g. ~/faust/include>")
endif()

get_filename_component(KIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(BUNDLE "${CMAKE_BINARY_DIR}/web/faust-aether.wclap")

# The flags and link options of the kit's DSP modules (modules/juce_webclap/juce_webclap.cmake), without JUCE: a
# standalone wasm module exporting clap_entry, malloc and the function table, importing only WASI. The generated
# Faust code needs no exceptions or RTTI; wasi_shim keeps the imports WASI-only. The effect bus's delay lines and
# reverb need about 16 MB, hence the initial memory.
set(WEBCLAP_FLAGS -msimd128)

add_executable(module
    aether_clap.cpp
    "${KIT_ROOT}/modules/webclap/wasi_shim.cpp")

target_include_directories(module PRIVATE
    "${KIT_ROOT}/external/clap/include"
    "${FAUST_INCLUDE}"
    "${GEN_DIR}")
target_compile_options(module PRIVATE ${WEBCLAP_FLAGS} -O3 -fno-exceptions -fno-rtti)
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
