/*
    ZLLimiter as module.wasm: the processor wrapped by juce_webclap_clap.cpp, compiled with ZL_WEBCLAP_HEADLESS
    (patches/zl-limiter-webclap.patch): no editor. The limiter's lookahead and oversampling latency reaches the
    host through clap.latency (the controller sets it from an AsyncUpdater, which the kit pumps).

    The PageExtension carries what the editor reads from the processor and its controller beside parameters and
    state (zl_limiter_frames.h).
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "zl_limiter_access.h"

namespace
{
namespace frames = zl_limiter_webclap;

struct ZLLimiterPage final : juce::webclap::PageExtension
{
    void receive (juce::AudioProcessor& processor, std::uint8_t type, const void* payload, std::size_t size) override
    {
        auto& controller = static_cast<PluginProcessor&> (processor).getController();

        if (type == frames::controls && size == sizeof (frames::Controls))
        {
            frames::Controls controls;
            std::memcpy (&controls, payload, sizeof (controls));
            zlp::ZLWebclapAccess::apply (controller, controls);
        }
    }

    void update (juce::AudioProcessor& processor, const Sender& send) override
    {
        auto& p = static_cast<PluginProcessor&> (processor);

        // The editor consumes the transport's value reset: here the page does
        if (p.getValueResetNotifier().check())
            ++valueResets;

        frames::Status status;
        zlp::ZLWebclapAccess::capture (p, valueResets, status);

        // Before the audio: a new generation makes the editor drop what its analyzer holds
        if (! pageHasStatus || std::memcmp (&status, &sentStatus, sizeof (status)) != 0)
        {
            pageHasStatus = true;
            sentStatus = status;
            send (frames::status, &status, sizeof (status));
        }

        if (zl_webclap::drainAnalyzer (p.getController().getMagAnalyzerSender(), audio))
            send (frames::analyzer, audio.data(), audio.size());
    }

    void opened() override { pageHasStatus = false; }

    /** The editor turns the analyzer off when it goes, on its stand-in: here the page's closing does. */
    void closed (juce::AudioProcessor& processor) override
    {
        static_cast<PluginProcessor&> (processor).getController().setAnalyzerEnabled (false);
    }

    std::vector<uint8_t> audio;
    frames::Status sentStatus {};
    uint32_t valueResets = 0;
    bool pageHasStatus = false;
};
} // namespace

std::unique_ptr<juce::webclap::PageExtension> juce::webclap::createPageExtension()
{
    return std::make_unique<ZLLimiterPage>();
}

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_LIMITER,
                                            CLAP_PLUGIN_FEATURE_STEREO, CLAP_PLUGIN_FEATURE_MONO, nullptr };

    static const ClapPluginInfo info {
        "com.zlaudio.zllimiter",
        "ZL Limiter",
        "ZL Audio",
        "https://github.com/ZL-Audio/ZLLimiter",
        ZL_VERSION,
        "Lookahead limiter with true peak and oversampling, the editor runs in the page",
        features,
        "/ui/index.html",
        600, // the editor's default size
        371
    };

    return info;
}
