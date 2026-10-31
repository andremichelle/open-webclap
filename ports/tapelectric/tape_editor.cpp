#include "tape_editor.h"

#include "WebFonts.h"

namespace tapelectric_webclap
{
namespace
{
using Processor = TapElectricAudioProcessor;

namespace colours
{
    const juce::Colour background { 0xff232323 }, header { 0xff1b1b1b }, panel { 0xff2c2c2c }, divider { 0xff191919 };
    const juce::Colour text { 0xffd8d8d8 }, label { 0xffa6a6a6 }, caption { 0xff7c7c7c }, grid { 0xff363636 };
    const juce::Colour track { 0xff3c3c3c }, pill { 0xff3a3a3a }, pillHover { 0xff4a4a4a };
    const juce::Colour teal { 0xff38c7b0 }, violet { 0xffa77bff }, orange { 0xfff2a33a }, pink { 0xffff7aa8 },
        blue { 0xff3aa9ff };
}

enum class Face { regular, medium, italic };

juce::Font font (float height, Face face = Face::medium)
{
    static const auto regular = juce::Typeface::createSystemTypefaceFor (WebFonts::InterRegular_ttf,
                                                                        WebFonts::InterRegular_ttfSize);
    static const auto medium = juce::Typeface::createSystemTypefaceFor (WebFonts::InterMedium_ttf,
                                                                       WebFonts::InterMedium_ttfSize);
    static const auto italic = juce::Typeface::createSystemTypefaceFor (WebFonts::InterMediumItalic_ttf,
                                                                       WebFonts::InterMediumItalic_ttfSize);
    const auto& typeface = face == Face::regular ? regular : face == Face::italic ? italic : medium;
    return juce::Font (juce::FontOptions (typeface).withHeight (height));
}

// Value texts: the parameters' own texts are plain numbers with four decimals
using Format = std::function<juce::String (float)>;

const juce::String times = juce::String::fromUTF8 ("\xc3\x97");

Format percent (int decimals = 0)
{
    return [decimals] (float v) { return juce::String (v * 100.0f, decimals) + "%"; };
}

Format number (int decimals)
{
    return [decimals] (float v) { return juce::String (v, decimals); };
}

Format multiple()
{
    return [] (float v) { return times + juce::String (v, 2); };
}

Format rate()
{
    return [] (float v) { return juce::String (v, 2) + " Hz"; };
}

Format pan()
{
    return [] (float v)
    {
        if (std::abs (v) < 0.005f)
            return juce::String ("C");

        return juce::String (v < 0.0f ? "L" : "R") + juce::String (juce::roundToInt (std::abs (v) * 100.0f));
    };
}

juce::String hertz (float hz)
{
    return hz < 1000.0f ? juce::String (hz, hz < 100.0f ? 1 : 0) + " Hz" : juce::String (hz / 1000.0f, 1) + " kHz";
}

/** A dial in the template's style: a dark knob with a pointer inside a thin track, the value as a coloured arc
    (from the centre for bipolar parameters). Drags up and down, a double click returns to the default. The label
    shows the value while hovered or dragged. */
class Dial final : public juce::Component
{
public:
    Dial (juce::AudioProcessorValueTreeState& state, const juce::String& id, const juce::String& name,
          juce::Colour colour, Format formatValue, bool bipolar = false)
        : caption (name), accent (colour), centred (bipolar), format (std::move (formatValue)),
          attachment (state, id, slider)
    {
        const auto& parameter = *state.getParameter (id);
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

    void setCaption (const juce::String& text)
    {
        if (text != caption)
        {
            caption = text;
            repaint();
        }
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        labelArea = bounds.removeFromBottom (16);
        slider.setBounds (bounds.withSizeKeepingCentre (bounds.getHeight(), bounds.getHeight()));
    }

    void paint (juce::Graphics& g) override
    {
        const auto showValue = slider.isMouseOverOrDragging();
        g.setFont (font (getWidth() < 50 ? 10.5f : 11.5f));
        g.setColour (showValue ? colours::text : colours::label);
        g.drawText (showValue ? format ((float) slider.getValue()) : caption, labelArea,
                    juce::Justification::centred);
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
            const auto stroke = radius > 30.0f ? 3.5f : 2.5f;
            const auto ring = radius - stroke * 0.6f;
            const auto angle = startAngle + position * (endAngle - startAngle);

            const auto arc = [&] (float from, float to, juce::Colour colour)
            {
                juce::Path path;
                path.addCentredArc (centre.x, centre.y, ring, ring, 0.0f, from, to, true);
                g.setColour (colour);
                g.strokePath (path, juce::PathStrokeType (stroke, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
            };

            arc (startAngle, endAngle, colours::track);

            const auto origin = dial->centred ? (startAngle + endAngle) * 0.5f : startAngle;

            if (std::abs (angle - origin) > 0.01f)
                arc (juce::jmin (origin, angle), juce::jmax (origin, angle), dial->accent);

            // The knob: a soft vertical gradient, a dark rim, a light top edge
            const auto knob = radius - stroke - 3.5f;
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
            g.setColour (juce::Colour (0xffeeeeee));
            g.drawLine ({ centre + direction * (knob * 0.25f), centre + direction * (knob * 0.85f) },
                        radius > 30.0f ? 3.0f : 2.0f);
        }

        Dial* dial = nullptr;
    };

    juce::Slider slider;
    juce::String caption;
    juce::Colour accent;
    bool centred;
    Format format;
    Look look;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
    juce::Rectangle<int> labelArea;
};

/** A vertical fader: a thin track filled from the bottom in the accent colour, a light cap. Drags, a double
    click returns to the default, the label shows the value while hovered or dragged. */
class Fader final : public juce::Component
{
public:
    Fader (juce::AudioProcessorValueTreeState& state, const juce::String& id, const juce::String& name,
           juce::Colour colour, Format formatValue)
        : caption (name), format (std::move (formatValue)), attachment (state, id, slider)
    {
        const auto& parameter = *state.getParameter (id);
        look.accent = colour;
        slider.setSliderStyle (juce::Slider::LinearVertical);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        slider.setDoubleClickReturnValue (true, (double) state.getParameterRange (id).convertFrom0to1 (
                                                    parameter.getDefaultValue()));
        slider.setLookAndFeel (&look);
        slider.addMouseListener (this, false);
        slider.onValueChange = [this] { repaint(); };
        addAndMakeVisible (slider);
    }

    ~Fader() override { slider.setLookAndFeel (nullptr); }

    void resized() override
    {
        auto bounds = getLocalBounds();
        labelArea = bounds.removeFromBottom (16);
        bounds.removeFromBottom (4);
        slider.setBounds (bounds.withSizeKeepingCentre (24, bounds.getHeight()));
    }

    void paint (juce::Graphics& g) override
    {
        const auto showValue = slider.isMouseOverOrDragging();
        g.setFont (font (11.5f));
        g.setColour (showValue ? colours::text : colours::label);
        g.drawText (showValue ? format ((float) slider.getValue()) : caption, labelArea,
                    juce::Justification::centred);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseUp (const juce::MouseEvent&) override { repaint(); }

private:
    struct Look final : juce::LookAndFeel_V4
    {
        int getSliderThumbRadius (juce::Slider&) override { return 5; }

        void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float position, float,
                               float, juce::Slider::SliderStyle, juce::Slider&) override
        {
            const auto centre = (float) x + (float) width * 0.5f;
            const auto top = (float) y, bottom = (float) (y + height);
            g.setColour (colours::track);
            g.fillRoundedRectangle (centre - 1.5f, top, 3.0f, bottom - top, 1.5f);
            g.setColour (accent);
            g.fillRoundedRectangle (centre - 1.5f, position, 3.0f, bottom - position, 1.5f);

            const auto cap = juce::Rectangle<float> (16.0f, 7.0f).withCentre ({ centre, position });
            g.setColour (juce::Colour (0xffd4d4d4));
            g.fillRoundedRectangle (cap, 2.0f);
            g.setColour (juce::Colour (0xff141414));
            g.drawRoundedRectangle (cap, 2.0f, 1.0f);
        }

        juce::Colour accent;
    };

    juce::Slider slider;
    juce::String caption;
    Format format;
    Look look;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
    juce::Rectangle<int> labelArea;
};

/** A choice as underlined text: a click steps to the next choice. */
class Choice final : public juce::Component, private juce::AudioProcessorValueTreeState::Listener,
                     private juce::AsyncUpdater
{
public:
    Choice (juce::AudioProcessorValueTreeState& s, const juce::String& parameterId)
        : state (s), id (parameterId),
          parameter (*dynamic_cast<juce::AudioParameterChoice*> (s.getParameter (parameterId)))
    {
        state.addParameterListener (id, this);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    ~Choice() override { state.removeParameterListener (id, this); }

    void paint (juce::Graphics& g) override
    {
        const auto name = parameter.getCurrentChoiceName();
        const auto typeface = font (12.5f);
        const auto width = juce::GlyphArrangement::getStringWidth (typeface, name);
        const auto area = getLocalBounds().toFloat();
        g.setFont (typeface);
        g.setColour (isMouseOver() ? colours::text : colours::label);
        g.drawText (name, getLocalBounds(), juce::Justification::centredRight);
        g.fillRect (juce::Rectangle<float> (area.getRight() - width, area.getCentreY() + 8.0f, width, 1.0f));
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! getLocalBounds().contains (event.getPosition()))
            return;

        const auto next = (parameter.getIndex() + 1) % parameter.choices.size();
        parameter.beginChangeGesture();
        parameter.setValueNotifyingHost (parameter.convertTo0to1 ((float) next));
        parameter.endChangeGesture();
    }

private:
    void parameterChanged (const juce::String&, float) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { repaint(); }

    juce::AudioProcessorValueTreeState& state;
    juce::String id;
    juce::AudioParameterChoice& parameter;
};

/** A button as a rounded pill. */
class Pill final : public juce::Component
{
public:
    void setText (const juce::String& newText)
    {
        if (newText != text)
        {
            text = newText;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (isMouseOver() ? colours::pillHover : colours::pill);
        g.fillRoundedRectangle (bounds, bounds.getHeight() * 0.5f);
        g.setColour (isMouseOver() ? colours::text : colours::label);
        g.setFont (font (11.5f));
        g.drawText (text, getLocalBounds(), juce::Justification::centred);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (getLocalBounds().contains (event.getPosition()) && onClick)
            onClick();
    }

    std::function<void()> onClick;

private:
    juce::String text;
};

/** The hum as a waveform: each harmonic's sine in grey, stacked like a wavetable's frames, their sum in violet
    over them. The levels move with the fluctuation (Flc, Spd) as the processor runs it. */
class HumView final : public juce::Component
{
public:
    explicit HumView (juce::AudioProcessorValueTreeState& state)
    {
        for (int i = 0; i < 6; ++i)
        {
            const auto n = juce::String (i + 1);
            level[(size_t) i] = state.getRawParameterValue ("h" + n);
            amount[(size_t) i] = state.getRawParameterValue ("h" + n + "RandAmt");
            speed[(size_t) i] = state.getRawParameterValue ("h" + n + "RandSpd");
        }

        setInterceptsMouseClicks (false, false);
    }

    /** The processor's harmonic LFOs (rates and depth as in processBlock), advanced by a frame. */
    void advance (double seconds)
    {
        static constexpr double rates[] { 1.0, 1.13, 0.87, 1.21, 0.93, 1.07 };
        auto changed = false;

        for (size_t i = 0; i < 6; ++i)
        {
            auto gain = 1.0f;
            const auto depth = amount[i]->load();

            if (depth > 0.001f)
            {
                phase[i] = std::fmod (phase[i] + juce::MathConstants<double>::twoPi * speed[i]->load() * 0.1
                                                     * rates[i] * seconds,
                                      juce::MathConstants<double>::twoPi);
                gain = std::pow (10.0f, (float) std::sin (phase[i]) * depth * 6.0f / 20.0f);
            }

            const auto value = level[i]->load() * gain;

            if (value != shown[i])
            {
                shown[i] = value;
                changed = true;
            }
        }

        if (changed)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto steps = juce::roundToInt (bounds.getWidth());
        const auto spacing = bounds.getHeight() / 7.0f;
        const auto cycles = 2.0f;

        const auto wave = [&] (const std::function<float (float)>& y)
        {
            juce::Path path;

            for (int step = 0; step <= steps; ++step)
            {
                const auto x = bounds.getX() + (float) step;
                const auto t = juce::MathConstants<float>::twoPi * cycles * (float) step / (float) steps;
                step == 0 ? path.startNewSubPath (x, y (t)) : path.lineTo (x, y (t));
            }

            return path;
        };

        for (size_t i = 0; i < 6; ++i)
        {
            const auto baseline = bounds.getY() + spacing * (float) (i + 1);
            const auto amplitude = spacing * 0.9f * juce::jmin (1.0f, shown[i]);
            const auto harmonic = (float) (i + 1);
            g.setColour (juce::Colour (0xff8a8a8a).withAlpha (0.45f));
            g.strokePath (wave ([&] (float t) { return baseline - amplitude * std::sin (harmonic * t); }),
                          juce::PathStrokeType (1.0f));
        }

        auto total = 0.0f;

        for (auto value : shown)
            total += value;

        const auto scale = bounds.getHeight() * 0.42f / juce::jmax (total, 0.001f);
        g.setColour (colours::violet);
        g.strokePath (wave ([&] (float t)
                            {
                                auto sum = 0.0f;

                                for (size_t i = 0; i < 6; ++i)
                                    sum += shown[i] * std::sin ((float) (i + 1) * t);

                                return bounds.getCentreY() - sum * scale;
                            }),
                      juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    std::array<std::atomic<float>*, 6> level {}, amount {}, speed {};
    std::array<double, 6> phase {};
    std::array<float, 6> shown { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
};

/** The noise EQ: the three bells' response over a log frequency axis, filled below, a node per band at its
    frequency and gain. Dragging a node moves the band, a double click resets it. */
class EqView final : public juce::Component
{
public:
    explicit EqView (juce::AudioProcessorValueTreeState& state)
    {
        const auto band = [&state] (const char* name, const char* freq, const char* gain, const char* q)
        {
            return Band { name, state.getParameter (freq), state.getParameter (gain), state.getRawParameterValue (q) };
        };

        bands = { band ("Low", Processor::PARAM_EQ_FREQ_1, Processor::PARAM_EQ_GAIN_1, Processor::PARAM_EQ_Q_1),
                  band ("Mid", Processor::PARAM_EQ_FREQ_2, Processor::PARAM_EQ_GAIN_2, Processor::PARAM_EQ_Q_2),
                  band ("High", Processor::PARAM_EQ_FREQ_3, Processor::PARAM_EQ_GAIN_3, Processor::PARAM_EQ_Q_3) };
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();

        g.setFont (font (9.5f, Face::regular));

        for (auto hz : { 100.0f, 1000.0f, 10000.0f })
        {
            const auto x = xFor (hz);
            g.setColour (colours::grid);
            g.drawVerticalLine (juce::roundToInt (x), bounds.getY(), bounds.getBottom());
            g.setColour (colours::caption);
            g.drawText (hz < 1000.0f ? "100" : hz < 10000.0f ? "1k" : "10k",
                        juce::Rectangle<float> (x + 3.0f, bounds.getBottom() - 13.0f, 30.0f, 12.0f),
                        juce::Justification::centredLeft);
        }

        g.setColour (colours::grid);

        for (auto db : { -12.0f, 12.0f })
            g.drawHorizontalLine (juce::roundToInt (yFor (db)), bounds.getX(), bounds.getRight());

        g.setColour (colours::track);
        g.drawHorizontalLine (juce::roundToInt (yFor (0.0f)), bounds.getX(), bounds.getRight());

        // The response
        std::array<juce::dsp::IIR::Coefficients<float>::Ptr, 3> filters;

        for (size_t i = 0; i < 3; ++i)
            filters[i] = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
                rate, value (*bands[i].freq), bands[i].q->load(), juce::Decibels::decibelsToGain (value (*bands[i].gain)));

        juce::Path curve;

        for (int x = 0; x <= getWidth(); ++x)
        {
            auto db = 0.0f;

            for (auto& filter : filters)
                db += juce::Decibels::gainToDecibels ((float) filter->getMagnitudeForFrequency (hzFor ((float) x), rate));

            const auto y = juce::jlimit (bounds.getY(), bounds.getBottom(), yFor (db));
            x == 0 ? curve.startNewSubPath ((float) x, y) : curve.lineTo ((float) x, y);
        }

        auto fill = curve;
        fill.lineTo (bounds.getRight(), bounds.getBottom());
        fill.lineTo (bounds.getX(), bounds.getBottom());
        fill.closeSubPath();
        g.setGradientFill (juce::ColourGradient (colours::blue.withAlpha (0.45f), 0.0f, bounds.getY(),
                                                 colours::blue.withAlpha (0.03f), 0.0f, bounds.getBottom(), false));
        g.fillPath (fill);
        g.setColour (colours::blue);
        g.strokePath (curve, juce::PathStrokeType (1.5f));

        // The nodes, the active one light with its values
        const auto active = dragged >= 0 ? dragged : hovered;

        for (int i = 0; i < 3; ++i)
        {
            const auto centre = nodeFor (i);
            const auto node = juce::Rectangle<float> (9.0f, 9.0f).withCentre (centre);
            g.setColour (i == active ? juce::Colours::white : colours::blue);
            g.fillEllipse (node);
            g.setColour (colours::panel);
            g.drawEllipse (node, 1.0f);
        }

        if (active >= 0)
        {
            const auto& band = bands[(size_t) active];
            g.setFont (font (10.5f));
            g.setColour (colours::text);
            g.drawText (juce::String (band.name) + "   " + hertz (value (*band.freq)) + "   "
                            + juce::String (value (*band.gain), 1) + " dB",
                        bounds.reduced (5.0f, 3.0f).removeFromTop (14.0f), juce::Justification::centredLeft);
        }
    }

    void mouseMove (const juce::MouseEvent& event) override { setHovered (nodeAt (event.position)); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (-1); }

    void mouseDown (const juce::MouseEvent& event) override
    {
        dragged = nodeAt (event.position);

        if (dragged < 0)
            return;

        const auto& band = bands[(size_t) dragged];
        band.freq->beginChangeGesture();
        band.gain->beginChangeGesture();
        offset = nodeFor (dragged) - event.position;
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (dragged < 0)
            return;

        const auto& band = bands[(size_t) dragged];
        const auto position = event.position + offset;
        set (*band.freq, juce::jlimit (20.0f, 20000.0f, hzFor (position.x)));
        set (*band.gain, juce::jlimit (-18.0f, 18.0f, dbFor (position.y)));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragged < 0)
            return;

        const auto& band = bands[(size_t) dragged];
        band.freq->endChangeGesture();
        band.gain->endChangeGesture();
        dragged = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        const auto index = nodeAt (event.position);

        if (index < 0)
            return;

        for (auto* parameter : { bands[(size_t) index].freq, bands[(size_t) index].gain })
        {
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost (parameter->getDefaultValue());
            parameter->endChangeGesture();
        }
    }

private:
    struct Band
    {
        const char* name;
        juce::RangedAudioParameter* freq;
        juce::RangedAudioParameter* gain;
        std::atomic<float>* q;
    };

    static constexpr double rate = 48000.0;
    static constexpr float range = 24.0f; // dB above and below the centre line

    static float value (juce::RangedAudioParameter& parameter)
    {
        return parameter.convertFrom0to1 (parameter.getValue());
    }

    static void set (juce::RangedAudioParameter& parameter, float plain)
    {
        parameter.setValueNotifyingHost (parameter.convertTo0to1 (plain));
    }

    // 20 Hz to 20 kHz and the dB range inside a margin, so that nodes at the ends stay whole
    static constexpr float margin = 6.0f;
    float innerWidth() const { return (float) getWidth() - 2.0f * margin; }
    float innerHeight() const { return (float) getHeight() - 2.0f * margin; }
    float xFor (float hz) const { return margin + innerWidth() * std::log (hz / 20.0f) / std::log (1000.0f); }
    float hzFor (float x) const { return 20.0f * std::pow (1000.0f, (x - margin) / innerWidth()); }
    float yFor (float db) const { return margin + innerHeight() * (0.5f - db / (2.0f * range)); }
    float dbFor (float y) const { return (0.5f - (y - margin) / innerHeight()) * 2.0f * range; }

    juce::Point<float> nodeFor (int index) const
    {
        const auto& band = bands[(size_t) index];
        return { xFor (value (*band.freq)), yFor (value (*band.gain)) };
    }

    int nodeAt (juce::Point<float> position) const
    {
        auto nearest = -1;
        auto best = 12.0f * 12.0f;

        for (int i = 0; i < 3; ++i)
        {
            const auto distance = nodeFor (i).getDistanceSquaredFrom (position);

            if (distance < best)
            {
                best = distance;
                nearest = i;
            }
        }

        return nearest;
    }

    void setHovered (int index)
    {
        if (index != hovered)
        {
            hovered = index;
            setMouseCursor (index >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    std::array<Band, 3> bands {};
    int hovered = -1, dragged = -1;
    juce::Point<float> offset;
};

/** The motor: the delay the input drift and wobble modulate, over four seconds, as the processor computes it
    (clamped to its 50 ms buffer). Flat and grey when both are off: the processor skips the delay then. */
class MotorView final : public juce::Component
{
public:
    explicit MotorView (juce::AudioProcessorValueTreeState& state)
        : drift (state.getRawParameterValue (Processor::PARAM_INPUT_DRIFT)),
          driftRate (state.getRawParameterValue (Processor::PARAM_INPUT_DRIFT_RATE)),
          driftDepth (state.getRawParameterValue (Processor::PARAM_INPUT_DRIFT_DEPTH)),
          wobble (state.getRawParameterValue (Processor::PARAM_INPUT_WOBBLE)),
          wobbleRate (state.getRawParameterValue (Processor::PARAM_INPUT_WOBBLE_RATE)),
          wobbleDepth (state.getRawParameterValue (Processor::PARAM_INPUT_WOBBLE_DEPTH)),
          base (state.getRawParameterValue (Processor::PARAM_INPUT_DELAY_BASE)),
          mod (state.getRawParameterValue (Processor::PARAM_INPUT_DELAY_MOD))
    {
        setInterceptsMouseClicks (false, false);
    }

    void paint (juce::Graphics& g) override
    {
        static constexpr double seconds = 4.0;
        static constexpr float buffer = 2400.0f; // samples, 50 ms at 48 kHz

        const auto bounds = getLocalBounds().toFloat();
        const auto active = drift->load() > 0.001f || wobble->load() > 0.001f;

        g.setColour (colours::grid);

        for (int second = 1; second < (int) seconds; ++second)
            g.drawVerticalLine (juce::roundToInt (bounds.getWidth() * (float) second / (float) seconds),
                                bounds.getY(), bounds.getBottom());

        g.drawHorizontalLine (juce::roundToInt (bounds.getCentreY()), bounds.getX(), bounds.getRight());

        const auto yFor = [&] (float delay) { return bounds.getBottom() - 2.0f - (bounds.getHeight() - 4.0f) * delay / buffer; };
        juce::Path curve;

        for (int x = 0; x <= getWidth(); ++x)
        {
            const auto t = seconds * x / getWidth();
            auto delay = base->load();

            if (active)
            {
                const auto lfo = std::sin (juce::MathConstants<double>::twoPi * driftRate->load() * t) * drift->load() * driftDepth->load()
                               + std::sin (juce::MathConstants<double>::twoPi * wobbleRate->load() * t) * wobble->load() * wobbleDepth->load();
                delay = juce::jlimit (2.0f, buffer - 2.0f, (float) (base->load() + lfo * mod->load()));
            }

            x == 0 ? curve.startNewSubPath (0.0f, yFor (delay)) : curve.lineTo ((float) x, yFor (delay));
        }

        g.setColour (active ? colours::teal : colours::caption);
        g.strokePath (curve, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (! active)
        {
            g.setColour (colours::caption);
            g.setFont (font (10.5f));
            g.drawText ("Off", bounds.reduced (5.0f, 3.0f).removeFromTop (14.0f), juce::Justification::centredRight);
        }
    }

private:
    std::atomic<float>* drift, * driftRate, * driftDepth, * wobble, * wobbleRate, * wobbleDepth, * base, * mod;
};

class Editor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit Editor (Processor& p)
        : AudioProcessorEditor (p), processor (p), state (p.apvts),
          humView (state), eqView (state), motorView (state),
          baseFrequency (state, Processor::PARAM_FREQ_MODE),
          humLevel (state, Processor::PARAM_HUM_VOLUME, "Level", colours::violet, percent()),
          humDrift (state, Processor::PARAM_HUM_DRIFT, "Drift", colours::violet, multiple()),
          noiseLevel (state, Processor::PARAM_NOISE_VOLUME, "Level", colours::blue, percent (1)),
          humPan (state, Processor::PARAM_HUM_PAN, "Pan", colours::violet, pan(), true),
          humStereo (state, Processor::PARAM_HUM_STEREO, "Stereo", colours::violet, percent()),
          humDriftRate (state, Processor::PARAM_HUM_DRIFT_RATE, "Rate", colours::violet, rate()),
          humDriftDepth (state, Processor::PARAM_HUM_DRIFT_DEPTH, "Depth", colours::violet, percent (2)),
          noisePan (state, Processor::PARAM_NOISE_PAN, "Pan", colours::blue, pan(), true),
          noiseStereo (state, Processor::PARAM_NOISE_STEREO, "Stereo", colours::blue, percent()),
          saturation (state, Processor::PARAM_SATURATION, "Saturation", colours::orange,
                      [] (float v) { return v <= 1.001f ? juce::String ("Off") : times + juce::String (v, 2); }),
          tapeMix (state, Processor::PARAM_INPUT_VOLUME, "Tape Mix", colours::orange, percent()),
          humNoise (state, Processor::PARAM_MIX, "Hum + Noise", colours::orange, percent (1)),
          inputDrift (state, Processor::PARAM_INPUT_DRIFT, "Drift", colours::teal, multiple()),
          inputDriftRate (state, Processor::PARAM_INPUT_DRIFT_RATE, "Rate", colours::teal, rate()),
          inputDriftDepth (state, Processor::PARAM_INPUT_DRIFT_DEPTH, "Depth", colours::teal, number (3)),
          delayBase (state, Processor::PARAM_INPUT_DELAY_BASE, "Base", colours::teal,
                     [] (float v) { return juce::String (v, 1) + " smp"; }),
          wobble (state, Processor::PARAM_INPUT_WOBBLE, "Wobble", colours::teal, multiple()),
          wobbleRate (state, Processor::PARAM_INPUT_WOBBLE_RATE, "Rate", colours::teal, rate()),
          wobbleDepth (state, Processor::PARAM_INPUT_WOBBLE_DEPTH, "Depth", colours::teal, number (3)),
          delayMod (state, Processor::PARAM_INPUT_DELAY_MOD, "Mod", colours::teal, number (0))
    {
        for (int i = 0; i < 6; ++i)
        {
            const auto n = juce::String (i + 1);
            harmonics.add (new Dial (state, "h" + n, {}, colours::violet, percent (1)));
            fluctuation.add (new Dial (state, "h" + n + "RandAmt", "Flc", colours::pink, percent()));
            speed.add (new Dial (state, "h" + n + "RandSpd", "Spd", colours::pink, number (2)));
        }

        const char* qs[] { Processor::PARAM_EQ_Q_1, Processor::PARAM_EQ_Q_2, Processor::PARAM_EQ_Q_3 };
        const char* names[] { "Low Q", "Mid Q", "High Q" };

        for (int i = 0; i < 3; ++i)
            q.add (new Dial (state, qs[i], names[i], colours::blue, number (2)));

        randomize.onClick = [this]
        {
            // Rand Tone sets these, or returns them to their defaults (randomizeHarmonics)
            static const char* const ids[] { "h1", "h2", "h3", "h4", "h5", "h6", "eqFreq1", "eqGain1",
                                             "eqFreq2", "eqGain2", "eqFreq3", "eqGain3" };

            for (auto* id : ids)
                state.getParameter (id)->beginChangeGesture();

            processor.randomizeHarmonics();

            for (auto* id : ids)
                state.getParameter (id)->endChangeGesture();
        };

        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &humView, &eqView, &motorView, &baseFrequency, &randomize, &humLevel, &humDrift, &noiseLevel,
                 &humPan, &humStereo, &humDriftRate, &humDriftDepth, &noisePan, &noiseStereo, &saturation,
                 &tapeMix, &humNoise, &inputDrift, &inputDriftRate, &inputDriftDepth, &delayBase, &wobble,
                 &wobbleRate, &wobbleDepth, &delayMod })
            addAndMakeVisible (c);

        for (auto* group : { &harmonics, &fluctuation, &speed, &q })
            for (auto* dial : *group)
                addAndMakeVisible (dial);

        for (auto* parameter : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                watched.push_back (state.getRawParameterValue (ranged->getParameterID()));

        refresh();
        setSize (940, 530);
        startTimerHz (30);
    }

    ~Editor() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::background);

        g.setColour (colours::header);
        g.fillRect (headerArea);
        g.setFont (font (16.0f).withExtraKerningFactor (0.08f));
        g.setColour (juce::Colour (0xff9a9a9a));
        g.drawText ("TAPELECTRIC", headerArea.withTrimmedLeft (14), juce::Justification::centredLeft);
        g.setFont (font (12.0f, Face::regular));
        g.setColour (colours::caption);
        g.drawText ("Tape & Power Hum", headerArea.withTrimmedLeft (148), juce::Justification::centredLeft);

        for (const auto& [area, title] : panels)
        {
            g.setColour (colours::panel);
            g.fillRect (area);
            g.setColour (colours::label);
            g.setFont (font (12.5f, Face::italic));
            g.drawText (title, area.reduced (10, 0).removeFromTop (28), juce::Justification::centredLeft);
        }

        // Resonances: a numbered badge over each harmonic, a bracket under each Flc and Spd pair
        for (int i = 0; i < 6; ++i)
        {
            const auto badge = juce::Rectangle<float> (16.0f, 16.0f).withCentre (
                harmonics[i]->getBounds().toFloat().getCentre().withY ((float) harmonics[i]->getY() - 12.0f));
            g.setColour (colours::violet.withAlpha (0.85f));
            g.fillEllipse (badge);
            g.setColour (colours::header);
            g.setFont (font (10.5f));
            g.drawText (juce::String (i + 1), badge, juce::Justification::centred);

            const auto left = (float) fluctuation[i]->getBounds().getCentreX();
            const auto right = (float) speed[i]->getBounds().getCentreX();
            const auto y = (float) fluctuation[i]->getBottom() + 5.0f;
            g.setColour (colours::track);
            g.drawLine (left, y, right, y, 1.0f);
            g.drawLine (left, y - 4.0f, left, y, 1.0f);
            g.drawLine (right, y - 4.0f, right, y, 1.0f);
        }
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        headerArea = bounds.removeFromTop (34);
        randomize.setBounds (headerArea.reduced (10, 6).removeFromRight (86));
        bounds.removeFromTop (2);

        auto top = bounds.removeFromTop (252);
        bounds.removeFromTop (2);
        auto bottom = bounds;

        const auto split = [] (juce::Rectangle<int>& row, int width)
        {
            const auto area = row.removeFromLeft (width);
            row.removeFromLeft (2);
            return area;
        };

        const auto hum = split (top, 448), noise = split (top, 178), tape = top;
        const auto resonances = split (bottom, 448), eq = split (bottom, 248), motor = bottom;
        panels = { { hum, "Hum" }, { noise, "Noise" }, { tape, "Tape" },
                   { resonances, "Resonances" }, { eq, "Noise EQ" }, { motor, "Motor" } };

        const auto place = [] (juce::Component& c, juce::Point<int> size, int x, int y)
        {
            c.setBounds (juce::Rectangle<int> (size.x, size.y).withCentre ({ x, y }));
        };

        const juce::Point<int> dial { 72, 70 }, small { 55, 56 }, mini { 36, 50 };

        // Hum: level fader, the waveform, drift fader; pan, stereo, drift rate and depth under the waveform
        baseFrequency.setBounds (hum.getRight() - 90, hum.getY() + 3, 80, 22);
        humLevel.setBounds (hum.getX() + 6, hum.getY() + 32, 48, 144);
        humView.setBounds (hum.getX() + 60, hum.getY() + 36, 328, 120);
        humDrift.setBounds (hum.getX() + 394, hum.getY() + 32, 48, 144);

        juce::Component* humDials[] { &humPan, &humStereo, &humDriftRate, &humDriftDepth };

        for (int i = 0; i < 4; ++i)
            place (*humDials[i], dial, hum.getX() + 60 + 41 + 82 * i, hum.getY() + 210);

        // Noise: level fader, pan and stereo
        noiseLevel.setBounds (noise.getX() + 16, noise.getY() + 32, 48, 212);
        place (noisePan, dial, noise.getX() + 118, noise.getY() + 88);
        place (noiseStereo, dial, noise.getX() + 118, noise.getY() + 200);

        // Tape: saturation large, tape mix and the hum and noise level under it
        place (saturation, { 124, 122 }, tape.getCentreX(), tape.getY() + 98);
        place (tapeMix, dial, tape.getX() + tape.getWidth() / 4, tape.getY() + 206);
        place (humNoise, dial, tape.getX() + tape.getWidth() * 3 / 4, tape.getY() + 206);

        // Resonances: per harmonic its level, fluctuation amount and speed
        for (int i = 0; i < 6; ++i)
        {
            const auto x = resonances.getX() + 8 + 36 + 72 * i;
            place (*harmonics[i], dial, x, resonances.getY() + 104);
            place (*fluctuation[i], mini, x - 17, resonances.getY() + 180);
            place (*speed[i], mini, x + 17, resonances.getY() + 180);
        }

        // Noise EQ: the response, a Q dial per band
        eqView.setBounds (eq.getX() + 10, eq.getY() + 32, 228, 118);

        for (int i = 0; i < 3; ++i)
            place (*q[i], dial, eq.getX() + 10 + 38 + 76 * i, eq.getY() + 194);

        // Motor: the modulated delay; drift and wobble with their rates and depths, the delay's base and depth
        motorView.setBounds (motor.getX() + 10, motor.getY() + 32, motor.getWidth() - 20, 62);
        juce::Component* motorDials[2][4] { { &inputDrift, &inputDriftRate, &inputDriftDepth, &delayBase },
                                            { &wobble, &wobbleRate, &wobbleDepth, &delayMod } };
        const auto column = (motor.getWidth() - 20) / 4;

        for (int row = 0; row < 2; ++row)
            for (int i = 0; i < 4; ++i)
                place (*motorDials[row][i], small, motor.getX() + 10 + column / 2 + column * i,
                       motor.getY() + 130 + 62 * row);
    }

private:
    void timerCallback() override
    {
        humView.advance (1.0 / 30.0);

        auto changed = false;

        for (size_t i = 0; i < watched.size(); ++i)
        {
            const auto value = watched[i]->load();

            if (i >= shown.size())
                shown.push_back (value - 1.0f);

            if (value != shown[i])
            {
                shown[i] = value;
                changed = true;
            }
        }

        if (changed)
            refresh();
    }

    /** What follows the parameters beside the controls themselves: the views, the harmonics' frequencies, Rand
        Tone or Restore, and the dimming of controls without effect (a rate without its depth, the delay without
        drift or wobble). */
    void refresh()
    {
        eqView.repaint();
        motorView.repaint();

        const auto base = state.getRawParameterValue (Processor::PARAM_FREQ_MODE)->load() < 0.5f ? 50 : 60;

        for (int i = 0; i < 6; ++i)
        {
            harmonics[i]->setCaption (juce::String (base * (i + 1)) + " Hz");
            dim (*speed[i], state.getRawParameterValue ("h" + juce::String (i + 1) + "RandAmt")->load() <= 0.001f);
        }

        auto* h1 = state.getParameter ("h1");
        randomize.setText (std::abs (h1->getValue() - h1->getDefaultValue()) > 0.001f ? "Restore" : "Rand Tone");

        const auto off = [this] (const char* id) { return state.getRawParameterValue (id)->load() <= 0.001f; };
        dim (humDriftRate, off (Processor::PARAM_HUM_DRIFT));
        dim (humDriftDepth, off (Processor::PARAM_HUM_DRIFT));
        dim (inputDriftRate, off (Processor::PARAM_INPUT_DRIFT));
        dim (inputDriftDepth, off (Processor::PARAM_INPUT_DRIFT));
        dim (wobbleRate, off (Processor::PARAM_INPUT_WOBBLE));
        dim (wobbleDepth, off (Processor::PARAM_INPUT_WOBBLE));
        dim (delayBase, off (Processor::PARAM_INPUT_DRIFT) && off (Processor::PARAM_INPUT_WOBBLE));
        dim (delayMod, off (Processor::PARAM_INPUT_DRIFT) && off (Processor::PARAM_INPUT_WOBBLE));
    }

    static void dim (juce::Component& c, bool dimmed) { c.setAlpha (dimmed ? 0.4f : 1.0f); }

    Processor& processor;
    juce::AudioProcessorValueTreeState& state;
    HumView humView;
    EqView eqView;
    MotorView motorView;
    Choice baseFrequency;
    Pill randomize;
    Fader humLevel, humDrift, noiseLevel;
    Dial humPan, humStereo, humDriftRate, humDriftDepth, noisePan, noiseStereo, saturation, tapeMix, humNoise;
    Dial inputDrift, inputDriftRate, inputDriftDepth, delayBase, wobble, wobbleRate, wobbleDepth, delayMod;
    juce::OwnedArray<Dial> harmonics, fluctuation, speed, q;
    std::vector<std::atomic<float>*> watched;
    std::vector<float> shown;
    juce::Rectangle<int> headerArea;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> panels;
};
} // namespace

juce::AudioProcessorEditor* createEditor (TapElectricAudioProcessor& processor)
{
    return new Editor (processor);
}
} // namespace tapelectric_webclap
