/*
    RipplerX as module.wasm: the processor, unchanged, wrapped by juce_webclap_clap.cpp. No editor code.

    PluginProcessor.cpp is compiled with RIPPLERX_HEADLESS (patches/ripplerx-headless.patch), which leaves out
    the editor. The processor needs JUCE's GUI modules all the same (AudioProcessorValueTreeState lives in
    juce_audio_processors), so this module links them; nothing reaches the windowing code, so the linker drops
    it and the module still imports only WASI.

    The PageExtension carries what the editor exchanges beside parameters and state (ripplerx_frames.h).
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "PluginProcessor.h"
#include "ripplerx_frames.h"

namespace
{
struct RipplerXPage final : juce::webclap::PageExtension
{
    void receive (juce::AudioProcessor& processor, std::uint8_t type, const void* payload, std::size_t size) override
    {
        auto& p = static_cast<RipplerXAudioProcessor&> (processor);

        if (size != sizeof (int32_t))
            return;

        int32_t value;
        std::memcpy (&value, payload, sizeof (value));

        if (type == ripplerx_webclap::polyphony && value != p.polyphony && value >= 1 && value <= globals::MAX_POLYPHONY)
            p.setPolyphony (value);

        if (type == ripplerx_webclap::program && value >= 0 && value < p.getNumPrograms())
            p.setCurrentProgram (value);
    }

    void update (juce::AudioProcessor& processor, const Sender& send) override
    {
        const auto rms = static_cast<RipplerXAudioProcessor&> (processor).rmsValue.load (std::memory_order_acquire);

        if (rms != sentRms) // silence is one frame, not 30 a second
        {
            sentRms = rms;
            send (ripplerx_webclap::meter, &rms, sizeof (rms));
        }
    }

    void opened() override { sentRms = -1.0f; }

    float sentRms = -1.0f;
};
} // namespace

std::unique_ptr<juce::webclap::PageExtension> juce::webclap::createPageExtension()
{
    return std::make_unique<RipplerXPage>();
}

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                            "physical modeling", nullptr };

    static const ClapPluginInfo info {
        "com.tilr.ripplerx",
        "RipplerX",
        "Tilr",
        "https://github.com/tiagolr/ripplerx",
        RIPPLERX_VERSION,
        "Physically modeled synth, the editor runs in the page",
        features,
        "/ui/index.html",
        650, // the editor at 100 % scale
        485
    };

    return info;
}
