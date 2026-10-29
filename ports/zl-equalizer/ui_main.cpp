/*
    ZLEqualizer's editor as ui.wasm (zl-common/zl_ui_main.h), with its glue: the analyzer audio and meters from
    the DSP side go into the stand-in's controller, what the editor sets on the controller goes back as controls.
*/

#include "PluginProcessor.hpp"
#include "zl_equalizer_access.h"
#include "../zl-common/zl_ui_main.h"

namespace frames = zl_equalizer_webclap;

namespace
{
frames::Controls sentControls {};
frames::Meters meters {}; // what the DSP side sent, deltas apply to it
bool controlsSent = false;
} // namespace

void zlPortInit (ZLApp& zl)
{
    zl.bridge->onPluginFrame = [&zl] (juce::uint8 type, const juce::uint8* payload, size_t size)
    {
        auto& controller = zl.processor->getController();

        if (type == frames::analyzer)
        {
            zl_webclap::AnalyzerHeader header;

            if (zl_webclap::readAnalyzerHeader (payload, size, header))
                zl.followSampleRate (header.sampleRate);

            zl_webclap::feedAnalyzer (controller.getAnalyzerSender(), payload, size);
        }
        else if (type == frames::meters && size == sizeof (frames::Meters))
        {
            std::memcpy (&meters, payload, sizeof (meters));
            zlp::ZLWebclapAccess::apply (controller, meters);
        }
        else if (type == frames::delta && size % frames::deltaEntrySize == 0)
        {
            for (size_t offset = 0; offset < size; offset += frames::deltaEntrySize)
                if (payload[offset] < std::size (meters.values))
                    std::memcpy (&meters.values[payload[offset]], payload + offset + 1, sizeof (float));

            zlp::ZLWebclapAccess::apply (controller, meters);
        }
    };
}

void zlPortFrame (ZLApp& zl)
{
    auto controls = zlp::ZLWebclapAccess::controls (zl.processor->getController());
    controls.analyzerMask = zlp::ZLWebclapAccess::analyzerMask (*zl.processor, zl.editor.get());

    if (! controlsSent || std::memcmp (&controls, &sentControls, sizeof (controls)) != 0)
    {
        controlsSent = true;
        sentControls = controls;
        zl.send (frames::controls, &controls, sizeof (controls));
    }
}
