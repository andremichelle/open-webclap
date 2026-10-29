/*
    ZL Loudness Makeup's editor for the WebCLAP port: the same parameters as upstream's editor, laid out and drawn
    anew. A header bar (name, mode tabs, Side Out), then three panels: Gain (the applied gain as a large readout, the
    Gain dial, Ceil), Detector (Segment, Window, Lookahead, Accurate, RMS or K-RMS) and Makeup (Strength, Bound,
    Sensitivity). Dials drag up and down. In Envelope mode the controls the mode does not use dim, as upstream.
*/

#pragma once

#include "PluginProcessor.h"

namespace zl_lmakeup_webclap
{
juce::AudioProcessorEditor* createEditor (PluginProcessor&);
}
