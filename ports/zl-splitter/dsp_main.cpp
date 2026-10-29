/*
    ZLSplitter as module.wasm: the processor wrapped by juce_webclap_clap.cpp, compiled with ZL_WEBCLAP_HEADLESS
    (patches/zl-splitter-webclap.patch): no editor. Upstream has two stereo outputs, one per part; the kit has one
    output port, so the second output bus is off and the main output carries output 1 (left, mid, low, transient
    or peak, by the split type). The Swap parameter puts the other part there.

    The PageExtension carries what the editor reads from the controller beside parameters and state
    (zl_splitter_frames.h).
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "zl_splitter_access.h"

namespace
{
namespace frames = zl_splitter_webclap;

struct ZLSplitterPage final : juce::webclap::PageExtension
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
        auto& controller = static_cast<PluginProcessor&> (processor).getController();

        if (zl_webclap::drainAnalyzer (controller.getAnalyzerSender(), audio))
            send (frames::analyzer, audio.data(), audio.size());
    }

    /** The editor turns the analyzer off when it goes, on its stand-in: here the page's closing does. A new page
        turns it back on with its first controls frame. */
    void closed (juce::AudioProcessor& processor) override
    {
        static_cast<PluginProcessor&> (processor).getController().setAnalyzerOn (false);
    }

    std::vector<uint8_t> audio;
};
} // namespace

std::unique_ptr<juce::webclap::PageExtension> juce::webclap::createPageExtension()
{
    return std::make_unique<ZLSplitterPage>();
}

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
                                            CLAP_PLUGIN_FEATURE_STEREO, nullptr };

    static const ClapPluginInfo info {
        "com.zlaudio.zlsplitter",
        "ZL Splitter",
        "ZL Audio",
        "https://github.com/ZL-Audio/ZLSplitter",
        ZL_VERSION,
        "Splits audio into left/right, mid/side, low/high, transient/steady or peak/steady, the editor runs in the page",
        features,
        "/ui/index.html",
        600, // the editor's default size
        371
    };

    return info;
}
