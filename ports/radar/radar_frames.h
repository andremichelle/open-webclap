/*
    The port frames between Radar's DSP side and its pages (juce_webclap PageExtension, ProcessorBridge), little
    endian.

    pattern   both ways   u32 source page, u8 index, the pattern (Pattern::write). A page sends what it edited, the
                          DSP side takes it and passes it on to every page; a page skips what it sent itself.
    sample    page to DSP u32 rate, u32 frames, u16 channels (1 or 2), u16 name bytes, the name (UTF-8), the frames
                          as 16 bit, a channel after the other. No frames clear the sample. The page's script decodes
                          the file and builds it (ui/index.html).
    waveform  DSP to page u32 frames, u32 rate, u16 channels, u16 name bytes, the name, u16 bins, then per bin the
                          lowest and the highest value as i8 (both channels): the sample's overview.
    playback  DSP to page Playback, about 30 times a second while it changes.
*/

#pragma once

#include <juce_webclap/juce_webclap_protocol.h>

namespace radar::Frame
{
constexpr std::uint8_t pattern = juce::webclap::protocol::firstPluginType;
constexpr std::uint8_t sample = pattern + 1;
constexpr std::uint8_t waveform = pattern + 2;
constexpr std::uint8_t playback = pattern + 3;

constexpr int waveformBins = 2048;
} // namespace radar::Frame
