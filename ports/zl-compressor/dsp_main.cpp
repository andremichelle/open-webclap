/*
    ZLCompressor as module.wasm: the processor wrapped by juce_webclap_clap.cpp, compiled with ZL_WEBCLAP_HEADLESS
    (patches/zl-compressor-webclap.patch): no editor. The sidechain is the second input port, the
    latency (oversampling, lookahead) goes to the host through clap.latency.

    The PageExtension carries what the editor reads from the controllers beside parameters and state
    (zl_compressor_frames.h).
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "zl_compressor_access.h"

namespace
{
namespace frames = zl_compressor_webclap;

struct ZLCompressorPage final : juce::webclap::PageExtension
{
    void receive (juce::AudioProcessor& processor, std::uint8_t type, const void* payload, std::size_t size) override
    {
        if (type == frames::controls && size == sizeof (frames::Controls))
        {
            frames::Controls controls;
            std::memcpy (&controls, payload, sizeof (controls));
            zlp::ZLWebclapAccess::apply (static_cast<PluginProcessor&> (processor), controls);
            magAnalyzerMask = controls.magAnalyzerMask;
        }
    }

    void update (juce::AudioProcessor& processor, const Sender& send) override
    {
        auto& p = static_cast<PluginProcessor&> (processor);

        if (zl_webclap::drainAnalyzer (p.getCompressController().getMagAnalyzerSender(), audio, 1 << 14, magAnalyzerMask))
            send (frames::magAnalyzer, audio.data(), audio.size());

        if (zl_webclap::drainAnalyzer (p.getEqualizeController().getFFTAnalyzerSender(), audio))
            send (frames::fftAnalyzer, audio.data(), audio.size());

        frames::Status status;
        zlp::ZLWebclapAccess::capture (p, status);

        if (! pageHasStatus || std::memcmp (&status, &sentStatus, sizeof (status)) != 0)
        {
            pageHasStatus = true;
            sentStatus = status;
            send (frames::status, &status, sizeof (status));
        }
    }

    void opened() override { pageHasStatus = false; }

    /** The editor says it is gone on its stand-in only: the page's closing stops the analyzers here. */
    void closed (juce::AudioProcessor& processor) override
    {
        zlp::ZLWebclapAccess::closed (static_cast<PluginProcessor&> (processor));
        magAnalyzerMask = 0;
    }

    std::vector<uint8_t> audio;
    frames::Status sentStatus {};
    uint32_t magAnalyzerMask = 0; // nothing until the editor says what it shows
    bool pageHasStatus = false;
};
} // namespace

std::unique_ptr<juce::webclap::PageExtension> juce::webclap::createPageExtension()
{
    return std::make_unique<ZLCompressorPage>();
}

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_COMPRESSOR,
                                            CLAP_PLUGIN_FEATURE_STEREO, CLAP_PLUGIN_FEATURE_MONO, nullptr };

    static const ClapPluginInfo info {
        "com.zlaudio.zlcompressor",
        "ZL Compressor",
        "ZL Audio",
        "https://github.com/ZL-Audio/ZLCompressor",
        ZL_VERSION,
        "Compressor with side chain EQ, the editor runs in the page",
        features,
        "/ui/index.html",
        600, // the editor's default size
        371
    };

    return info;
}
