/*
    ZLLMakeup as module.wasm: the processor wrapped by juce_webclap_clap.cpp, compiled with ZL_WEBCLAP_HEADLESS
    (patches/zl-lmakeup-webclap.patch): no editor. The Aux input, the reference Effect mode makes the main input's
    loudness up to, is a second CLAP input port (silent when the host feeds it nothing: Effect mode then turns the
    input down as far as Bound allows).

    The PageExtension carries what the editor reads from the controller beside parameters and state
    (zl_lmakeup_frames.h).
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "PluginProcessor.h"
#include "zl_lmakeup_frames.h"

namespace
{
namespace frames = zl_lmakeup_webclap;

struct ZLLMakeupPage final : juce::webclap::PageExtension
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
    return std::make_unique<ZLLMakeupPage>();
}

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
                                            CLAP_PLUGIN_FEATURE_STEREO, CLAP_PLUGIN_FEATURE_MONO, nullptr };

    static const ClapPluginInfo info {
        "com.zlaudio.zllmatch",
        "ZL Loudness Makeup",
        "ZL Audio",
        "https://github.com/ZL-Audio/ZLLMakeup",
        ZL_VERSION,
        "Loudness make-up to a reference, the editor runs in the page",
        features,
        "/ui/index.html",
        341, // the editor's default size
        173
    };

    return info;
}
