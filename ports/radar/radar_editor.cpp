#include "radar_editor.h"

#include "WebFonts.h"

namespace radar
{
namespace
{
namespace colours
{
    const juce::Colour background { 0xff232323 }, header { 0xff1b1b1b }, panel { 0xff2c2c2c }, divider { 0xff191919 };
    const juce::Colour text { 0xffd8d8d8 }, label { 0xffa6a6a6 }, caption { 0xff7c7c7c };
    const juce::Colour track { 0xff3c3c3c }, pill { 0xff383838 }, pillHover { 0xff444444 };
    const juce::Colour teal { 0xff38c7b0 }, violet { 0xffa77bff }, orange { 0xfff2a33a }, pink { 0xffff7aa8 };
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

using Format = std::function<juce::String (float)>;

const juce::String minus = juce::String::fromUTF8 ("\xe2\x88\x92");

Format milliseconds()
{
    // String (v, 0) is not "no decimals" but JUCE's default format: whole numbers go through roundToInt
    return [] (float v) { return v < 10.0f ? juce::String (v, 1) + " ms" : v < 1000.0f ? juce::String (juce::roundToInt (v)) + " ms" : juce::String (v / 1000.0f, 2) + " s"; };
}

Format semitones()
{
    return [] (float v) { return (v < -0.005f ? minus : v > 0.005f ? juce::String ("+") : juce::String()) + juce::String (std::abs (v), 2) + " st"; };
}

Format decibels()
{
    return [] (float v) { return v <= -47.9f ? minus + juce::String::fromUTF8 ("\xe2\x88\x9e") : (v < -0.05f ? minus : juce::String()) + juce::String (std::abs (v), 1) + " dB"; };
}

Format bars()
{
    return [] (float v) { const auto n = juce::roundToInt (v); return juce::String (n) + (n == 1 ? " bar" : " bars"); };
}

void gesture (juce::RangedAudioParameter& parameter, float normalised)
{
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost (normalised);
    parameter.endChangeGesture();
}

juce::RangedAudioParameter& parameterOf (Processor& processor, const char* id)
{
    return *processor.state.getParameter (id);
}

int patternIndexOf (Processor& processor)
{
    return juce::jlimit (0, numPatterns - 1, juce::roundToInt (processor.state.getRawParameterValue ("pattern")->load()) - 1);
}

/** A control of one parameter: the editors tell the host which parameter is under the mouse
    (getControlParameterIndex), which offers its menu there (openDAW: automation, MIDI learn). */
struct ParameterControl
{
    virtual ~ParameterControl() = default;
    virtual juce::AudioProcessorParameter& controlledParameter() = 0;
};

/** The parameter of the component or of the control it is part of, -1 for none. */
int controlParameterIndex (juce::Component& component)
{
    for (auto* c = &component; c != nullptr; c = c->getParentComponent())
        if (auto* control = dynamic_cast<ParameterControl*> (c))
            return control->controlledParameter().getParameterIndex();

    return -1;
}

//==============================================================================
/** A dial in the template's style: a dark knob with a pointer inside a thin track, the value as a coloured arc
    (from the centre for bipolar parameters). Drags up and down, a double click returns to the default. The label
    shows the value while hovered or dragged. */
class Dial final : public juce::Component, public ParameterControl
{
public:
    Dial (juce::AudioProcessorValueTreeState& state, const juce::String& id, const juce::String& name,
          juce::Colour colour, Format formatValue, bool bipolar = false, int knobSize = 36)
        : caption (name), accent (colour), centred (bipolar), knob (knobSize), format (std::move (formatValue)),
          attachment (state, id, slider), parameter (*state.getParameter (id))
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
        const auto side = juce::jmin (knob, bounds.getWidth(), bounds.getHeight());
        slider.setBounds (bounds.withSizeKeepingCentre (side, side).withY (labelArea.getY() - side - 2));
    }

    void paint (juce::Graphics& g) override
    {
        const auto showValue = slider.isMouseOverOrDragging();
        g.setFont (font (10.5f));
        g.setColour (showValue ? colours::text : colours::label);
        g.drawText (showValue ? format ((float) slider.getValue()) : caption, labelArea, juce::Justification::centred);
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
                g.strokePath (path, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            };

            arc (startAngle, endAngle, colours::track);

            const auto origin = dial->centred ? (startAngle + endAngle) * 0.5f : startAngle;

            if (std::abs (angle - origin) > 0.01f)
                arc (juce::jmin (origin, angle), juce::jmax (origin, angle), dial->accent);

            const auto knob = radius - stroke - 3.0f;
            const auto body = juce::Rectangle<float> (knob * 2.0f, knob * 2.0f).withCentre (centre);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4a4a), centre.x, body.getY(),
                                                     juce::Colour (0xff262626), centre.x, body.getBottom(), false));
            g.fillEllipse (body);
            g.setColour (juce::Colour (0xff141414));
            g.drawEllipse (body, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawEllipse (body.reduced (1.0f), 1.0f);

            const auto direction = juce::Point<float> (std::sin (angle), -std::cos (angle));
            g.setColour (juce::Colour (0xffeeeeee));
            g.drawLine ({ centre + direction * (knob * 0.25f), centre + direction * (knob * 0.85f) }, radius > 30.0f ? 3.0f : 2.0f);
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
    juce::AudioProcessorParameter& parameter;

public:
    juce::AudioProcessorParameter& controlledParameter() override { return parameter; }
};

/** A choice as text tabs, the current one light. */
class Choice final : public juce::Component, public ParameterControl
{
public:
    explicit Choice (juce::AudioParameterChoice& p)
        : parameter (p), attachment (p, [this] (float) { repaint(); })
    {
        attachment.sendInitialUpdate();
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paint (juce::Graphics& g) override
    {
        g.setFont (font (11.0f));
        auto bounds = getLocalBounds();

        for (int index = 0; index < parameter.choices.size(); ++index)
        {
            const auto area = bounds.removeFromLeft (widthOf (index));
            g.setColour (index == parameter.getIndex() ? colours::text : colours::caption);
            g.drawText (parameter.choices[index], area, juce::Justification::centred);
        }
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        auto left = 0;

        for (int index = 0; index < parameter.choices.size(); ++index)
        {
            left += widthOf (index);

            if (event.x < left)
            {
                gesture (parameter, parameter.convertTo0to1 ((float) index));
                return;
            }
        }
    }

private:
    int widthOf (int index) const
    {
        return juce::roundToInt (juce::GlyphArrangement::getStringWidth (font (11.0f), parameter.choices[index])) + 14;
    }

    juce::AudioParameterChoice& parameter;
    juce::ParameterAttachment attachment;

public:
    juce::AudioProcessorParameter& controlledParameter() override { return parameter; }
};

/** A button as a rounded pill; lit while on. */
class Pill final : public juce::Component
{
public:
    explicit Pill (juce::String pillText = {}) : text (std::move (pillText)) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void setOn (bool on)
    {
        if (std::exchange (lit, on) != on)
            repaint();
    }

    void setEnabledLook (bool enabled)
    {
        if (std::exchange (usable, enabled) != enabled)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const auto hover = isMouseOver() && usable;
        g.setColour (lit ? colours::violet.withAlpha (0.28f) : hover ? colours::pillHover : colours::pill);
        g.fillRoundedRectangle (bounds, bounds.getHeight() * 0.5f);
        g.setColour (! usable ? colours::caption.withAlpha (0.6f) : lit || hover ? colours::text : colours::label);
        g.setFont (font (11.0f));
        g.drawText (text, getLocalBounds(), juce::Justification::centred);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (usable && getLocalBounds().contains (event.getPosition()) && onClick)
            onClick();
    }

    std::function<void()> onClick;

private:
    juce::String text;
    bool lit = false, usable = true;
};

/** Icons drawn in a 20 x 20 box. */
namespace icons
{
juce::Path select()
{
    juce::Path p;
    p.startNewSubPath (5.0f, 2.5f);
    p.lineTo (15.5f, 12.0f);
    p.lineTo (10.5f, 12.5f);
    p.lineTo (13.5f, 18.0f);
    p.lineTo (11.5f, 19.0f);
    p.lineTo (8.5f, 13.5f);
    p.lineTo (5.0f, 17.0f);
    p.closeSubPath();
    return p;
}

juce::Path line()
{
    juce::Path p;
    p.startNewSubPath (3.5f, 16.5f);
    p.lineTo (16.5f, 3.5f);
    return p;
}

juce::Path arc()
{
    juce::Path p;
    p.addCentredArc (10.0f, 14.0f, 7.5f, 7.5f, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
    return p;
}

juce::Path curve()
{
    juce::Path p;
    p.startNewSubPath (3.0f, 16.0f);
    p.cubicTo (6.0f, 0.0f, 14.0f, 20.0f, 17.0f, 4.0f);
    return p;
}

juce::Path duplicate()
{
    juce::Path p;
    p.addRoundedRectangle (3.0f, 6.0f, 10.0f, 10.0f, 1.5f);
    p.addRoundedRectangle (7.0f, 2.5f, 10.0f, 10.0f, 1.5f);
    return p;
}

juce::Path flipHorizontal()
{
    juce::Path p;
    p.addTriangle (8.0f, 4.0f, 8.0f, 16.0f, 2.0f, 16.0f);
    p.addTriangle (12.0f, 4.0f, 12.0f, 16.0f, 18.0f, 16.0f);
    return p;
}

juce::Path flipVertical()
{
    auto p = flipHorizontal();
    p.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::halfPi, 10.0f, 10.0f));
    return p;
}

juce::Path rotate()
{
    juce::Path p;
    p.addCentredArc (10.0f, 10.5f, 6.5f, 6.5f, 0.0f, -2.2f, 2.0f, true);
    p.addTriangle (13.0f, 2.5f, 17.5f, 6.5f, 12.0f, 7.5f);
    return p;
}

juce::Path remove()
{
    juce::Path p;
    p.startNewSubPath (4.0f, 5.5f);
    p.lineTo (16.0f, 5.5f);
    p.startNewSubPath (8.0f, 5.5f);
    p.lineTo (8.5f, 3.0f);
    p.lineTo (11.5f, 3.0f);
    p.lineTo (12.0f, 5.5f);
    p.startNewSubPath (5.5f, 5.5f);
    p.lineTo (6.5f, 17.0f);
    p.lineTo (13.5f, 17.0f);
    p.lineTo (14.5f, 5.5f);
    return p;
}

juce::Path origin()
{
    juce::Path p;
    p.addEllipse (6.0f, 6.0f, 8.0f, 8.0f);
    p.startNewSubPath (10.0f, 1.5f);
    p.lineTo (10.0f, 5.0f);
    p.startNewSubPath (10.0f, 15.0f);
    p.lineTo (10.0f, 18.5f);
    p.startNewSubPath (1.5f, 10.0f);
    p.lineTo (5.0f, 10.0f);
    p.startNewSubPath (15.0f, 10.0f);
    p.lineTo (18.5f, 10.0f);
    return p;
}
} // namespace icons

/** A tool or an action: an icon over its name. A tool stays lit while chosen. */
class IconButton final : public juce::Component
{
public:
    IconButton (juce::String buttonName, juce::Path iconPath, bool filled, juce::Colour colour)
        : name (std::move (buttonName)), icon (std::move (iconPath)), fill (filled), accent (colour)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void setOn (bool on)
    {
        if (std::exchange (lit, on) != on)
            repaint();
    }

    void setEnabledLook (bool enabled)
    {
        if (std::exchange (usable, enabled) != enabled)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        const auto hover = isMouseOver() && usable;

        if (lit)
        {
            g.setColour (colours::panel.brighter (0.12f));
            g.fillRoundedRectangle (bounds.toFloat().reduced (4.0f, 1.0f), 4.0f);
        }

        const auto labelArea = bounds.removeFromBottom (15);
        const auto colour = ! usable ? colours::caption.withAlpha (0.5f) : lit ? accent : hover ? colours::text : colours::label;
        const auto iconArea = bounds.withSizeKeepingCentre (20, 20).toFloat();
        const auto transform = juce::AffineTransform::translation (iconArea.getX(), iconArea.getY() + 1.0f);
        g.setColour (colour);

        if (fill)
            g.fillPath (icon, transform);
        else
            g.strokePath (icon, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), transform);

        g.setColour (! usable ? colours::caption.withAlpha (0.5f) : lit || hover ? colours::text : colours::label);
        g.setFont (font (10.0f));
        g.drawText (name, labelArea, juce::Justification::centredTop);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (usable && getLocalBounds().contains (event.getPosition()) && onClick)
            onClick();
    }

    std::function<void()> onClick;

private:
    juce::String name;
    juce::Path icon;
    bool fill, lit = false, usable = true;
    juce::Colour accent;
};

/** A number to step through a list of values: the name, then the value between arrows. */
class Stepper final : public juce::Component
{
public:
    Stepper (juce::String stepperName, std::vector<int> stepValues, int initial)
        : name (std::move (stepperName)), values (std::move (stepValues))
    {
        index = (int) std::distance (values.begin(), std::find (values.begin(), values.end(), initial));
        index = juce::jlimit (0, (int) values.size() - 1, index);
    }

    int value() const { return values[(size_t) index]; }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.setFont (font (11.0f));
        g.setColour (colours::label);
        g.drawText (name, bounds.removeFromLeft (bounds.getWidth() - 64), juce::Justification::centredLeft);

        const auto arrow = [&] (juce::Rectangle<int> area, bool right, bool enabled)
        {
            const auto c = area.toFloat().getCentre();
            juce::Path p;
            p.addTriangle (c.x + (right ? -2.5f : 2.5f), c.y - 4.0f, c.x + (right ? -2.5f : 2.5f), c.y + 4.0f, c.x + (right ? 3.0f : -3.0f), c.y);
            g.setColour (enabled ? colours::label : colours::track);
            g.fillPath (p);
        };

        arrow (bounds.removeFromLeft (16), false, index > 0);
        arrow (bounds.removeFromRight (16), true, index + 1 < (int) values.size());
        g.setColour (colours::text);
        g.drawText (juce::String (value()), bounds, juce::Justification::centred);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        const auto right = event.x > getWidth() - 32;
        const auto left = event.x > getWidth() - 64 && ! right;
        step (right ? 1 : left ? -1 : 0);
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        step (wheel.deltaY > 0.0f ? 1 : wheel.deltaY < 0.0f ? -1 : 0);
    }

    std::function<void (int)> onChange;

private:
    void step (int by)
    {
        const auto next = juce::jlimit (0, (int) values.size() - 1, index + by);

        if (next == index)
            return;

        index = next;
        repaint();

        if (onChange)
            onChange (value());
    }

    juce::String name;
    std::vector<int> values;
    int index = 0;
};

//==============================================================================
/** The circle: the sample's waveform around it, the grid, the radar's sweep and ray, the pattern's segments and its
    origin, the grain that plays lit on the waveform. Editable in the window: tools to select, draw and change
    segments, the origin to drag. */
class RadarView final : public juce::Component
{
public:
    enum class Tool { select, line, arc, curve };

    RadarView (Processor& p, Session& s, bool editable)
        : processor (p), session (s), canEdit (editable)
    {
        setWantsKeyboardFocus (editable);
        setMouseClickGrabsKeyboardFocus (editable);
    }

    std::function<int()> shownPattern;                // the one drawn and edited
    std::function<void (const Pattern& before)> onCommit; // an edit finished: the pattern before it, for undo
    std::function<void()> onEdited;                   // the pattern changed (also during a drag)
    std::function<void()> onSelectionChanged;
    std::function<void (Tool)> onToolKey;

    Tool tool = Tool::select;
    int angleDivisions = 32, ringDivisions = 8;
    bool snapping = true;
    std::vector<int> selection;

    Pattern& pattern() { return processor.patterns[(size_t) shownPattern()]; }

    void setTool (Tool newTool)
    {
        tool = newTool;
        cursorPoint.reset();
        setMouseCursor (tool == Tool::select ? juce::MouseCursor::NormalCursor : juce::MouseCursor::CrosshairCursor);
        repaint();
    }

    void clearSelection()
    {
        if (selection.empty())
            return;

        selection.clear();
        selectionChanged();
    }

    void waveformChanged()
    {
        waveformImage = {};
        repaint();
    }

    //==========================================================================
    // Actions on the selection, or on the whole pattern when nothing is selected

    void deleteSelection()
    {
        if (selection.empty())
            return;

        const auto before = pattern();
        auto sorted = selection;
        std::sort (sorted.rbegin(), sorted.rend());

        for (const auto index : sorted)
            pattern().segments.erase (pattern().segments.begin() + index);

        selection.clear();
        commit (before);
        selectionChanged();
    }

    /** Copies, turned on by one angle step: repeated, they go round the circle. */
    void duplicate()
    {
        const auto before = pattern();
        auto& segments = pattern().segments;
        const auto sources = targets();
        std::vector<int> copies;

        for (const auto index : sources)
        {
            if ((int) segments.size() >= Pattern::maxSegments)
                break;

            segments.push_back (transformed (segments[(size_t) index], rotation (1.0f / (float) angleDivisions), false));
            copies.push_back ((int) segments.size() - 1);
        }

        selection = copies;
        commit (before);
        selectionChanged();
    }

    void rotate()
    {
        apply (rotation (1.0f / (float) angleDivisions), false);
    }

    void flip (bool horizontal)
    {
        apply ([horizontal] (Point p) { return horizontal ? Point { -p.x, p.y } : Point { p.x, -p.y }; }, true);
    }

    void centreOrigin()
    {
        const auto before = pattern();
        pattern().origin = {};
        commit (before);
    }

    void selectAll()
    {
        selection.resize (pattern().segments.size());
        std::iota (selection.begin(), selection.end(), 0);
        selectionChanged();
    }

    //==========================================================================
    void resized() override
    {
        const auto bounds = getLocalBounds().toFloat();
        centre = bounds.getCentre();
        outer = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 2.0f;
        ringWidth = canEdit ? 30.0f : 15.0f;
        radius = outer - ringWidth - (canEdit ? 10.0f : 5.0f);
        waveformImage = {};
    }

    void paint (juce::Graphics& g) override
    {
        const auto& shown = pattern();
        const auto circle = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);

        paintRing (g);

        g.setColour (colours::background);
        g.fillEllipse (circle);
        g.setColour (colours::track);
        g.drawEllipse (circle, 1.0f);
        paintGrid (g);

        // Inside the circle: the sweep, the ray, the segments
        {
            juce::Graphics::ScopedSaveState clip (g);
            juce::Path disc;
            disc.addEllipse (circle);
            g.reduceClipRegion (disc);
            paintSweep (g, shown);
            paintRay (g, shown);
            paintSegments (g, shown);
        }

        paintOrigin (g, shown);

        if (canEdit)
            paintEditing (g, shown);

        paintMessage (g);
    }

    //==========================================================================
    void mouseMove (const juce::MouseEvent& event) override
    {
        if (! canEdit)
            return;

        if (tool == Tool::select)
        {
            const auto handle = handleAt (event.position);
            const auto segment = handle ? -1 : segmentAt (event.position);
            const auto newHover = handle ? handle->segment : segment;

            if (newHover != hovered || handle.has_value() != hoveringHandle)
            {
                hovered = newHover;
                hoveringHandle = handle.has_value();
                setMouseCursor (handle ? juce::MouseCursor::DraggingHandCursor
                                       : segment >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
                repaint();
            }
        }
        else
        {
            cursorPoint = snap (toModel (event.position), event.mods, -1);
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hovered = -1;
        cursorPoint.reset();
        repaint();
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (! canEdit)
        {
            if (session.waveform.frames == 0 && session.openSampleFile)
                session.openSampleFile();

            return;
        }

        drag = {};
        before = pattern();

        if (event.mods.isPopupMenu())
        {
            if (const auto index = segmentAt (event.position); index >= 0)
            {
                selection = { index };
                deleteSelection();
            }

            return;
        }

        if (tool != Tool::select)
        {
            startCreating (event);
            return;
        }

        if (const auto handle = handleAt (event.position))
        {
            drag.mode = Drag::Mode::handle;
            drag.handle = *handle;

            if (handle->segment >= 0 && ! isSelected (handle->segment))
            {
                selection = { handle->segment };
                selectionChanged();
            }

            return;
        }

        if (const auto index = segmentAt (event.position); index >= 0)
        {
            if (event.mods.isShiftDown())
            {
                if (isSelected (index))
                    selection.erase (std::find (selection.begin(), selection.end(), index));
                else
                    selection.push_back (index);

                selectionChanged();
                return;
            }

            if (! isSelected (index))
            {
                selection = { index };
                selectionChanged();
            }

            // Moving the selection: the grabbed segment's end nearest the pointer snaps, the rest follows
            const auto& grabbed = pattern().segments[(size_t) index];
            const auto at = toModel (event.position);
            drag.mode = Drag::Mode::move;
            drag.start = at;
            drag.anchor = (grabbed.a - at).length() < (grabbed.b - at).length() ? grabbed.a : grabbed.b;
            drag.anchorSegment = index;
            return;
        }

        drag.mode = Drag::Mode::band;
        drag.bandStart = event.position;
        drag.band = {};
        drag.initial = event.mods.isShiftDown() ? selection : std::vector<int> {};
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (! canEdit)
            return;

        switch (drag.mode)
        {
            case Drag::Mode::handle: dragHandle (event); break;
            case Drag::Mode::move:   dragSelection (event); break;
            case Drag::Mode::band:   dragBand (event); break;
            case Drag::Mode::create: dragCreated (event); break;
            case Drag::Mode::none:   break;
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! canEdit)
            return;

        if (drag.mode == Drag::Mode::create && ! selection.empty())
        {
            // A click without a drag draws nothing
            const auto& created = pattern().segments[(size_t) selection.front()];

            if ((created.b - created.a).length() * radius < 4.0f)
            {
                pattern().segments.pop_back();
                selection.clear();
                selectionChanged();
                edited();
            }
        }

        if (drag.mode == Drag::Mode::band)
            drag.band = {};

        if (drag.mode != Drag::Mode::none && drag.mode != Drag::Mode::band)
            commit (before);

        drag = {};
        repaint();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (! canEdit)
            return false;

        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            deleteSelection();
            return true;
        }

        if (key == juce::KeyPress::escapeKey)
        {
            clearSelection();

            if (onToolKey)
                onToolKey (Tool::select);

            return true;
        }

        const auto nudge = (key.getModifiers().isShiftDown() ? 10.0f : 1.0f) / radius;
        const auto code = key.getKeyCode();
        const Point offset { code == juce::KeyPress::leftKey ? -nudge : code == juce::KeyPress::rightKey ? nudge : 0.0f,
                             code == juce::KeyPress::upKey ? -nudge : code == juce::KeyPress::downKey ? nudge : 0.0f };

        if (offset.x != 0.0f || offset.y != 0.0f)
        {
            if (! selection.empty())
            {
                const auto previous = pattern();
                moveSelection (previous, offset);
                commit (previous);
            }

            return true;
        }

        if (key.getModifiers().isAnyModifierKeyDown())
            return false;

        const auto character = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
        const std::map<juce::juce_wchar, Tool> keys { { 'v', Tool::select }, { 'l', Tool::line }, { 'a', Tool::arc }, { 'c', Tool::curve } };

        if (const auto found = keys.find (character); found != keys.end() && onToolKey)
        {
            onToolKey (found->second);
            return true;
        }

        return false;
    }

private:
    struct Handle
    {
        int segment; // -1: the origin
        int which;   // 0 the start, 1 the end, 2 the bend (an arc's peak, a curve's middle)
    };

    struct Drag
    {
        enum class Mode { none, handle, move, band, create } mode = Mode::none;
        Handle handle { -1, 0 };
        Point start, anchor;
        int anchorSegment = -1;
        juce::Point<float> bandStart;
        juce::Rectangle<float> band;
        std::vector<int> initial;
    };

    //==========================================================================
    juce::Point<float> toView (Point p) const { return { centre.x + p.x * radius, centre.y + p.y * radius }; }
    Point toModel (juce::Point<float> v) const { return { (v.x - centre.x) / radius, (v.y - centre.y) / radius }; }

    bool isSelected (int index) const { return std::find (selection.begin(), selection.end(), index) != selection.end(); }

    std::vector<int> targets()
    {
        if (! selection.empty())
            return selection;

        std::vector<int> all (pattern().segments.size());
        std::iota (all.begin(), all.end(), 0);
        return all;
    }

    void selectionChanged()
    {
        repaint();

        if (onSelectionChanged)
            onSelectionChanged();
    }

    void edited()
    {
        repaint();

        if (onEdited)
            onEdited();
    }

    void commit (const Pattern& previous)
    {
        if (previous == pattern())
            return;

        edited();

        if (onCommit)
            onCommit (previous);
    }

    static std::function<Point (Point)> rotation (float turns)
    {
        const auto angle = turns * juce::MathConstants<float>::twoPi;
        const auto c = std::cos (angle), s = std::sin (angle);
        return [c, s] (Point p) { return Point { p.x * c - p.y * s, p.x * s + p.y * c }; };
    }

    /** A segment moved by a map of the plane; a mirror turns an arc's bulge to the other side. */
    static Segment transformed (Segment s, const std::function<Point (Point)>& map, bool mirrors)
    {
        s.a = inside (map (s.a));
        s.b = inside (map (s.b));
        s.c = map (s.c);

        if (mirrors)
            s.bend = -s.bend;

        return s;
    }

    void apply (const std::function<Point (Point)>& map, bool mirrors)
    {
        const auto previous = pattern();

        for (const auto index : targets())
            pattern().segments[(size_t) index] = transformed (pattern().segments[(size_t) index], map, mirrors);

        if (selection.empty())
            pattern().origin = inside (map (pattern().origin), 0.98f);

        commit (previous);
    }

    /** Moves the selected segments of a pattern by an offset, as far as their ends stay inside the circle. */
    void moveSelection (const Pattern& from, Point offset)
    {
        const auto fits = [&] (float scale)
        {
            for (const auto index : selection)
            {
                const auto& s = from.segments[(size_t) index];

                if ((s.a + offset * scale).length() > 1.0f || (s.b + offset * scale).length() > 1.0f)
                    return false;
            }

            return true;
        };

        auto scale = 1.0f;

        if (! fits (1.0f))
        {
            auto low = 0.0f, high = 1.0f;

            for (int i = 0; i < 12; ++i)
            {
                const auto middle = (low + high) * 0.5f;
                (fits (middle) ? low : high) = middle;
            }

            scale = low;
        }

        for (const auto index : selection)
        {
            auto s = from.segments[(size_t) index];
            s.a = s.a + offset * scale;
            s.b = s.b + offset * scale;
            s.c = s.c + offset * scale;
            pattern().segments[(size_t) index] = s;
        }

        edited();
    }

    /** A point snapped to a segment's end nearby (not the excluded segment's), else to the grid: the angle steps
        and the rings. Alt snaps to nothing. */
    Point snap (Point p, const juce::ModifierKeys& mods, int exclude) const
    {
        magnet = false;

        if (mods.isAltDown())
            return p;

        const auto reach = 8.0f / radius;
        const auto& segments = processor.patterns[(size_t) shownPattern()].segments;

        for (int i = 0; i < (int) segments.size(); ++i)
            if (i != exclude)
                for (const auto end : { segments[(size_t) i].a, segments[(size_t) i].b })
                    if ((end - p).length() < reach)
                    {
                        magnet = true;
                        return end;
                    }

        if (! snapping)
            return p;

        const auto distance = juce::jmin (1.0f, std::round (p.length() * (float) ringDivisions) / (float) ringDivisions);
        const auto turns = std::round (turnsOf (p) * (float) angleDivisions) / (float) angleDivisions;
        return onCircle (turns, distance);
    }

    std::optional<Handle> handleAt (juce::Point<float> at) const
    {
        const auto& shown = processor.patterns[(size_t) shownPattern()];
        const auto reach = 7.0f;
        std::optional<Handle> nearest;
        auto best = reach;
        const auto consider = [&] (Point p, Handle handle)
        {
            if (const auto d = toView (p).getDistanceFrom (at); d < best)
            {
                best = d;
                nearest = handle;
            }
        };

        for (int i = 0; i < (int) shown.segments.size(); ++i)
        {
            const auto& s = shown.segments[(size_t) i];
            consider (s.a, { i, 0 });
            consider (s.b, { i, 1 });

            if (isSelected (i) && s.shape != Shape::line)
                consider (s.shape == Shape::arc ? s.bulge() : s.curvePeak(), { i, 2 });
        }

        if (toView (shown.origin).getDistanceFrom (at) < 9.0f && ! nearest)
            nearest = Handle { -1, 0 };

        return nearest;
    }

    int segmentAt (juce::Point<float> at) const
    {
        const auto& shown = processor.patterns[(size_t) shownPattern()];
        const auto p = toModel (at);
        auto nearest = -1;
        auto best = 6.0f / radius;

        for (int i = 0; i < (int) shown.segments.size(); ++i)
            if (const auto d = shown.segments[(size_t) i].distanceTo (p); d < best)
            {
                best = d;
                nearest = i;
            }

        return nearest;
    }

    void startCreating (const juce::MouseEvent& event)
    {
        if ((int) pattern().segments.size() >= Pattern::maxSegments)
            return;

        const auto p = inside (snap (toModel (event.position), event.mods, -1));
        Segment s;
        s.shape = tool == Tool::arc ? Shape::arc : tool == Tool::curve ? Shape::curve : Shape::line;
        s.a = s.b = s.c = p;
        s.bend = s.shape == Shape::arc ? 0.6f : 0.0f;
        pattern().segments.push_back (s);
        selection = { (int) pattern().segments.size() - 1 };
        drag.mode = Drag::Mode::create;
        selectionChanged();
    }

    void dragCreated (const juce::MouseEvent& event)
    {
        auto& s = pattern().segments[(size_t) selection.front()];
        s.b = inside (snap (toModel (event.position), event.mods, selection.front()));

        // A curve starts as a gentle bow, to the right of the drawing direction
        if (s.shape == Shape::curve)
        {
            const auto d = s.b - s.a;
            s.setCurvePeak ((s.a + s.b) * 0.5f + Point { d.y, -d.x } * 0.25f);
        }

        cursorPoint = s.b;
        edited();
    }

    void dragHandle (const juce::MouseEvent& event)
    {
        const auto [index, which] = drag.handle;

        if (index < 0)
        {
            pattern().origin = inside (snap (toModel (event.position), event.mods, -1), 0.98f);
            edited();
            return;
        }

        auto& s = pattern().segments[(size_t) index];

        if (which == 2)
        {
            const auto p = toModel (event.position);

            if (s.shape == Shape::arc)
            {
                // The bend that puts the peak where the pointer is, measured across the chord
                const auto middle = (s.a + s.b) * 0.5f;
                const auto d = s.b - s.a;
                const auto lengthSquared = d.x * d.x + d.y * d.y;

                if (lengthSquared > 1.0e-8f)
                    s.bend = juce::jlimit (-4.0f, 4.0f, 2.0f * (d.y * (p.x - middle.x) - d.x * (p.y - middle.y)) / lengthSquared);
            }
            else
            {
                s.setCurvePeak (inside (p, 1.5f));
            }
        }
        else
        {
            const auto p = inside (snap (toModel (event.position), event.mods, index));
            (which == 0 ? s.a : s.b) = p;
        }

        edited();
    }

    void dragSelection (const juce::MouseEvent& event)
    {
        const auto delta = toModel (event.position) - drag.start;
        const auto snapped = snap (drag.anchor + delta, event.mods, drag.anchorSegment);
        moveSelection (before, snapped - drag.anchor);
    }

    void dragBand (const juce::MouseEvent& event)
    {
        drag.band = juce::Rectangle<float> (drag.bandStart, event.position);
        const auto& shown = pattern();
        std::vector<Point> points;
        selection = drag.initial;

        for (int i = 0; i < (int) shown.segments.size(); ++i)
        {
            if (isSelected (i))
                continue;

            shown.segments[(size_t) i].sample (points, 24);

            if (std::any_of (points.begin(), points.end(), [&] (Point p) { return drag.band.contains (toView (p)); }))
                selection.push_back (i);
        }

        selectionChanged();
    }

    //==========================================================================
    /** The sample around the circle, its lines grey like the dials' tracks; the grain that plays in the accent,
        its stretch as an arc on the track around the circle, as a dial shows its value. */
    void paintRing (juce::Graphics& g)
    {
        const auto& waveform = session.waveform;
        const auto trackRadius = radius + (canEdit ? 4.0f : 2.0f);
        g.setColour (colours::track);
        g.drawEllipse (juce::Rectangle<float> (trackRadius * 2.0f, trackRadius * 2.0f).withCentre (centre), canEdit ? 2.0f : 1.5f);

        if (waveform.frames == 0 || waveform.bins.empty())
            return;

        const auto scale = g.getInternalContext().getPhysicalPixelScaleFactor();

        if (waveformImage.isNull() || waveformScale != scale)
        {
            waveformScale = scale;
            const auto render = [&] (juce::Colour colour)
            {
                juce::Image image (juce::Image::ARGB, juce::jmax (1, juce::roundToInt ((float) getWidth() * scale)),
                                   juce::jmax (1, juce::roundToInt ((float) getHeight() * scale)), true);
                juce::Graphics ig (image);
                ig.addTransform (juce::AffineTransform::scale (scale));
                paintWaveform (ig, colour);
                return image;
            };

            waveformImage = render (colours::label.withAlpha (0.45f));
            grainImage = render (colours::violet);
        }

        const auto unscale = juce::AffineTransform::scale (1.0f / scale);
        g.drawImageTransformed (waveformImage, unscale);

        const auto& playback = session.playback;

        if (playback.playing == 0 || playback.pattern != shownPattern())
            return;

        // Each head's stretch, as bright as the head is loud
        const auto grain = juce::jlimit (0.002f, 0.5f, playback.grain) * juce::MathConstants<float>::twoPi;
        const auto mixA = std::cos (playback.mix * juce::MathConstants<float>::halfPi);
        const auto mixB = std::sin (playback.mix * juce::MathConstants<float>::halfPi);
        const auto ring = juce::Rectangle<float> (outer * 2.0f, outer * 2.0f).withCentre (centre);

        for (const auto [head, level] : { std::pair { playback.headA, mixA }, std::pair { playback.headB, mixB } })
        {
            if (level < 0.02f)
                continue;

            const auto from = head * juce::MathConstants<float>::twoPi;
            {
                juce::Graphics::ScopedSaveState clip (g);
                juce::Path sector;
                sector.addPieSegment (ring, from, from + grain, 0.0f);
                g.reduceClipRegion (sector);
                g.setOpacity (level);
                g.drawImageTransformed (grainImage, unscale);
            }

            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, trackRadius, trackRadius, 0.0f, from, from + grain, true);
            g.setColour (colours::violet.withAlpha (level));
            g.strokePath (arc, juce::PathStrokeType (canEdit ? 3.0f : 2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    void paintWaveform (juce::Graphics& g, juce::Colour colour)
    {
        const auto& bins = session.waveform.bins;
        const auto count = (int) bins.size() / 2;
        const auto middle = outer - ringWidth * 0.5f;
        const auto amplitude = ringWidth * 0.5f - 1.0f;
        juce::Path lines;

        for (int i = 0; i < count; ++i)
        {
            const auto direction = onCircle (((float) i + 0.5f) / (float) count);
            auto low = middle + amplitude * (float) bins[(size_t) i * 2] / 127.0f;
            auto high = middle + amplitude * (float) bins[(size_t) i * 2 + 1] / 127.0f;

            if (high - low < 0.6f)
            {
                low -= 0.3f;
                high += 0.3f;
            }

            lines.startNewSubPath (centre.x + direction.x * low, centre.y + direction.y * low);
            lines.lineTo (centre.x + direction.x * high, centre.y + direction.y * high);
        }

        g.setColour (colour);
        g.strokePath (lines, juce::PathStrokeType (canEdit ? 1.0f : 0.8f));
    }

    /** Where points snap to: a dot per angle step on every ring (the window only), and the quarters of the loop as
        ticks on the edge. */
    void paintGrid (juce::Graphics& g)
    {
        if (canEdit && snapping)
        {
            g.setColour (colours::track);

            for (int ring = 1; ring <= ringDivisions; ++ring)
                for (int spoke = 0; spoke < angleDivisions; ++spoke)
                    g.fillEllipse (juce::Rectangle<float> (2.0f, 2.0f).withCentre (toView (onCircle ((float) spoke / (float) angleDivisions, (float) ring / (float) ringDivisions))));
        }

        for (int i = 0; i < 4; ++i)
        {
            const auto d = onCircle ((float) i / 4.0f);
            g.setColour (colours::caption);
            g.drawLine ({ centre + juce::Point<float> (d.x, d.y) * (radius - (canEdit ? 7.0f : 4.0f)), centre + juce::Point<float> (d.x, d.y) * radius }, 1.0f);
        }
    }

    /** The radar's sweep: the directions the ray just pointed in, a faint light fading behind it. */
    void paintSweep (juce::Graphics& g, const Pattern& shown)
    {
        const auto origin = inside (shown.origin, 0.98f);
        const auto hitCircle = [&] (float turns)
        {
            const auto d = onCircle (turns);
            const auto along = origin.x * d.x + origin.y * d.y;
            const auto t = -along + std::sqrt (juce::jmax (0.0f, along * along - (origin.x * origin.x + origin.y * origin.y - 1.0f)));
            return toView (origin + d * t);
        };

        constexpr int slices = 16;
        constexpr float width = 0.004f;

        for (int i = 0; i < slices; ++i)
        {
            const auto from = session.playback.phase - (float) (i + 1) * width;
            juce::Path slice;
            slice.addTriangle (toView (origin), hitCircle (from), hitCircle (from + width));
            g.setColour (juce::Colours::white.withAlpha (0.05f * (1.0f - (float) i / slices)));
            g.fillPath (slice);
        }
    }

    void paintRay (juce::Graphics& g, const Pattern& shown)
    {
        const auto traced = trace (shown, session.playback.phase, &path);

        if (path.size() < 2)
            return;

        // Each reflection dims the ray a little more
        const auto colour = traced.escaped ? colours::text : colours::pink;
        const juce::PathStrokeType stroke (canEdit ? 1.2f : 1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        auto alpha = 0.9f;

        for (size_t i = 1; i < path.size(); ++i, alpha *= 0.8f)
        {
            juce::Path leg;
            leg.startNewSubPath (toView (path[i - 1]));
            leg.lineTo (toView (path[i]));
            g.setColour (colour.withAlpha (alpha));
            g.strokePath (leg, stroke);
        }

        if (traced.escaped)
        {
            g.setColour (colours::violet);
            g.fillEllipse (juce::Rectangle<float> (canEdit ? 6.0f : 4.0f, canEdit ? 6.0f : 4.0f).withCentre (toView (path.back())));
        }
    }

    void paintSegments (juce::Graphics& g, const Pattern& shown)
    {
        for (int i = 0; i < (int) shown.segments.size(); ++i)
        {
            shown.segments[(size_t) i].sample (points);
            juce::Path stroke;
            stroke.startNewSubPath (toView (points.front()));

            for (size_t k = 1; k < points.size(); ++k)
                stroke.lineTo (toView (points[k]));

            const auto selected = canEdit && isSelected (i);
            g.setColour (selected ? colours::orange : canEdit && i == hovered ? colours::text : colours::label);
            g.strokePath (stroke, juce::PathStrokeType (canEdit ? 2.0f : 1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    void paintOrigin (juce::Graphics& g, const Pattern& shown)
    {
        const auto at = toView (shown.origin);
        const auto size = canEdit ? 10.0f : 6.0f;
        g.setColour (colours::background);
        g.fillEllipse (juce::Rectangle<float> (size, size).withCentre (at));
        g.setColour (canEdit && hovered == -1 && hoveringHandle ? colours::text : colours::label);
        g.drawEllipse (juce::Rectangle<float> (size, size).withCentre (at), canEdit ? 1.5f : 1.0f);
    }

    void paintEditing (juce::Graphics& g, const Pattern& shown)
    {
        // The handles: every segment's ends, the selected ones' bends
        for (int i = 0; i < (int) shown.segments.size(); ++i)
        {
            const auto& s = shown.segments[(size_t) i];
            const auto selected = isSelected (i);

            for (const auto end : { s.a, s.b })
            {
                const auto area = juce::Rectangle<float> (selected ? 8.0f : 6.0f, selected ? 8.0f : 6.0f).withCentre (toView (end));
                g.setColour (colours::background);
                g.fillEllipse (area);
                g.setColour (selected ? colours::orange : colours::label);
                g.drawEllipse (area, 1.5f);
            }

            if (selected && s.shape != Shape::line)
            {
                const auto peak = toView (s.shape == Shape::arc ? s.bulge() : s.curvePeak());
                juce::Path diamond;
                diamond.addRectangle (-3.5f, -3.5f, 7.0f, 7.0f);
                diamond.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::pi * 0.25f).translated (peak));
                g.setColour (colours::orange);
                g.fillPath (diamond);

                if (s.shape == Shape::curve)
                {
                    g.setColour (colours::orange.withAlpha (0.35f));
                    const float dashes[] = { 3.0f, 3.0f };
                    g.drawDashedLine ({ toView ((s.a + s.b) * 0.5f), peak }, dashes, 2, 1.0f);
                }
            }
        }

        if (cursorPoint && tool != Tool::select)
        {
            const auto at = toView (*cursorPoint);
            g.setColour (magnet ? colours::orange : colours::text);
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (at));

            if (magnet)
                g.drawEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (at), 1.0f);
        }

        if (! drag.band.isEmpty())
        {
            g.setColour (colours::orange.withAlpha (0.08f));
            g.fillRect (drag.band);
            g.setColour (colours::orange.withAlpha (0.6f));
            g.drawRect (drag.band, 1.0f);
        }
    }

    void paintMessage (juce::Graphics& g)
    {
        const auto& waveform = session.waveform;
        juce::String message;

        if (session.dragging)
            message = "Drop to load";
        else if (session.loading)
            message = "Loading...";
        else if (waveform.frames == 0)
            message = canEdit ? "Drop a sample here" : "Drop a sample";

        if (session.dragging)
        {
            g.setColour (colours::violet.withAlpha (0.8f));
            g.drawEllipse (juce::Rectangle<float> (outer * 2.0f, outer * 2.0f).withCentre (centre).reduced (1.0f), 2.0f);
        }

        if (message.isEmpty() && session.playback.playing != 0 && session.playback.pattern != shownPattern())
            message = "Pattern " + juce::String (session.playback.pattern + 1) + " plays";

        if (message.isEmpty())
            return;

        g.setFont (font (canEdit ? 13.0f : 10.5f, Face::italic));
        g.setColour (colours::label);
        const auto area = juce::Rectangle<float> (radius * 1.6f, 20.0f).withCentre ({ centre.x, centre.y + radius * 0.5f });
        g.drawText (message, area, juce::Justification::centred);
    }

    Processor& processor;
    Session& session;
    const bool canEdit;

    juce::Point<float> centre;
    float outer = 100.0f, ringWidth = 20.0f, radius = 80.0f;

    juce::Image waveformImage, grainImage;
    float waveformScale = 1.0f;

    Drag drag;
    Pattern before;
    int hovered = -1;
    bool hoveringHandle = false;
    std::optional<Point> cursorPoint;
    mutable bool magnet = false;
    std::vector<Point> points, path;
};

//==============================================================================
/** A pattern's slot: its number and a small picture of its segments. Lit while picked, marked while it plays. */
class PatternCell final : public juce::Component, public ParameterControl
{
public:
    PatternCell (Processor& p, Session& s, int slot, bool compact)
        : processor (p), session (s), index (slot), small (compact)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    /** Repaints when what it shows changed. */
    void refresh()
    {
        const auto& pattern = processor.patterns[(size_t) index];
        const auto picked = patternIndexOf (processor) == index;
        const auto playing = session.playback.playing != 0 && session.playback.pattern == index;

        if (pattern == shown && picked == wasPicked && playing == wasPlaying)
            return;

        shown = pattern;
        wasPicked = picked;
        wasPlaying = playing;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (1.0f);
        const auto hover = isMouseOver();
        g.setColour (wasPicked ? colours::panel.brighter (0.1f) : hover ? colours::panel.brighter (0.05f) : colours::panel);
        g.fillRoundedRectangle (bounds, 3.0f);

        if (wasPlaying || wasPicked)
        {
            g.setColour (wasPlaying ? colours::orange : colours::label.withAlpha (0.5f));
            g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);
        }

        // The number, with a play mark while it plays
        auto label = bounds.reduced (5.0f, 2.0f);
        g.setFont (font (small ? 10.0f : 11.0f));
        g.setColour (wasPicked ? colours::text : colours::label);

        if (wasPlaying)
        {
            juce::Path play;
            const auto y = label.getY() + (small ? 5.0f : 6.0f);
            play.addTriangle (label.getX(), y - 3.5f, label.getX(), y + 3.5f, label.getX() + 6.0f, y);
            g.setColour (colours::orange);
            g.fillPath (play);
            label.removeFromLeft (9.0f);
            g.setColour (colours::text);
        }

        const auto number = juce::String (index + 1);
        const auto numberEnd = label.getX() + juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), number);
        g.drawText (number, label.removeFromTop (small ? 11.0f : 13.0f), juce::Justification::topLeft);

        // The picture, on the right, clear of the number (two digits take more room)
        const auto side = juce::jmin (bounds.getHeight() - 6.0f, bounds.getRight() - 4.0f - (numberEnd + 4.0f));
        const auto picture = juce::Rectangle<float> (side, side).withCentre ({ bounds.getRight() - side * 0.5f - 4.0f, bounds.getCentreY() });
        g.setColour (colours::background);
        g.fillEllipse (picture);
        g.setColour (colours::track);
        g.drawEllipse (picture, 1.0f);

        const auto r = side * 0.5f;
        const auto c = picture.getCentre();
        std::vector<Point> points;
        juce::Path segments;

        for (const auto& s : shown.segments)
        {
            s.sample (points, 16);
            segments.startNewSubPath (c.x + points.front().x * r, c.y + points.front().y * r);

            for (size_t k = 1; k < points.size(); ++k)
                segments.lineTo (c.x + points[k].x * r, c.y + points[k].y * r);
        }

        g.setColour (wasPicked ? colours::text : colours::label);
        g.strokePath (segments, juce::PathStrokeType (1.0f));
        g.setColour (colours::caption);
        g.fillEllipse (juce::Rectangle<float> (2.5f, 2.5f).withCentre ({ c.x + shown.origin.x * r, c.y + shown.origin.y * r }));
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! getLocalBounds().contains (event.getPosition()))
            return;

        auto& parameter = parameterOf (processor, "pattern");
        gesture (parameter, parameter.convertTo0to1 ((float) (index + 1)));
    }

private:
    Processor& processor;
    Session& session;
    const int index;
    const bool small;

public:
    juce::AudioProcessorParameter& controlledParameter() override { return parameterOf (processor, "pattern"); }

private:
    Pattern shown;
    bool wasPicked = false, wasPlaying = false;
};

/** A section's italic title, as the template's panels have them. */
void drawTitle (juce::Graphics& g, const juce::String& title, juce::Rectangle<int> area)
{
    g.setColour (colours::label);
    g.setFont (font (12.0f, Face::italic));
    g.drawText (title, area, juce::Justification::centredLeft);
}

juce::String sampleText (const Waveform& waveform)
{
    if (waveform.frames == 0)
        return "No sample";

    return waveform.name + "  " + juce::String ((double) waveform.frames / juce::jmax (1, waveform.rate), 2) + " s";
}

//==============================================================================
class WindowEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    int getControlParameterIndex (juce::Component& component) override { return controlParameterIndex (component); }

    WindowEditor (Processor& p, Session& s)
        : AudioProcessorEditor (p), processor (p), session (s), view (p, s, true),
          selectTool ("Select", icons::select(), true, colours::violet),
          lineTool ("Line", icons::line(), false, colours::violet),
          arcTool ("Arc", icons::arc(), false, colours::violet),
          curveTool ("Curve", icons::curve(), false, colours::violet),
          duplicateAction ("Duplicate", icons::duplicate(), false, colours::text),
          rotateAction ("Rotate", icons::rotate(), false, colours::text),
          flipHorizontalAction ("Flip H", icons::flipHorizontal(), false, colours::text),
          flipVerticalAction ("Flip V", icons::flipVertical(), false, colours::text),
          originAction ("Centre", icons::origin(), false, colours::text),
          deleteAction ("Delete", icons::remove(), false, colours::text),
          barsDial (p.state, "bars", "Bars", colours::teal, bars(), false, 42),
          grainDial (p.state, "grain", "Grain", colours::orange, milliseconds(), false, 42),
          pitchDial (p.state, "pitch", "Pitch", colours::violet, semitones(), true, 42),
          attackDial (p.state, "attack", "Attack", colours::pink, milliseconds(), false, 42),
          releaseDial (p.state, "release", "Release", colours::pink, milliseconds(), false, 42),
          volumeDial (p.state, "volume", "Volume", colours::teal, decibels(), false, 42),
          keys (*dynamic_cast<juce::AudioParameterChoice*> (p.state.getParameter ("keys"))),
          play (*dynamic_cast<juce::AudioParameterChoice*> (p.state.getParameter ("play"))),
          snapPill ("Snap"), loadPill ("Load..."), undoPill ("Undo"), redoPill ("Redo"),
          copyPill ("Copy"), pastePill ("Paste"), clearPill ("Clear"),
          angles ("Angles", { 4, 6, 8, 12, 16, 24, 32, 48, 64, 96 }, 32),
          rings ("Rings", { 1, 2, 3, 4, 6, 8, 12, 16, 24 }, 8)
    {
        view.shownPattern = [this] { return patternIndexOf (processor); };
        view.onEdited = [this] { patternEdited(); };
        view.onCommit = [this] (const Pattern& previous) { pushUndo (previous); };
        view.onSelectionChanged = [this] { updateActions(); };
        view.onToolKey = [this] (RadarView::Tool tool) { chooseTool (tool); };
        addAndMakeVisible (view);

        const std::pair<IconButton*, RadarView::Tool> tools[] = { { &selectTool, RadarView::Tool::select }, { &lineTool, RadarView::Tool::line },
                                                                   { &arcTool, RadarView::Tool::arc }, { &curveTool, RadarView::Tool::curve } };

        for (const auto& [button, tool] : tools)
        {
            button->onClick = [this, tool = tool] { chooseTool (tool); };
            addAndMakeVisible (*button);
        }

        duplicateAction.onClick = [this] { view.duplicate(); };
        rotateAction.onClick = [this] { view.rotate(); };
        flipHorizontalAction.onClick = [this] { view.flip (true); };
        flipVerticalAction.onClick = [this] { view.flip (false); };
        originAction.onClick = [this] { view.centreOrigin(); };
        deleteAction.onClick = [this] { view.deleteSelection(); };

        for (auto* action : { &duplicateAction, &rotateAction, &flipHorizontalAction, &flipVerticalAction, &originAction, &deleteAction })
            addAndMakeVisible (*action);

        for (auto* dial : { &barsDial, &grainDial, &pitchDial, &attackDial, &releaseDial, &volumeDial })
            addAndMakeVisible (*dial);

        addAndMakeVisible (keys);
        addAndMakeVisible (play);

        snapPill.onClick = [this]
        {
            view.snapping = ! view.snapping;
            snapPill.setOn (view.snapping);
            view.repaint();
        };
        snapPill.setOn (true);
        angles.onChange = [this] (int value) { view.angleDivisions = value; view.repaint(); };
        rings.onChange = [this] (int value) { view.ringDivisions = value; view.repaint(); };
        loadPill.onClick = [this] { if (session.openSampleFile) session.openSampleFile(); };
        undoPill.onClick = [this] { undoOrRedo (undo, redo); };
        redoPill.onClick = [this] { undoOrRedo (redo, undo); };
        copyPill.onClick = [this] { clipboard = view.pattern(); pastePill.setEnabledLook (true); };
        pastePill.onClick = [this] { if (clipboard) replacePattern (*clipboard); };
        clearPill.onClick = [this] { replacePattern ({}); };
        pastePill.setEnabledLook (false);

        for (auto* component : std::initializer_list<juce::Component*> { &snapPill, &angles, &rings, &loadPill, &undoPill, &redoPill, &copyPill, &pastePill, &clearPill })
            addAndMakeVisible (component);

        for (int i = 0; i < numPatterns; ++i)
            addAndMakeVisible (cells.emplace_back (std::make_unique<PatternCell> (p, s, i, false)).get());

        chooseTool (RadarView::Tool::select);
        updateActions();
        setSize (width, height);
        startTimerHz (30);
    }

    static constexpr int width = RADAR_WINDOW_WIDTH, height = RADAR_WINDOW_HEIGHT;

    // The side's sections: the title, then the content, then the space to the next section
    static constexpr int titleHeight = 24, dialHeight = 42 + 2 + 14; // knob, gap, label
    static constexpr int rowInset = 5; // a row's text sits this far inside it: rows start higher, so that text and knobs line up

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::background);

        g.setColour (colours::header);
        g.fillRect (headerArea);
        g.setFont (font (15.0f).withExtraKerningFactor (0.18f));
        g.setColour (colours::text);
        g.drawText ("RADAR", headerArea.withTrimmedLeft (14), juce::Justification::centredLeft);
        g.setFont (font (12.0f, Face::italic));
        g.setColour (colours::caption);
        g.drawText ("Ray Loop Sampler", headerArea.withTrimmedLeft (92), juce::Justification::centredLeft);
        g.setFont (font (12.0f));
        g.setColour (session.waveform.frames > 0 ? colours::text : colours::caption);
        g.drawText (sampleText (session.waveform), sampleArea, juce::Justification::centredRight);

        for (const auto& panel : { toolArea, viewArea, sideArea, stripArea })
        {
            g.setColour (colours::panel);
            g.fillRect (panel);
        }

        g.setColour (colours::divider);
        g.drawHorizontalLine (toolDivider, (float) toolArea.getX() + 10.0f, (float) toolArea.getRight() - 10.0f);

        for (const auto& [title, area] : { std::pair { "Loop", loopArea }, std::pair { "Mode", modeArea },
                                           std::pair { "Amp", ampArea }, std::pair { "Edit", snapArea } })
            drawTitle (g, title, area.withHeight (24).withTrimmedLeft (12).translated (0, 2));

        g.setColour (colours::divider);

        for (const auto y : { modeArea.getY(), ampArea.getY(), snapArea.getY() })
            g.fillRect (sideArea.getX(), y - 1, sideArea.getWidth(), 2);

        g.setColour (colours::label);
        g.setFont (font (11.0f));
        g.drawText ("Play", playLabel, juce::Justification::centredLeft);
        g.drawText ("Keys", keysLabel, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        headerArea = bounds.removeFromTop (34);
        {
            auto header = headerArea.reduced (10, 6);
            redoPill.setBounds (header.removeFromRight (52));
            header.removeFromRight (6);
            undoPill.setBounds (header.removeFromRight (52));
            header.removeFromRight (24);
            loadPill.setBounds (header.removeFromRight (64));
            header.removeFromRight (10);
            sampleArea = header.withTrimmedLeft (220);
        }

        bounds.removeFromTop (2);
        stripArea = bounds.removeFromBottom (48);
        bounds.removeFromBottom (2);

        toolArea = bounds.removeFromLeft (72);
        bounds.removeFromLeft (2);
        sideArea = bounds.removeFromRight (278);
        bounds.removeFromRight (2);
        viewArea = bounds;
        view.setBounds (viewArea.reduced (8));

        // Tools, then actions
        {
            auto column = toolArea.reduced (0, 10);

            for (auto* tool : { &selectTool, &lineTool, &arcTool, &curveTool })
                tool->setBounds (column.removeFromTop (46));

            toolDivider = column.removeFromTop (14).getCentreY();

            for (auto* action : { &duplicateAction, &rotateAction, &flipHorizontalAction, &flipVerticalAction, &originAction, &deleteAction })
                action->setBounds (column.removeFromTop (46));
        }

        // The side: Loop, Mode, Amp, Edit. The height left over is shared out evenly: every section has the same
        // gap between its title and its content and below its content, the last one's is the padding at the bottom.
        {
            constexpr int rows = 24 + 4 + 24, edits = 22 + 8 + 22 + 4 + 22;
            const int contents[] = { dialHeight, rows - 2 * rowInset, dialHeight, edits - 2 * rowInset };
            const auto used = 4 * titleHeight + std::accumulate (std::begin (contents), std::end (contents), 0);
            const auto gap = juce::jmax (6, (sideArea.getHeight() - used) / 8);
            auto side = sideArea;
            juce::Rectangle<int>* sections[] = { &loopArea, &modeArea, &ampArea, &snapArea };
            juce::Rectangle<int> content[4];

            for (int i = 0; i < 4; ++i)
            {
                *sections[i] = i < 3 ? side.removeFromTop (titleHeight + 2 * gap + contents[i]) : side;
                content[i] = sections[i]->withTrimmedTop (titleHeight + gap).withHeight (contents[i]);
            }

            auto loopDials = content[0].reduced (8, 0);
            const auto dialWidth = loopDials.getWidth() / 3;

            for (auto* dial : { &barsDial, &grainDial, &pitchDial })
                dial->setBounds (loopDials.removeFromLeft (dialWidth));

            // Rows start higher by their inset, so that their text lines up with where the knobs begin
            auto mode = content[1].expanded (0, rowInset).reduced (14, 0);
            auto playRow = mode.removeFromTop (24);
            playLabel = playRow.removeFromLeft (52);
            play.setBounds (playRow.removeFromLeft (130));
            mode.removeFromTop (4);
            auto keysRow = mode.removeFromTop (24);
            keysLabel = keysRow.removeFromLeft (52);
            keys.setBounds (keysRow.removeFromLeft (130));

            auto amp = content[2].reduced (8, 0);

            for (auto* dial : { &attackDial, &releaseDial, &volumeDial })
                dial->setBounds (amp.removeFromLeft (dialWidth));

            auto edit = content[3].expanded (0, rowInset).reduced (14, 0);
            snapPill.setBounds (edit.removeFromTop (22).removeFromLeft (64));
            edit.removeFromTop (8);
            angles.setBounds (edit.removeFromTop (22).withWidth (170));
            edit.removeFromTop (4);
            rings.setBounds (edit.removeFromTop (22).withWidth (170));
        }

        // The patterns along the bottom, the clipboard at the end
        {
            auto strip = stripArea.reduced (8, 6);
            clearPill.setBounds (strip.removeFromRight (52).withSizeKeepingCentre (52, 22));
            strip.removeFromRight (6);
            pastePill.setBounds (strip.removeFromRight (52).withSizeKeepingCentre (52, 22));
            strip.removeFromRight (6);
            copyPill.setBounds (strip.removeFromRight (52).withSizeKeepingCentre (52, 22));
            strip.removeFromRight (12);
            const auto cellWidth = strip.getWidth() / numPatterns;

            for (auto& cell : cells)
                cell->setBounds (strip.removeFromLeft (cellWidth).withTrimmedRight (3));
        }
    }

private:
    void timerCallback() override
    {
        const auto picked = patternIndexOf (processor);

        if (picked != shownPattern)
        {
            shownPattern = picked;
            view.clearSelection();
            view.repaint();
            updateActions();
        }

        if (std::exchange (shownWaveform, session.waveformRevision) != session.waveformRevision)
        {
            view.waveformChanged();
            repaint (headerArea);
        }

        if (std::exchange (shownPlayback, session.playbackRevision) != session.playbackRevision)
            view.repaint();

        if (std::exchange (shownPatterns, session.patternsRevision) != session.patternsRevision)
        {
            // Changed elsewhere (the other page, the host's undo, a loaded state): this history no longer fits
            undo.clear();
            redo.clear();

            // The selection may point nowhere now
            const auto count = (int) view.pattern().segments.size();
            view.selection.erase (std::remove_if (view.selection.begin(), view.selection.end(), [count] (int i) { return i >= count; }), view.selection.end());
            view.repaint();
            updateActions();
        }

        if (std::exchange (shownLoading, session.loading || session.dragging) != (session.loading || session.dragging))
            view.repaint();

        for (auto& cell : cells)
            cell->refresh();

        // Free follows the transport: the notes' Attack and Release do nothing then
        const auto free = processor.state.getRawParameterValue ("play")->load() > 0.5f;

        for (auto* dial : { &attackDial, &releaseDial })
        {
            dial->setEnabled (! free);
            dial->setAlpha (free ? 0.35f : 1.0f);
        }
    }

    void chooseTool (RadarView::Tool tool)
    {
        view.setTool (tool);
        selectTool.setOn (tool == RadarView::Tool::select);
        lineTool.setOn (tool == RadarView::Tool::line);
        arcTool.setOn (tool == RadarView::Tool::arc);
        curveTool.setOn (tool == RadarView::Tool::curve);
    }

    void updateActions()
    {
        const auto any = ! view.pattern().segments.empty();
        duplicateAction.setEnabledLook (any);
        rotateAction.setEnabledLook (any || ! (view.pattern().origin == Point {}));
        flipHorizontalAction.setEnabledLook (any || ! (view.pattern().origin == Point {}));
        flipVerticalAction.setEnabledLook (any || ! (view.pattern().origin == Point {}));
        originAction.setEnabledLook (! (view.pattern().origin == Point {}));
        deleteAction.setEnabledLook (! view.selection.empty());
        undoPill.setEnabledLook (! undo.empty());
        redoPill.setEnabledLook (! redo.empty());
    }

    void patternEdited()
    {
        if (session.sendPattern)
            session.sendPattern (patternIndexOf (processor));

        cells[(size_t) patternIndexOf (processor)]->refresh();
        updateActions();
    }

    struct Step
    {
        int index;
        Pattern pattern;
    };

    void pushUndo (const Pattern& previous)
    {
        undo.push_back ({ patternIndexOf (processor), previous });

        if (undo.size() > 200)
            undo.erase (undo.begin());

        redo.clear();
        updateActions();
    }

    /** Restores the last step of one history, the pattern it replaces going to the other. */
    void undoOrRedo (std::vector<Step>& from, std::vector<Step>& to)
    {
        if (from.empty())
            return;

        auto step = std::move (from.back());
        from.pop_back();
        auto& pattern = processor.patterns[(size_t) step.index];
        to.push_back ({ step.index, pattern });
        pattern = std::move (step.pattern);

        if (session.sendPattern)
            session.sendPattern (step.index);

        // The step may be in another pattern: show that one
        if (step.index != patternIndexOf (processor))
        {
            auto& parameter = parameterOf (processor, "pattern");
            gesture (parameter, parameter.convertTo0to1 ((float) (step.index + 1)));
        }

        view.clearSelection();
        view.repaint();
        updateActions();
    }

    void replacePattern (const Pattern& replacement)
    {
        const auto previous = view.pattern();

        if (previous == replacement)
            return;

        view.pattern() = replacement;
        view.clearSelection();
        pushUndo (previous);
        patternEdited();
        view.repaint();
    }

    Processor& processor;
    Session& session;
    RadarView view;
    IconButton selectTool, lineTool, arcTool, curveTool;
    IconButton duplicateAction, rotateAction, flipHorizontalAction, flipVerticalAction, originAction, deleteAction;
    Dial barsDial, grainDial, pitchDial, attackDial, releaseDial, volumeDial;
    Choice keys, play;
    Pill snapPill, loadPill, undoPill, redoPill, copyPill, pastePill, clearPill;
    Stepper angles, rings;
    std::vector<std::unique_ptr<PatternCell>> cells;

    std::vector<Step> undo, redo;
    std::optional<Pattern> clipboard;

    juce::Rectangle<int> headerArea, sampleArea, toolArea, viewArea, sideArea, stripArea;
    juce::Rectangle<int> loopArea, modeArea, ampArea, snapArea, keysLabel, playLabel;
    int toolDivider = 0;
    int shownPattern = -1, shownWaveform = -1, shownPlayback = -1, shownPatterns = -1;
    bool shownLoading = false;
};

//==============================================================================
/** The device panel: the playing pattern's circle, the patterns to pick, the main dials. */
class PanelEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    int getControlParameterIndex (juce::Component& component) override { return controlParameterIndex (component); }

    PanelEditor (Processor& p, Session& s)
        : AudioProcessorEditor (p), processor (p), session (s), view (p, s, false),
          barsDial (p.state, "bars", "Bars", colours::teal, bars(), false, 30),
          grainDial (p.state, "grain", "Grain", colours::orange, milliseconds(), false, 30),
          pitchDial (p.state, "pitch", "Pitch", colours::violet, semitones(), true, 30),
          volumeDial (p.state, "volume", "Volume", colours::teal, decibels(), false, 30),
          play (*dynamic_cast<juce::AudioParameterChoice*> (p.state.getParameter ("play")))
    {
        addAndMakeVisible (play);
        edit.onClick = [this] { if (session.openWindow) session.openWindow(); };
        addAndMakeVisible (edit);
        // The pattern that plays (a note may pick it), else the picked one
        view.shownPattern = [this]
        {
            return session.playback.playing != 0 ? juce::jlimit (0, numPatterns - 1, (int) session.playback.pattern)
                                                 : patternIndexOf (processor);
        };
        addAndMakeVisible (view);

        for (auto* dial : { &barsDial, &grainDial, &pitchDial, &volumeDial })
            addAndMakeVisible (*dial);

        for (int i = 0; i < numPatterns; ++i)
            addAndMakeVisible (cells.emplace_back (std::make_unique<PatternCell> (p, s, i, true)).get());

        setSize (width, height);
        startTimerHz (30);
    }

    static constexpr int width = RADAR_PANEL_WIDTH, height = RADAR_PANEL_HEIGHT;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::background);
        g.setColour (colours::panel);
        g.fillRect (sideArea);
        g.setFont (font (11.0f, Face::italic));
        g.setColour (session.waveform.frames > 0 ? colours::label : colours::caption);
        g.drawText (sampleText (session.waveform), nameArea, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        view.setBounds (bounds.removeFromLeft (height).reduced (2));
        bounds.removeFromLeft (2);
        sideArea = bounds;

        auto side = bounds.reduced (8, 6);
        side.removeFromTop (2);
        nameArea = side.removeFromTop (16);
        edit.setBounds (nameArea.removeFromRight (40).withSizeKeepingCentre (40, 16));
        nameArea.removeFromRight (4);
        play.setBounds (nameArea.removeFromRight (84));
        side.removeFromTop (4);
        auto grid = side.removeFromTop (60);
        const auto cellWidth = grid.getWidth() / 6;

        for (int row = 0; row < 2; ++row)
        {
            auto line = grid.removeFromTop (30);

            for (int column = 0; column < 6; ++column)
                cells[(size_t) (row * 6 + column)]->setBounds (line.removeFromLeft (cellWidth).withTrimmedRight (2).withTrimmedBottom (2));
        }

        side.removeFromTop (6);
        auto dials = side;
        const auto dialWidth = dials.getWidth() / 4;

        for (auto* dial : { &barsDial, &grainDial, &pitchDial, &volumeDial })
            dial->setBounds (dials.removeFromLeft (dialWidth));
    }

private:
    void timerCallback() override
    {
        if (std::exchange (shownWaveform, session.waveformRevision) != session.waveformRevision)
        {
            view.waveformChanged();
            repaint (nameArea);
        }

        const auto picked = patternIndexOf (processor);
        const auto changed = std::exchange (shownPlayback, session.playbackRevision) != session.playbackRevision
                             || std::exchange (shownPatterns, session.patternsRevision) != session.patternsRevision
                             || std::exchange (shownPattern, picked) != picked
                             || std::exchange (shownLoading, session.loading || session.dragging) != (session.loading || session.dragging);

        if (changed)
            view.repaint();

        for (auto& cell : cells)
            cell->refresh();
    }

    Processor& processor;
    Session& session;
    RadarView view;
    Dial barsDial, grainDial, pitchDial, volumeDial;
    Choice play;
    Pill edit { "Edit" };
    std::vector<std::unique_ptr<PatternCell>> cells;
    juce::Rectangle<int> sideArea, nameArea;
    int shownWaveform = -1, shownPlayback = -1, shownPatterns = -1, shownPattern = -1;
    bool shownLoading = false;
};
} // namespace

juce::AudioProcessorEditor* createWindowEditor (Processor& processor, Session& session)
{
    return new WindowEditor (processor, session);
}

juce::AudioProcessorEditor* createPanelEditor (Processor& processor, Session& session)
{
    return new PanelEditor (processor, session);
}
} // namespace radar
