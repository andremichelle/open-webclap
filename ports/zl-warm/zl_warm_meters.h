/*
    Reads and writes the controller's meters (zlMeter::SingleMeter, its peaks are public) for the frames in
    zl_warm_frames.h. The DSP side captures them, the editor's stand-in shows them.
*/

#pragma once

#include "PluginProcessor.h"
#include "zl_warm_frames.h"

namespace zl_warm_webclap
{
inline void capture (zlMeter::SingleMeter<float>& meter, Meter& m, uint32_t resets)
{
    juce::ScopedLock lock (meter.getLock());
    const auto& buffer = meter.getBufferPeak();
    const auto& max = meter.getmaxPeak();

    for (size_t i = 0; i < maxChannels; ++i)
    {
        m.bufferPeak[i] = i < buffer.size() ? buffer[i].load() : -160.0f;
        m.maxPeak[i] = i < max.size() ? max[i].load() : -160.0f;
    }

    m.resets = resets;
}

/** UI side: a channel the DSP side does not have stays empty. */
inline void apply (zlMeter::SingleMeter<float>& meter, const Meter& m, size_t numChannels, bool withMax)
{
    juce::ScopedLock lock (meter.getLock());
    auto& buffer = meter.getBufferPeak();
    auto& max = meter.getmaxPeak();

    for (size_t i = 0; i < buffer.size(); ++i)
    {
        const auto has = i < numChannels && i < maxChannels;
        buffer[i].store (has ? m.bufferPeak[i] : -160.0f);

        if (withMax && i < max.size())
            max[i].store (has ? m.maxPeak[i] : -160.0f);
    }
}
} // namespace zl_warm_webclap
