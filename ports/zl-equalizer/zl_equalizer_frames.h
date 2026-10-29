/*
    ZLEqualizer's port frames (juce_webclap_protocol.h, firstPluginType and up). Parameters and state are the kit's
    generic path; these carry what the editor reads from or tells the processor's controller directly.

        analyzer  DSP -> UI   The pre, post and side audio the analyzers and the meter read (zl_analyzer_stream.h),
                              only the sources the editor shows
        meters    DSP -> UI   Meters: per band the dynamic gain, side loudness, learned threshold and knee, then
                              the displayed output gain and the loudness matcher's difference. Sent whole to a page
                              that just opened, then only when something changed.
        delta     DSP -> UI   The Meters values that changed, (u8 index, f32 value) each, as Meters::values counts
                              them, when that is smaller than the whole
        controls  UI -> DSP   Controls: what the editor sets on the controller (solo, loudness match, match
                              bypass, editor shown) and which analyzer sources it shows, when it changed.
*/

#pragma once

#include <juce_webclap/juce_webclap_protocol.h>

#include "../zl-common/zl_analyzer_stream.h"

namespace zl_equalizer_webclap
{
enum Frame : juce::webclap::protocol::uint8
{
    analyzer = juce::webclap::protocol::firstPluginType,
    meters,
    controls,
    delta
};

constexpr std::size_t numBands = ZL_EQ_BAND_NUM;

struct MeterValues
{
    float gains[numBands], sideLoudness[numBands], thresholds[numBands], knees[numBands];
    float displayedGain, lufsDiff;
};

union Meters
{
    MeterValues m;
    float values[sizeof (MeterValues) / sizeof (float)];
};

static_assert (std::size (Meters {}.values) <= 256);

constexpr std::size_t deltaEntrySize = 1 + sizeof (float);

struct Controls
{
    uint32_t soloWholeIdx;
    float soloGain;
    uint8_t matchBypass, loudnessMatch, editorOn, resetSoloGain;
    uint32_t analyzerMask; // bit n: analyzer source n (pre, post, side)
};
} // namespace zl_equalizer_webclap
