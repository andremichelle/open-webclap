/*
    oXygen's port frames (juce_webclap_protocol.h, firstPluginType and up). Everything else is the kit's generic
    path: parameters and state.

        meters     DSP -> UI   oxygen_webclap::Meters, 44 bytes: the dynamic EQ's and the multiband compressor's
                               reduction per band and the maximizer's, in dB, and the input and output peak, the
                               most since the last frame. Up to 30 a second, none while nothing changes.
        spectrum   DSP -> UI   128 bytes: the input's and then the output's spectrum, 64 bands each from 20 Hz to
                               20 kHz, 0 for -90 dBFS to 255 for 0 dBFS (Processor::computeSpectrum). Up to 30 a
                               second while the page is open, none while it does not change (silence).
*/

#pragma once

#include <juce_webclap/juce_webclap_protocol.h>

namespace oxygen_webclap
{
enum Frame : juce::webclap::protocol::uint8
{
    meters = juce::webclap::protocol::firstPluginType,
    spectrum
};
}
