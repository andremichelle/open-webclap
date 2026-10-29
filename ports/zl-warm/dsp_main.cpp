/*
    ZLWarm as module.wasm: the processor wrapped by juce_webclap_clap.cpp, compiled with ZL_WEBCLAP_HEADLESS
    (patches/zl-warm-webclap.patch): no editor. Its oversampling reports the latency (clap.latency).

    The PageExtension carries what the editor's meter panel reads from the controller beside parameters and state
    (zl_warm_frames.h). Upstream the meter panel switches the meters on and off; here the open page does.
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "zl_warm_meters.h"

namespace
{
namespace frames = zl_warm_webclap;

struct ZLWarmPage final : juce::webclap::PageExtension
{
    void receive (juce::AudioProcessor& processor, std::uint8_t type, const void* payload, std::size_t size) override
    {
        auto& controller = static_cast<PluginProcessor&> (processor).getController();

        if (type == frames::controls && size == sizeof (frames::Controls))
        {
            frames::Controls controls;
            std::memcpy (&controls, payload, sizeof (controls));

            if (controls.inResets != resets.inResets)
                controller.getInMeter().reset();

            if (controls.outResets != resets.outResets)
                controller.getOutMeter().reset();

            resets = controls;
        }
    }

    void update (juce::AudioProcessor& processor, const Sender& send) override
    {
        auto& controller = static_cast<PluginProcessor&> (processor).getController();
        controller.getInMeter().enable (true);
        controller.getOutMeter().enable (true);

        frames::Meters meters {};
        frames::capture (controller.getInMeter(), meters.in, resets.inResets);
        frames::capture (controller.getOutMeter(), meters.out, resets.outResets);
        meters.numChannels = (uint32_t) std::min (controller.getInMeter().getBufferPeak().size(), frames::maxChannels);

        if (! pageHasMeters || std::memcmp (&meters, &sentMeters, sizeof (meters)) != 0)
        {
            pageHasMeters = true;
            sentMeters = meters;
            send (frames::meters, &meters, sizeof (meters));
        }
    }

    /** A new page counts its resets from zero. */
    void opened() override
    {
        pageHasMeters = false;
        resets = {};
    }

    /** No page, nothing to measure for. */
    void closed (juce::AudioProcessor& processor) override
    {
        auto& controller = static_cast<PluginProcessor&> (processor).getController();
        controller.getInMeter().enable (false);
        controller.getOutMeter().enable (false);
    }

    frames::Meters sentMeters {};
    frames::Controls resets {};
    bool pageHasMeters = false;
};
} // namespace

std::unique_ptr<juce::webclap::PageExtension> juce::webclap::createPageExtension()
{
    return std::make_unique<ZLWarmPage>();
}

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DISTORTION,
                                            CLAP_PLUGIN_FEATURE_STEREO, CLAP_PLUGIN_FEATURE_MONO, nullptr };

    static const ClapPluginInfo info {
        "com.zlaudio.zlwarm",
        "ZL Warm",
        "ZL Audio",
        "https://github.com/ZL-Audio/ZLWarm",
        ZL_VERSION,
        "Distortion/saturation, the editor runs in the page",
        features,
        "/ui/index.html",
        280, // the editor's default size
        190
    };

    return info;
}
