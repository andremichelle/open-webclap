/*
    Radar as module.wasm: the port's processor (radar_processor.h) wrapped by juce_webclap_clap.cpp. No editor code.

    Where openDAW keeps the sample (opendaw.samples/1), SampleSlots takes its frames and the state keeps only its name.

    The PageExtension takes the pages' edits and samples and tells every page about them (radar_frames.h): a
    pattern a page edited goes to all pages (the one that sent it skips it), a new sample or a loaded state sends the
    waveform and the patterns again, the playback goes out while it changes. The pages get no state in their
    snapshot: it would carry the sample, which they never read.
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "radar_frames.h"
#include "radar_processor.h"

bool radar::Processor::hasEditor() const { return false; }
juce::AudioProcessorEditor* radar::Processor::createEditor() { return nullptr; }

namespace
{
struct RadarPage final : juce::webclap::PageExtension
{
    void receive (juce::AudioProcessor& processor, std::uint8_t type, const void* payload, std::size_t size) override
    {
        auto& radar = static_cast<radar::Processor&> (processor);

        if (type == radar::Frame::pattern && size >= 5)
        {
            juce::MemoryInputStream in (payload, size, false);
            const auto source = (std::uint32_t) in.readInt();
            const auto byte = (std::uint8_t) in.readByte();
            const auto index = (int) (byte & ~radar::Frame::live);
            radar::Pattern pattern;

            if (index < radar::numPatterns && pattern.read (in))
            {
                radar.patterns[(size_t) index] = std::move (pattern);
                due[(size_t) index] = source;

                // a drag's frames play at once; the host hears of the edit once it is finished (one undo step)
                if ((byte & radar::Frame::live) == 0)
                    changed (processor);
            }
        }
        else if (type == radar::Frame::sampleName)
        {
            radar.nameSample (juce::String::fromUTF8 (static_cast<const char*> (payload), (int) juce::jmin (size, (std::size_t) 1024)));
            changed (processor);
        }
        else if (type == radar::Frame::sample)
        {
            if (radar.loadSample (static_cast<const std::uint8_t*> (payload), size))
                changed (processor);
            else
                waveformDue = true; // the page waits for it: what it sent did not load
        }
    }

    void update (juce::AudioProcessor& processor, const Sender& send) override
    {
        auto& radar = static_cast<radar::Processor&> (processor);

        if (std::exchange (seenState, radar.stateRevision) != radar.stateRevision)
            opened();

        if (std::exchange (seenSample, radar.sampleRevision) != radar.sampleRevision)
            waveformDue = true;

        if (std::exchange (waveformDue, false))
        {
            const auto waveform = radar.describeSample();
            send (radar::Frame::waveform, waveform.getData(), waveform.getSize());
        }

        for (size_t index = 0; index < due.size(); ++index)
        {
            if (! due[index])
                continue;

            juce::MemoryOutputStream out;
            out.writeInt ((int) *due[index]);
            out.writeByte ((char) index);
            radar.patterns[index].write (out);
            send (radar::Frame::pattern, out.getData(), out.getDataSize());
            due[index].reset();
        }

        if (std::memcmp (&radar.playback, &sentPlayback, sizeof (radar::Playback)) != 0)
        {
            sentPlayback = radar.playback;
            send (radar::Frame::playback, &sentPlayback, sizeof (sentPlayback));
        }
    }

    void opened() override
    {
        waveformDue = true;
        due.fill (0u); // from no page: every page takes them
        sentPlayback.phase = -1.0f;
    }

    bool pageState (juce::AudioProcessor&, juce::MemoryBlock& state) override
    {
        state.reset();
        return true;
    }

    static void changed (juce::AudioProcessor& processor)
    {
        processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
    }

    std::array<std::optional<std::uint32_t>, radar::numPatterns> due;
    bool waveformDue = true;
    int seenState = 0, seenSample = 0;
    radar::Playback sentPlayback;
};

/** openDAW keeps Radar's sample in the project (opendaw.samples/1, slot 0) and hands its frames over. */
struct RadarSamples final : juce::webclap::SampleSlots
{
    static radar::Processor& radarOf (juce::AudioProcessor& processor) { return static_cast<radar::Processor&> (processor); }

    void hostKeepsSamples (juce::AudioProcessor& processor) override { radarOf (processor).hostKeepsSamples = true; }

    float* prepare (juce::AudioProcessor& processor, std::uint32_t key, std::uint32_t frames, std::uint32_t channels, double rate) override
    {
        return key == 0 ? radarOf (processor).prepareSample (frames, channels, rate) : nullptr;
    }

    void commit (juce::AudioProcessor& processor, std::uint32_t key) override
    {
        if (key != 0)
            return;

        // Marked only when the state changes (it embedded the sample): a delivery after a project load is no edit
        if (radarOf (processor).commitSample())
            RadarPage::changed (processor);
    }

    void clear (juce::AudioProcessor& processor, std::uint32_t key) override
    {
        if (key == 0)
            radarOf (processor).clearHostSample();
    }
};
} // namespace

std::unique_ptr<juce::webclap::SampleSlots> juce::webclap::createSampleSlots()
{
    return std::make_unique<RadarSamples>();
}

std::unique_ptr<juce::webclap::PageExtension> juce::webclap::createPageExtension()
{
    return std::make_unique<RadarPage>();
}

#define STRING(value) #value
#define STRINGIFY(value) STRING(value)
static_assert (RADAR_PANEL_HEIGHT == 176, "the panel width feature is the width at openDAW's 176 px");

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SAMPLER,
                                            CLAP_PLUGIN_FEATURE_STEREO, "granular", "opendaw:device-panel",
                                            // the panel's width at openDAW's 176 px, read from its cloud index
                                            "opendaw:device-panel-width:" STRINGIFY (RADAR_PANEL_WIDTH), nullptr };

    static const ClapPluginInfo info {
        "com.andremichelle.radar",
        "Radar",
        "André Michelle",
        "https://github.com/andremichelle/radar",
        RADAR_VERSION,
        "Loop player: a ray reflecting off drawn segments picks where in the sample to play",
        features,
        "/ui/index.html",
        RADAR_WINDOW_WIDTH,
        RADAR_WINDOW_HEIGHT,
        true,
        "/ui/index.html?panel",
        RADAR_PANEL_WIDTH,
        RADAR_PANEL_HEIGHT
    };

    return info;
}
