# Helm as a WebCLAP bundle: module.wasm (the synth as a CLAP instrument) and ui/ (its editor and the factory patches,
# copied by the build script). Plain CLAP, no JUCE: upstream's engine (mopo, src/synthesis) behind helm_clap.cpp.
# Configure with emcmake, see scripts/build-helm.sh. The bundle lands in web/helm.wclap.

cmake_minimum_required(VERSION 3.20)
project(helm-webclap LANGUAGES CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake (Emscripten toolchain)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(KIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(HELM "${KIT_ROOT}/external/helm")
set(BUNDLE "${CMAKE_BINARY_DIR}/web/helm.wclap")

# Helm's last release
set(HELM_VERSION "0.9.0")

# The flags and link options of the kit's DSP modules (modules/juce_webclap/juce_webclap.cmake), without JUCE: a
# standalone wasm module exporting clap_entry, malloc and the function table, importing only WASI
set(WEBCLAP_FLAGS -msimd128 -fwasm-exceptions)

file(GLOB MOPO_SOURCES "${HELM}/mopo/src/*.cpp")
file(GLOB SYNTHESIS_SOURCES "${HELM}/src/synthesis/*.cpp")

add_executable(module
    helm_clap.cpp
    ${MOPO_SOURCES}
    ${SYNTHESIS_SOURCES}
    "${HELM}/src/common/helm_common.cpp"
    "${KIT_ROOT}/modules/webclap/wasi_shim.cpp")

target_include_directories(module PRIVATE
    "${HELM}/mopo/src" "${HELM}/src/synthesis" "${HELM}/src/common" "${KIT_ROOT}/external/clap/include")
target_compile_definitions(module PRIVATE HELM_VERSION="${HELM_VERSION}")
target_compile_options(module PRIVATE ${WEBCLAP_FLAGS} -O3 -Wno-deprecated-declarations)
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
