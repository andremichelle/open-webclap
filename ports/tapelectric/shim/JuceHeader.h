// What the Projucer writes as JuceHeader.h for TapElectric, for the wasm build: the modules its sources use. The DSP
// module includes the same headers: the processor's header is shared with the editor.

#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#if ! JUCE_DONT_DECLARE_PROJECTINFO
namespace ProjectInfo
{
    const char* const projectName    = "TapElectric";
    const char* const companyName    = "aquanodemusic";
    const char* const versionString  = TAPELECTRIC_VERSION;
    const int         versionNumber  = 0x10000;
}
#endif
