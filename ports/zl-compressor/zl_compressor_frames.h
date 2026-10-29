/*
    ZLCompressor's port frames (juce_webclap_protocol.h, firstPluginType and up). Parameters and state are the
    kit's generic path; these carry what the editor reads from or tells the processor's controllers directly.

        magAnalyzer  DSP -> UI   The pre, compressed, post and side audio of the compress controller's magnitude
                                 analyzer (peak, RMS and meter panels), zl_analyzer_stream.h, only the sources the
                                 editor shows
        fftAnalyzer  DSP -> UI   The side chain audio of the equalize controller's FFT analyzer (side EQ panel)
        status       DSP -> UI   Status: the LUFS matcher's difference (the learn button reads it when it turns
                                 off) and the sample rate. Up to 30 times a second, only when something changed.
        controls     UI -> DSP   Controls: what the editor sets on the controllers (analyzers on, LUFS matcher,
                                 side EQ solo band and gain) and which magnitude analyzer sources it shows, when it
                                 changed.
*/

#pragma once

#include <juce_webclap/juce_webclap_protocol.h>

#include "../zl-common/zl_analyzer_stream.h"

namespace zl_compressor_webclap
{
enum Frame : juce::webclap::protocol::uint8
{
    magAnalyzer = juce::webclap::protocol::firstPluginType,
    fftAnalyzer,
    status,
    controls
};

struct Status
{
    float lufsDiff, sampleRate;
};

struct Controls
{
    uint32_t soloBand;
    float soloGain;
    uint8_t magAnalyzerOn, fftAnalyzerOn, lufsMatcherOn, resetSoloGain;
    uint32_t magAnalyzerMask; // bit n: magnitude analyzer source n (pre, compressed, post, side)
};
} // namespace zl_compressor_webclap
