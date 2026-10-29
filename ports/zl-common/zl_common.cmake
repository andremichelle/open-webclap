# What the ZL Audio ports share: their libraries built for wasm, and the ZL plugin sources split into the processor
# (module.wasm and the editor's stand-in) and the editor (ui.wasm). Include it after juce_webclap.cmake.
#
#   zl_hwy                  Google Highway, static dispatch to its WASM target (wasm SIMD128)
#   zl_nlopt                nlopt, as nlopt's own CMake builds it (no bindings, static)
#   zl_kfr                  KFR (kfr_dsp, kfr_dft) as KFR's own CMake sets it up for Emscripten (sse41)
#   zl_sources(<dir> <processor var> <editor var> [<exclude regex>...])
#                           the plugin's .cpp files under <dir>/source: PluginEditor.cpp and source/panel/ and
#                           source/gui/ are the editor's, everything else the processor's

set(ZL_EXT "${KIT_ROOT}/external")
set(ZL_COMMON "${KIT_ROOT}/ports/zl-common")

if(ZL_USE_HWY AND NOT TARGET zl_hwy)
    set(ZL_HWY "${ZL_EXT}/zl-highway")
    add_library(zl_hwy STATIC
        "${ZL_HWY}/hwy/abort.cc"
        "${ZL_HWY}/hwy/aligned_allocator.cc"
        "${ZL_HWY}/hwy/per_target.cc"
        "${ZL_HWY}/hwy/print.cc"
        "${ZL_HWY}/hwy/targets.cc"
        "${ZL_HWY}/hwy/timer.cc")
    target_include_directories(zl_hwy PUBLIC "${ZL_HWY}")
    target_compile_definitions(zl_hwy PUBLIC HWY_COMPILE_ONLY_STATIC=1 HWY_STATIC_DEFINE)
    target_compile_options(zl_hwy PUBLIC ${JUCE_WEBCLAP_FLAGS})
    target_compile_options(zl_hwy PRIVATE -w)
endif()

if(ZL_USE_NLOPT AND NOT TARGET nlopt)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    foreach(option NLOPT_PYTHON NLOPT_OCTAVE NLOPT_MATLAB NLOPT_GUILE NLOPT_SWIG NLOPT_FORTRAN NLOPT_TESTS)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(DISABLE_FP_CONTRACT OFF CACHE BOOL "" FORCE) # as ZLEqualizer builds it
    set(WITH_THREADLOCAL OFF CACHE BOOL "" FORCE)     # one thread
    add_subdirectory("${ZL_EXT}/zl-nlopt" "${CMAKE_BINARY_DIR}/nlopt" EXCLUDE_FROM_ALL)
    target_compile_options(nlopt PRIVATE ${JUCE_WEBCLAP_FLAGS} -w)
    add_library(zl_nlopt INTERFACE)
    target_link_libraries(zl_nlopt INTERFACE nlopt)
    target_include_directories(zl_nlopt INTERFACE "${CMAKE_BINARY_DIR}/nlopt" "${ZL_EXT}/zl-nlopt/src/api")
endif()

# KFR built here rather than by its own CMake, which needs files fetch-deps.py leaves out and links pthread. The same
# libraries as KFR's CMake sets up for Emscripten: one architecture, sse41 (Emscripten's SSE4.1 headers on wasm
# SIMD128), no runtime dispatch. The headers pick the architecture from the compiler's flags, so everything that
# includes them gets -msse4.1 too.
if(ZL_USE_KFR AND NOT TARGET zl_kfr)
    set(ZL_KFR "${ZL_EXT}/zl-kfr")
    file(GLOB ZL_KFR_DSP_SOURCES "${ZL_KFR}/src/dsp/*.cpp")
    file(GLOB ZL_KFR_DFT_SOURCES "${ZL_KFR}/src/dft/*.cpp")

    add_library(zl_kfr_headers INTERFACE)
    target_include_directories(zl_kfr_headers INTERFACE "${ZL_KFR}/include")
    target_compile_options(zl_kfr_headers INTERFACE ${JUCE_WEBCLAP_FLAGS} -msse4.1)

    add_library(zl_kfr_dsp STATIC ${ZL_KFR_DSP_SOURCES})
    add_library(zl_kfr_dft STATIC ${ZL_KFR_DFT_SOURCES})
    target_compile_options(zl_kfr_dft PRIVATE -ffp-contract=fast -O3) # as KFR's src/dft/CMakeLists.txt

    foreach(library zl_kfr_dsp zl_kfr_dft)
        target_link_libraries(${library} PUBLIC zl_kfr_headers)
        target_compile_options(${library} PRIVATE -w)
    endforeach()

    add_library(zl_kfr INTERFACE)
    target_link_libraries(zl_kfr INTERFACE zl_kfr_dsp zl_kfr_dft)
endif()

function(zl_sources dir processor_var editor_var)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${dir}/source/*.cpp")
    list(SORT sources)
    set(processor)
    set(editor)
    foreach(source ${sources})
        file(RELATIVE_PATH relative "${dir}/source" "${source}")
        set(skip FALSE)
        foreach(pattern ${ARGN})
            if(relative MATCHES "${pattern}")
                set(skip TRUE)
            endif()
        endforeach()
        if(skip)
        elseif(relative MATCHES "^(PluginEditor\\.cpp|panel/|gui/|[Pp]anel/|GUI/)")
            list(APPEND editor "${source}")
        else()
            list(APPEND processor "${source}")
        endif()
    endforeach()
    set(${processor_var} ${processor} PARENT_SCOPE)
    set(${editor_var} ${editor} PARENT_SCOPE)
endfunction()
