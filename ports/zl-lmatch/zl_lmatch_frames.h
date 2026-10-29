/*
    ZLLMatch's port frames (juce_webclap_protocol.h, firstPluginType and up). Parameters and state (with the learned
    loudness differences) are the kit's generic path; this carries what the editor reads from the controller
    directly.

        gain  DSP -> UI   float: the controller's gain (dB) the editor shows, what the matcher applies at the
                          current position in Effect mode. Up to 30 times a second, only when it changed.
*/

#pragma once

#include <juce_webclap/juce_webclap_protocol.h>

namespace zl_lmatch_webclap
{
enum Frame : juce::webclap::protocol::uint8
{
    gain = juce::webclap::protocol::firstPluginType
};
} // namespace zl_lmatch_webclap
