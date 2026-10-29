/*
    ZLLimiter's port frames (juce_webclap_protocol.h, firstPluginType and up). Parameters and state are the kit's
    generic path; these carry what the editor reads from or tells the processor and its controller directly.

        analyzer  DSP -> UI   The pre, gained, limited and post audio the peak, meter and value displays read
                              (zl_analyzer_stream.h)
        status    DSP -> UI   Status: the analyzer's generation (bumped when its audio has a gap: reset,
                              oversampling switch, overflow), the main bus's channels, whether the transport lets
                              the value display measure, and a count of the transport's value resets. Sent before
                              the analyzer audio of the same update, only when something changed.
        controls  UI -> DSP   Controls: what the editor sets on the controller (the analyzer on while the editor
                              shows), when it changed.
*/

#pragma once

#include <juce_webclap/juce_webclap_protocol.h>

#include "../zl-common/zl_analyzer_stream.h"

namespace zl_limiter_webclap
{
enum Frame : juce::webclap::protocol::uint8
{
    analyzer = juce::webclap::protocol::firstPluginType,
    status,
    controls
};

struct Status
{
    uint32_t generation, numChannels, valueResets;
    uint8_t valueActive, reserved[3];
};

struct Controls
{
    uint8_t analyzerEnabled;
};
} // namespace zl_limiter_webclap
