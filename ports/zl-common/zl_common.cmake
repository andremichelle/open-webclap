# What the ZL Audio ports share: their libraries built for wasm, and the ZL plugin sources split into the processor
# (module.wasm and the editor's stand-in) and the editor (ui.wasm). Include it after juce_webclap.cmake.
#
#   zl_hwy                  Google Highway, static dispatch to its WASM target (wasm SIMD128)
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
