/*
    Reads and writes the controller's private display values, dynamic responses and editor settings for the frames
    in zl_spectrum_equalizer_frames.h, as a friend (patches/zl-spectrum-equalizer-webclap.patch). The DSP side
    captures meters and dynamic responses and applies controls, the editor's stand-in the other way round.
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "PluginProcessor.hpp"
#include "zl_spectrum_equalizer_frames.h"

namespace zlp
{
struct ZLWebclapAccess
{
    static constexpr auto relaxed = std::memory_order::relaxed;

    static_assert (zl_spectrum_equalizer_webclap::numChannels == std::tuple_size_v<decltype (Controller::tri_buffers_)>);

    static void capture (Controller& c, double sampleRate, zl_spectrum_equalizer_webclap::Meters& m)
    {
        m.displayedGain = (float) c.displayed_gain_.load (relaxed);
        m.lufsDiff = (float) c.loudness_matcher_.getDiff();
        m.sampleRate = (float) sampleRate;
    }

    static void apply (Controller& c, const zl_spectrum_equalizer_webclap::Meters& m)
    {
        c.displayed_gain_.store (m.displayedGain, relaxed);
        c.loudness_matcher_.setDiff (m.lufsDiff);
    }

    /** DSP side: the channel's dynamic response as a frame payload, if the controller published one since the
        last call. */
    static bool captureDynamics (Controller& c, uint8_t channel, std::vector<uint8_t>& out)
    {
        namespace frames = zl_spectrum_equalizer_webclap;
        const std::lock_guard lock (c.tri_buffer_lock_);
        auto& buffer = c.tri_buffers_[channel];

        if (! buffer.pull())
            return false;

        const auto& shared = buffer.getReader();
        const auto bins = shared.delta.size();
        const auto end = std::min (shared.dyn_end, bins);
        const auto start = std::min (shared.dyn_start, end);
        const frames::DynamicsHeader header { (uint32_t) bins, (uint32_t) start, (uint32_t) end, channel, {} };

        out.resize (sizeof (header) + (end - start) * sizeof (int16_t));
        std::memcpy (out.data(), &header, sizeof (header));
        auto* p = out.data() + sizeof (header);

        for (auto i = start; i < end; ++i, p += sizeof (int16_t))
        {
            const auto centiDb = std::clamp (std::round (shared.delta[i] * frames::logToCentiDb), -32767.f, 32767.f);
            const auto value = (int16_t) centiDb;
            std::memcpy (p, &value, sizeof (value));
        }

        return true;
    }

    /** UI side: publishes a dynamic response in the stand-in's tri buffer, where the response panel pulls it. */
    static void applyDynamics (Controller& c, const uint8_t* payload, std::size_t size)
    {
        namespace frames = zl_spectrum_equalizer_webclap;
        frames::DynamicsHeader header;

        if (size < sizeof (header))
            return;

        std::memcpy (&header, payload, sizeof (header));

        if (header.channel >= frames::numChannels || header.start > header.end || header.end > header.bins
            || header.bins > (1u << 20) || size < sizeof (header) + (header.end - header.start) * sizeof (int16_t))
            return;

        const std::lock_guard lock (c.tri_buffer_lock_);
        auto& buffer = c.tri_buffers_[header.channel];
        auto& shared = buffer.getWriter();
        shared.delta.resize (header.bins);
        shared.dyn_start = header.start;
        shared.dyn_end = header.end;
        const auto* p = payload + sizeof (header);

        for (auto i = header.start; i < header.end; ++i, p += sizeof (int16_t))
        {
            int16_t value;
            std::memcpy (&value, p, sizeof (value));
            shared.delta[i] = (float) value / frames::logToCentiDb;
        }

        buffer.publish();
    }

    /** UI side: the analyzer sources the editor reads, as its settings say. The FFT reads the ones switched on,
        the output meter pre and post. */
    static uint32_t analyzerMask (PluginProcessor& p)
    {
        const auto on = [&p] (const char* id) { return p.parameters_NA_.getRawParameterValue (id)->load (relaxed) > .5f; };
        const auto meter = on (zlstate::PMeterDisplayON::kID);

        return (on (zlstate::PFFTPreON::kID) || meter ? 1u : 0u)
             | (on (zlstate::PFFTPostON::kID) || meter ? 2u : 0u)
             | (on (zlstate::PFFTSideON::kID) ? 4u : 0u);
    }

    /** UI side: the controls as the editor left them. Consumes the editor's solo gain reset. */
    static zl_spectrum_equalizer_webclap::Controls controls (PluginProcessor& p)
    {
        auto& c = p.getController();

        return { (uint32_t) c.a_solo_whole_idx_.load (relaxed),
                 c.a_solo_gain_db_.load (relaxed),
                 (uint8_t) c.a_loudness_matcher_on_.load (relaxed),
                 (uint8_t) c.a_editor_on_.load (relaxed),
                 (uint8_t) c.to_reset_solo_gain_.check(),
                 0,
                 analyzerMask (p) };
    }

    /** DSP side: the editor's controls, through the controller's own setters. */
    static void apply (Controller& c, const zl_spectrum_equalizer_webclap::Controls& controls)
    {
        if (controls.soloWholeIdx != c.a_solo_whole_idx_.load (relaxed) && controls.soloWholeIdx <= 2 * kBandNum)
            c.setSoloWholeIdx (controls.soloWholeIdx);

        if (controls.resetSoloGain)
            c.resetSoloGain();
        else if (controls.soloGain != c.a_solo_gain_db_.load (relaxed))
            c.setSoloGain (controls.soloGain);

        if ((controls.loudnessMatch != 0) != c.a_loudness_matcher_on_.load (relaxed))
            c.setLoudnessMatchON (controls.loudnessMatch != 0);

        if ((controls.editorOn != 0) != c.a_editor_on_.load (relaxed))
            c.setEditorON (controls.editorOn != 0);
    }
};
} // namespace zlp
