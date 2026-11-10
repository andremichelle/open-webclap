/*
    ZLInflator as module.wasm: the processor wrapped by juce_webclap_clap.cpp, compiled with ZL_WEBCLAP_HEADLESS
    (patches/zl-inflator-webclap.patch): no editor. Its oversampling reports the latency (clap.latency).

    The PageExtension carries what the editor reads from the processor beside parameters and state: the input and
    output meters (zl_inflator_frames.h). openDAW's device panel is a page of its own (ui/panel.html, on the shared
    controls in modules/opendaw-page), 384 x 176.
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "PluginProcessor.h"
#include "zl_inflator_frames.h"

namespace
{
namespace frames = zl_inflator_webclap;

struct ZLInflatorPage final : juce::webclap::PageExtension
{
    static void take (MeterSource<float>& source, frames::Meter& meter, uint32_t numChannels)
    {
        std::fill (std::begin (meter.rms), std::end (meter.rms), -100.0f);
        std::fill (std::begin (meter.peak), std::end (meter.peak), -100.0f);
        source.takeBuffer (meter.rms, meter.peak, numChannels);
    }

    void update (juce::AudioProcessor& processor, const Sender& send) override
    {
        auto& p = static_cast<ZLInflatorAudioProcessor&> (processor);

        frames::Meters meters {};
        meters.numChannels = (uint32_t) std::min (p.getInputMeterSource()->getNumChannels(), frames::maxChannels);
        take (*p.getInputMeterSource(), meters.in, meters.numChannels);
        take (*p.getOutputMeterSource(), meters.out, meters.numChannels);

        if (! pageHasMeters || std::memcmp (&meters, &sentMeters, sizeof (meters)) != 0)
        {
            pageHasMeters = true;
            sentMeters = meters;
            send (frames::meters, &meters, sizeof (meters));
        }
    }

    void opened() override { pageHasMeters = false; }

    frames::Meters sentMeters {};
    bool pageHasMeters = false;
};
} // namespace

std::unique_ptr<juce::webclap::PageExtension> juce::webclap::createPageExtension()
{
    return std::make_unique<ZLInflatorPage>();
}

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DISTORTION,
                                            CLAP_PLUGIN_FEATURE_STEREO, CLAP_PLUGIN_FEATURE_MONO,
                                            "opendaw:device-panel", "opendaw:device-panel-width:384", nullptr };

    static const ClapPluginInfo info {
        "com.zlaudio.zlinflator",
        "ZL Inflator",
        "ZL Audio",
        "https://github.com/ZL-Audio/ZLInflator",
        ZL_VERSION "-panel", // the device panel alone, no window (the editor's version string stays the state's)
        "Distortion/saturation to increase loudness",
        features,
        "/ui/panel.html", // no window: the webview's page is the device panel's
        384,
        176,
        true,
        "/ui/panel.html", // openDAW's device panel, its width at 176 px
        384,
        176,
        false // no window (clap.gui): openDAW shows the device panel
    };

    return info;
}
