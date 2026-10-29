/*
    ZLInflator's editor as ui.wasm (zl-common/zl_ui_main.h), with its glue: the meters from the DSP side go into
    the stand-in's meter sources, which the editor's meters read and decay as upstream. The editor keeps its UI
    settings (style, window size) in a second APVTS the processor does not save, so they are not host state.
*/

#include "PluginProcessor.h"
#include "zl_inflator_frames.h"

using PluginProcessor = ZLInflatorAudioProcessor;
#define ZL_NO_SETTINGS_STATE 1
#include "../zl-common/zl_ui_main.h"

namespace frames = zl_inflator_webclap;

namespace
{
frames::Meters lastMeters {};
} // namespace

void zlPortInit (ZLApp& zl)
{
    zl.bridge->onPluginFrame = [] (juce::uint8 type, const juce::uint8* payload, size_t size)
    {
        if (type == frames::meters && size == sizeof (frames::Meters))
            std::memcpy (&lastMeters, payload, sizeof (lastMeters));
    };
}

void zlPortFrame (ZLApp& zl)
{
    // The DSP side sends only changes: until the next frame, every repaint sees the last values (a meter resets
    // its buffer after painting)
    const auto numChannels = std::min ((size_t) lastMeters.numChannels, frames::maxChannels);
    zl.processor->getInputMeterSource()->feedBuffer (lastMeters.in.rms, lastMeters.in.peak, numChannels);
    zl.processor->getOutputMeterSource()->feedBuffer (lastMeters.out.rms, lastMeters.out.peak, numChannels);
}
