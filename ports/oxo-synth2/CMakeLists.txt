# Oxo Synth2 as a WebCLAP bundle: module.wasm (the synth as a CLAP instrument) and ui/ (its editor and the factory
# patches, copied by the build script). Plain CLAP, no JUCE: upstream's header-only engine (external/oxo-synth2/
# EngineCore) behind oxo_synth2_clap.cpp, unchanged (no patches).
# Configure with emcmake, see scripts/build-oxo-synth2.sh. The bundle lands in web/oxo-synth2.wclap.

cmake_minimum_required(VERSION 3.20)
project(oxo-synth2-webclap LANGUAGES CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake (Emscripten toolchain)")
endif()

# gnu++20: the engine uses M_PI, which musl hides under strict -std=c++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

get_filename_component(KIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(OXO "${KIT_ROOT}/external/oxo-synth2")
set(BUNDLE "${CMAKE_BINARY_DIR}/web/oxo-synth2.wclap")

# The descriptor's version follows the pinned upstream commit: MARKETING_VERSION in its Identity.xcconfig
file(STRINGS "${OXO}/Identity.xcconfig" OXO_VERSION_LINE REGEX "^MARKETING_VERSION")
string(REGEX REPLACE "^[^=]*=[ \t]*" "" OXO_SYNTH2_VERSION "${OXO_VERSION_LINE}")
if(NOT OXO_SYNTH2_VERSION)
    set(OXO_SYNTH2_VERSION "0.0.0")
endif()

# The flags and link options of the kit's DSP modules (modules/juce_webclap/juce_webclap.cmake), without JUCE: a
# standalone wasm module exporting clap_entry, malloc and the function table, importing only WASI. The engine needs
# no exceptions or RTTI; wasi_shim keeps the imports WASI-only.
set(WEBCLAP_FLAGS -msimd128)

add_executable(module
    oxo_synth2_clap.cpp
    "${KIT_ROOT}/modules/webclap/wasi_shim.cpp")

target_include_directories(module PRIVATE
    "${OXO}" "${KIT_ROOT}/external/clap/include")
target_compile_definitions(module PRIVATE OXO_SYNTH2_VERSION="${OXO_SYNTH2_VERSION}")
target_compile_options(module PRIVATE ${WEBCLAP_FLAGS} -O3 -fno-exceptions -fno-rtti)
target_link_options(module PRIVATE
    ${WEBCLAP_FLAGS}
    -O3
    -sSTANDALONE_WASM=1
    --no-entry
    -sEXPORTED_FUNCTIONS=_malloc,_free
    -Wl,--export=clap_entry
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=32MB
    -sSTACK_SIZE=1MB
    -sALLOW_TABLE_GROWTH=1
    -sERROR_ON_UNDEFINED_SYMBOLS=1)
set_target_properties(module PROPERTIES
    SUFFIX ".wasm"
    RUNTIME_OUTPUT_DIRECTORY "${BUNDLE}")
