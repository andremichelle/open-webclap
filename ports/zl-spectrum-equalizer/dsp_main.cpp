/*
    ZLSpectrumEqualizer as module.wasm: the processor wrapped by juce_webclap_clap.cpp, compiled with
    ZL_WEBCLAP_HEADLESS (patches/zl-spectrum-equalizer-webclap.patch): no editor. The sidechain is the second input
    port. The FFT's latency reaches the host through clap.latency.

    The PageExtension carries what the editor reads from the controller beside parameters and state
    (zl_spectrum_equalizer_frames.h).
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "zl_spectrum_equalizer_access.h"

namespace
{
namespace frames = zl_spectrum_equalizer_webclap;

struct ZLSpectrumEqualizerPage final : juce::webclap::PageExtension
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
        auto& plugin = static_cast<PluginProcessor&> (processor);
        auto& controller = plugin.getController();

        if (zl_webclap::drainAnalyzer (controller.getAnalyzerSender(), audio, 1 << 14, analyzerMask))
            send (frames::analyzer, audio.data(), audio.size());

        frames::Meters meters;
        zlp::ZLWebclapAccess::capture (controller, plugin.getAtomicSampleRate(), meters);

        if (! pageHasMeters || std::memcmp (&meters, &sentMeters, sizeof (meters)) != 0)
        {
            pageHasMeters = true;
            sentMeters = meters;
            send (frames::meters, &meters, sizeof (meters));
        }

        for (uint8_t channel = 0; channel < frames::numChannels; ++channel)
        {
            auto& sent = sentDynamics[channel];

            const auto fresh = zlp::ZLWebclapAccess::captureDynamics (controller, channel, dynamics) && dynamics != sent;

            if (fresh)
                sent.swap (dynamics);

            if ((fresh || ! pageHasDynamics) && ! sent.empty())
                send (frames::dynamics, sent.data(), sent.size());
        }

        pageHasDynamics = true;
    }

    void opened() override { pageHasMeters = pageHasDynamics = false; }

    /** The editor says it is gone on its stand-in only: the page's closing stops the analyzers and the dynamic
        responses here. */
    void closed (juce::AudioProcessor& processor) override
    {
        static_cast<PluginProcessor&> (processor).getController().setEditorON (false);
        analyzerMask = 0;
    }

    std::vector<uint8_t> audio, dynamics;
    std::array<std::vector<uint8_t>, frames::numChannels> sentDynamics;
    frames::Meters sentMeters {};
    uint32_t analyzerMask = 0; // until the page's controls say which sources it reads
    bool pageHasMeters = false, pageHasDynamics = false;
};
} // namespace

std::unique_ptr<juce::webclap::PageExtension> juce::webclap::createPageExtension()
{
    return std::make_unique<ZLSpectrumEqualizerPage>();
}

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_EQUALIZER,
                                            CLAP_PLUGIN_FEATURE_STEREO, nullptr };

    static const ClapPluginInfo info {
        "com.zlaudio.zlspectrumequalizer",
        "ZL Spectrum Equalizer",
        "ZL Audio",
        "https://github.com/ZL-Audio/ZLSpectrumEqualizer",
        ZL_VERSION,
        "Spectrum equalizer with dynamic bands, the editor runs in the page",
        features,
        "/ui/index.html",
        600, // the editor's default size
        371
    };

    return info;
}
