# oXygen as a WebCLAP bundle: module.wasm (DSP, CLAP entry) and ui/ui.wasm (the editor). Configure with emcmake,
# see scripts/build-oxygen.sh. The bundle lands in web/oxygen.wclap, next to the test host page.

cmake_minimum_required(VERSION 3.20)
project(oxygen-webclap LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake (Emscripten toolchain)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(KIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EXT "${KIT_ROOT}/external")
set(OXYGEN "${EXT}/oxygen")
set(BUNDLE "${CMAKE_BINARY_DIR}/web/oxygen.wclap")
set(OXYGEN_VERSION "1.2.1")

# The editor's size: openDAW's device panel height, so the window and the panel show it unscaled
set(OXYGEN_EDITOR_WIDTH 500)
set(OXYGEN_EDITOR_HEIGHT 176)

# ---------------------------------------------------------------------------------------------------------------
# JUCE with the wasm platform layer. Both modules use the GUI library: the modules need juce_audio_processors
# (AudioProcessorValueTreeState), which depends on the GUI modules.

set(JUCE_WEBCLAP_PLUGIN_DEFINITIONS
    JucePlugin_Name="oXygen"
    JucePlugin_Desc="oXygen"
    JucePlugin_Manufacturer="Wamphyre"
    JucePlugin_VersionString="${OXYGEN_VERSION}"
    JucePlugin_IsSynth=0
    JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0
    JucePlugin_IsMidiEffect=0
    JucePlugin_EditorRequiresKeyboardFocus=0)
include("${KIT_ROOT}/modules/juce_webclap/juce_webclap.cmake")

set(WEB_FONT_FILES "${EXT}/fonts/Inter-Regular.ttf" "${EXT}/fonts/Inter-Medium.ttf" "${EXT}/fonts/Inter-MediumItalic.ttf")
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_custom_command(
    OUTPUT "${GENERATED}/WebFonts.h" "${GENERATED}/WebFonts.cpp"
    COMMAND python3 "${KIT_ROOT}/scripts/binary_data.py" "${GENERATED}" WebFonts ${WEB_FONT_FILES}
    DEPENDS ${WEB_FONT_FILES} "${KIT_ROOT}/scripts/binary_data.py"
    VERBATIM)

# ---------------------------------------------------------------------------------------------------------------
# oXygen's modules, unchanged but for patches/oxygen-webclap.patch, under the port's processor in both modules.
# Upstream's editor, assistant and graph are not built: the port has its own processor and editor.

set(OXYGEN_SOURCES
    oxygen_processor.cpp
    "${OXYGEN}/Source/Modules/EqualizerModule.cpp"
    "${OXYGEN}/Source/Modules/DynamicEqModule.cpp"
    "${OXYGEN}/Source/Modules/MultibandCompressorModule.cpp"
    "${OXYGEN}/Source/Modules/StereoImagerModule.cpp"
    "${OXYGEN}/Source/Modules/MaximizerModule.cpp")

set(OXYGEN_INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/shim"
    "${GENERATED}"
    "${OXYGEN}/Source")

set(OXYGEN_DEFINITIONS
    OXYGEN_WEBCLAP=1
    OXYGEN_VERSION="${OXYGEN_VERSION}"
    OXYGEN_EDITOR_WIDTH=${OXYGEN_EDITOR_WIDTH}
    OXYGEN_EDITOR_HEIGHT=${OXYGEN_EDITOR_HEIGHT})

# ---------------------------------------------------------------------------------------------------------------
# ui.wasm: the port's editor + stand-in processor

add_executable(ui
    ui_main.cpp
    oxygen_editor.cpp
    ${OXYGEN_SOURCES}
    "${GENERATED}/WebFonts.cpp")

target_include_directories(ui PRIVATE ${OXYGEN_INCLUDE_DIRECTORIES})
target_compile_definitions(ui PRIVATE ${OXYGEN_DEFINITIONS})
target_compile_options(ui PRIVATE -Wno-deprecated-declarations)
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
    ${OXYGEN_SOURCES})

target_include_directories(module PRIVATE ${OXYGEN_INCLUDE_DIRECTORIES} "${EXT}/clap/include")
target_compile_definitions(module PRIVATE ${OXYGEN_DEFINITIONS})
target_compile_options(module PRIVATE -Wno-deprecated-declarations)
target_link_libraries(module PRIVATE juce_wasm)
target_link_options(module PRIVATE ${JUCE_WEBCLAP_DSP_LINK_OPTIONS})
set_target_properties(module PROPERTIES
    SUFFIX ".wasm"
    RUNTIME_OUTPUT_DIRECTORY "${BUNDLE}")
