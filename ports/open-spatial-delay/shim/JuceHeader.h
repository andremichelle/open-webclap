// What juce_generate_juce_header writes for OpenSpatialDelay (its CMake links juce_audio_utils, juce_dsp and
// juce_osc), for the wasm build. juce_osc is the socket-free stand-in next to this file.

#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "juce_osc_webclap.h"

#if ! DONT_SET_USING_JUCE_NAMESPACE
using namespace juce;
#endif

#if ! JUCE_DONT_DECLARE_PROJECTINFO
namespace ProjectInfo
{
    const char* const projectName    = "OpenSpatialDelay";
    const char* const companyName    = "Spatial Media Lab";
    const char* const versionString  = OSD_VERSION;
    const int         versionNumber  = 0x10000;
}
#endif
