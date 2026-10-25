/*
    Odin 2 as module.wasm: the processor, wrapped by juce_webclap_clap.cpp. No editor code.

    PluginProcessor.cpp is compiled with ODIN_HEADLESS (patches/odin2-webclap.patch), which leaves out the editor.

    The PageExtension applies what the editor changes beside parameters (odin2_frames.h) to this processor, doing
    what the editor's direct calls into the processor do on desktop, where both share one processor:

      - a patch load runs readPatch here, then builds the drawn wavetables and resets the engine
        (the editor's forceValueTreeOntoComponents does both on desktop)
      - a tree property is set on the same child tree, so Odin's own tree listeners react. The ones without a
        listener get the editor's call: drawn tables are rebuilt, the FX order and the play mode are applied.
      - the tuning is replaced
    Every change marks the host's state dirty. The editor's zoom is kept here too, for pages that reconnect.
*/

#include <juce_webclap/juce_webclap_clap.h>

#include <clap/plugin-features.h>

#include "PluginProcessor.h"
#include "odin2_frames.h"

namespace
{
using namespace odin2_webclap;

String readString (const uint8*& p, const uint8* end)
{
    const auto* start = p;

    while (p < end && *p != 0)
        ++p;

    String s = String::fromUTF8 ((const char*) start, (int) (p - start));

    if (p < end)
        ++p;

    return s;
}

struct OdinPage final : juce::webclap::PageExtension
{
    void receive (AudioProcessor& processor, std::uint8_t type, const void* payload, std::size_t size) override
    {
        auto& p = static_cast<OdinAudioProcessor&> (processor);
        const auto* data = static_cast<const uint8*> (payload);
        const auto* end = data + size;

        if (type == readPatch)
        {
            const auto patch = ValueTree::readFromData (payload, size);

            if (! patch.isValid())
                return;

            p.readPatch (patch);
            pendingDrawTables = allDrawTables;
            p.resetAudioEngine();
        }
        else if (type == tree)
        {
            const auto childName = readString (data, end);
            const auto property = readString (data, end);
            MemoryInputStream in (data, (size_t) (end - data), false);
            const auto value = var::readFromStream (in);
            auto child = p.getValueTreeState().state.getChildWithName (childName);

            if (! child.isValid() || property.isEmpty())
                return;

            child.setProperty (property, value, nullptr);
            applyEditorCall (p, child, childName, property);
        }
        else if (type == zoom)
        {
            int32_t value = 0;

            if (size == sizeof (value))
                std::memcpy (&value, payload, sizeof (value));

            if (value == 0)
                zoomRequested = true;
            else if (value >= int (GuiScale::Z100) && value <= int (GuiScale::Z200))
                keptZoom = value;

            return; // a view setting, not state
        }
        else if (type == tuning)
        {
            const auto scl = readString (data, end);
            const auto kbm = readString (data, end);

            try
            {
                p.m_tuning = Tunings::Tuning (Tunings::parseSCLData (scl.toStdString()),
                                              Tunings::parseKBMData (kbm.toStdString()));
            }
            catch (...)
            {
                return; // the editor checked it already, it would not have sent a broken one
            }
        }
        else
        {
            return;
        }

        p.updateHostDisplay (AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
    }

    void update (AudioProcessor& processor, const Sender& send) override
    {
        auto& p = static_cast<OdinAudioProcessor&> (processor);

        // Drawing sends a property per moved point and frame: the tables are rebuilt here, not per property
        if (pendingDrawTables != 0)
            buildDrawTables (p, std::exchange (pendingDrawTables, 0));

        if (std::exchange (zoomRequested, false) && keptZoom != 0)
            send (zoom, &keptZoom, sizeof (keptZoom));

        const auto step = (int32_t) p.m_step_led_active.get();

        if (step != sentStep)
        {
            sentStep = step;
            send (arpStep, &step, sizeof (step));
        }

        auto& state = p.getValueTreeState();
        const float wheelValues[] = { state.getRawParameterValue ("pitchbend")->load(),
                                      state.getRawParameterValue ("modwheel")->load() };

        if (wheelValues[0] != sentWheels[0] || wheelValues[1] != sentWheels[1])
        {
            std::copy (std::begin (wheelValues), std::end (wheelValues), sentWheels);
            send (wheels, wheelValues, sizeof (wheelValues));
        }
    }

private:
    // One bit per oscillator and kind of drawn table
    enum DrawKind { wavedraw, chipdraw, specdraw };
    static constexpr int drawBit (int osc, DrawKind kind) { return 1 << (osc * 3 + kind); }
    static constexpr int allDrawTables = (1 << 9) - 1;

    void applyEditorCall (OdinAudioProcessor& p, const ValueTree& child, const String& childName, const String& property)
    {
        if (childName == "draw")
        {
            // osc<n>_<kind>_values_<i>
            const auto osc = property.substring (3, 4).getIntValue() - 1;

            if (isPositiveAndBelow (osc, 3))
            {
                if (property.contains ("_wavedraw_"))      pendingDrawTables |= drawBit (osc, wavedraw);
                else if (property.contains ("_chipdraw_")) pendingDrawTables |= drawBit (osc, chipdraw);
                else if (property.contains ("_specdraw_")) pendingDrawTables |= drawBit (osc, specdraw);
            }
        }
        else if (childName == "fx" && property.endsWith ("_position"))
        {
            p.setFXButtonsPosition ((int) (float) child["delay_position"],
                                    (int) (float) child["phaser_position"],
                                    (int) (float) child["flanger_position"],
                                    (int) (float) child["chorus_position"],
                                    (int) (float) child["reverb_position"]);
        }
        else if (childName == "misc" && property == "legato")
        {
            p.setMonoPolyLegato (VALUETREETOPLAYMODE ((int) child["legato"]));
        }
    }

    /** What OdinAudioProcessor::createDrawTablesFromValueTree does, for the given tables only. */
    static void buildDrawTables (OdinAudioProcessor& p, int tables)
    {
        const auto draw = p.getValueTreeState().state.getChildWithName ("draw");
        auto* container = p.getWavetableContainerPointer();
        float values[WAVEDRAW_STEPS_X];

        auto read = [&] (int osc, const char* kind, int steps)
        {
            for (int i = 0; i < steps; ++i)
                values[i] = (float) draw["osc" + String (osc + 1) + "_" + kind + "_values_" + String (i)];
        };

        for (int osc = 0; osc < 3; ++osc)
        {
            if (tables & drawBit (osc, wavedraw))
            {
                read (osc, "wavedraw", WAVEDRAW_STEPS_X);
                container->createWavedrawTable (osc, values, 44100);
            }

            if (tables & drawBit (osc, chipdraw))
            {
                read (osc, "chipdraw", CHIPDRAW_STEPS_X);
                container->createChipdrawTable (osc, values, 44100);
            }

            if (tables & drawBit (osc, specdraw))
            {
                read (osc, "specdraw", SPECDRAW_STEPS_X);
                container->createSpecdrawTable (osc, values, 44100);
            }
        }
    }

    void opened() override
    {
        sentStep = -2;
        sentWheels[0] = sentWheels[1] = -2.0f;
    }

    int pendingDrawTables = 0;
    int32_t keptZoom = 0;
    bool zoomRequested = false;
    int32_t sentStep = -2;
    float sentWheels[2] = { -2.0f, -2.0f };
};
} // namespace

std::unique_ptr<juce::webclap::PageExtension> juce::webclap::createPageExtension()
{
    return std::make_unique<OdinPage>();
}

const juce::webclap::ClapPluginInfo& juce::webclap::getClapPluginInfo()
{
    static const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                            "virtual analog", nullptr };

    // The CLAP id of Odin's own CLAP build (clap-juce-extensions), so hosts see the same plugin
    static const ClapPluginInfo info {
        "com.thewavewarden.odin2",
        "Odin2",
        "TheWaveWarden",
        "https://thewavewarden.com/odin2",
        "2.4.1",
        "24-voice polyphonic synthesizer, the editor runs in the page",
        features,
        "/ui/index.html",
        800, // the editor at the browser build's default 100 % zoom
        616
    };

    return info;
}
