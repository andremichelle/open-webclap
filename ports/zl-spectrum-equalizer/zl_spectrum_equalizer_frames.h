/*
    ZLSpectrumEqualizer's port frames (juce_webclap_protocol.h, firstPluginType and up). Parameters and state are
    the kit's generic path; these carry what the editor reads from or tells the processor's controller directly.

        analyzer  DSP -> UI   The pre, post and side audio the analyzers and the meter read (zl_analyzer_stream.h)
        meters    DSP -> UI   Meters: the displayed output gain, the loudness matcher's difference and the sample
                              rate. Up to 30 times a second, only when something changed.
        dynamics  DSP -> UI   One channel's dynamic response, the curve the dynamic bands add to the static one,
                              which the controller publishes in its tri buffers (stereo, left, right, mid, side):
                              DynamicsHeader, then int16 hundredths of a dB for the bins [start, end). Only when
                              the controller published a new one that differs from the last sent.
        controls  UI -> DSP   Controls: what the editor sets on the controller (solo, solo gain, loudness match,
                              editor shown) and which analyzer sources it reads, when it changed.
*/

#pragma once

#include <juce_webclap/juce_webclap_protocol.h>

#include "../zl-common/zl_analyzer_stream.h"

namespace zl_spectrum_equalizer_webclap
{
enum Frame : juce::webclap::protocol::uint8
{
    analyzer = juce::webclap::protocol::firstPluginType,
    meters,
    dynamics,
    controls
};

constexpr std::size_t numChannels = 5; // the controller's tri buffers: stereo, left, right, mid, side

/** The controller's dynamic response is in natural log units (the editor shows it times 20 / ln 10, in dB). */
constexpr float logToCentiDb = 868.5889638065037f;

struct Meters
{
    float displayedGain, lufsDiff, sampleRate;
};

struct DynamicsHeader
{
    uint32_t bins, start, end; // the response's size and the range the payload carries
    uint8_t channel, reserved[3];
};

static_assert (sizeof (DynamicsHeader) == 16);

struct Controls
{
    uint32_t soloWholeIdx;
    float soloGain;
    uint8_t loudnessMatch, editorOn, resetSoloGain, reserved;
    uint32_t analyzerMask; // bit n: analyzer source n (pre, post, side)
};
} // namespace zl_spectrum_equalizer_webclap
