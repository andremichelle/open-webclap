/*
    ZLCompressor's editor as ui.wasm (zl-common/zl_ui_main.h), with its glue: the analyzer audio and status from
    the DSP side go into the stand-in's controllers, what the editor sets on the controllers goes back as controls.

    The stand-in's dummy processor holds the saved analyzer settings (na_parameters_) and the UI settings
    (state_), which the editor saves to its own file: only the former report a state change.
*/

#include "PluginProcessor.hpp"
#include "zl_compressor_access.h"
#include "../zl-common/zl_ui_main.h"

namespace frames = zl_compressor_webclap;

namespace
{
frames::Controls sentControls {};
bool controlsSent = false;

void followAnalyzer (ZLApp& zl, const juce::uint8* payload, size_t size)
{
    zl_webclap::AnalyzerHeader header;

    if (zl_webclap::readAnalyzerHeader (payload, size, header))
        zl.followSampleRate (header.sampleRate);
}
} // namespace

void zlPortInit (ZLApp& zl)
{
    // UI settings are no host state
    for (auto* parameter : zl.processor->dummy_processor_.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
            if (zl.processor->na_parameters_.getParameter (ranged->getParameterID()) == nullptr)
                parameter->removeListener (zl.stateWatcher.get());

    zl.bridge->onPluginFrame = [&zl] (juce::uint8 type, const juce::uint8* payload, size_t size)
    {
        if (type == frames::magAnalyzer)
        {
            followAnalyzer (zl, payload, size);
            zl_webclap::feedAnalyzer (zl.processor->getCompressController().getMagAnalyzerSender(), payload, size);
        }
        else if (type == frames::fftAnalyzer)
        {
            followAnalyzer (zl, payload, size);
            zl_webclap::feedAnalyzer (zl.processor->getEqualizeController().getFFTAnalyzerSender(), payload, size);
        }
        else if (type == frames::status && size == sizeof (frames::Status))
        {
            frames::Status status;
            std::memcpy (&status, payload, sizeof (status));
            zl.followSampleRate (status.sampleRate);
            zlp::ZLWebclapAccess::apply (*zl.processor, status);
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
