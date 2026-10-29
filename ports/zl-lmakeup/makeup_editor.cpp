#include "makeup_editor.h"

#include "BinaryData.h"

namespace zl_lmakeup_webclap
{
namespace
{
namespace colours
{
    const juce::Colour background { 0xff232323 }, header { 0xff1b1b1b }, panel { 0xff2c2c2c }, divider { 0xff191919 };
    const juce::Colour text { 0xffd8d8d8 }, label { 0xffa6a6a6 }, caption { 0xff7c7c7c };
    const juce::Colour track { 0xff3c3c3c }, pill { 0xff3a3a3a }, pillOn { 0xff5c5c5c };
    const juce::Colour teal { 0xff38c7b0 }, violet { 0xffa77bff }, orange { 0xfff2a33a }, pink { 0xffff7aa8 },
        blue { 0xff3aa9ff };
}

juce::Font font (float height)
{
    static const auto typeface = juce::Typeface::createSystemTypefaceFor (BinaryData::OpenSansSemiBold_ttf,
                                                                         BinaryData::OpenSansSemiBold_ttfSize);
    return juce::Font (juce::FontOptions (typeface).withHeight (height));
}

/** A dial in the template's style: a dark knob with a pointer inside a thin track, the value as a coloured arc
    (from the centre for bipolar parameters). Drags up and down. The label shows the value while hovered or
    dragged. */
class Dial final : public juce::Component
{
public:
    Dial (juce::AudioProcessorValueTreeState& state, const juce::String& id, const juce::String& name,
          juce::Colour colour, bool bipolar = false)
        : caption (name), accent (colour), centred (bipolar),
          attachment (state, id, slider), parameter (*state.getParameter (id))
    {
        slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        slider.setMouseDragSensitivity (180);
        slider.setVelocityBasedMode (false);
        slider.setDoubleClickReturnValue (true, (double) state.getParameterRange (id).convertFrom0to1 (
                                                    parameter.getDefaultValue()));
        slider.setScrollWheelEnabled (true);
        slider.setLookAndFeel (&look);
        slider.addMouseListener (this, false);
        slider.onValueChange = [this] { repaint(); };
        look.dial = this;
        addAndMakeVisible (slider);
    }

    ~Dial() override { slider.setLookAndFeel (nullptr); }

    /** Shows the value without taking input (Gain in Effect mode, which the plugin sets itself). */
    void setReadOnly (bool readOnly) { slider.setInterceptsMouseClicks (! readOnly, ! readOnly); }

    void resized() override
    {
        auto bounds = getLocalBounds();
        labelArea = bounds.removeFromBottom (16);
        slider.setBounds (bounds.withSizeKeepingCentre (bounds.getHeight(), bounds.getHeight()));
    }

    void paint (juce::Graphics& g) override
    {
        const auto showValue = slider.isMouseOverOrDragging() && isEnabled();
        g.setFont (font (11.5f));
        g.setColour (showValue ? colours::text : colours::label);
        g.drawText (showValue ? parameter.getCurrentValueAsText() : caption, labelArea, juce::Justification::centred);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseUp (const juce::MouseEvent&) override { repaint(); }

private:
    struct Look final : juce::LookAndFeel_V4
    {
        void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position,
                               float startAngle, float endAngle, juce::Slider&) override
        {
            const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
            const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
            const auto centre = bounds.getCentre();
            const auto ring = radius - 1.5f;
            const auto angle = startAngle + position * (endAngle - startAngle);

            const auto arc = [&] (float from, float to, juce::Colour colour)
            {
                juce::Path path;
                path.addCentredArc (centre.x, centre.y, ring, ring, 0.0f, from, to, true);
                g.setColour (colour);
                g.strokePath (path, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
            };

            arc (startAngle, endAngle, colours::track);

            if (dial->isEnabled())
            {
                const auto origin = dial->centred ? (startAngle + endAngle) * 0.5f : startAngle;

                if (std::abs (angle - origin) > 0.01f)
                    arc (juce::jmin (origin, angle), juce::jmax (origin, angle), dial->accent);
            }

            // The knob: a soft vertical gradient, a dark rim, a light top edge
            const auto knob = radius - 6.0f;
            const auto body = juce::Rectangle<float> (knob * 2.0f, knob * 2.0f).withCentre (centre);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4a4a), centre.x, body.getY(),
                                                     juce::Colour (0xff262626), centre.x, body.getBottom(), false));
            g.fillEllipse (body);
            g.setColour (juce::Colour (0xff141414));
            g.drawEllipse (body, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawEllipse (body.reduced (1.0f), 1.0f);

            // The pointer
            const auto direction = juce::Point<float> (std::sin (angle), -std::cos (angle));
            g.setColour (dial->isEnabled() ? juce::Colour (0xffeeeeee) : colours::caption);
            g.drawLine ({ centre + direction * (knob * 0.25f), centre + direction * (knob * 0.85f) }, 2.0f);
        }

        Dial* dial = nullptr;
    };

    juce::Slider slider;
    juce::String caption;
    juce::Colour accent;
    bool centred;
    Look look;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
    juce::RangedAudioParameter& parameter;
    juce::Rectangle<int> labelArea;
};

/** A switch as a rounded pill, light when on. */
class Pill final : public juce::Component, private juce::AudioProcessorValueTreeState::Listener,
                   private juce::AsyncUpdater
{
public:
    Pill (juce::AudioProcessorValueTreeState& s, const juce::String& parameterId, const juce::String& name)
        : state (s), id (parameterId), text (name), parameter (*s.getParameter (parameterId))
    {
        state.addParameterListener (id, this);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    ~Pill() override { state.removeParameterListener (id, this); }

    void paint (juce::Graphics& g) override
    {
        const auto on = parameter.getValue() > 0.5f;
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (on ? colours::pillOn : colours::pill);
        g.fillRoundedRectangle (bounds, bounds.getHeight() * 0.5f);
        g.setColour (on ? juce::Colours::white : colours::label);
        g.setFont (font (11.5f));
        g.drawText (text, getLocalBounds(), juce::Justification::centred);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! isEnabled() || ! getLocalBounds().contains (event.getPosition()))
            return;

        parameter.beginChangeGesture();
        parameter.setValueNotifyingHost (parameter.getValue() > 0.5f ? 0.0f : 1.0f);
        parameter.endChangeGesture();
    }

private:
    void parameterChanged (const juce::String&, float) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { repaint(); }

    juce::AudioProcessorValueTreeState& state;
    juce::String id, text;
    juce::RangedAudioParameter& parameter;
};

/** A choice as underlined text: a click steps to the next choice. With tabs, every choice is shown and the
    current one is light (the mode switch in the header). */
class Choice final : public juce::Component, private juce::AudioProcessorValueTreeState::Listener,
                     private juce::AsyncUpdater
{
public:
    Choice (juce::AudioProcessorValueTreeState& s, const juce::String& parameterId, bool asTabs, float textHeight)
        : state (s), id (parameterId), tabs (asTabs), height (textHeight),
          parameter (*dynamic_cast<juce::AudioParameterChoice*> (s.getParameter (parameterId)))
    {
        state.addParameterListener (id, this);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    ~Choice() override { state.removeParameterListener (id, this); }

    void paint (juce::Graphics& g) override
    {
        g.setFont (font (height));

        if (! tabs)
        {
            draw (g, parameter.getCurrentChoiceName(), getLocalBounds(), true);
            return;
        }

        auto bounds = getLocalBounds();

        for (int index = 0; index < parameter.choices.size(); ++index)
        {
            const auto& name = parameter.choices[index];
            const auto width = juce::roundToInt (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), name)) + 16;
            draw (g, name, bounds.removeFromLeft (width), index == parameter.getIndex());
        }
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! isEnabled())
            return;

        auto next = (parameter.getIndex() + 1) % parameter.choices.size();

        if (tabs)
        {
            auto x = 0;
            const auto typeface = font (height);

            for (int index = 0; index < parameter.choices.size(); ++index)
            {
                x += juce::roundToInt (juce::GlyphArrangement::getStringWidth (typeface, parameter.choices[index])) + 16;

                if (event.x < x)
                {
                    next = index;
                    break;
                }
            }
        }

        parameter.beginChangeGesture();
        parameter.setValueNotifyingHost (parameter.convertTo0to1 ((float) next));
        parameter.endChangeGesture();
    }

private:
    void draw (juce::Graphics& g, const juce::String& name, juce::Rectangle<int> area, bool current) const
    {
        const auto colour = current ? colours::text : colours::caption;
        g.setColour (colour);
        g.drawText (name, area, tabs ? juce::Justification::centred : juce::Justification::centredRight);

        if (current)
        {
            const auto width = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), name);
            const auto left = tabs ? (float) area.getCentreX() - width * 0.5f : (float) area.getRight() - width;
            const auto baseline = (float) area.getCentreY() + height * 0.62f;
            g.fillRect (juce::Rectangle<float> (left, baseline, width, 1.0f));
        }
    }

    void parameterChanged (const juce::String&, float) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { repaint(); }

    juce::AudioProcessorValueTreeState& state;
    juce::String id;
    bool tabs;
    float height;
    juce::AudioParameterChoice& parameter;
};

/** The gain the plugin applies, as a large readout: the whole decibels large, the hundredths smaller. */
class Readout final : public juce::Component, private juce::Timer
{
public:
    explicit Readout (Controller<float>& source) : controller (source) { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        const auto text = juce::String (shown, 2);
        const auto sign = shown > 0.004f ? juce::String ("+") : juce::String();
        const auto point = text.indexOfChar ('.');
        const auto whole = sign + (point >= 0 ? text.substring (0, point) : text);
        const auto fraction = point >= 0 ? text.substring (point) : juce::String();

        const auto large = font (34.0f), small = font (19.0f);
        const auto wholeWidth = juce::GlyphArrangement::getStringWidth (large, whole);
        const auto fractionWidth = juce::GlyphArrangement::getStringWidth (small, fraction);
        auto x = ((float) getWidth() - wholeWidth - fractionWidth) * 0.5f;
        const auto baseline = (float) getHeight() * 0.62f;

        g.setColour (colours::text);
        g.setFont (large);
        g.drawSingleLineText (whole, juce::roundToInt (x), juce::roundToInt (baseline));
        g.setFont (small);
        g.drawSingleLineText (fraction, juce::roundToInt (x + wholeWidth), juce::roundToInt (baseline));

        g.setColour (colours::caption);
        g.setFont (font (11.5f));
        g.drawText ("dB", getLocalBounds().removeFromBottom (16), juce::Justification::centred);
    }

private:
    void timerCallback() override
    {
        const auto gain = controller.getGain();

        if (std::abs (gain - shown) >= 0.005f)
        {
            shown = gain;
            repaint();
        }
    }

    Controller<float>& controller;
    float shown = 0.0f;
};

class Editor final : public juce::AudioProcessorEditor, private juce::AudioProcessorValueTreeState::Listener,
                     private juce::AsyncUpdater
{
public:
    explicit Editor (PluginProcessor& p)
        : AudioProcessorEditor (p), state (p.parameters),
          mode (state, zldsp::mode::ID, true, 13.0f),
          measurement (state, zldsp::measurement::ID, false, 12.0f),
          sideOut (state, zldsp::sideout::ID, "Side Out"),
          ceil (state, zldsp::ceil::ID, "Ceil"),
          accurate (state, zldsp::accurate::ID, "Accurate"),
          readout (*p.getController()),
          gain (state, zldsp::gain::ID, "Gain", colours::blue, true),
          segment (state, zldsp::segment::ID, "Segment", colours::teal),
          window (state, zldsp::window::ID, "Window", colours::teal),
          lookahead (state, zldsp::lookahead::ID, "Lookahead", colours::teal),
          strength (state, zldsp::strength::ID, "Strength", colours::violet),
          bound (state, zldsp::bound::ID, "Bound", colours::orange),
          sensitivity (state, zldsp::sensitivity::ID, "Sensitivity", colours::pink)
    {
        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &mode, &measurement, &sideOut, &ceil, &accurate, &readout, &gain, &segment, &window, &lookahead,
                 &strength, &bound, &sensitivity })
            addAndMakeVisible (c);

        state.addParameterListener (zldsp::mode::ID, this);
        updateMode();
        setSize (640, 248);
    }

    ~Editor() override { state.removeParameterListener (zldsp::mode::ID, this); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::background);

        g.setColour (colours::header);
        g.fillRect (headerArea);
        g.setColour (colours::caption);
        g.setFont (font (13.0f));
        g.drawText ("ZL LOUDNESS MAKEUP", headerArea.withTrimmedLeft (14), juce::Justification::centredLeft);

        for (const auto& [area, title] : panels)
        {
            g.setColour (colours::panel);
            g.fillRect (area);
            g.setColour (colours::caption);
            g.setFont (font (12.0f));
            g.drawText (title, area.reduced (10, 0).removeFromTop (26), juce::Justification::centredLeft);
        }

        g.setColour (colours::divider);

        for (const auto& [area, title] : panels)
            g.fillRect (area.getRight(), area.getY(), 2, area.getHeight());
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        headerArea = bounds.removeFromTop (34);
        auto header = headerArea.reduced (10, 7);
        sideOut.setBounds (header.removeFromRight (70));
        header.removeFromLeft (150);
        mode.setBounds (header.removeFromLeft (140));

        bounds.removeFromTop (2);
        auto gainPanel = bounds.removeFromLeft (150);
        bounds.removeFromLeft (2);
        auto detectorPanel = bounds.removeFromLeft (264);
        bounds.removeFromLeft (2);
        auto makeupPanel = bounds;
        panels = { { gainPanel, "Gain" }, { detectorPanel, "Detector" }, { makeupPanel, "Makeup" } };

        const auto dialSize = juce::Point<int> (72, 70);
        const auto place = [&dialSize] (juce::Component& c, juce::Point<int> centre)
        {
            c.setBounds (juce::Rectangle<int> (dialSize.x, dialSize.y).withCentre (centre));
        };

        // Gain: the readout, the Gain dial, Ceil
        readout.setBounds (gainPanel.getX(), gainPanel.getY() + 28, gainPanel.getWidth(), 58);
        place (gain, { gainPanel.getCentreX(), gainPanel.getY() + 128 });
        ceil.setBounds (juce::Rectangle<int> (64, 20).withCentre ({ gainPanel.getCentreX(), gainPanel.getBottom() - 26 }));

        // Detector: RMS or K-RMS in the title row, three dials, Accurate
        measurement.setBounds (detectorPanel.getRight() - 90, detectorPanel.getY() + 3, 80, 22);
        const auto row = detectorPanel.getY() + 92;
        const auto third = detectorPanel.getWidth() / 3;
        place (segment, { detectorPanel.getX() + third / 2, row });
        place (window, { detectorPanel.getX() + third + third / 2, row });
        place (lookahead, { detectorPanel.getX() + 2 * third + third / 2, row });
        accurate.setBounds (juce::Rectangle<int> (76, 20).withCentre ({ detectorPanel.getCentreX(), detectorPanel.getBottom() - 26 }));

        // Makeup: three dials
        const auto makeupThird = makeupPanel.getWidth() / 3;
        place (strength, { makeupPanel.getX() + makeupThird / 2, row });
        place (bound, { makeupPanel.getX() + makeupThird + makeupThird / 2, row });
        place (sensitivity, { makeupPanel.getX() + 2 * makeupThird + makeupThird / 2, row });
    }

private:
    void parameterChanged (const juce::String&, float) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { updateMode(); }

    /** Envelope mode applies the Gain parameter (or its automation): what only the Effect mode uses dims, the Gain
        dial turns on. Effect mode writes the gain it measures to the parameter, so the dial follows it there. */
    void updateMode()
    {
        const auto effect = state.getRawParameterValue (zldsp::mode::ID)->load() < 0.5f;

        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &measurement, &ceil, &accurate, &segment, &window, &lookahead, &bound, &sensitivity })
        {
            c->setEnabled (effect);
            c->setAlpha (effect ? 1.0f : 0.35f);
        }

        gain.setReadOnly (effect);
        repaint();
    }

    juce::AudioProcessorValueTreeState& state;
    Choice mode, measurement;
    Pill sideOut, ceil, accurate;
    Readout readout;
    Dial gain, segment, window, lookahead, strength, bound, sensitivity;
    juce::Rectangle<int> headerArea;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> panels;
};
} // namespace

juce::AudioProcessorEditor* createEditor (PluginProcessor& processor)
{
    return new Editor (processor);
}
} // namespace zl_lmakeup_webclap
