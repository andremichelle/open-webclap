/*
    ZLWarm's port frames (juce_webclap_protocol.h, firstPluginType and up). Parameters and state are the kit's
    generic path; these carry what the editor's meter panel reads from or tells the controller's meters directly.

        meters    DSP -> UI   Meters: the input and output meters' decaying peak and held maximum per channel (dB),
                              and the reset count each maximum is from. Up to 30 times a second, only when something
                              changed.
        controls  UI -> DSP   Controls: how often the editor reset each meter's maximum (a click on the meter), when
                              that changed.
*/

#pragma once

#include <juce_webclap/juce_webclap_protocol.h>

namespace zl_warm_webclap
{
enum Frame : juce::webclap::protocol::uint8
{
    meters = juce::webclap::protocol::firstPluginType,
    controls
};

constexpr std::size_t maxChannels = 2;

struct Meter
{
    float bufferPeak[maxChannels], maxPeak[maxChannels];
    uint32_t resets;
};

struct Meters
{
    Meter in, out;
    uint32_t numChannels;
};

struct Controls
{
    uint32_t inResets, outResets;
};
} // namespace zl_warm_webclap
