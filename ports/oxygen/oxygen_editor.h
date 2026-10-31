/*
    oXygen's editor for the WebCLAP port, sized for openDAW's device panel (176 pixels high), in the dark panel style
    of TapElectric's. Five tabs on the left, one per module, each with its bypass; the page of the selected module
    beside them: EQ (the 15 bands' response, a node per band to drag), Dyn EQ (the cuts as they happen, threshold and
    range per band), Multiband (the crossovers to drag, the reduction per band, the selected band's dials), Imager
    (the crossovers, the width per band) and Maximizer (threshold, ceiling, release, the trim gain before it, the
    mode, input, reduction and output meters). Dials drag up and down and show their values while hovered.
*/

#pragma once

#include "oxygen_processor.h"

namespace oxygen_webclap
{
juce::AudioProcessorEditor* createEditor (Processor&);

/** The meters from the DSP side, for an editor createEditor made. */
void showMeters (juce::AudioProcessorEditor&, const Meters&);
}
