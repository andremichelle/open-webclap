# ZLLMatch as a WebCLAP bundle: module.wasm (DSP, CLAP entry) and ui/ui.wasm (the editor). Configure with
# emcmake, see scripts/build-zl-lmatch.sh. The bundle lands in web/zl-lmatch.wclap, next to the test host page.

cmake_minimum_required(VERSION 3.20)
project(zl-lmatch-webclap LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake (Emscripten toolchain)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(KIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EXT "${KIT_ROOT}/external")
set(ZL "${EXT}/zl-lmatch")
set(BUNDLE "${CMAKE_BINARY_DIR}/web/zl-lmatch.wclap")
set(ZL_VERSION "0.2.2") # VERSION at the commit fetch-deps.py pins (0a567f8): saved states hold it

# ---------------------------------------------------------------------------------------------------------------
# JUCE with the wasm platform layer. Both modules use the GUI library: the processor needs juce_audio_processors
# (AudioProcessorValueTreeState), which depends on the GUI modules.

set(JUCE_WEBCLAP_PLUGIN_DEFINITIONS
    JucePlugin_Name="ZL Loudness Match"
    JucePlugin_Desc="ZL Loudness Match"
    JucePlugin_Manufacturer="ZL"
    JucePlugin_VersionString="${ZL_VERSION}"
    JucePlugin_IsSynth=0
    JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0
    JucePlugin_IsMidiEffect=0
    JucePlugin_EditorRequiresKeyboardFocus=1)
include("${KIT_ROOT}/modules/juce_webclap/juce_webclap.cmake")

include("${KIT_ROOT}/ports/zl-common/zl_common.cmake")

# ---------------------------------------------------------------------------------------------------------------
# Resources: what juce_add_binary_data embeds upstream (the Open Sans font, logos), DejaVu for the kit

set(ZL_ASSET_FILES
    "${ZL}/resources/logo.svg"
    "${ZL}/resources/zlaudio.svg"
    "${ZL}/resources/font/OpenSans-SemiBold.ttf")
set(WEB_FONT_FILES "${EXT}/fonts/DejaVuSans.ttf" "${EXT}/fonts/DejaVuSans-Bold.ttf")
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_custom_command(
    OUTPUT "${GENERATED}/BinaryData.h" "${GENERATED}/BinaryData.cpp"
    COMMAND python3 "${KIT_ROOT}/scripts/binary_data.py" "${GENERATED}" BinaryData ${ZL_ASSET_FILES}
    DEPENDS ${ZL_ASSET_FILES} "${KIT_ROOT}/scripts/binary_data.py"
    VERBATIM)

add_custom_command(
    OUTPUT "${GENERATED}/WebFonts.h" "${GENERATED}/WebFonts.cpp"
    COMMAND python3 "${KIT_ROOT}/scripts/binary_data.py" "${GENERATED}" WebFonts ${WEB_FONT_FILES}
    DEPENDS ${WEB_FONT_FILES} "${KIT_ROOT}/scripts/binary_data.py"
    VERBATIM)

# ---------------------------------------------------------------------------------------------------------------
# ZLLMatch, its sources as upstream globs them (one patch, see patches/zl-lmatch-webclap.patch): the processor
# in both modules (in ui.wasm as the editor's stand-in), the editor only in ui.wasm. State/property.cpp, the
# editor's settings file, is the editor's.

zl_sources("${ZL}" ZL_PROCESSOR_SOURCES ZL_EDITOR_SOURCES "^State/property\\.cpp$")

set(ZL_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}" "${GENERATED}" "${ZL}/source")

set(ZL_DEFINITIONS
    ZL_WEBCLAP=1
    JUCE_USE_FLAC=0
    JUCE_USE_OGGVORBIS=0
    JUCE_USE_WEBP=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

set(ZL_LIBRARIES juce_wasm)

# ---------------------------------------------------------------------------------------------------------------
# ui.wasm: editor + stand-in processor

add_executable(ui
    ui_main.cpp
    ${ZL_PROCESSOR_SOURCES}
    ${ZL_EDITOR_SOURCES}
    "${ZL}/source/State/property.cpp"
    "${GENERATED}/BinaryData.cpp"
    "${GENERATED}/WebFonts.cpp")

target_include_directories(ui PRIVATE ${ZL_INCLUDE_DIRECTORIES})
target_compile_definitions(ui PRIVATE ${ZL_DEFINITIONS} ZL_WEBCLAP_HEADLESS=0)
target_compile_options(ui PRIVATE -Wno-deprecated-declarations)
target_link_libraries(ui PRIVATE ${ZL_LIBRARIES})
target_link_options(ui PRIVATE ${JUCE_WEBCLAP_UI_LINK_OPTIONS})
set_target_properties(ui PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${BUNDLE}/ui")

# ---------------------------------------------------------------------------------------------------------------
# module.wasm: the processor as a CLAP plugin, without its editor

add_executable(module
    dsp_main.cpp
    "${KIT_ROOT}/modules/juce_webclap/juce_webclap_clap.cpp"
    "${KIT_ROOT}/modules/webclap/wasi_shim.cpp"
    ${ZL_PROCESSOR_SOURCES})

target_include_directories(module PRIVATE ${ZL_INCLUDE_DIRECTORIES} "${EXT}/clap/include")
target_compile_definitions(module PRIVATE ${ZL_DEFINITIONS} ZL_WEBCLAP_HEADLESS=1 ZL_VERSION="${ZL_VERSION}")
target_compile_options(module PRIVATE -Wno-deprecated-declarations)
target_link_libraries(module PRIVATE ${ZL_LIBRARIES})
target_link_options(module PRIVATE ${JUCE_WEBCLAP_DSP_LINK_OPTIONS})
set_target_properties(module PROPERTIES
    SUFFIX ".wasm"
    RUNTIME_OUTPUT_DIRECTORY "${BUNDLE}")
