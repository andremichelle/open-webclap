/*
    The ZL plugins' editor as ui.wasm, shared by the ZL ports: include it once from the port's ui_main.cpp after
    defining the port's hooks.

    The stand-in processor is the plugin's own PluginProcessor, never processing audio. Parameters go through the
    APVTS attachments as usual. The editor also keeps analyzer settings in a second APVTS on a dummy processor
    (parameters_NA_), saved in the processor's state: a change there reports a non-parameter state change, so the
    bridge sends the state over and the host stores it. What the editor reads from or tells the processor's
    controller directly is the port's glue, in frames of its own types:

        void zlPortInit (ZLApp&)                     after the bridge exists: install onPluginFrame
        void zlPortFrame (ZLApp&)                    every animation frame, before the bridge flushes

    PluginProcessor is the plugin's processor class (a using declaration for plugins that name it otherwise).
    ZL_SETTINGS_PROCESSOR(p) names the dummy processor of the settings the processor saves in its state,
    (p).dummy_processor_ by default; ports whose processor does not save them define ZL_NO_SETTINGS_STATE.

    The editor is created once the DSP side's snapshot is in (or after a second without an answer).

    Exports (called from webclap-ui.js):
        wclap_ui_init, wclap_ui_frame, wclap_ui_set_desktop, wclap_ui_mouse, wclap_ui_wheel, wclap_ui_key,
        wclap_ui_focus, wclap_ui_clipboard, wclap_ui_receive, wclap_ui_framebuffer, wclap_ui_dirty_rects,
        wclap_ui_describe, wclap_ui_value_text
*/

#pragma once

#include <emscripten.h>

#include <juce_webclap/juce_webclap.h>
#include <juce_webclap/juce_webclap_bridge.h>

#include "WebFonts.h"

#ifndef ZL_SETTINGS_PROCESSOR
 #define ZL_SETTINGS_PROCESSOR(p) (p).dummy_processor_
#endif

EM_JS (void, wclap_js_send, (const void* data, int size), {
    const host = Module["webclapHost"];
    if (host && host.send) host.send (HEAPU8.slice (data, data + size));
});

struct ZLApp;
void zlPortInit (ZLApp&);
void zlPortFrame (ZLApp&);

/** Reports changes of the editor's non-automatable settings (parameters_NA_) as non-parameter state. */
struct ZLStateWatcher final : juce::AudioProcessorParameter::Listener
{
    explicit ZLStateWatcher (juce::AudioProcessor& p) : processor (p) {}

    void parameterValueChanged (int, float) override
    {
        if (! muted)
            processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
    }

    void parameterGestureChanged (int, bool) override {}

    juce::AudioProcessor& processor;
    bool muted = false;
};

struct ZLApp
{
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juce;
    std::unique_ptr<PluginProcessor> processor;
    std::unique_ptr<juce::webclap::ProcessorBridge> bridge;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<ZLStateWatcher> stateWatcher;
    int lastWidth = 0, lastHeight = 0;
    double pixelRatio = 1.0, startTime = -1.0;
    double sampleRate = 48000.0;

    void send (juce::uint8 type, const void* payload, size_t size) { bridge->sendPluginFrame (type, payload, size); }

    /** The DSP side runs at another rate: the stand-in follows, so the editor draws its curves for it. */
    void followSampleRate (double rate)
    {
        if (rate < 8000.0 || std::abs (rate - sampleRate) < 0.5)
            return;

        sampleRate = rate;
        processor->setRateAndBufferSizeDetails (rate, 512);
        processor->prepareToPlay (rate, 512);
    }
};

namespace
{
ZLApp* app = nullptr;

void sendToHost (const void* data, size_t size) { wclap_js_send (data, (int) size); }

void createEditor()
{
    app->editor.reset (app->processor->createEditorIfNeeded());

    if (app->editor == nullptr)
        return;

    app->lastWidth = app->editor->getWidth();
    app->lastHeight = app->editor->getHeight();
    juce::webclap::setDesktop (app->lastWidth, app->lastHeight, app->pixelRatio);

    app->editor->addToDesktop (0);
    app->editor->setVisible (true);
    app->bridge->sendResize (app->lastWidth, app->lastHeight);
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

    app = new ZLApp();
    app->pixelRatio = pixelRatio;

    juce::webclap::registerFont (WebFonts::DejaVuSans_ttf, (size_t) WebFonts::DejaVuSans_ttfSize, true);
    juce::webclap::registerFont (WebFonts::DejaVuSansBold_ttf, (size_t) WebFonts::DejaVuSansBold_ttfSize);
    juce::webclap::setDesktop (16, 16, pixelRatio);

    app->juce = std::make_unique<juce::ScopedJuceInitialiser_GUI>();

    app->processor = std::make_unique<PluginProcessor>();
    app->processor->setRateAndBufferSizeDetails (app->sampleRate, 512);
    app->processor->prepareToPlay (app->sampleRate, 512);

    app->bridge = std::make_unique<juce::webclap::ProcessorBridge> (*app->processor, sendToHost);

    app->stateWatcher = std::make_unique<ZLStateWatcher> (*app->processor);

#ifndef ZL_NO_SETTINGS_STATE
    for (auto* parameter : ZL_SETTINGS_PROCESSOR (*app->processor).getParameters())
        parameter->addListener (app->stateWatcher.get());
#endif

    zlPortInit (*app);

    // The editor follows in wclap_ui_frame, once the DSP side's state is in
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

    if (app->editor == nullptr && (app->bridge->getSnapshotsReceived() > 0 || timeMs - app->startTime > 1000.0))
        createEditor();

    const auto changed = juce::webclap::tick (timeMs);
    zlPortFrame (*app);
    app->bridge->flush();

    if (app->editor != nullptr && (app->editor->getWidth() != app->lastWidth || app->editor->getHeight() != app->lastHeight))
    {
        app->lastWidth = app->editor->getWidth();
        app->lastHeight = app->editor->getHeight();
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

/** A protocol frame from the DSP side (relayed by the host). A state frame replaces the editor's settings
    without reporting them back. */
EMSCRIPTEN_KEEPALIVE void wclap_ui_receive (const uint8_t* data, int size)
{
    if (app == nullptr)
        return;

    app->stateWatcher->muted = true;
    app->bridge->receive (data, (size_t) size);
    app->stateWatcher->muted = false;
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
