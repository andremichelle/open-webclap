/*
    ZLSplitter's port frames (juce_webclap_protocol.h, firstPluginType and up). Parameters and state are the kit's
    generic path; these carry what the editor reads from or tells the processor's controller directly.

        analyzer  DSP -> UI   The two parts' audio (output 1 and output 2, stereo each) the FFT, magnitude and
                              waveform analyzers read (zl_analyzer_stream.h). Its header carries the sample rate
                              the stand-in follows.
        controls  UI -> DSP   Controls: what the editor sets on the controller (the analyzer on while the curve
                              panel exists), when it changed.
*/

#pragma once

#include <juce_webclap/juce_webclap_protocol.h>

#include "../zl-common/zl_analyzer_stream.h"

namespace zl_splitter_webclap
{
enum Frame : juce::webclap::protocol::uint8
{
    analyzer = juce::webclap::protocol::firstPluginType,
    controls
};

struct Controls
{
    uint8_t analyzerOn;
};
} // namespace zl_splitter_webclap
