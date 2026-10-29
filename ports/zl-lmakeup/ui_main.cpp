/*
    ZLLMakeup's editor as ui.wasm (zl-common/zl_ui_main.h), with its glue: the gain from the DSP side goes into the
    stand-in's controller, which the editor's gain readout reads. The editor is the port's own (makeup_editor.cpp),
    on the same parameters as upstream's.
*/

#include "PluginProcessor.h"
#include "makeup_editor.h"
#include "zl_lmakeup_frames.h"

#define ZL_NO_SETTINGS_STATE 1
#define ZL_PORT_EDITOR 1
#include "../zl-common/zl_ui_main.h"

namespace frames = zl_lmakeup_webclap;

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

juce::AudioProcessorEditor* zlPortCreateEditor (PluginProcessor& processor)
{
    return zl_lmakeup_webclap::createEditor (processor);
}
