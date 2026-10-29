/*
    ZLSplitter's editor as ui.wasm (zl-common/zl_ui_main.h), with its glue: the analyzer audio from the DSP side
    goes into the stand-in's controller, what the editor sets on the controller goes back as controls.

    The editor's settings APVTS sits on the dummy processor (dummy_processor_) as in the other ZL plugins, saved in
    the processor's state as na_parameters_.
*/

#include "PluginProcessor.hpp"
#include "zl_splitter_access.h"
#include "../zl-common/zl_ui_main.h"

namespace frames = zl_splitter_webclap;

namespace
{
frames::Controls sentControls {};
bool controlsSent = false;
} // namespace

void zlPortInit (ZLApp& zl)
{
    zl.bridge->onPluginFrame = [&zl] (juce::uint8 type, const juce::uint8* payload, size_t size)
    {
        if (type == frames::analyzer)
        {
            zl_webclap::AnalyzerHeader header;

            if (zl_webclap::readAnalyzerHeader (payload, size, header))
                zl.followSampleRate (header.sampleRate);

            zl_webclap::feedAnalyzer (zl.processor->getController().getAnalyzerSender(), payload, size);
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
