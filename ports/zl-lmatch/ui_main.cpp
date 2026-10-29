/*
    ZLLMatch's editor as ui.wasm (zl-common/zl_ui_main.h), with its glue: the gain from the DSP side goes into the
    stand-in's controller, which the editor's gain display reads. The editor keeps its UI settings (style, window
    size) in a second APVTS the processor does not save, so they are not host state.
*/

#include "PluginProcessor.h"
#include "zl_lmatch_frames.h"

#define ZL_NO_SETTINGS_STATE 1
#include "../zl-common/zl_ui_main.h"

namespace frames = zl_lmatch_webclap;

void zlPortInit (ZLApp& zl)
{
    zl.bridge->onPluginFrame = [&zl] (juce::uint8 type, const juce::uint8* payload, size_t size)
    {
        if (type == frames::gain && size == sizeof (float))
        {
            float gain;
            std::memcpy (&gain, payload, sizeof (gain));
            zl.processor->getController()->setGain (gain);
        }
    };
}

void zlPortFrame (ZLApp&) {}
