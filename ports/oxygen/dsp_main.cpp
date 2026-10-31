/*
    oXygen as module.wasm: the port's processor (oxygen_processor.h) wrapped by juce_webclap_clap.cpp. No editor
    code: the modules build without upstream's (patches/oxygen-webclap.patch), the port's runs in the page.

    The PageExtension sends the meters (oxygen_frames.h).
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "oxygen_frames.h"
#include "oxygen_processor.h"

bool oxygen_webclap::Processor::hasEditor() const { return false; }
juce::AudioProcessorEditor* oxygen_webclap::Processor::createEditor() { return nullptr; }

namespace
{
struct OxygenPage final : juce::webclap::PageExtension
{
    void update (juce::AudioProcessor& processor, const Sender& send) override
    {
        const auto meters = static_cast<oxygen_webclap::Processor&> (processor).takeMeters();

        if (std::memcmp (&meters, &sent, sizeof (meters)) != 0) // silence is one frame, not 30 a second
        {
            sent = meters;
            send (oxygen_webclap::Frame::meters, &meters, sizeof (meters));
        }
    }

    void opened() override { sent.input = -1.0f; }

    oxygen_webclap::Meters sent;
};
} // namespace

std::unique_ptr<juce::webclap::PageExtension> juce::webclap::createPageExtension()
{
    return std::make_unique<OxygenPage>();
}

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_MASTERING,
                                            CLAP_PLUGIN_FEATURE_EQUALIZER, CLAP_PLUGIN_FEATURE_COMPRESSOR,
                                            CLAP_PLUGIN_FEATURE_LIMITER, CLAP_PLUGIN_FEATURE_STEREO,
                                            "opendaw:device-panel", nullptr };

    static const ClapPluginInfo info {
        "com.wamphyre.oxygen",
        "oXygen",
        "Wamphyre",
        "https://github.com/Wamphyre/oXygen",
        OXYGEN_VERSION,
        "Mastering chain: graphic EQ, dynamic EQ, multiband compressor, stereo imager, maximizer",
        features,
        "/ui/index.html",
        OXYGEN_EDITOR_WIDTH, // the device panel's height, so the panel and the window show it unscaled
        OXYGEN_EDITOR_HEIGHT,
        true
    };

    return info;
}
