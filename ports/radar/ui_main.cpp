/*
    Radar's editor as ui.wasm, the window's (radar_editor.h); the device panel is ui/panel.html. The stand-in
    processor is the port's own, never processing audio; the Session holds what the DSP side sends beside parameters
    (radar_frames.h). Inter is the default font.

    The page decodes a dropped or picked sample and hands it over (wclap_ui_page_input), the editor asks the page to
    pick one (host.toPage).

    Exports (called from webclap-ui.js):
        wclap_ui_init, wclap_ui_frame, wclap_ui_set_desktop, wclap_ui_mouse, wclap_ui_wheel,
        wclap_ui_key, wclap_ui_focus, wclap_ui_clipboard, wclap_ui_receive, wclap_ui_page_input,
        wclap_ui_framebuffer, wclap_ui_dirty_rects, wclap_ui_describe, wclap_ui_value_text
*/

#include <emscripten.h>

#include <juce_webclap/juce_webclap.h>
#include <juce_webclap/juce_webclap_bridge.h>
#include <juce_webclap/juce_webclap_hover.h>

#include "radar_editor.h"
#include "radar_frames.h"

bool radar::Processor::hasEditor() const { return true; }
juce::AudioProcessorEditor* radar::Processor::createEditor() { return nullptr; } // ui_main makes the editors
#include "WebFonts.h"

EM_JS (void, wclap_js_send, (const void* data, int size), {
    const host = Module["webclapHost"];
    if (host && host.send) host.send (HEAPU8.slice (data, data + size));
});

EM_JS (void, wclap_js_to_page, (const char* text, int size), {
    const host = Module["webclapHost"];
    if (host && host.toPage) host.toPage (new TextDecoder().decode (HEAPU8.slice (text, text + size)));
});

namespace
{
struct App
{
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juce;
    std::unique_ptr<radar::Processor> processor;
    radar::Session session;
    std::uint32_t pageId = 0;
    bool toldHostKeeps = false;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<juce::webclap::ProcessorBridge> bridge;
    std::unique_ptr<juce::webclap::ParameterHover> hover; // after the editor and the bridge: destroyed first
    double pixelRatio = 1.0;
    int lastWidth = 0, lastHeight = 0;
};

App* app = nullptr;

void sendToHost (const void* data, size_t size) { wclap_js_send (data, (int) size); }

void toPage (const juce::String& text) { wclap_js_to_page (text.toRawUTF8(), (int) text.getNumBytesAsUTF8()); }

void sendPattern (int index, bool live)
{
    juce::MemoryOutputStream out;
    out.writeInt ((int) app->pageId);
    out.writeByte ((char) (index | (live ? radar::Frame::live : 0)));
    app->processor->patterns[(size_t) index].write (out);
    app->bridge->sendPluginFrame (radar::Frame::pattern, out.getData(), out.getDataSize());
}

void receiveFrame (juce::uint8 type, const juce::uint8* payload, size_t size)
{
    auto& session = app->session;

    if (type == radar::Frame::pattern && size >= 5)
    {
        juce::MemoryInputStream in (payload, size, false);
        const auto source = (std::uint32_t) in.readInt();
        const auto index = (int) (std::uint8_t) in.readByte();
        radar::Pattern pattern;

        // What this page sent comes back to every page: it has it already, and may have changed it again since
        if (source != app->pageId && index < radar::numPatterns && pattern.read (in)
            && ! (pattern == app->processor->patterns[(size_t) index]))
        {
            app->processor->patterns[(size_t) index] = std::move (pattern);
            ++session.patternsRevision;
        }
    }
    else if (type == radar::Frame::waveform && size >= 14)
    {
        juce::MemoryInputStream in (payload, size, false);
        radar::Waveform waveform;
        waveform.frames = in.readInt();
        waveform.rate = in.readInt();
        waveform.channels = in.readShort();
        const auto nameBytes = (int) (std::uint16_t) in.readShort();
        juce::MemoryBlock name;
        in.readIntoMemoryBlock (name, nameBytes);
        waveform.name = juce::String::fromUTF8 ((const char*) name.getData(), (int) name.getSize());
        const auto bins = (int) (std::uint16_t) in.readShort();
        waveform.bins.resize ((size_t) bins * 2);

        if (in.read (waveform.bins.data(), bins * 2) == bins * 2)
        {
            // The host keeps samples: the page hands it its files (once is enough)
            if (const auto hostKeeps = ! in.isExhausted() && (in.readByte() & 1) != 0; hostKeeps && ! app->toldHostKeeps)
            {
                app->toldHostKeeps = true;
                toPage ("samples:host");
            }

            session.waveform = std::move (waveform);
            session.loading = false;
            ++session.waveformRevision;
        }
    }
    else if (type == radar::Frame::playback && size == sizeof (radar::Playback))
    {
        std::memcpy (&session.playback, payload, sizeof (radar::Playback));
        ++session.playbackRevision;
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

    juce::webclap::registerFont (WebFonts::InterRegular_ttf, (size_t) WebFonts::InterRegular_ttfSize, true);
    juce::webclap::setDesktop (16, 16, pixelRatio);

    app->juce = std::make_unique<juce::ScopedJuceInitialiser_GUI>();

    app->processor = std::make_unique<radar::Processor>();
    app->pageId = (std::uint32_t) juce::Random::getSystemRandom().nextInt (0x7fffffff) + 1;
    app->processor->setRateAndBufferSizeDetails (48000.0, 512);
    app->processor->prepareToPlay (48000.0, 512);

    app->bridge = std::make_unique<juce::webclap::ProcessorBridge> (*app->processor, sendToHost);

    app->session.sendPattern = [] (int index, bool live) { sendPattern (index, live); };
    app->session.openSampleFile = [] { toPage ("open-sample"); };
    app->editor.reset (radar::createWindowEditor (*app->processor, app->session));
    app->bridge->onPluginFrame = [] (juce::uint8 type, const juce::uint8* payload, size_t size) { receiveFrame (type, payload, size); };

    if (app->editor != nullptr)
        app->hover = std::make_unique<juce::webclap::ParameterHover> (*app->editor, *app->bridge);

    if (app->editor == nullptr)
        return 0;

    app->lastWidth = app->editor->getWidth();
    app->lastHeight = app->editor->getHeight();
    juce::webclap::setDesktop (app->lastWidth, app->lastHeight, pixelRatio);

    app->editor->addToDesktop (0);
    app->editor->setVisible (true);

    app->bridge->sendResize (app->lastWidth, app->lastHeight);

    app->bridge->sendHello();
    return 1;
}

/** Returns the number of dirty rectangles to copy to the canvas. */
EMSCRIPTEN_KEEPALIVE int wclap_ui_frame (double timeMs)
{
    if (app == nullptr)
        return 0;

    const auto changed = juce::webclap::tick (timeMs);
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

/** What the page has for the editor: 1 and a sample frame's payload (radar_frames.h), decoded from a dropped or
    picked file; 2 and 1 or 0 while a file is dragged over the page or not; 3 and the name of a file the page handed
    the host, which keeps the sample. */
EMSCRIPTEN_KEEPALIVE void wclap_ui_page_input (const uint8_t* data, int size)
{
    if (app == nullptr || size < 2)
        return;

    if (data[0] == 1)
    {
        app->session.loading = true;
        app->session.dragging = false;
        app->bridge->sendPluginFrame (radar::Frame::sample, data + 1, (size_t) size - 1);
    }
    else if (data[0] == 2)
    {
        app->session.dragging = data[1] != 0;
    }
    else if (data[0] == 3)
    {
        // The page handed a file to the host (opendaw.samples/1): its name goes to the DSP side, the frames come
        // from the host
        app->session.loading = true;
        app->session.dragging = false;
        app->bridge->sendPluginFrame (radar::Frame::sampleName, data + 1, (size_t) size - 1);
    }
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
