/*
    ZLSpectrumEqualizer's editor as ui.wasm (zl-common/zl_ui_main.h), with its glue: the analyzer audio, meters and
    dynamic responses from the DSP side go into the stand-in's controller, what the editor sets on the controller
    goes back as controls, with the analyzer sources the editor reads.
*/

#include "PluginProcessor.hpp"
#include "zl_spectrum_equalizer_access.h"
#include "../zl-common/zl_ui_main.h"

namespace frames = zl_spectrum_equalizer_webclap;

namespace
{
frames::Controls sentControls {};
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
            frames::Meters meters;
            std::memcpy (&meters, payload, sizeof (meters));
            zl.followSampleRate (meters.sampleRate);
            zlp::ZLWebclapAccess::apply (controller, meters);
        }
        else if (type == frames::dynamics)
        {
            zlp::ZLWebclapAccess::applyDynamics (controller, payload, size);
        }
    };
}

void zlPortFrame (ZLApp& zl)
{
    const auto controls = zlp::ZLWebclapAccess::controls (*zl.processor);

    if (! controlsSent || std::memcmp (&controls, &sentControls, sizeof (controls)) != 0)
    {
        controlsSent = true;
        sentControls = controls;
        zl.send (frames::controls, &controls, sizeof (controls));
    }
}
