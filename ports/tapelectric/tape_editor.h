/*
    TapElectric's editor for the WebCLAP port: the same parameters as upstream's editor, laid out and drawn anew in a
    dark panel style. A header bar (name, Rand Tone), then six panels: Hum (level and drift faders, the hum waveform,
    pan, stereo, drift rate and depth, base frequency), Noise (level, pan, stereo), Tape (saturation, tape mix, the
    hum and noise level), Resonances (six harmonics with their fluctuation), Noise EQ (the response with a draggable
    node per band, Q dials) and Motor (input drift and wobble, the delay they modulate as a curve). Dials drag up and
    down, controls without effect at the current settings dim.
*/

#pragma once

#include "PluginProcessor.h"

namespace tapelectric_webclap
{
juce::AudioProcessorEditor* createEditor (TapElectricAudioProcessor&);
}
