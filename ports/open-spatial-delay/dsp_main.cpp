/*
    OpenSpatialDelay as module.wasm: the processor wrapped by juce_webclap_clap.cpp. No editor code.

    PluginProcessor.cpp is compiled with OSD_HEADLESS and OSD_WEBCLAP (patches/open-spatial-delay-webclap.patch):
    no editor, a stereo output bus instead of 50 discrete channels, and only the three HRTF sets the bundle carries.

    The PageExtension carries what the editor shows beside parameters and state (osd_frames.h).
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "osd_access.h"

namespace
{
struct OpenSpatialDelayPage final : juce::webclap::PageExtension
{
    void receive (juce::AudioProcessor& processor, std::uint8_t type, const void* payload, std::size_t size) override
    {
        auto& p = static_cast<OpenSpatialDelayProcessor&> (processor);

        if (type != osd_webclap::preset || size != sizeof (int32_t))
            return;

        int32_t index;
        std::memcpy (&index, payload, sizeof (index));

        if (index < 0 || index >= p.getPresetNames().size())
            return;

        p.loadPreset (index);
        // The preset's configuration is state, not parameters: the host stores it only when told
        p.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
    }

    void update (juce::AudioProcessor& processor, const Sender& send) override
    {
        const auto& p = static_cast<const OpenSpatialDelayProcessor&> (processor);
        osd_webclap::MotionFrame frame;
        OsdWebclapAccess::capture (p, frame);

        sendMotion (frame, send);
        const auto& noise = OsdWebclapAccess::noise (p);

        if (! pageHasNoise || std::memcmp (&noise, &sentNoise, sizeof (noise)) != 0)
        {
            pageHasNoise = true;
            std::memcpy (&sentNoise, &noise, sizeof (noise));
            send (osd_webclap::noise, &noise, sizeof (noise));
        }
    }

    void opened() override { pageHasMotion = pageHasNoise = false; }

    /** Only the values that changed (a still scene sends nothing), the whole frame when that is smaller. */
    void sendMotion (const osd_webclap::MotionFrame& frame, const Sender& send)
    {
        size_t size = 0;

        if (pageHasMotion)
        {
            for (size_t index = 0; index < std::size (frame.values); ++index)
            {
                if (std::memcmp (&frame.values[index], &sentMotion.values[index], sizeof (float)) == 0)
                    continue;

                if (size + osd_webclap::deltaEntrySize > sizeof (frame))
                    break;

                delta[size] = (std::uint8_t) index;
                std::memcpy (delta + size + 1, &frame.values[index], sizeof (float));
                size += osd_webclap::deltaEntrySize;
            }

            if (size == 0)
                return;
        }

        if (! pageHasMotion || size + osd_webclap::deltaEntrySize > sizeof (frame))
            send (osd_webclap::motion, &frame, sizeof (frame));
        else
            send (osd_webclap::delta, delta, size);

        pageHasMotion = true;
        sentMotion = frame;
    }

    osd_webclap::MotionFrame sentMotion {};
    OsdWebclapAccess::NoiseFrame sentNoise {};
    std::uint8_t delta[sizeof (osd_webclap::MotionFrame)];
    bool pageHasMotion = false, pageHasNoise = false;
};
} // namespace

std::unique_ptr<juce::webclap::PageExtension> juce::webclap::createPageExtension()
{
    return std::make_unique<OpenSpatialDelayPage>();
}

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DELAY,
                                            CLAP_PLUGIN_FEATURE_STEREO, "spatial", nullptr };

    static const ClapPluginInfo info {
        "org.spatialmedialab.openspatialdelay",
        "OpenSpatialDelay",
        "Spatial Media Lab",
        "https://github.com/Spatial-Media-Lab/OpenSpatialDelay",
        OSD_VERSION,
        "Spatial delay, each echo placed and moving in 3D space, the editor runs in the page",
        features,
        "/ui/index.html",
        820, // the editor's fixed size
        580
    };

    return info;
}
