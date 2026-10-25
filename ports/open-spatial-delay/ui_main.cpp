/*
    OpenSpatialDelay editor as ui.wasm.

    The stand-in processor is OpenSpatialDelay's own processor, never processing audio, as in the RipplerX port.
    Parameters go through APVTS attachments, the configuration combos (algorithm, HRTF profile, output and input
    format) through state: the processor reports them as a non-parameter state change. The glue for the rest:

      - the spatial map and tap glow read trajectory positions and tap activity, which advance in processBlock:
        filled from the DSP side's motion, delta and noise frames
      - loading a preset: sent as a preset frame ahead of the preset's values, so the DSP side loads it itself and
        resets its trajectories, and a reopened editor shows the preset's name
      - the editor reads the configuration when it is constructed, so it is created once the DSP side's snapshot
        is in (or after a second without an answer), not before

    Exports (called from webclap-ui.js):
        wclap_ui_init, wclap_ui_frame, wclap_ui_set_desktop, wclap_ui_mouse, wclap_ui_wheel, wclap_ui_key,
        wclap_ui_focus, wclap_ui_clipboard, wclap_ui_receive, wclap_ui_framebuffer, wclap_ui_dirty_rects,
        wclap_ui_describe, wclap_ui_value_text
*/

#include <emscripten.h>

#include <juce_webclap/juce_webclap.h>
#include <juce_webclap/juce_webclap_bridge.h>

#include "osd_access.h"
#include "WebFonts.h"

EM_JS (void, wclap_js_send, (const void* data, int size), {
    const host = Module["webclapHost"];
    if (host && host.send) host.send (HEAPU8.slice (data, data + size));
});

namespace
{
struct App
{
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juce;
    std::unique_ptr<OpenSpatialDelayProcessor> processor;
    std::unique_ptr<juce::webclap::ProcessorBridge> bridge;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    int lastWidth = 0, lastHeight = 0;
    int knownPreset = -1;
    osd_webclap::MotionFrame motion {}; // what the DSP side sent, deltas apply to it
    double pixelRatio = 1.0, startTime = -1.0;
};

App* app = nullptr;

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

/** The glue that is not parameters, once per frame. Goes out now, the preset's values with the next flush. */
void syncPreset()
{
    const auto preset = app->processor->getCurrentPresetIndex();

    if (preset == app->knownPreset)
        return;

    app->knownPreset = preset;

    if (preset >= 0)
    {
        const auto index = (int32_t) preset;
        app->bridge->sendPluginFrame (osd_webclap::preset, &index, sizeof (index));
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

    app->processor = std::make_unique<OpenSpatialDelayProcessor>();
    app->processor->setRateAndBufferSizeDetails (48000.0, 512);
    app->processor->prepareToPlay (48000.0, 512);
    app->knownPreset = app->processor->getCurrentPresetIndex();

    app->bridge = std::make_unique<juce::webclap::ProcessorBridge> (*app->processor, sendToHost);
    app->bridge->onRemoteState = [] { app->knownPreset = app->processor->getCurrentPresetIndex(); };
    app->bridge->onPluginFrame = [] (juce::uint8 type, const juce::uint8* payload, size_t size)
    {
        if (type == osd_webclap::motion && size == sizeof (osd_webclap::MotionFrame))
        {
            std::memcpy (&app->motion, payload, sizeof (app->motion));
            OsdWebclapAccess::apply (*app->processor, app->motion);
        }
        else if (type == osd_webclap::delta && size % osd_webclap::deltaEntrySize == 0)
        {
            for (size_t offset = 0; offset < size; offset += osd_webclap::deltaEntrySize)
                if (payload[offset] < std::size (app->motion.values))
                    std::memcpy (&app->motion.values[payload[offset]], payload + offset + 1, sizeof (float));

            OsdWebclapAccess::apply (*app->processor, app->motion);
        }
        else if (type == osd_webclap::noise && size == sizeof (OsdWebclapAccess::NoiseFrame))
        {
            OsdWebclapAccess::applyNoise (*app->processor, payload);
        }
    };

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
    syncPreset();
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
