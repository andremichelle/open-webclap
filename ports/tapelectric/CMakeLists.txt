# TapElectric as a WebCLAP bundle: module.wasm (DSP, CLAP entry) and ui/ui.wasm (the editor). Configure with emcmake,
# see scripts/build-tapelectric.sh. The bundle lands in web/tapelectric.wclap, next to the test host page.

cmake_minimum_required(VERSION 3.20)
project(tapelectric-webclap LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake (Emscripten toolchain)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(KIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EXT "${KIT_ROOT}/external")
set(TAPELECTRIC "${EXT}/tapelectric/effects/tapelectric")
set(BUNDLE "${CMAKE_BINARY_DIR}/web/tapelectric.wclap")
set(TAPELECTRIC_VERSION "1.0")

# ---------------------------------------------------------------------------------------------------------------
# JUCE with the wasm platform layer. Both modules use the GUI library: the processor needs juce_audio_processors
# (AudioProcessorValueTreeState), which depends on the GUI modules.

set(JUCE_WEBCLAP_PLUGIN_DEFINITIONS
    JucePlugin_Name="TapElectric"
    JucePlugin_Desc="TapElectric"
    JucePlugin_Manufacturer="aquanode"
    JucePlugin_VersionString="${TAPELECTRIC_VERSION}"
    JucePlugin_IsSynth=0
    JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0
    JucePlugin_IsMidiEffect=0
    JucePlugin_EditorRequiresKeyboardFocus=0)
include("${KIT_ROOT}/modules/juce_webclap/juce_webclap.cmake")

set(WEB_FONT_FILES "${EXT}/fonts/DejaVuSans.ttf" "${EXT}/fonts/DejaVuSans-Bold.ttf")
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_custom_command(
    OUTPUT "${GENERATED}/WebFonts.h" "${GENERATED}/WebFonts.cpp"
    COMMAND python3 "${KIT_ROOT}/scripts/binary_data.py" "${GENERATED}" WebFonts ${WEB_FONT_FILES}
    DEPENDS ${WEB_FONT_FILES} "${KIT_ROOT}/scripts/binary_data.py"
    VERBATIM)

# ---------------------------------------------------------------------------------------------------------------
# TapElectric, unchanged sources (one patch, see below): the processor in both modules (in ui.wasm as the editor's
# stand-in), the editor only in ui.wasm

set(TAPELECTRIC_INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_LIST_DIR}/shim"
    "${GENERATED}"
    "${TAPELECTRIC}/src")

set(TAPELECTRIC_DEFINITIONS TAPELECTRIC_VERSION="${TAPELECTRIC_VERSION}")

# ---------------------------------------------------------------------------------------------------------------
# ui.wasm: editor + stand-in processor

add_executable(ui
    ui_main.cpp
    "${TAPELECTRIC}/src/PluginProcessor.cpp"
    "${TAPELECTRIC}/src/PluginEditor.cpp"
    "${GENERATED}/WebFonts.cpp")

target_include_directories(ui PRIVATE ${TAPELECTRIC_INCLUDE_DIRECTORIES})
target_compile_definitions(ui PRIVATE ${TAPELECTRIC_DEFINITIONS})
target_compile_options(ui PRIVATE -Wno-deprecated-declarations)
target_link_libraries(ui PRIVATE juce_wasm)
target_link_options(ui PRIVATE ${JUCE_WEBCLAP_UI_LINK_OPTIONS})
set_target_properties(ui PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${BUNDLE}/ui")

# ---------------------------------------------------------------------------------------------------------------
# module.wasm: the processor as a CLAP plugin, without its editor (patches/tapelectric-headless.patch)

add_executable(module
    dsp_main.cpp
    "${KIT_ROOT}/modules/juce_webclap/juce_webclap_clap.cpp"
    "${KIT_ROOT}/modules/webclap/wasi_shim.cpp"
    "${TAPELECTRIC}/src/PluginProcessor.cpp")

target_include_directories(module PRIVATE ${TAPELECTRIC_INCLUDE_DIRECTORIES} "${EXT}/clap/include")
target_compile_definitions(module PRIVATE ${TAPELECTRIC_DEFINITIONS} TAPELECTRIC_HEADLESS=1)
target_compile_options(module PRIVATE -Wno-deprecated-declarations)
target_link_libraries(module PRIVATE juce_wasm)
target_link_options(module PRIVATE ${JUCE_WEBCLAP_DSP_LINK_OPTIONS})
set_target_properties(module PROPERTIES
    SUFFIX ".wasm"
    RUNTIME_OUTPUT_DIRECTORY "${BUNDLE}")
