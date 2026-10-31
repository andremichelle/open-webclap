/*
    juce_webclap: tells the host which parameter is under the mouse in the editor (clap.param-hovered), so hosts offer
    their parameter menu on a right click there (openDAW: automation, MIDI learn, a typed value on a double click).

    The editor answers through JUCE's own AudioProcessorEditor::getControlParameterIndex, asked for the component under
    the mouse and then its parents: an editor that maps its controls to parameters there (as hosts on the desktop also
    ask it) gets this for free.

        ParameterHover hover (*editor, *bridge); // UI side, after both exist
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "juce_webclap_bridge.h"

namespace juce::webclap
{
class ParameterHover final : private MouseListener
{
public:
    ParameterHover (AudioProcessorEditor& e, ProcessorBridge& b) : editor (e), bridge (b) { editor.addMouseListener (this, true); }
    ~ParameterHover() override { editor.removeMouseListener (this); }

private:
    void mouseMove (const MouseEvent& event) override { update (event); }
    void mouseDrag (const MouseEvent& event) override { update (event); }
    void mouseEnter (const MouseEvent& event) override { update (event); }

    void mouseExit (const MouseEvent& event) override
    {
        // Leaving a child enters another one or the editor; leaving the editor hovers nothing
        if (! editor.getLocalBounds().contains (event.getEventRelativeTo (&editor).getPosition()))
            send (ProcessorBridge::noHover);
    }

    void update (const MouseEvent& event)
    {
        const auto at = event.getEventRelativeTo (&editor).getPosition();
        auto id = ProcessorBridge::noHover;

        for (auto* c = editor.getComponentAt (at); c != nullptr && c != &editor; c = c->getParentComponent())
        {
            const auto index = editor.getControlParameterIndex (*c);
            const auto& parameters = editor.processor.getParameters();

            if (isPositiveAndBelow (index, parameters.size()))
            {
                id = ProcessorBridge::clapIdFor (*parameters[index]);
                break;
            }
        }

        send (id);
    }

    void send (uint32 id)
    {
        if (std::exchange (hovered, id) != id)
            bridge.sendHover (id);
    }

    AudioProcessorEditor& editor;
    ProcessorBridge& bridge;
    uint32 hovered = ProcessorBridge::noHover;
};
} // namespace juce::webclap
