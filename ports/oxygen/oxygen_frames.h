/*
    oXygen's port frames (juce_webclap_protocol.h, firstPluginType and up). Everything else is the kit's generic
    path: parameters and state.

        meters     DSP -> UI   oxygen_webclap::Meters, 44 bytes: the dynamic EQ's and the multiband compressor's
                               reduction per band and the maximizer's, in dB, and the input and output peak, the
                               most since the last frame. Up to 30 a second, none while nothing changes.
*/

#pragma once

#include <juce_webclap/juce_webclap_protocol.h>

namespace oxygen_webclap
{
enum Frame : juce::webclap::protocol::uint8
{
    meters = juce::webclap::protocol::firstPluginType
};
}
