/*
    Radar's editor, in the dark panel style of the port's TapElectric and oXygen editors.

    The window: the circle large, the sample's waveform around it, the ray of the moment and the grain that plays;
    tools on the left to select, draw lines, arcs and curves and to duplicate, mirror, rotate and delete; the
    sound's dials on the right; the twelve patterns along the bottom to pick, copy, paste and clear.

    It reads the patterns from the stand-in processor and sends what it changes (Frame::pattern); the sample, the
    waveform and the playback come in frames from the DSP side (Session).

    openDAW's device panel is not this editor but ui/panel.html, an HTML page on modules/opendaw-page.
*/

#pragma once

#include "radar_processor.h"

namespace radar
{
/** The sample as the DSP side describes it (Frame::waveform). */
struct Waveform
{
    juce::String name;
    int frames = 0, rate = 0, channels = 0;
    std::vector<std::int8_t> bins; // lowest and highest per bin
};

/** What the editor knows beside the stand-in's parameters and patterns, and what it asks of the page and the DSP
    side. ui_main.cpp fills it. */
struct Session
{
    Waveform waveform;
    int waveformRevision = 0;

    Playback playback;
    int playbackRevision = 0;

    int patternsRevision = 0; // a pattern arrived from the DSP side (another page edited it, a state load)

    bool loading = false;  // a sample is on its way to the DSP side
    bool dragging = false; // a file is dragged over the page

    std::function<void (int index, bool live)> sendPattern; // live: a drag's frame, the edit is not finished
    std::function<void()> openSampleFile;
};

juce::AudioProcessorEditor* createWindowEditor (Processor&, Session&);
} // namespace radar
