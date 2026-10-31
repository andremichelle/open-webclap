/*
    Radar's editors, in the dark panel style of the port's TapElectric and oXygen editors.

    The window: the circle large, the sample's waveform around it, the ray of the moment and the grain that plays;
    tools on the left to select, draw lines, arcs and curves and to duplicate, mirror, rotate and delete; the
    sound's dials on the right; the twelve patterns along the bottom to pick, copy, paste and clear.

    The device panel (openDAW's, 176 pixels high): the circle of the playing pattern with its grain, the patterns to
    pick, and the main dials.

    Both read the patterns from the stand-in processor and send what they change (Frame::pattern); the sample, the
    waveform and the playback come in frames from the DSP side (Session).
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

/** What the editors know beside the stand-in's parameters and patterns, and what they ask of the page and the DSP
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

    std::function<void (int index)> sendPattern;
    std::function<void()> openSampleFile;
    std::function<void()> openWindow; // the device panel asks the host for the window
};

juce::AudioProcessorEditor* createWindowEditor (Processor&, Session&);
juce::AudioProcessorEditor* createPanelEditor (Processor&, Session&);
} // namespace radar
