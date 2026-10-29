/*
    ZLEqualizer as module.wasm: the processor wrapped by juce_webclap_clap.cpp, compiled with ZL_WEBCLAP_HEADLESS
    (patches/zl-equalizer-webclap.patch): no editor. The sidechain is the second input port.

    The PageExtension carries what the editor reads from the controller beside parameters and state
    (zl_equalizer_frames.h).
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "zl_equalizer_access.h"

namespace
{
namespace frames = zl_equalizer_webclap;

struct ZLEqualizerPage final : juce::webclap::PageExtension
{
    void receive (juce::AudioProcessor& processor, std::uint8_t type, const void* payload, std::size_t size) override
    {
        auto& controller = static_cast<PluginProcessor&> (processor).getController();

        if (type == frames::controls && size == sizeof (frames::Controls))
        {
            frames::Controls controls;
            std::memcpy (&controls, payload, sizeof (controls));
            zlp::ZLWebclapAccess::apply (controller, controls);
            analyzerMask = controls.analyzerMask;
        }
    }

    void update (juce::AudioProcessor& processor, const Sender& send) override
    {
        auto& controller = static_cast<PluginProcessor&> (processor).getController();

        if (zl_webclap::drainAnalyzer (controller.getAnalyzerSender(), audio, 1 << 14, analyzerMask))
            send (frames::analyzer, audio.data(), audio.size());

        frames::Meters meters;
        zlp::ZLWebclapAccess::capture (controller, meters);
        sendMeters (meters, send);
    }

    void opened() override { pageHasMeters = false; }

    /** The editor says it is gone on its stand-in only: the page's closing stops the analyzers here. */
    void closed (juce::AudioProcessor& processor) override
    {
        static_cast<PluginProcessor&> (processor).getController().setEditorON (false);
        analyzerMask = 0;
    }

    /** Only the values that changed (nothing when nothing did), the whole frame when that is smaller. */
    void sendMeters (const frames::Meters& meters, const Sender& send)
    {
        size_t size = 0;

        if (pageHasMeters)
        {
            for (size_t index = 0; index < std::size (meters.values); ++index)
            {
                if (std::memcmp (&meters.values[index], &sentMeters.values[index], sizeof (float)) == 0)
                    continue;

                if (size + frames::deltaEntrySize > sizeof (meters))
                    break;

                delta[size] = (std::uint8_t) index;
                std::memcpy (delta + size + 1, &meters.values[index], sizeof (float));
                size += frames::deltaEntrySize;
            }

            if (size == 0)
                return;
        }

        if (! pageHasMeters || size + frames::deltaEntrySize > sizeof (meters))
            send (frames::meters, &meters, sizeof (meters));
        else
            send (frames::delta, delta, size);

        pageHasMeters = true;
        sentMeters = meters;
    }

    std::vector<uint8_t> audio;
    frames::Meters sentMeters {};
    std::uint8_t delta[sizeof (frames::Meters)];
    uint32_t analyzerMask = 0; // nothing until the editor says what it shows
    bool pageHasMeters = false;
};
} // namespace

std::unique_ptr<juce::webclap::PageExtension> juce::webclap::createPageExtension()
{
    return std::make_unique<ZLEqualizerPage>();
}

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_EQUALIZER,
                                            CLAP_PLUGIN_FEATURE_STEREO, CLAP_PLUGIN_FEATURE_MONO, nullptr };

    static const ClapPluginInfo info {
        "com.zlaudio.zlequalizer",
        "ZL Equalizer 2",
        "ZL Audio",
        "https://github.com/ZL-Audio/ZLEqualizer",
        ZL_VERSION,
        "Dynamic equalizer with match EQ, the editor runs in the page",
        features,
        "/ui/index.html",
        600, // the editor's default size
        371
    };

    return info;
}
