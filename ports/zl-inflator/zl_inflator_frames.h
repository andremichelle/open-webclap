/*
    ZLInflator's port frames (juce_webclap_protocol.h, firstPluginType and up). Parameters and state are the kit's
    generic path; these carry what the editor reads from the processor directly.

        meters  DSP -> UI   Meters: the input and output meters' RMS and peak per channel (dB), the most each saw
                            since the last frame (MeterSource's buffer). Up to 30 times a second, only when
                            something changed: the editor's stand-in holds the last values until the next frame.
*/

#pragma once

#include <juce_webclap/juce_webclap_protocol.h>

namespace zl_inflator_webclap
{
enum Frame : juce::webclap::protocol::uint8
{
    meters = juce::webclap::protocol::firstPluginType
};

constexpr std::size_t maxChannels = 2;

struct Meter
{
    float rms[maxChannels], peak[maxChannels];
};

struct Meters
{
    Meter in, out;
    uint32_t numChannels;
};
} // namespace zl_inflator_webclap
