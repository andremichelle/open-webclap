/*
    Reads and writes the controller's private analyzer state and the processor's value transport for the frames in
    zl_limiter_frames.h, as a friend of both (patches/zl-limiter-webclap.patch). The DSP side captures the status
    and applies controls, the editor's stand-in the other way round.
*/

#pragma once

#include "PluginProcessor.hpp"
#include "zl_limiter_frames.h"

namespace zlp
{
struct ZLWebclapAccess
{
    static constexpr auto relaxed = std::memory_order::relaxed;

    /** DSP side: the status as the processor has it. The value resets are the port's count (the editor's
        notifier is consumed by the caller). */
    static void capture (PluginProcessor& p, uint32_t valueResets, zl_limiter_webclap::Status& s)
    {
        auto& c = p.controller_;
        s = {};
        s.generation = (uint32_t) c.analyzer_generation_.load (relaxed);
        s.numChannels = (uint32_t) c.analyzer_num_channels_.load (relaxed);
        s.valueResets = valueResets;
        s.valueActive = (uint8_t) p.isValueMeasurementActive();
    }

    /** UI side: the DSP side's status into the stand-in. A new generation or channel count restarts the
        editor's displays as upstream, a new reset count signals the value reset. `applied` is the last status
        applied; applying it again re-establishes what the stand-in's own prepareToPlay overwrote. */
    static void apply (PluginProcessor& p, const zl_limiter_webclap::Status& s, zl_limiter_webclap::Status& applied)
    {
        auto& c = p.controller_;
        bool restart = s.generation != applied.generation;

        if (s.numChannels > 0 && s.numChannels != c.analyzer_num_channels_.load (relaxed))
        {
            c.analyzer_num_channels_.store (s.numChannels, std::memory_order::release);
            restart = true;
        }

        if (restart)
            c.analyzer_generation_.fetch_add (1, std::memory_order::release);

        if (s.valueResets != applied.valueResets)
            p.getValueResetNotifier().signal();

        p.value_measurement_active_.store (s.valueActive != 0, std::memory_order::release);
        applied = s;
    }

    /** UI side: the controls as the editor left them. */
    static zl_limiter_webclap::Controls controls (Controller& c)
    {
        return { (uint8_t) c.analyzer_enabled_.load (relaxed) };
    }

    /** DSP side: the editor's controls, through the controller's own setters. */
    static void apply (Controller& c, const zl_limiter_webclap::Controls& controls)
    {
        c.setAnalyzerEnabled (controls.analyzerEnabled != 0);
    }
};
} // namespace zlp
