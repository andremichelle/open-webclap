/*
    ZLLimiter's editor as ui.wasm (zl-common/zl_ui_main.h), with its glue: the analyzer audio and status from the
    DSP side go into the stand-in's controller and processor, what the editor sets on the controller goes back as
    controls.
*/

#include "PluginProcessor.hpp"
#include "zl_limiter_access.h"
#include "../zl-common/zl_ui_main.h"

namespace frames = zl_limiter_webclap;

namespace
{
frames::Controls sentControls {};
bool controlsSent = false;
frames::Status appliedStatus {};
} // namespace

void zlPortInit (ZLApp& zl)
{
    zl.bridge->onPluginFrame = [&zl] (juce::uint8 type, const juce::uint8* payload, size_t size)
    {
        if (type == frames::analyzer)
        {
            zl_webclap::AnalyzerHeader header;

            if (zl_webclap::readAnalyzerHeader (payload, size, header))
            {
                const auto rate = zl.sampleRate;
                zl.followSampleRate (header.sampleRate);

                // The stand-in's prepareToPlay set its own channels and transport: the DSP side's again
                if (zl.sampleRate != rate)
                    zlp::ZLWebclapAccess::apply (*zl.processor, appliedStatus, appliedStatus);
            }

            zl_webclap::feedAnalyzer (zl.processor->getController().getMagAnalyzerSender(), payload, size);
        }
        else if (type == frames::status && size == sizeof (frames::Status))
        {
            frames::Status status;
            std::memcpy (&status, payload, sizeof (status));
            zlp::ZLWebclapAccess::apply (*zl.processor, status, appliedStatus);
        }
    };
}

void zlPortFrame (ZLApp& zl)
{
    const auto controls = zlp::ZLWebclapAccess::controls (zl.processor->getController());

    if (! controlsSent || std::memcmp (&controls, &sentControls, sizeof (controls)) != 0)
    {
        controlsSent = true;
        sentControls = controls;
        zl.send (frames::controls, &controls, sizeof (controls));
    }
}
