/*
    juce_webclap: the DSP side. Wraps a juce::AudioProcessor as a WebCLAP plugin (module.wasm).

    juce_webclap_clap.cpp exports `clap_entry` with one plugin. It creates the processor through JUCE's usual
    createPluginFilter() and implements clap.params, clap.state, clap.audio-ports, clap.note-ports, clap.tail,
    clap.gui (webview API only) and clap.webview/3. The webview page runs the editor as ui.wasm and talks to
    this side with the frames of juce_webclap_protocol.h.

    The module has no threads, so every CLAP call arrives on the same thread. JUCE's message queue and timers
    are pumped from the plugin's calls (process, flush, receive, on_main_thread), which keeps code that defers
    work with callAsync or Timer::callAfterDelay working without a message thread.

    The port defines getClapPluginInfo() and createPluginFilter(), and compiles juce_webclap_clap.cpp with the
    JUCE modules juce_core (juce_core_wasm.cpp), juce_events (juce_events_wasm.cpp), juce_data_structures,
    juce_audio_basics and juce_audio_processors_headless. No GUI module is needed. With ClapPluginInfo::devicePanel
    it also implements opendaw.device-panel/1.

    A port whose editor needs more than parameters, state and keyboard notes (a meter, a setting kept outside
    the state) also defines createPageExtension().
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace juce
{
    class AudioProcessor;
}

namespace juce::webclap
{
    struct ClapPluginInfo
    {
        const char* id;
        const char* name;
        const char* vendor;
        const char* url;
        const char* version;
        const char* description;
        const char* const* features; // null-terminated, e.g. { CLAP_PLUGIN_FEATURE_INSTRUMENT, ..., nullptr }

        const char* webviewUri = "/ui/index.html"; // page in the bundle, see clap.webview get_uri
        int editorWidth = 800;                     // window size until the page reports the editor's own
        int editorHeight = 600;

        /** openDAW's device panel (opendaw.device-panel/1): the window's page, its width scaled to the panel's
            height from the editor's size. List the feature "opendaw:device-panel" too. */
        bool devicePanel = false;
    };

    /** Defined by the port. */
    const ClapPluginInfo& getClapPluginInfo();

    /** The DSP side of what a port's editor exchanges besides parameters and state, in frames of the port's
        own types (protocol::firstPluginType and up). The UI side is ProcessorBridge::sendPluginFrame and
        ProcessorBridge::onPluginFrame. Everything runs on the plugin's (only) thread. */
    struct PageExtension
    {
        using Sender = std::function<void (std::uint8_t type, const void* payload, std::size_t size)>;

        virtual ~PageExtension() = default;

        /** A port frame from the page. Parameter changes made here go to the host, not back to the page (it
            made them on its stand-in already); updateHostDisplay with nonParameterStateChanged marks the host's
            state dirty. */
        virtual void receive (AudioProcessor&, std::uint8_t type, const void* payload, std::size_t size) {}

        /** Called about 30 times a second while the page is open: send it what it shows (meters). */
        virtual void update (AudioProcessor&, const Sender& send) {}

        /** A page said hello: it knows nothing yet, the next update sends everything, not only what changed. */
        virtual void opened() {}

        /** The page went away (clap.gui destroy): stop what only it needs, such as feeding an analyzer. */
        virtual void closed (AudioProcessor&) {}
    };

    /** Defined by ports that need it. The kit's default returns nullptr. */
    std::unique_ptr<PageExtension> createPageExtension();
}
