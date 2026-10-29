/*
    ZLLMatch as module.wasm: the processor wrapped by juce_webclap_clap.cpp, compiled with ZL_WEBCLAP_HEADLESS
    (patches/zl-lmatch-webclap.patch): no editor. The Aux input is a second CLAP input port, the reference Learn
    measures with Side: Aux (silent when the host feeds it nothing). Learn needs the host's transport playing, the
    matcher's position is its time in samples.

    The PageExtension carries what the editor reads from the controller beside parameters and state
    (zl_lmatch_frames.h).
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "PluginProcessor.h"
#include "zl_lmatch_frames.h"

namespace
{
namespace frames = zl_lmatch_webclap;

struct ZLLMatchPage final : juce::webclap::PageExtension
{
    void update (juce::AudioProcessor& processor, const Sender& send) override
    {
        const auto gain = static_cast<PluginProcessor&> (processor).getController()->getGain();

        if (! pageHasGain || gain != sentGain)
        {
            pageHasGain = true;
            sentGain = gain;
            send (frames::gain, &gain, sizeof (gain));
        }
    }

    void opened() override { pageHasGain = false; }

    float sentGain = 0.0f;
    bool pageHasGain = false;
};
} // namespace

std::unique_ptr<juce::webclap::PageExtension> juce::webclap::createPageExtension()
{
    return std::make_unique<ZLLMatchPage>();
}

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
                                            CLAP_PLUGIN_FEATURE_STEREO, CLAP_PLUGIN_FEATURE_MONO, nullptr };

    static const ClapPluginInfo info {
        "com.zlaudio.zllmatch",
        "ZL Loudness Match",
        "ZL Audio",
        "https://github.com/ZL-Audio/ZLLMatch",
        ZL_VERSION,
        "Loudness matching, the editor runs in the page",
        features,
        "/ui/index.html",
        276, // the editor's default size
        173
    };

    return info;
}
