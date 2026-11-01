# OB-Xf as a WebCLAP bundle: module.wasm (DSP, CLAP entry) and ui/ui.wasm (the editor). Configure with emcmake,
# see scripts/build-obxf.sh. The bundle lands in web/obxf.wclap, next to the test host page.

cmake_minimum_required(VERSION 3.20)
project(obxf-webclap LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake (Emscripten toolchain)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(KIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EXT "${KIT_ROOT}/external")
set(OBXF "${EXT}/OB-Xf")
set(OBXF_INSTALLER "${OBXF}/assets/installer/Surge Synth Team/OB-Xf")
set(BUNDLE "${CMAKE_BINARY_DIR}/web/obxf.wclap")

option(OBXF_WEB_PNG_THEME "Package the bitmap Default theme (about 9 MB) besides the embedded vector theme" ON)

# ---------------------------------------------------------------------------------------------------------------
# Generated resources

file(GLOB OBXF_VECTOR_THEME "${OBXF}/assets/binary/VectorTheme/*")
set(OBXF_BINARY_FILES
    "${OBXF}/assets/binary/icon.svg"
    "${OBXF}/assets/binary/icon-links.svg"
    "${OBXF}/assets/binary/logo.svg"
    "${OBXF}/assets/binary/Jersey20.ttf"
    "${OBXF}/assets/binary/Trek.ttf"
    ${OBXF_VECTOR_THEME})
set(WEB_FONT_FILES "${EXT}/fonts/DejaVuSans.ttf" "${EXT}/fonts/DejaVuSans-Bold.ttf")
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_custom_command(
    OUTPUT "${GENERATED}/BinaryData.h" "${GENERATED}/BinaryData.cpp"
    COMMAND python3 "${KIT_ROOT}/scripts/binary_data.py" "${GENERATED}" BinaryData ${OBXF_BINARY_FILES}
    DEPENDS ${OBXF_BINARY_FILES} "${KIT_ROOT}/scripts/binary_data.py"
    VERBATIM)

add_custom_command(
    OUTPUT "${GENERATED}/WebFonts.h" "${GENERATED}/WebFonts.cpp"
    COMMAND python3 "${KIT_ROOT}/scripts/binary_data.py" "${GENERATED}" WebFonts ${WEB_FONT_FILES}
    DEPENDS ${WEB_FONT_FILES} "${KIT_ROOT}/scripts/binary_data.py"
    VERBATIM)

# ---------------------------------------------------------------------------------------------------------------
# JUCE with the wasm platform layer: juce_wasm for the editor, juce_wasm_dsp (no GUI) for the DSP module

set(JUCE_WEBCLAP_PLUGIN_DEFINITIONS
    JucePlugin_Name="OB-Xf"
    JucePlugin_Desc="OB-Xf"
    JucePlugin_Manufacturer="Surge Synth Team"
    JucePlugin_VersionString="1.1.0"
    JucePlugin_IsSynth=1
    JucePlugin_WantsMidiInput=1
    JucePlugin_ProducesMidiOutput=0
    JucePlugin_IsMidiEffect=0
    JucePlugin_EditorRequiresKeyboardFocus=1)
include("${KIT_ROOT}/modules/juce_webclap/juce_webclap.cmake")

# ---------------------------------------------------------------------------------------------------------------
# OB-Xf, unchanged sources: the processor in both modules (in ui.wasm as the editor's stand-in), the editor only in
# ui.wasm

set(OBXF_PROCESSOR_SOURCES
    "${OBXF}/src/ObxfProcessor.cpp"
    "${OBXF}/src/Utils.cpp"
    "${OBXF}/src/state/StateManager.cpp"
    "${OBXF}/src/state/ObxdImporter.cpp"
    "${OBXF}/src/midi/MidiHandler.cpp"
    "${OBXF}/src/parameter/ParameterUpdateHandler.cpp"
    "${OBXF}/src/parameter/ParameterCoordinator.cpp"
    "${KIT_ROOT}/ports/sst-shim/sst_plugininfra_wasm.cpp"
    shim/version_information.cpp
    "${EXT}/sst-plugininfra/libs/strnatcmp/strnatcmp.cpp"
    "${EXT}/MTS-ESP/Client/libMTSClient.cpp")

set(OBXF_EDITOR_SOURCES
    "${OBXF}/src/utilities/KeyCommandHandler.cpp"
    "${OBXF}/src/components/ScalingImageCache.cpp"
    "${OBXF}/src/editor/ObxfEditorLayout.cpp"
    "${OBXF}/src/editor/ObxfEditorMenus.cpp"
    "${OBXF}/src/editor/ObxfEditorMidiLearn.cpp"
    "${OBXF}/src/editor/ObxfEditorProgrammer.cpp"
    "${OBXF}/src/editor/ObxfEditorTheme.cpp"
    "${OBXF}/src/ObxfEditor.cpp"
    "${OBXF}/src/gui/LookAndFeel.cpp")

set(OBXF_INCLUDE_DIRECTORIES
    "${KIT_ROOT}/ports/sst-shim"
    "${CMAKE_CURRENT_LIST_DIR}/shim"
    "${OBXF}/src"
    "${OBXF}/src/components"
    "${OBXF}/src/editor"
    "${OBXF}/src/engine"
    "${OBXF}/src/gui"
    "${OBXF}/src/interface"
    "${OBXF}/src/core"
    "${OBXF}/src/midi"
    "${OBXF}/src/utilities"
    "${OBXF}/src/parameter"
    "${OBXF}/src/state"
    "${EXT}/sst-cpputils/include"
    "${EXT}/sst-basic-blocks/include"
    "${EXT}/sst-plugininfra/include"
    "${EXT}/sst-plugininfra/libs/strnatcmp"
    "${EXT}/ghc-filesystem/include"
    "${EXT}/fmt/include"
    "${EXT}/simde"
    "${EXT}/MTS-ESP/Client")

set(OBXF_DEFINITIONS
    FMT_HEADER_ONLY=1
    SST_PLUGININFRA_HAS_VERSION_INFORMATION=1
    OBXF_VERSION_STR="1.1.0"
    OBXF_WEBCLAP=1
    SIMDE_ENABLE_NATIVE_ALIASES=1)

# ---------------------------------------------------------------------------------------------------------------
# ui.wasm: editor + stand-in processor

add_executable(ui
    ui_main.cpp
    ${OBXF_PROCESSOR_SOURCES}
    ${OBXF_EDITOR_SOURCES}
    "${GENERATED}/BinaryData.cpp"
    "${GENERATED}/WebFonts.cpp")

target_include_directories(ui PRIVATE "${GENERATED}" ${OBXF_INCLUDE_DIRECTORIES})
target_compile_definitions(ui PRIVATE ${OBXF_DEFINITIONS})
target_compile_options(ui PRIVATE -Wno-deprecated-declarations -Wno-macro-redefined)
target_link_libraries(ui PRIVATE juce_wasm)

set(UI_LINK_OPTIONS ${JUCE_WEBCLAP_UI_LINK_OPTIONS})

# The factory folder (patches, bitmap theme) goes into Emscripten's filesystem at /factory, see the sst shim.
set(FACTORY_DIR "${CMAKE_CURRENT_BINARY_DIR}/factory")
file(REMOVE_RECURSE "${FACTORY_DIR}")
file(COPY "${OBXF_INSTALLER}/Patches" DESTINATION "${FACTORY_DIR}")

if(OBXF_WEB_PNG_THEME)
    # 1x and 2x only; 4x would add 18 MB for screens that rarely exist.
    file(GLOB THEME_FILES "${OBXF_INSTALLER}/Themes/Default/*")
    file(MAKE_DIRECTORY "${FACTORY_DIR}/Themes/Default")
    foreach(f ${THEME_FILES})
        if(NOT f MATCHES "@4x")
            file(COPY "${f}" DESTINATION "${FACTORY_DIR}/Themes/Default")
        endif()
    endforeach()
endif()

list(APPEND UI_LINK_OPTIONS "SHELL:--preload-file ${FACTORY_DIR}@/factory")

target_link_options(ui PRIVATE ${UI_LINK_OPTIONS})
set_target_properties(ui PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${BUNDLE}/ui")

# ---------------------------------------------------------------------------------------------------------------
# module.wasm: the processor as a CLAP plugin

# ObxfProcessor.cpp includes the editor's header even when headless, hence the generated BinaryData.h (only the
# header, nothing from it is linked).
add_executable(module
    dsp_main.cpp
    shim/colour_headless.cpp
    "${KIT_ROOT}/modules/juce_webclap/juce_webclap_clap.cpp"
    "${KIT_ROOT}/modules/webclap/wasi_shim.cpp"
    ${OBXF_PROCESSOR_SOURCES}
    "${GENERATED}/BinaryData.h")

target_include_directories(module PRIVATE "${GENERATED}" ${OBXF_INCLUDE_DIRECTORIES} "${EXT}/clap/include")
target_compile_definitions(module PRIVATE ${OBXF_DEFINITIONS} OBXF_HEADLESS=1)
target_compile_options(module PRIVATE -Wno-deprecated-declarations -Wno-macro-redefined)
target_link_libraries(module PRIVATE juce_wasm_dsp)
target_link_options(module PRIVATE ${JUCE_WEBCLAP_DSP_LINK_OPTIONS})
set_target_properties(module PROPERTIES
    SUFFIX ".wasm"
    RUNTIME_OUTPUT_DIRECTORY "${BUNDLE}")
