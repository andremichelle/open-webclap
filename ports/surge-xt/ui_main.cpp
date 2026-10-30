/*
    Surge XT editor as ui.wasm.

    The stand-in processor is Surge's own SurgeSynthProcessor, a full engine that never plays: the editor reads and
    writes SurgeSynthesizer and SurgePatch directly far beyond parameters (oscillator and LFO displays run the
    engine's code), so nothing less would do. Parameters and macros cross over through the kit's bridge; the glue
    for the rest lives here:

      - the editor creates itself once the DSP side's snapshot is in (or after a second without an answer), so it
        opens on the patch the DSP side plays
      - the engine never processes, so what process() would do for the editor runs here every frame: queued
        patch loads (the patch browser), and the "patch changed" notice, which sends the whole patch to the DSP side
        (patches/surge-webclap.patch raises it for loads without audio). A patch from the DSP side raises it
        too; that one is not sent back.
      - the zoom: sent as a frame when it changes, and asked for at start, so a page that reconnects opens at the
        zoom it had (surge_frames.h). The editor is created once that answer is in, too.
      - modulation routings: a listener on the engine's modulation API sends them as frames (surge_frames.h)
      - the meters: the DSP side sends its VU peaks and load, which go into this engine for the editor to show. As
        long as they arrive, it counts as running audio (Surge's own idle check turns it off when they stop).
      - the skin is Surge's dark one until the user picks another (user settings live in memory, at /user)

    Exports (called from webclap-ui.js):
        wclap_ui_init, wclap_ui_frame, wclap_ui_set_desktop, wclap_ui_mouse, wclap_ui_wheel, wclap_ui_key,
        wclap_ui_focus, wclap_ui_clipboard, wclap_ui_receive, wclap_ui_framebuffer, wclap_ui_dirty_rects,
        wclap_ui_describe, wclap_ui_value_text
*/

#include <emscripten.h>

#include <juce_webclap/juce_webclap.h>
#include <juce_webclap/juce_webclap_bridge.h>

#include "SurgeSynthEditor.h"
#include "SurgeSynthProcessor.h"
#include "UserDefaults.h"
#include "gui/SkinSupport.h"
#include "surge_frames.h"
#include "WebFonts.h"

EM_JS (void, wclap_js_send, (const void* data, int size), {
    const host = Module["webclapHost"];
    if (host && host.send) host.send (HEAPU8.slice (data, data + size));
});

namespace
{
/** The plugin window: follows the editor's size, with a scale transform applied (Surge's zoom). */
struct Window final : juce::Component, private juce::ComponentListener
{
    explicit Window (std::unique_ptr<juce::AudioProcessorEditor> e) : editor (std::move (e))
    {
        addAndMakeVisible (*editor);
        editor->addComponentListener (this);
        fit();
    }

    ~Window() override { editor->removeComponentListener (this); }

    void fit()
    {
        editor->setTopLeftPosition (0, 0);
        const auto bounds = editor->getBoundsInParent();
        setSize (bounds.getRight(), bounds.getBottom());
    }

    void componentMovedOrResized (juce::Component&, bool, bool) override { fit(); }

    std::unique_ptr<juce::AudioProcessorEditor> editor;
};

/** Sends the editor's modulation routings to the DSP side. */
struct ModulationForwarder final : SurgeSynthesizer::ModulationAPIListener
{
    std::function<void (juce::uint8, const void*, size_t)> send;

    void modSet (long ptag, modsources source, int scene, int index, float value, bool) override
    {
        forward (surge_webclap::modSet, ptag, source, scene, index, [&] (auto& f) { f.depth01 = value; });
    }

    void modMuted (long ptag, modsources source, int scene, int index, bool mute) override
    {
        forward (surge_webclap::modMute, ptag, source, scene, index, [&] (auto& f) { f.mute = mute ? 1 : 0; });
    }

    void modCleared (long ptag, modsources source, int scene, int index) override
    {
        forward (surge_webclap::modClear, ptag, source, scene, index, [] (auto&) {});
    }

    template <typename Fill>
    void forward (juce::uint8 type, long ptag, modsources source, int scene, int index, Fill&& fill)
    {
        surge_webclap::ModulationFrame f {};
        f.ptag = (int32_t) ptag;
        f.modsource = (int32_t) source;
        f.modsourceScene = (int32_t) scene;
        f.index = (int32_t) index;
        fill (f);
        send (type, &f, type == surge_webclap::modClear ? surge_webclap::routingSize : sizeof (f));
    }
};

struct App
{
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juce;
    std::unique_ptr<SurgeSynthProcessor> processor;
    std::unique_ptr<juce::webclap::ProcessorBridge> bridge;
    std::unique_ptr<Window> window;
    ModulationForwarder modulation;
    int lastWidth = 0, lastHeight = 0;
    int32_t sentZoom = -1, keptZoom = -1;
    bool zoomAnswered = false;
    double pixelRatio = 1.0, startTime = -1.0;
};

App* app = nullptr;

void sendToHost (const void* data, size_t size) { wclap_js_send (data, (int) size); }

/** The editor's zoom in percent, where it streams it: the patch's dawExtraState. */
int32_t currentZoom()
{
    auto* editor = app->window != nullptr ? dynamic_cast<SurgeSynthEditor*> (app->window->editor.get()) : nullptr;

    if (editor == nullptr)
        return -1;

    auto& synth = *app->processor->surge;
    auto& des = synth.storage.getPatch().dawExtraState;
    const auto wasPopulated = des.isPopulated;
    editor->populateForStreaming (&synth);
    des.isPopulated = wasPopulated;
    return (int32_t) des.editor.instanceZoomFactor;
}

void sendZoomIfChanged()
{
    const auto percent = currentZoom();

    if (percent > 0 && percent != app->sentZoom)
    {
        app->sentZoom = percent;
        app->bridge->sendPluginFrame (surge_webclap::zoom, &percent, sizeof (percent));
    }
}

void createWindow()
{
    // Where the editor looks for its instance's zoom when it is constructed
    if (app->keptZoom > 0)
        app->processor->surge->storage.getPatch().dawExtraState.editor.instanceZoomFactor = app->keptZoom;

    std::unique_ptr<juce::AudioProcessorEditor> editor (app->processor->createEditorIfNeeded());

    if (editor == nullptr)
        return;

    app->window = std::make_unique<Window> (std::move (editor));
    app->lastWidth = app->window->getWidth();
    app->lastHeight = app->window->getHeight();
    juce::webclap::setDesktop (app->lastWidth, app->lastHeight, app->pixelRatio);

    app->window->addToDesktop (0);
    app->window->setVisible (true);
    app->bridge->sendResize (app->lastWidth, app->lastHeight);
    app->sentZoom = currentZoom(); // the one it opened at, which the DSP side has (or the default)
}

/** What the engine's process() does for the editor, once per frame. */
void runEngineChores()
{
    auto& synth = *app->processor->surge;

    // Patch browser loads are queued for the audio thread; with none, they run here, also while the meters say
    // audio is running (that is the DSP side's audio, this engine never plays)
    synth.processAudioThreadOpsWhenAudioEngineUnavailable (true);

    // A new patch in the editor: the DSP side gets all of it (ProcessorBridge sends the state). Not before the DSP
    // side's own patch is in: until then the editor's engine has a patch of its own, which must not replace it.
    if (app->bridge->getSnapshotsReceived() == 0)
        return;

    if (synth.patchChanged.exchange (false))
        app->processor->updateHostDisplay (juce::AudioProcessorListener::ChangeDetails()
                                               .withNonParameterStateChanged (true));
}
} // namespace

extern "C"
{
EMSCRIPTEN_KEEPALIVE void wclap_ui_set_screen (int width, int height)
{
    juce::webclap::setScreenSize (width, height);
}

EMSCRIPTEN_KEEPALIVE int wclap_ui_init (double pixelRatio)
{
    if (app != nullptr)
        return 1;

    app = new App();
    app->pixelRatio = pixelRatio;

    juce::webclap::registerFont (WebFonts::DejaVuSans_ttf, (size_t) WebFonts::DejaVuSans_ttfSize, true);
    juce::webclap::registerFont (WebFonts::DejaVuSansBold_ttf, (size_t) WebFonts::DejaVuSansBold_ttfSize);
    juce::webclap::setDesktop (16, 16, pixelRatio);

    app->juce = std::make_unique<juce::ScopedJuceInitialiser_GUI>();

    app->processor = std::make_unique<SurgeSynthProcessor>();
    app->processor->setRateAndBufferSizeDetails (48000.0, 512);
    app->processor->prepareToPlay (48000.0, 512);
    app->processor->surge->patchChanged = false; // the init patch it loaded, not an edit

    auto* storage = &app->processor->surge->storage;

    if (Surge::Storage::getUserDefaultValue (storage, Surge::Storage::DefaultSkin, "").empty())
    {
        Surge::Storage::updateUserDefaultValue (storage, Surge::Storage::DefaultSkin, "dark-mode.surge-skin/");
        Surge::Storage::updateUserDefaultValue (storage, Surge::Storage::DefaultSkinRootType,
                                                (int) Surge::GUI::FACTORY);
    }

    app->bridge = std::make_unique<juce::webclap::ProcessorBridge> (*app->processor, sendToHost);
    app->bridge->onRemoteState = [] { app->processor->surge->patchChanged = false; };

    app->modulation.send = [] (juce::uint8 type, const void* data, size_t size)
    {
        app->bridge->sendPluginFrame (type, data, size);
    };
    app->processor->surge->addModulationAPIListener (&app->modulation);

    app->bridge->onPluginFrame = [] (juce::uint8 type, const juce::uint8* payload, size_t size)
    {
        if (type == surge_webclap::zoom && size == sizeof (int32_t))
        {
            std::memcpy (&app->keptZoom, payload, sizeof (int32_t));
            app->zoomAnswered = true;
        }
        else if (type == surge_webclap::meters && size == sizeof (surge_webclap::MetersFrame))
        {
            surge_webclap::MetersFrame m;
            std::memcpy (&m, payload, sizeof (m));
            auto& synth = *app->processor->surge;
            std::memcpy (synth.vu_peak, m.vuPeak, sizeof (m.vuPeak));
            synth.cpu_level.store (m.cpuLevel);
            synth.audio_processing_active = true;
            synth.processRunning = 0;
        }
    };

    // The editor follows in wclap_ui_frame, once the DSP side's patch is in
    app->bridge->sendHello();

    const int32_t ask = 0;
    app->bridge->sendPluginFrame (surge_webclap::zoom, &ask, sizeof (ask));
    return 1;
}

/** Returns the number of dirty rectangles to copy to the canvas. */
EMSCRIPTEN_KEEPALIVE int wclap_ui_frame (double timeMs)
{
    if (app == nullptr)
        return 0;

    if (app->startTime < 0)
        app->startTime = timeMs;

    runEngineChores();

    const auto answered = app->bridge->getSnapshotsReceived() > 0 && app->zoomAnswered;

    if (app->window == nullptr && (answered || timeMs - app->startTime > 1000.0))
        createWindow();

    const auto changed = juce::webclap::tick (timeMs);
    app->bridge->flush();

    if (app->window != nullptr
        && (app->window->getWidth() != app->lastWidth || app->window->getHeight() != app->lastHeight))
    {
        app->lastWidth = app->window->getWidth();
        app->lastHeight = app->window->getHeight();
        app->bridge->sendResize (app->lastWidth, app->lastHeight);
        sendZoomIfChanged();
    }

    return changed ? (int) juce::webclap::getDirtyRects().size() : 0;
}

EMSCRIPTEN_KEEPALIVE void wclap_ui_set_desktop (int width, int height, double pixelRatio)
{
    if (app != nullptr)
        app->pixelRatio = pixelRatio;

    juce::webclap::setDesktop (width, height, pixelRatio);
}

EMSCRIPTEN_KEEPALIVE const uint8_t* wclap_ui_framebuffer() { return juce::webclap::getFramebuffer(); }
EMSCRIPTEN_KEEPALIVE int wclap_ui_framebuffer_width()      { return juce::webclap::getFramebufferWidth(); }
EMSCRIPTEN_KEEPALIVE int wclap_ui_framebuffer_height()     { return juce::webclap::getFramebufferHeight(); }

/** x, y, width, height per rectangle, physical pixels. */
EMSCRIPTEN_KEEPALIVE const int* wclap_ui_dirty_rects()
{
    return reinterpret_cast<const int*> (juce::webclap::getDirtyRects().data());
}

EMSCRIPTEN_KEEPALIVE void wclap_ui_mouse (int type, float x, float y, int buttons, int modifiers)
{
    juce::webclap::mouse ((juce::webclap::MouseEventType) type, x, y, buttons, modifiers);
}

EMSCRIPTEN_KEEPALIVE int wclap_ui_wheel (float x, float y, float deltaX, float deltaY, int isSmooth, int modifiers)
{
    return juce::webclap::wheel (x, y, deltaX, deltaY, isSmooth != 0, modifiers) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int wclap_ui_key (int isDown, int keyCode, int textCharacter, int modifiers)
{
    return juce::webclap::key (isDown != 0, keyCode, (juce::juce_wchar) textCharacter, modifiers) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void wclap_ui_focus (int hasFocus) { juce::webclap::focus (hasFocus != 0); }

EMSCRIPTEN_KEEPALIVE void wclap_ui_clipboard (const char* utf8)
{
    juce::webclap::setClipboardText (juce::String::fromUTF8 (utf8));
}

/** A protocol frame from the DSP side (relayed by the host). */
EMSCRIPTEN_KEEPALIVE void wclap_ui_receive (const uint8_t* data, int size)
{
    if (app != nullptr)
        app->bridge->receive (data, (size_t) size);
}

/** Parameter list as JSON, for test hosts that have no DSP module to ask. Not part of the protocol. */
EMSCRIPTEN_KEEPALIVE const char* wclap_ui_describe()
{
    static std::string json;

    if (app == nullptr)
        return "[]";

    juce::Array<juce::var> list;

    for (auto* parameter : app->processor->getParameters())
    {
        auto* object = new juce::DynamicObject();
        object->setProperty ("id", (juce::int64) juce::webclap::ProcessorBridge::clapIdFor (*parameter));
        object->setProperty ("name", parameter->getName (64));
        object->setProperty ("value", parameter->getValue());
        object->setProperty ("text", parameter->getCurrentValueAsText());
        list.add (juce::var (object));
    }

    json = juce::JSON::toString (juce::var (list), true).toStdString();
    return json.c_str();
}

/** Display text for a normalised value, for the same test hosts. */
EMSCRIPTEN_KEEPALIVE const char* wclap_ui_value_text (uint32_t clapId, double value)
{
    static std::string text;

    if (app == nullptr)
        return "";

    if (auto* parameter = app->bridge->findParameter (clapId))
        text = parameter->getText ((float) value, 32).toStdString();
    else
        text.clear();

    return text.c_str();
}
}
