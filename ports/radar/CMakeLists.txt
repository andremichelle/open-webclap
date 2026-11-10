# Radar as a WebCLAP bundle: module.wasm (DSP, CLAP entry) and ui/ui.wasm (the editors). Configure with emcmake,
# see scripts/build-radar.sh. The bundle lands in web/radar.wclap.

cmake_minimum_required(VERSION 3.20)
project(radar-webclap LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake (Emscripten toolchain)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(KIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EXT "${KIT_ROOT}/external")
set(BUNDLE "${CMAKE_BINARY_DIR}/web/radar.wclap")
set(RADAR_VERSION "1.0.0")

# The window's size, and the device panel's at openDAW's panel height (176)
set(RADAR_WINDOW_WIDTH 900)
set(RADAR_WINDOW_HEIGHT 600)
set(RADAR_PANEL_WIDTH 435)
set(RADAR_PANEL_HEIGHT 176)

# ---------------------------------------------------------------------------------------------------------------
# JUCE with the wasm platform layer. Both modules use the GUI library: AudioProcessorValueTreeState lives in
# juce_audio_processors, which depends on the GUI modules.

set(JUCE_WEBCLAP_PLUGIN_DEFINITIONS
    JucePlugin_Name="Radar"
    JucePlugin_Desc="Radar"
    JucePlugin_Manufacturer="André Michelle"
    JucePlugin_VersionString="${RADAR_VERSION}"
    JucePlugin_IsSynth=1
    JucePlugin_WantsMidiInput=1
    JucePlugin_ProducesMidiOutput=0
    JucePlugin_IsMidiEffect=0
    JucePlugin_EditorRequiresKeyboardFocus=1)
include("${KIT_ROOT}/modules/juce_webclap/juce_webclap.cmake")

set(WEB_FONT_FILES "${EXT}/fonts/Inter-Regular.ttf" "${EXT}/fonts/Inter-Medium.ttf" "${EXT}/fonts/Inter-MediumItalic.ttf")
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_custom_command(
    OUTPUT "${GENERATED}/WebFonts.h" "${GENERATED}/WebFonts.cpp"
    COMMAND python3 "${KIT_ROOT}/scripts/binary_data.py" "${GENERATED}" WebFonts ${WEB_FONT_FILES}
    DEPENDS ${WEB_FONT_FILES} "${KIT_ROOT}/scripts/binary_data.py"
    VERBATIM)

# ---------------------------------------------------------------------------------------------------------------
# The model and the processor, in both modules

set(RADAR_SOURCES
    radar_model.cpp
    radar_processor.cpp)

set(RADAR_INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/shim"
    "${GENERATED}")

set(RADAR_DEFINITIONS
    RADAR_VERSION="${RADAR_VERSION}"
    RADAR_WINDOW_WIDTH=${RADAR_WINDOW_WIDTH}
    RADAR_WINDOW_HEIGHT=${RADAR_WINDOW_HEIGHT}
    RADAR_PANEL_WIDTH=${RADAR_PANEL_WIDTH}
    RADAR_PANEL_HEIGHT=${RADAR_PANEL_HEIGHT})

# ---------------------------------------------------------------------------------------------------------------
# ui.wasm: the editors + stand-in processor

add_executable(ui
    ui_main.cpp
    radar_editor.cpp
    ${RADAR_SOURCES}
    "${GENERATED}/WebFonts.cpp")

target_include_directories(ui PRIVATE ${RADAR_INCLUDE_DIRECTORIES})
target_compile_definitions(ui PRIVATE ${RADAR_DEFINITIONS})
target_link_libraries(ui PRIVATE juce_wasm)
target_link_options(ui PRIVATE ${JUCE_WEBCLAP_UI_LINK_OPTIONS})
set_target_properties(ui PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${BUNDLE}/ui")

# ---------------------------------------------------------------------------------------------------------------
# module.wasm: the processor as a CLAP plugin

add_executable(module
    dsp_main.cpp
    "${KIT_ROOT}/modules/juce_webclap/juce_webclap_clap.cpp"
    "${KIT_ROOT}/modules/webclap/wasi_shim.cpp"
    ${RADAR_SOURCES})

target_include_directories(module PRIVATE ${RADAR_INCLUDE_DIRECTORIES} "${EXT}/clap/include")
target_compile_definitions(module PRIVATE ${RADAR_DEFINITIONS})
target_link_libraries(module PRIVATE juce_wasm)
target_link_options(module PRIVATE ${JUCE_WEBCLAP_DSP_LINK_OPTIONS})
set_target_properties(module PROPERTIES
    SUFFIX ".wasm"
    RUNTIME_OUTPUT_DIRECTORY "${BUNDLE}")
