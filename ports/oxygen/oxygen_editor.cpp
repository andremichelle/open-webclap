#include "oxygen_editor.h"

#include <complex>

#include "WebFonts.h"

namespace oxygen_webclap
{
namespace
{
namespace colours
{
    const juce::Colour background { 0xff232323 }, header { 0xff1b1b1b }, panel { 0xff2c2c2c }, divider { 0xff191919 };
    const juce::Colour text { 0xffd8d8d8 }, label { 0xffa6a6a6 }, caption { 0xff7c7c7c }, grid { 0xff363636 };
    const juce::Colour track { 0xff3c3c3c };
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

// Value texts: the parameters' own texts are plain numbers
using Format = std::function<juce::String (float)>;

const juce::String minus = juce::String::fromUTF8 ("\xe2\x88\x92");

juce::String signedText (float v, int decimals)
{
    const auto text = juce::String (std::abs (v), decimals);
    return v < -0.0049f ? minus + text : v > 0.0049f ? "+" + text : text;
}

Format decibels (bool sign = false)
{
    return [sign] (float v) { return (sign ? signedText (v, 1) : (v < 0.0f ? minus : juce::String()) + juce::String (std::abs (v), 1)) + " dB"; };
}

Format milliseconds()
{
    return [] (float v) { return juce::String (v, v < 10.0f ? 1 : 0) + " ms"; };
}

Format ratio()
{
    return [] (float v) { return juce::String (v, 1) + ":1"; };
}

Format percent()
{
    return [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; };
}

Format linearGain()
{
    return [] (float v) { return v <= 0.0001f ? minus + juce::String::fromUTF8 ("\xe2\x88\x9e") : signedText (juce::Decibels::gainToDecibels (v), 1) + " dB"; };
}

juce::String hertz (float hz)
{
    return hz < 1000.0f ? juce::String (juce::roundToInt (hz)) + " Hz" : juce::String (hz / 1000.0f, hz < 10000.0f ? 1 : 0) + " kHz";
}

/** An RBJ bell's magnitude in dB at a frequency, at 48 kHz (what the views draw; the processor's own rate only
    moves the top octave). */
float bellDb (float hz, float centre, float q, float gainDb)
{
    constexpr double rate = 48000.0;
    const auto omega = juce::MathConstants<double>::twoPi * centre / rate;
    const auto alpha = std::sin (omega) / (2.0 * q);
    const auto a = std::pow (10.0, gainDb / 40.0);
    const auto cosine = std::cos (omega);
    const std::complex<double> z = std::polar (1.0, -juce::MathConstants<double>::twoPi * hz / rate);
    const auto numerator = (1.0 + alpha * a) + (-2.0 * cosine) * z + (1.0 - alpha * a) * z * z;
    const auto denominator = (1.0 + alpha / a) + (-2.0 * cosine) * z + (1.0 - alpha / a) * z * z;
    return (float) (20.0 * std::log10 (std::abs (numerator / denominator)));
}

/** A log frequency axis, 20 Hz to 20 kHz, inside a margin so that nodes at the ends stay whole. */
struct Axis
{
    float left, width;

    float xFor (float hz) const { return left + width * std::log (hz / 20.0f) / std::log (1000.0f); }
    float hzFor (float x) const { return 20.0f * std::pow (1000.0f, (x - left) / width); }
};

void drawGrid (juce::Graphics& g, juce::Rectangle<float> bounds, const Axis& axis)
{
    g.setFont (font (9.0f, Face::regular));

    for (auto hz : { 100.0f, 1000.0f, 10000.0f })
    {
        const auto x = axis.xFor (hz);
        g.setColour (colours::grid);
        g.drawVerticalLine (juce::roundToInt (x), bounds.getY(), bounds.getBottom());
        g.setColour (colours::caption);
        g.drawText (hz < 1000.0f ? "100" : hz < 10000.0f ? "1k" : "10k",
                    juce::Rectangle<float> (x + 3.0f, bounds.getBottom() - 12.0f, 30.0f, 11.0f),
                    juce::Justification::centredLeft);
    }
}

/** The analyzer's bands in dBFS, as the editor shows them (falling slower than they arrive). */
struct Spectrum
{
    std::array<float, Processor::spectrumBands> input, output;

    Spectrum() { input.fill (-90.0f); output.fill (-90.0f); }
};

/** The output's spectrum filled, the input's as a line, behind a view's curves. Tilted by 3 dB an octave around
    1 kHz, so that music reads level. */
void drawSpectrum (juce::Graphics& g, juce::Rectangle<float> area, const Axis& axis, const Spectrum& spectrum)
{
    const auto path = [&] (const std::array<float, Processor::spectrumBands>& bands)
    {
        juce::Path line;

        for (int band = 0; band < Processor::spectrumBands; ++band)
        {
            const auto hz = 20.0f * std::pow (1000.0f, ((float) band + 0.5f) / Processor::spectrumBands);
            const auto db = bands[(size_t) band] + 3.0f * std::log2 (hz / 1000.0f);
            const auto x = juce::jlimit (area.getX(), area.getRight(), axis.xFor (hz));
            const auto y = area.getBottom() - area.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 78.0f) / 78.0f);
            band == 0 ? line.startNewSubPath (area.getX(), y) : line.lineTo (x, y);
        }

        line.lineTo (area.getRight(), line.getCurrentPosition().y);
        return line;
    };

    auto output = path (spectrum.output);
    auto fill = output;
    fill.lineTo (area.getRight(), area.getBottom());
    fill.lineTo (area.getX(), area.getBottom());
    fill.closeSubPath();
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.13f), 0.0f, area.getY(),
                                             juce::Colours::white.withAlpha (0.03f), 0.0f, area.getBottom(), false));
    g.fillPath (fill);
    g.setColour (juce::Colours::white.withAlpha (0.22f));
    g.strokePath (output, juce::PathStrokeType (1.0f));
    g.setColour (juce::Colours::white.withAlpha (0.18f));
    g.strokePath (path (spectrum.input), juce::PathStrokeType (1.0f));
}

void gesture (juce::RangedAudioParameter& parameter, float normalised)
{
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost (normalised);
    parameter.endChangeGesture();
}

/** A dial in the template's style: a dark knob with a pointer inside a thin track, the value as a coloured arc
    (from the centre for bipolar parameters). Drags up and down, a double click returns to the default. The label
    shows the value while hovered or dragged. */
class Dial final : public juce::Component
{
public:
    Dial (juce::AudioProcessorValueTreeState& state, const juce::String& id, const juce::String& name,
          juce::Colour colour, Format formatValue, bool bipolar = false, int knobSize = 36)
        : caption (name), accent (colour), centred (bipolar), knob (knobSize), format (std::move (formatValue)),
          attachment (state, id, slider)
    {
        const auto& parameter = *state.getParameter (id);
        slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        slider.setMouseDragSensitivity (160);
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

    void resized() override
    {
        auto bounds = getLocalBounds();
        labelArea = bounds.removeFromBottom (14);
        const auto side = juce::jmin (knob, bounds.getWidth(), bounds.getHeight()); // the cell around it is the spacing
        slider.setBounds (bounds.withSizeKeepingCentre (side, side).withY (labelArea.getY() - side - 2));
    }

    void paint (juce::Graphics& g) override
    {
        const auto showValue = slider.isMouseOverOrDragging();
        g.setFont (font (10.5f));
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
            const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (1.5f);
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
            const auto knob = radius - stroke - 3.0f;
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
    int knob;
    Format format;
    Look look;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
    juce::Rectangle<int> labelArea;
};

/** A choice as text tabs, the current one light. */
class Choice final : public juce::Component
{
public:
    explicit Choice (juce::AudioParameterChoice& p) : parameter (p) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void paint (juce::Graphics& g) override
    {
        g.setFont (font (11.0f));
        auto bounds = getLocalBounds();

        for (int index = parameter.choices.size(); --index >= 0;)
        {
            const auto& name = parameter.choices[index];
            const auto width = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), name);
            const auto area = bounds.removeFromRight (juce::roundToInt (width) + 12);
            const auto current = index == parameter.getIndex();
            g.setColour (current ? colours::text : colours::caption);
            g.drawText (name, area, juce::Justification::centred);
        }
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        auto right = getWidth();
        const auto typeface = font (11.0f);

        for (int index = parameter.choices.size(); --index >= 0;)
        {
            right -= juce::roundToInt (juce::GlyphArrangement::getStringWidth (typeface, parameter.choices[index])) + 12;

            if (event.x >= right)
            {
                gesture (parameter, parameter.convertTo0to1 ((float) index));
                repaint();
                return;
            }
        }
    }

private:
    juce::AudioParameterChoice& parameter;
};

/** A vertical meter: a dark track filled in the accent colour, from the bottom (levels) or the top (reduction). */
void drawMeter (juce::Graphics& g, juce::Rectangle<float> area, float fraction, juce::Colour colour, bool fromTop,
                const juce::String& name)
{
    auto bar = area.removeFromTop (area.getHeight() - 14.0f);
    bar = bar.withSizeKeepingCentre (6.0f, bar.getHeight());
    g.setColour (colours::track);
    g.fillRoundedRectangle (bar, 2.0f);
    const auto height = bar.getHeight() * juce::jlimit (0.0f, 1.0f, fraction);
    g.setColour (colour);
    g.fillRoundedRectangle (fromTop ? bar.withHeight (height) : bar.withTrimmedTop (bar.getHeight() - height), 2.0f);
    g.setColour (colours::label);
    g.setFont (font (10.0f));
    g.drawText (name, area, juce::Justification::centredBottom);
}

//==============================================================================
/** The modules' pages share this: a title (italic), what each draws and lays out itself. */
class Page : public juce::Component
{
public:
    Page (juce::String pageTitle, juce::Colour pageAccent, const Spectrum& analyzer)
        : title (std::move (pageTitle)), accent (pageAccent), spectrum (analyzer) {}

    /** About 30 times a second: the meters (decaying), whether the spectrum changed, and anything that follows the
        parameters. */
    virtual void refresh (const Meters&, bool spectrumChanged) {}

    void paint (juce::Graphics& g) override
    {
        g.setColour (colours::label);
        g.setFont (font (12.0f, Face::italic));
        g.drawText (title, getLocalBounds().reduced (10, 0).removeFromTop (24), juce::Justification::centredLeft);
    }

    const juce::String title;
    const juce::Colour accent;
    const Spectrum& spectrum;

protected:
    juce::Rectangle<int> content() const { return getLocalBounds().withTrimmedTop (24).reduced (8, 0).withTrimmedBottom (6); }
};

/** Graphic EQ: the response of the 15 fixed bells, a node per band at its frequency and gain. Dragging a node up and
    down sets the band's gain, a double click resets it. */
class EqPage final : public Page
{
public:
    EqPage (juce::AudioProcessorValueTreeState& state, const Spectrum& analyzer) : Page ("Graphic EQ", colours::violet, analyzer)
    {
        for (int i = 0; i < bands; ++i)
            gains[(size_t) i] = state.getParameter (Processor::id (Processor::eq, "Gain" + juce::String (i)));
    }

    void refresh (const Meters&, bool spectrumChanged) override
    {
        auto changed = false;

        for (size_t i = 0; i < (size_t) bands; ++i)
            if (const auto v = gains[i]->getValue(); v != shown[i])
            {
                shown[i] = v;
                changed = true;
            }

        if (changed)
            curve.clear(); // built again when painted

        if (changed || spectrumChanged)
            repaint();
    }

    void resized() override { curve.clear(); }

    void paint (juce::Graphics& g) override
    {
        Page::paint (g);
        const auto area = content().toFloat();
        const auto axis = axisFor (area);
        drawSpectrum (g, area, axis, spectrum);
        drawGrid (g, area, axis);
        g.setColour (colours::track);
        g.drawHorizontalLine (juce::roundToInt (yFor (area, 0.0f)), area.getX(), area.getRight());

        if (curve.isEmpty()) // the 15 bells per pixel only when a gain or the size changed
        {
            for (auto x = area.getX(); x <= area.getRight(); x += 1.0f)
            {
                auto db = 0.0f;

                for (int i = 0; i < bands; ++i)
                    db += bellDb (axis.hzFor (x), oxygen::EqualizerModule::Frequencies[i],
                                  oxygen::EqualizerModule::BandQValues[(size_t) i], gainOf (i));

                const auto y = juce::jlimit (area.getY(), area.getBottom(), yFor (area, db));
                x == area.getX() ? curve.startNewSubPath (x, y) : curve.lineTo (x, y);
            }
        }

        // Filled between the curve and 0 dB only, so the spectrum stays visible under a flat EQ
        const auto zero = yFor (area, 0.0f);
        auto fill = curve;
        fill.lineTo (area.getRight(), zero);
        fill.lineTo (area.getX(), zero);
        fill.closeSubPath();
        g.setColour (accent.withAlpha (0.25f));
        g.fillPath (fill);
        g.setColour (accent);
        g.strokePath (curve, juce::PathStrokeType (1.5f));

        const auto active = dragged >= 0 ? dragged : hovered;

        for (int i = 0; i < bands; ++i)
        {
            const auto node = juce::Rectangle<float> (8.0f, 8.0f).withCentre (nodeFor (area, i));
            g.setColour (i == active ? juce::Colours::white : accent);
            g.fillEllipse (node);
            g.setColour (colours::panel);
            g.drawEllipse (node, 1.0f);
        }

        if (active >= 0)
        {
            g.setFont (font (10.5f));
            g.setColour (colours::text);
            g.drawText (hertz (oxygen::EqualizerModule::Frequencies[active]) + "   " + decibels (true) (gainOf (active)),
                        getLocalBounds().removeFromTop (24).withTrimmedRight (10), juce::Justification::centredRight);
        }
    }

    void mouseMove (const juce::MouseEvent& event) override { setHovered (nodeAt (event.position)); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (-1); }

    void mouseDown (const juce::MouseEvent& event) override
    {
        dragged = nodeAt (event.position);

        if (dragged >= 0)
        {
            gains[(size_t) dragged]->beginChangeGesture();
            offset = nodeFor (content().toFloat(), dragged).y - event.position.y;
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (dragged < 0)
            return;

        auto& gain = *gains[(size_t) dragged];
        const auto db = juce::jlimit (-12.0f, 12.0f, dbFor (content().toFloat(), event.position.y + offset));
        gain.setValueNotifyingHost (gain.convertTo0to1 (db));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragged >= 0)
            gains[(size_t) dragged]->endChangeGesture();

        dragged = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (const auto index = nodeAt (event.position); index >= 0)
            gesture (*gains[(size_t) index], gains[(size_t) index]->getDefaultValue());
    }

private:
    static constexpr int bands = oxygen::EqualizerModule::NumBands;
    static constexpr float range = 15.0f; // dB above and below the centre line

    static Axis axisFor (juce::Rectangle<float> area) { return { area.getX() + 6.0f, area.getWidth() - 12.0f }; }
    static float yFor (juce::Rectangle<float> area, float db) { return area.getY() + 5.0f + (area.getHeight() - 10.0f) * (0.5f - db / (2.0f * range)); }
    static float dbFor (juce::Rectangle<float> area, float y) { return (0.5f - (y - area.getY() - 5.0f) / (area.getHeight() - 10.0f)) * 2.0f * range; }

    float gainOf (int i) const { return gains[(size_t) i]->convertFrom0to1 (gains[(size_t) i]->getValue()); }

    juce::Point<float> nodeFor (juce::Rectangle<float> area, int i) const
    {
        return { axisFor (area).xFor (oxygen::EqualizerModule::Frequencies[i]), yFor (area, gainOf (i)) };
    }

    int nodeAt (juce::Point<float> position) const
    {
        auto nearest = -1;
        auto best = 9.0f * 9.0f;
        const auto area = content().toFloat();

        for (int i = 0; i < bands; ++i)
            if (const auto distance = nodeFor (area, i).getDistanceSquaredFrom (position); distance < best)
            {
                best = distance;
                nearest = i;
            }

        return nearest;
    }

    void setHovered (int index)
    {
        if (index != hovered)
        {
            hovered = index;
            setMouseCursor (index >= 0 ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    std::array<juce::RangedAudioParameter*, (size_t) bands> gains {};
    std::array<float, (size_t) bands> shown {};
    juce::Path curve;
    int hovered = -1, dragged = -1;
    float offset = 0.0f;
};

const char* const bandIds[] { "Low", "LowMid", "HighMid", "High" };
const char* const bandNames[] { "Low", "Low Mid", "High Mid", "High" };

/** Dynamic EQ: each band's range as a faint bell, the cut it applies now filled, threshold and range per band. */
class DynamicEqPage final : public Page
{
public:
    DynamicEqPage (juce::AudioProcessorValueTreeState& state, const Spectrum& analyzer) : Page ("Dynamic EQ", colours::teal, analyzer)
    {
        for (size_t i = 0; i < 4; ++i)
        {
            const auto band = juce::String (bandIds[i]);
            ranges[i] = state.getParameter (Processor::id (Processor::dynamicEq, band + "Range"));
            thresholds.add (new Dial (state, Processor::id (Processor::dynamicEq, band + "Thresh"),
                                      "Thresh", accent, decibels(), false, 30));
            rangeDials.add (new Dial (state, Processor::id (Processor::dynamicEq, band + "Range"),
                                      "Range", accent, decibels(), false, 30));
            addAndMakeVisible (thresholds.getLast());
            addAndMakeVisible (rangeDials.getLast());
        }
    }

    void refresh (const Meters& meters, bool spectrumChanged) override
    {
        auto changed = spectrumChanged;

        for (size_t i = 0; i < 4; ++i)
        {
            const auto range = ranges[i]->convertFrom0to1 (ranges[i]->getValue());

            if (range != shownRange[i] || meters.dynamicEq[i] != shownCut[i])
            {
                shownRange[i] = range;
                shownCut[i] = meters.dynamicEq[i];
                changed = true;
            }
        }

        if (changed)
            repaint (view.toNearestInt());
    }

    void resized() override
    {
        // The view takes what the dials and the frequencies over them leave
        auto area = content();
        auto dials = area.removeFromBottom (50);
        captions = area.removeFromBottom (16).toFloat();
        view = area.withTrimmedBottom (2).toFloat();
        const auto column = dials.getWidth() / 4;

        for (int i = 0; i < 4; ++i)
        {
            auto cell = dials.removeFromLeft (column);
            thresholds[i]->setBounds (cell.removeFromLeft (cell.getWidth() / 2));
            rangeDials[i]->setBounds (cell);
        }
    }

    void paint (juce::Graphics& g) override
    {
        Page::paint (g);
        const Axis axis { view.getX() + 4.0f, view.getWidth() - 8.0f };
        drawSpectrum (g, view, axis, spectrum);
        drawGrid (g, view, axis);

        // Up to 12 dB of cut over the view's height, from the top line down
        const auto yFor = [this] (float db) { return view.getY() + 2.0f - (view.getHeight() - 4.0f) * db / 12.0f; };
        const auto curve = [&] (const std::array<float, 4>& cuts)
        {
            juce::Path path;

            for (auto x = view.getX(); x <= view.getRight(); x += 1.0f)
            {
                auto db = 0.0f;

                for (size_t i = 0; i < 4; ++i)
                    if (cuts[i] > 0.0f)
                        db += bellDb (axis.hzFor (x), oxygen::DynamicEqModule::CenterFrequencies[i],
                                      oxygen::DynamicEqModule::BandQValues[i], -cuts[i]);

                const auto y = juce::jlimit (view.getY(), view.getBottom(), yFor (db));
                x == view.getX() ? path.startNewSubPath (x, y) : path.lineTo (x, y);
            }

            return path;
        };

        g.setColour (colours::track);
        g.drawHorizontalLine (juce::roundToInt (yFor (0.0f)), view.getX(), view.getRight());

        juce::Path rangeCurve = curve (shownRange), dashed;
        const float dashes[] { 3.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, rangeCurve, dashes, 2);
        g.setColour (accent.withAlpha (0.45f));
        g.fillPath (dashed);

        auto cut = curve (shownCut);
        auto fill = cut;
        fill.lineTo (view.getRight(), yFor (0.0f));
        fill.lineTo (view.getX(), yFor (0.0f));
        fill.closeSubPath();
        g.setColour (accent.withAlpha (0.35f));
        g.fillPath (fill);
        g.setColour (accent);
        g.strokePath (cut, juce::PathStrokeType (1.5f));

        // The bands' frequencies over their dials
        g.setFont (font (10.0f));
        const auto column = captions.getWidth() / 4.0f;

        for (size_t i = 0; i < 4; ++i)
        {
            const auto cell = captions.withX (captions.getX() + column * (float) i).withWidth (column);
            g.setColour (shownCut[i] > 0.05f ? colours::text : colours::caption);
            g.drawText (hertz (oxygen::DynamicEqModule::CenterFrequencies[i])
                            + (shownCut[i] > 0.05f ? "  " + minus + juce::String (shownCut[i], 1) : juce::String()),
                        cell, juce::Justification::centred);
        }
    }

private:
    std::array<juce::RangedAudioParameter*, 4> ranges {};
    std::array<float, 4> shownRange {}, shownCut {};
    juce::OwnedArray<Dial> thresholds, rangeDials;
    juce::Rectangle<float> view, captions;
};

/** The crossovers of the multiband compressor or the imager: the four bands over a log frequency axis, a line per
    crossover to drag. A band shows a bar (reduction or width) and can be picked with a click. */
class CrossoverView final : public juce::Component
{
public:
    CrossoverView (juce::AudioProcessorValueTreeState& state, const char* module, juce::Colour colour,
                   const Spectrum& analyzer)
        : accent (colour), spectrum (analyzer)
    {
        const char* ids[] { "LowMidX", "MidHighX", "HighX" };

        for (size_t i = 0; i < 3; ++i)
            crossovers[i] = state.getParameter (Processor::id (module, ids[i]));
    }

    /** Per band 0..1, drawn as a bar from the top (reduction) or the bottom (width). */
    void setBars (const std::array<float, 4>& values, bool fromTop)
    {
        if (values != bars || fromTop != barsFromTop)
        {
            bars = values;
            barsFromTop = fromTop;
            repaint();
        }
    }

    void setSelected (int band)
    {
        selected = band;
        repaint();
    }

    std::function<void (int band)> onSelect;

    bool crossoversChanged()
    {
        auto changed = false;

        for (size_t i = 0; i < 3; ++i)
            if (const auto v = crossovers[i]->getValue(); v != shown[i])
            {
                shown[i] = v;
                changed = true;
            }

        return changed;
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto axis = axisOf();
        drawSpectrum (g, bounds, axis, spectrum);
        drawGrid (g, bounds, axis);

        float edges[] { bounds.getX(), xOf (0), xOf (1), xOf (2), bounds.getRight() };

        for (int band = 0; band < 4; ++band)
        {
            const auto region = juce::Rectangle<float>::leftTopRightBottom (edges[band], bounds.getY(), edges[band + 1], bounds.getBottom());

            if (band == selected)
            {
                g.setColour (accent.withAlpha (0.1f));
                g.fillRect (region);
            }

            const auto height = (bounds.getHeight() - 14.0f) * juce::jlimit (0.0f, 1.0f, bars[(size_t) band]);
            const auto bar = region.reduced (4.0f, 0.0f).withTrimmedBottom (14.0f);
            g.setColour (accent.withAlpha (band == selected ? 0.6f : 0.35f));
            g.fillRect (barsFromTop ? bar.withHeight (height) : bar.withTrimmedTop (bar.getHeight() - height));
        }

        for (int i = 0; i < 3; ++i)
        {
            const auto active = i == (dragged >= 0 ? dragged : hovered);
            g.setColour (active ? juce::Colours::white : accent);
            g.fillRect (juce::Rectangle<float> (xOf (i) - 1.0f, bounds.getY(), 2.0f, bounds.getHeight()));

            if (active)
            {
                g.setFont (font (10.0f));
                const auto text = hertz (value (i));
                const auto width = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 8.0f;
                const auto left = juce::jlimit (bounds.getX(), bounds.getRight() - width, xOf (i) - width * 0.5f);
                g.setColour (colours::header);
                g.fillRoundedRectangle ({ left, bounds.getY() + 2.0f, width, 14.0f }, 3.0f);
                g.setColour (colours::text);
                g.drawText (text, juce::Rectangle<float> (left, bounds.getY() + 2.0f, width, 14.0f), juce::Justification::centred);
            }
        }
    }

    void mouseMove (const juce::MouseEvent& event) override { setHovered (lineAt (event.position.x)); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (-1); }

    void mouseDown (const juce::MouseEvent& event) override
    {
        dragged = lineAt (event.position.x);

        if (dragged >= 0)
        {
            crossovers[(size_t) dragged]->beginChangeGesture();
            return;
        }

        if (onSelect)
        {
            auto band = 0;

            while (band < 3 && event.position.x > xOf (band))
                ++band;

            onSelect (band);
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (dragged < 0)
            return;

        auto& crossover = *crossovers[(size_t) dragged];
        const auto range = crossover.getNormalisableRange();
        const auto hz = juce::jlimit (range.start, range.end, axisOf().hzFor (event.position.x));
        crossover.setValueNotifyingHost (crossover.convertTo0to1 (hz));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragged >= 0)
            crossovers[(size_t) dragged]->endChangeGesture();

        dragged = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (const auto index = lineAt (event.position.x); index >= 0)
            gesture (*crossovers[(size_t) index], crossovers[(size_t) index]->getDefaultValue());
    }

private:
    Axis axisOf() const { return { 0.0f, (float) getWidth() }; }
    float value (int i) const { return crossovers[(size_t) i]->convertFrom0to1 (crossovers[(size_t) i]->getValue()); }
    float xOf (int i) const { return axisOf().xFor (value (i)); }

    int lineAt (float x) const
    {
        for (int i = 0; i < 3; ++i)
            if (std::abs (x - xOf (i)) < 5.0f)
                return i;

        return -1;
    }

    void setHovered (int index)
    {
        if (index != hovered)
        {
            hovered = index;
            setMouseCursor (index >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                                       : onSelect ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    juce::Colour accent;
    const Spectrum& spectrum;
    std::array<juce::RangedAudioParameter*, 3> crossovers {};
    std::array<float, 3> shown {};
    std::array<float, 4> bars {};
    bool barsFromTop = true;
    int selected = -1, hovered = -1, dragged = -1;
};

/** Multiband compressor: the crossovers with each band's reduction, the selected band's five dials. */
class MultibandPage final : public Page
{
public:
    MultibandPage (juce::AudioProcessorValueTreeState& s, const Spectrum& analyzer)
        : Page ("Multiband", colours::orange, analyzer), state (s), view (s, Processor::multiband, accent, analyzer)
    {
        view.onSelect = [this] (int band) { select (band); };
        addAndMakeVisible (view);
        select (0);
    }

    void refresh (const Meters& meters, bool spectrumChanged) override
    {
        std::array<float, 4> reductions {};

        for (size_t i = 0; i < 4; ++i)
            reductions[i] = meters.multiband[i] / 12.0f;

        view.setBars (reductions, true);

        if (view.crossoversChanged() || spectrumChanged)
            view.repaint();
    }

    void resized() override
    {
        auto area = content();
        auto row = area.removeFromBottom (54);
        view.setBounds (area.withTrimmedBottom (4));
        const auto column = row.getWidth() / 5;

        for (auto* dial : dials)
            dial->setBounds (row.removeFromLeft (column));
    }

    void paint (juce::Graphics& g) override
    {
        Page::paint (g);
        g.setFont (font (11.0f));
        g.setColour (colours::text);
        g.drawText (bandNames[selected], getLocalBounds().removeFromTop (24).withTrimmedRight (10), juce::Justification::centredRight);
    }

private:
    void select (int band)
    {
        selected = band;
        view.setSelected (band);
        const auto prefix = juce::String (bandIds[band]);
        const auto id = [&prefix] (const char* name) { return Processor::id (Processor::multiband, prefix + name); };
        dials.clear();
        dials.add (new Dial (state, id ("Thresh"), "Thresh", accent, decibels()));
        dials.add (new Dial (state, id ("Ratio"), "Ratio", accent, ratio()));
        dials.add (new Dial (state, id ("Attack"), "Attack", accent, milliseconds()));
        dials.add (new Dial (state, id ("Release"), "Release", accent, milliseconds()));
        dials.add (new Dial (state, id ("Gain"), "Gain", accent, decibels (true), true));

        for (auto* dial : dials)
            addAndMakeVisible (dial);

        resized();
        repaint();
    }

    juce::AudioProcessorValueTreeState& state;
    CrossoverView view;
    juce::OwnedArray<Dial> dials;
    int selected = 0;
};

/** Stereo imager: the crossovers with each band's width, a width dial per band (100 % leaves it as it is). */
class ImagerPage final : public Page
{
public:
    ImagerPage (juce::AudioProcessorValueTreeState& state, const Spectrum& analyzer)
        : Page ("Stereo Imager", colours::pink, analyzer), view (state, Processor::imager, accent, analyzer)
    {
        addAndMakeVisible (view);

        for (size_t i = 0; i < 4; ++i)
        {
            widths[i] = state.getParameter (Processor::id (Processor::imager, juce::String (bandIds[i]) + "Width"));
            dials.add (new Dial (state, Processor::id (Processor::imager, juce::String (bandIds[i]) + "Width"),
                                 bandNames[i], accent, percent(), true));
            addAndMakeVisible (dials.getLast());
        }
    }

    void refresh (const Meters&, bool spectrumChanged) override
    {
        std::array<float, 4> bars {};

        for (size_t i = 0; i < 4; ++i)
            bars[i] = widths[i]->getValue(); // 0..200 %

        view.setBars (bars, false);

        if (view.crossoversChanged() || spectrumChanged)
            view.repaint();
    }

    void resized() override
    {
        auto area = content();
        auto row = area.removeFromBottom (54);
        view.setBounds (area.withTrimmedBottom (4));
        const auto column = row.getWidth() / 4;

        for (auto* dial : dials)
            dial->setBounds (row.removeFromLeft (column));
    }

private:
    CrossoverView view;
    std::array<juce::RangedAudioParameter*, 4> widths {};
    juce::OwnedArray<Dial> dials;
};

/** Maximizer: threshold, ceiling, release, the trim gain before it, the mode; input, reduction and output. */
class MaximizerPage final : public Page
{
public:
    MaximizerPage (juce::AudioProcessorValueTreeState& state, const Spectrum& analyzer)
        : Page ("Maximizer", colours::blue, analyzer),
          modeParameter (*dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (Processor::id (Processor::maximizer, "Mode")))),
          mode (modeParameter),
          threshold (state, Processor::id (Processor::maximizer, "Threshold"), "Threshold", accent, decibels(), false, 60),
          ceiling (state, Processor::id (Processor::maximizer, "Ceiling"), "Ceiling", accent, decibels()),
          release (state, Processor::id (Processor::maximizer, "Release"), "Release", accent, milliseconds()),
          trim (state, Processor::id (Processor::trim, "gain"), "Trim", colours::label, linearGain(), true)
    {
        for (juce::Component* c : std::initializer_list<juce::Component*> { &mode, &threshold, &ceiling, &release, &trim })
            addAndMakeVisible (c);
    }

    void refresh (const Meters& meters, bool) override
    {
        if (const auto index = modeParameter.getIndex(); index != shownMode) // changed here or by the host
        {
            shownMode = index;
            mode.repaint();
        }

        const auto level = [] (float gain) { return juce::jlimit (0.0f, 1.0f, 1.0f + juce::Decibels::gainToDecibels (gain, -48.0f) / 48.0f); };
        const std::array<float, 3> values { level (meters.input), meters.maximizer / 12.0f, level (meters.output) };

        if (values != shown)
        {
            shown = values;
            repaint (meterArea);
        }
    }

    void resized() override
    {
        mode.setBounds (getLocalBounds().removeFromTop (24).withTrimmedRight (6).removeFromRight (190));
        auto area = content();
        meterArea = area.removeFromRight (84).withTrimmedTop (8); // apart from the modes above
        area.removeFromRight (6);
        // Threshold large, the three small dials beside it, their labels on one line
        const auto bottom = area.getCentreY() + 40;
        threshold.setBounds (area.removeFromLeft (110).withTop (bottom - 80).withBottom (bottom));
        auto row = area.withTop (bottom - 56).withBottom (bottom);
        const auto column = row.getWidth() / 3;
        ceiling.setBounds (row.removeFromLeft (column));
        release.setBounds (row.removeFromLeft (column));
        trim.setBounds (row);
    }

    void paint (juce::Graphics& g) override
    {
        Page::paint (g);
        auto area = meterArea.toFloat();
        const auto column = area.getWidth() / 3.0f;
        drawMeter (g, area.removeFromLeft (column), shown[0], colours::label, false, "In");
        drawMeter (g, area.removeFromLeft (column), shown[1], accent, true, "GR");
        drawMeter (g, area, shown[2], colours::text, false, "Out");
    }

private:
    juce::AudioParameterChoice& modeParameter;
    Choice mode;
    int shownMode = -1;
    Dial threshold, ceiling, release, trim;
    juce::Rectangle<int> meterArea;
    std::array<float, 3> shown {};
};

//==============================================================================
/** The modules as tabs: a click on the name shows its page, on the dot switches its bypass. */
class Tabs final : public juce::Component
{
public:
    struct Tab
    {
        juce::String name;
        juce::Colour accent;
        juce::RangedAudioParameter* bypass;
    };

    explicit Tabs (std::vector<Tab> list) : tabs (std::move (list)) {}

    std::function<void (int)> onSelect;

    void setSelected (int index)
    {
        selected = index;
        repaint();
    }

    void refresh()
    {
        auto changed = false;

        for (size_t i = 0; i < tabs.size(); ++i)
            if (const auto on = tabs[i].bypass->getValue() < 0.5f; on != shownOn[i])
            {
                shownOn[i] = on;
                changed = true;
            }

        if (changed)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::header);
        const auto height = (float) getHeight() / (float) tabs.size();

        for (size_t i = 0; i < tabs.size(); ++i)
        {
            auto row = juce::Rectangle<float> (0.0f, height * (float) i, (float) getWidth(), height);
            const auto current = (int) i == selected;
            const auto on = shownOn[i];

            if (current)
            {
                g.setColour (colours::panel);
                g.fillRect (row);
                g.setColour (tabs[i].accent);
                g.fillRect (row.removeFromLeft (2.0f));
            }
            else
            {
                row.removeFromLeft (2.0f);
            }

            const auto dot = row.removeFromRight (22.0f).withSizeKeepingCentre (7.0f, 7.0f);
            g.setColour (on ? tabs[i].accent : colours::caption);
            on ? g.fillEllipse (dot) : g.drawEllipse (dot, 1.0f);

            g.setFont (font (11.5f));
            g.setColour (current ? (on ? colours::text : colours::label) : (on ? colours::label : colours::caption));
            g.drawText (tabs[i].name, row.withTrimmedLeft (10.0f), juce::Justification::centredLeft);
        }
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! getLocalBounds().contains (event.getPosition()))
            return;

        const auto index = juce::jlimit (0, (int) tabs.size() - 1, (int) (event.position.y * (float) tabs.size() / (float) getHeight()));

        if (event.position.x > (float) getWidth() - 24.0f)
        {
            auto& bypass = *tabs[(size_t) index].bypass;
            gesture (bypass, bypass.getValue() < 0.5f ? 1.0f : 0.0f);
        }
        else if (onSelect)
        {
            onSelect (index);
        }
    }

private:
    std::vector<Tab> tabs;
    std::array<bool, 5> shownOn { true, true, true, true, true };
    int selected = 0;
};

class Editor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit Editor (Processor& p)
        : AudioProcessorEditor (p), state (p.state),
          tabs ({ { "EQ", colours::violet, bypass (Processor::eq) },
                  { "Dyn EQ", colours::teal, bypass (Processor::dynamicEq) },
                  { "Multiband", colours::orange, bypass (Processor::multiband) },
                  { "Imager", colours::pink, bypass (Processor::imager) },
                  { "Maximizer", colours::blue, bypass (Processor::maximizer) } })
    {
        pages.add (new EqPage (state, spectrum));
        pages.add (new DynamicEqPage (state, spectrum));
        pages.add (new MultibandPage (state, spectrum));
        pages.add (new ImagerPage (state, spectrum));
        pages.add (new MaximizerPage (state, spectrum));

        for (auto* page : pages)
            addChildComponent (page);

        tabs.onSelect = [this] (int index) { show (index); };
        addAndMakeVisible (tabs);
        show (0);

        setSize (OXYGEN_EDITOR_WIDTH, OXYGEN_EDITOR_HEIGHT);
        startTimerHz (30);
    }

    ~Editor() override { stopTimer(); }

    /** The meters rise at once and fall about 20 dB a second. */
    void setMeters (const Meters& incoming)
    {
        const auto hold = [] (float& shown, float value, float fall) { shown = juce::jmax (value, shown * fall); };

        for (size_t i = 0; i < 4; ++i)
        {
            hold (meters.dynamicEq[i], incoming.dynamicEq[i], 0.85f);
            hold (meters.multiband[i], incoming.multiband[i], 0.85f);
        }

        hold (meters.maximizer, incoming.maximizer, 0.85f);
        hold (meters.input, incoming.input, 0.85f);
        hold (meters.output, incoming.output, 0.85f);
        received = true;
    }

    /** The spectrum rises at once and falls about 36 dB a second. */
    void setSpectrum (const std::uint8_t* bytes)
    {
        for (size_t band = 0; band < (size_t) Processor::spectrumBands; ++band)
        {
            const auto decode = [] (std::uint8_t byte) { return (float) byte * 90.0f / 255.0f - 90.0f; };
            spectrum.input[band] = juce::jmax (decode (bytes[band]), spectrum.input[band] - 1.2f);
            spectrum.output[band] = juce::jmax (decode (bytes[band + Processor::spectrumBands]), spectrum.output[band] - 1.2f);
        }

        spectrumReceived = spectrumChanged = true;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::panel);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        tabs.setBounds (bounds.removeFromLeft (96));

        for (auto* page : pages)
            page->setBounds (bounds);
    }

private:
    juce::RangedAudioParameter* bypass (const char* module) { return state.getParameter (Processor::id (module, "Bypass")); }

    void show (int index)
    {
        for (int i = 0; i < pages.size(); ++i)
            pages[i]->setVisible (i == index);

        tabs.setSelected (index);
        current = index;
    }

    void timerCallback() override
    {
        // Without frames (the DSP side sends none while nothing changes) the meters fall on their own
        if (! received)
            setMeters ({});

        received = false;

        const auto above = [] (const auto& bands) { return std::any_of (bands.begin(), bands.end(), [] (float db) { return db > -90.0f; }); };

        if (! spectrumReceived && (above (spectrum.input) || above (spectrum.output))) // no frames in silence: falls
        {
            std::array<std::uint8_t, 2 * Processor::spectrumBands> floor {};
            setSpectrum (floor.data());
        }

        spectrumReceived = false;
        tabs.refresh();
        pages[current]->refresh (meters, spectrumChanged);
        spectrumChanged = false;
    }

    juce::AudioProcessorValueTreeState& state;
    Tabs tabs;
    juce::OwnedArray<Page> pages;
    Meters meters;
    Spectrum spectrum;
    int current = 0;
    bool received = false, spectrumReceived = false, spectrumChanged = false;
};
} // namespace

juce::AudioProcessorEditor* createEditor (Processor& processor)
{
    return new Editor (processor);
}

void showSpectrum (juce::AudioProcessorEditor& editor, const std::uint8_t* bytes)
{
    if (auto* own = dynamic_cast<Editor*> (&editor))
        own->setSpectrum (bytes);
}

void showMeters (juce::AudioProcessorEditor& editor, const Meters& meters)
{
    if (auto* own = dynamic_cast<Editor*> (&editor))
        own->setMeters (meters);
}
} // namespace oxygen_webclap
