# OpenSpatialDelay as a WebCLAP bundle: module.wasm (DSP, CLAP entry) and ui/ui.wasm (the editor). Configure with
# emcmake, see scripts/build-open-spatial-delay.sh. The bundle lands in web/open-spatial-delay.wclap, next to the
# test host page.

cmake_minimum_required(VERSION 3.20)
project(open-spatial-delay-webclap LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake (Emscripten toolchain)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(KIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EXT "${KIT_ROOT}/external")
set(OSD "${EXT}/open-spatial-delay")
set(MYSOFA "${EXT}/libmysofa")
set(BUNDLE "${CMAKE_BINARY_DIR}/web/open-spatial-delay.wclap")
set(OSD_VERSION "1.0.0")

# ---------------------------------------------------------------------------------------------------------------
# JUCE with the wasm platform layer. Both modules use the GUI library: the processor needs juce_audio_processors
# (AudioProcessorValueTreeState) and juce_dsp, which depend on the GUI modules.

set(JUCE_WEBCLAP_PLUGIN_DEFINITIONS
    JucePlugin_Name="OpenSpatialDelay"
    JucePlugin_Desc="OpenSpatialDelay"
    JucePlugin_Manufacturer="Spatial Media Lab"
    JucePlugin_VersionString="${OSD_VERSION}"
    JucePlugin_IsSynth=0
    JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0
    JucePlugin_IsMidiEffect=0
    JucePlugin_EditorRequiresKeyboardFocus=0)
include("${KIT_ROOT}/modules/juce_webclap/juce_webclap.cmake")

# ---------------------------------------------------------------------------------------------------------------
# libmysofa (SOFA reader for the HRTF sets), its sources as its CMake lists them, zlib from Emscripten's port

set(MYSOFA_SOURCES
    "${MYSOFA}/src/hrtf/reader.c"
    "${MYSOFA}/src/hdf/superblock.c"
    "${MYSOFA}/src/hdf/dataobject.c"
    "${MYSOFA}/src/hdf/btree.c"
    "${MYSOFA}/src/hdf/fractalhead.c"
    "${MYSOFA}/src/hdf/gunzip.c"
    "${MYSOFA}/src/hdf/gcol.c"
    "${MYSOFA}/src/hrtf/check.c"
    "${MYSOFA}/src/hrtf/spherical.c"
    "${MYSOFA}/src/hrtf/lookup.c"
    "${MYSOFA}/src/hrtf/tools.c"
    "${MYSOFA}/src/hrtf/kdtree.c"
    "${MYSOFA}/src/hrtf/neighbors.c"
    "${MYSOFA}/src/hrtf/interpolate.c"
    "${MYSOFA}/src/hrtf/resample.c"
    "${MYSOFA}/src/hrtf/loudness.c"
    "${MYSOFA}/src/hrtf/minphase.c"
    "${MYSOFA}/src/hrtf/easy.c"
    "${MYSOFA}/src/hrtf/cache.c"
    "${MYSOFA}/src/resampler/speex_resampler.c")

add_library(mysofa STATIC ${MYSOFA_SOURCES})
set_target_properties(mysofa PROPERTIES C_STANDARD 99)
target_include_directories(mysofa PRIVATE "${CMAKE_CURRENT_LIST_DIR}/mysofa" PUBLIC "${MYSOFA}/src/hrtf")
target_compile_options(mysofa PRIVATE -w -msimd128 -fwasm-exceptions -sUSE_ZLIB=1)
target_link_options(mysofa INTERFACE -sUSE_ZLIB=1)

# ---------------------------------------------------------------------------------------------------------------
# Resources: what juce_add_binary_data embeds upstream (the three hosted HRTF sets, fonts)

set(OSD_HRTF_FILES
    "${OSD}/HRTF/mit_kemar_large_pinna.sofa"
    "${OSD}/HRTF/cipic_subject_003.sofa"
    "${OSD}/HRTF/hutubs_pp2.sofa")
file(GLOB OSD_FONT_FILES "${OSD}/fonts/*.ttf")
list(SORT OSD_FONT_FILES)
set(WEB_FONT_FILES "${EXT}/fonts/DejaVuSans.ttf" "${EXT}/fonts/DejaVuSans-Bold.ttf")
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_custom_command(
    OUTPUT "${GENERATED}/HRTFData.h" "${GENERATED}/HRTFData.cpp"
    COMMAND python3 "${KIT_ROOT}/scripts/binary_data.py" "${GENERATED}" HRTFData ${OSD_HRTF_FILES}
    DEPENDS ${OSD_HRTF_FILES} "${KIT_ROOT}/scripts/binary_data.py"
    VERBATIM)

# The editor's stand-in never renders binaural audio: empty sets of the same names, which fail to load as Simple
set(UI_HRTF_FILES)
foreach(file ${OSD_HRTF_FILES})
    get_filename_component(name "${file}" NAME)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ui-hrtf/${name}" "")
    list(APPEND UI_HRTF_FILES "${CMAKE_CURRENT_BINARY_DIR}/ui-hrtf/${name}")
endforeach()

add_custom_command(
    OUTPUT "${GENERATED}/ui/HRTFData.h" "${GENERATED}/ui/HRTFData.cpp"
    COMMAND python3 "${KIT_ROOT}/scripts/binary_data.py" "${GENERATED}/ui" HRTFData ${UI_HRTF_FILES}
    DEPENDS "${KIT_ROOT}/scripts/binary_data.py"
    VERBATIM)

add_custom_command(
    OUTPUT "${GENERATED}/FontData.h" "${GENERATED}/FontData.cpp"
    COMMAND python3 "${KIT_ROOT}/scripts/binary_data.py" "${GENERATED}" FontData ${OSD_FONT_FILES}
    DEPENDS ${OSD_FONT_FILES} "${KIT_ROOT}/scripts/binary_data.py"
    VERBATIM)

add_custom_command(
    OUTPUT "${GENERATED}/WebFonts.h" "${GENERATED}/WebFonts.cpp"
    COMMAND python3 "${KIT_ROOT}/scripts/binary_data.py" "${GENERATED}" WebFonts ${WEB_FONT_FILES}
    DEPENDS ${WEB_FONT_FILES} "${KIT_ROOT}/scripts/binary_data.py"
    VERBATIM)

# ---------------------------------------------------------------------------------------------------------------
# OpenSpatialDelay, unchanged sources (one patch, see below): the processor and DSP in both modules (in ui.wasm as
# the editor's stand-in), the editor only in ui.wasm

set(OSD_PROCESSOR_SOURCES
    "${OSD}/Source/PluginProcessor.cpp"
    "${OSD}/Source/PresetData.cpp"
    "${OSD}/Source/DopplerVelocity.cpp"
    "${OSD}/Source/TrajectoryEngine.cpp"
    "${OSD}/Source/FilterBank.cpp")

set(OSD_INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/shim"
    "${GENERATED}"
    "${OSD}/Source")

set(OSD_DEFINITIONS OSD_VERSION="${OSD_VERSION}" OSD_WEBCLAP=1)

set(OSD_JUCE juce_wasm_keyboard juce_wasm_audio_formats juce_wasm_gui_extra mysofa)

# ---------------------------------------------------------------------------------------------------------------
# ui.wasm: editor + stand-in processor

add_executable(ui
    ui_main.cpp
    ${OSD_PROCESSOR_SOURCES}
    "${GENERATED}/ui/HRTFData.cpp"
    "${OSD}/Source/PluginEditor.cpp"
    "${GENERATED}/FontData.cpp"
    "${GENERATED}/WebFonts.cpp")

target_include_directories(ui PRIVATE "${GENERATED}/ui" ${OSD_INCLUDE_DIRECTORIES})
target_compile_definitions(ui PRIVATE ${OSD_DEFINITIONS} OSD_HEADLESS=0)
target_compile_options(ui PRIVATE -Wno-deprecated-declarations)
target_link_libraries(ui PRIVATE ${OSD_JUCE})
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
    ${OSD_PROCESSOR_SOURCES}
    "${GENERATED}/HRTFData.cpp")

target_include_directories(module PRIVATE ${OSD_INCLUDE_DIRECTORIES} "${EXT}/clap/include")
target_compile_definitions(module PRIVATE ${OSD_DEFINITIONS} OSD_HEADLESS=1)
target_compile_options(module PRIVATE -Wno-deprecated-declarations)
target_link_libraries(module PRIVATE ${OSD_JUCE})
target_link_options(module PRIVATE ${JUCE_WEBCLAP_DSP_LINK_OPTIONS})
set_target_properties(module PROPERTIES
    SUFFIX ".wasm"
    RUNTIME_OUTPUT_DIRECTORY "${BUNDLE}")
