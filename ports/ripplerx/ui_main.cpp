/*
    RipplerX editor as ui.wasm.

    The stand-in processor is RipplerX's own RipplerXAudioProcessor, never processing audio, as in the OB-Xf port.
    The editor is parameter-driven (APVTS attachments); the glue for the rest lives here:

      - the on-screen keyboard plays into the processor's keyboardState: forwarded as midi frames
      - the output meter reads rmsValue: filled from the DSP side's meter frames
      - polyphony is a setting outside state and parameters: sent as a polyphony frame when it changes
      - picking a factory program: sent as a program frame ahead of the program's values, so the DSP side loads
        it the way a native host would (see ripplerx_frames.h), and a reopened editor shows the program's name
      - UI scale is a transform on the editor: the window takes the scaled size
      - the editor reads currentProgram only when it is constructed, so it is created once the DSP side's
        snapshot is in (or after a second without an answer), not before

    Exports (called from webclap-ui.js):
        wclap_ui_init, wclap_ui_frame, wclap_ui_set_desktop, wclap_ui_mouse, wclap_ui_wheel, wclap_ui_key,
        wclap_ui_focus, wclap_ui_clipboard, wclap_ui_receive, wclap_ui_framebuffer, wclap_ui_dirty_rects,
        wclap_ui_describe, wclap_ui_value_text
*/

#include <emscripten.h>

#include <juce_webclap/juce_webclap.h>
#include <juce_webclap/juce_webclap_bridge.h>

#include "PluginProcessor.h"
#include "ripplerx_frames.h"
#include "WebFonts.h"

EM_JS (void, wclap_js_send, (const void* data, int size), {
    const host = Module["webclapHost"];
    if (host && host.send) host.send (HEAPU8.slice (data, data + size));
});

namespace
{
/** The plugin window: the editor scales itself with a transform (UI Scale menu), the window follows. */
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
        const auto bounds = editor->getBoundsInParent(); // with the scale transform applied
        setSize (bounds.getRight(), bounds.getBottom());
    }

    void componentMovedOrResized (juce::Component&, bool, bool) override { fit(); }

    std::unique_ptr<juce::AudioProcessorEditor> editor;
};

struct App
{
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juce;
    std::unique_ptr<RipplerXAudioProcessor> processor;
    std::unique_ptr<juce::webclap::ProcessorBridge> bridge;
    std::unique_ptr<Window> window;
    int lastWidth = 0, lastHeight = 0;
    int sentPolyphony = -1, knownProgram = -1;
    float lastScale = 1.0f;
    double pixelRatio = 1.0, startTime = -1.0;
};

App* app = nullptr;

void sendToHost (const void* data, size_t size) { wclap_js_send (data, (int) size); }

void createWindow()
{
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
}

/** The glue that is not parameters, once per frame. */
void syncNonParameterState()
{
    auto& p = *app->processor;

    if (p.polyphony != app->sentPolyphony)
    {
        app->sentPolyphony = p.polyphony;
        const auto voices = (int32_t) p.polyphony;
        app->bridge->sendPluginFrame (ripplerx_webclap::polyphony, &voices, sizeof (voices));
    }

    // Goes out now, the program's values with the next flush
    if (p.currentProgram != app->knownProgram)
    {
        app->knownProgram = p.currentProgram;

        if (p.currentProgram >= 0)
        {
            const auto program = (int32_t) p.currentProgram;
            app->bridge->sendPluginFrame (ripplerx_webclap::program, &program, sizeof (program));
        }
    }

    // AudioProcessorEditor::setScaleFactor changes the transform without moving or resizing the editor
    if (p.scale != app->lastScale && app->window != nullptr)
    {
        app->lastScale = p.scale;
        app->window->fit();
    }
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

    app->processor = std::make_unique<RipplerXAudioProcessor>();
    app->processor->setRateAndBufferSizeDetails (48000.0, 512);
    app->processor->prepareToPlay (48000.0, 512);
    app->knownProgram = app->processor->currentProgram;
    app->lastScale = app->processor->scale;

    app->bridge = std::make_unique<juce::webclap::ProcessorBridge> (*app->processor, sendToHost);
    app->bridge->forwardKeyboard (app->processor->keyboardState);
    app->bridge->onRemoteState = [] { app->knownProgram = app->processor->currentProgram; };
    app->bridge->onPluginFrame = [] (juce::uint8 type, const juce::uint8* payload, size_t size)
    {
        if (type == ripplerx_webclap::meter && size == sizeof (float))
        {
            float rms;
            std::memcpy (&rms, payload, sizeof (rms));
            app->processor->rmsValue.store (rms, std::memory_order_release);
        }
    };

    // The editor follows in wclap_ui_frame, once the DSP side's patch is in
    app->bridge->sendHello();
    return 1;
}

/** Returns the number of dirty rectangles to copy to the canvas. */
EMSCRIPTEN_KEEPALIVE int wclap_ui_frame (double timeMs)
{
    if (app == nullptr)
        return 0;

    if (app->startTime < 0)
        app->startTime = timeMs;

    if (app->window == nullptr && (app->bridge->getSnapshotsReceived() > 0 || timeMs - app->startTime > 1000.0))
        createWindow();

    const auto changed = juce::webclap::tick (timeMs);
    syncNonParameterState();
    app->bridge->flush();

    if (app->window != nullptr && (app->window->getWidth() != app->lastWidth || app->window->getHeight() != app->lastHeight))
    {
        app->lastWidth = app->window->getWidth();
        app->lastHeight = app->window->getHeight();
        app->bridge->sendResize (app->lastWidth, app->lastHeight);
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
