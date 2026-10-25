/*
    juce_webclap: a juce::AudioProcessor as a WebCLAP plugin, see juce_webclap_clap.h.

    Parameters keep JUCE's normalised range (0..1) and use clapIdFor() as their CLAP id, so the editor's
    ui.wasm, this module and the host all name a parameter the same way.

    Who changed a parameter decides where the change goes:

        host (process/flush events, state load)   -> the page
        page (param frames)                       -> the host, as output events
        plugin (timers, MIDI CC, deferred loads)  -> the host and the page

    The page gets its updates from on_main_thread after host->request_callback(). A host that never calls
    back (a single-threaded host may not bother) gets them from process() instead, at most 30 times a second.
    The module has no threads, so this is the same thread either way.
*/

#include "juce_webclap_clap.h"
#include "juce_webclap_bridge.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>

#include <unordered_set>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace juce::webclap
{
int dispatchPendingMessages(); // juce_wasm_Messaging.cpp

/** Ports without a PageExtension get none (a port defines its own createPageExtension to replace this). */
__attribute__ ((weak)) std::unique_ptr<PageExtension> createPageExtension() { return nullptr; }

namespace
{
//==============================================================================
/** Runs what JUCE code queued for the message thread: callAsync messages and due timers. */
void pumpMessages()
{
    dispatchPendingMessages();
    Timer::callPendingTimersSynchronously();
}

class ClapPlayHead final : public AudioPlayHead
{
public:
    void set (const clap_event_transport_t* transport) { current = transport != nullptr ? *transport : clap_event_transport_t {}; valid = transport != nullptr; }

    Optional<PositionInfo> getPosition() const override
    {
        if (! valid)
            return {};

        PositionInfo info;
        const auto flags = current.flags;

        if ((flags & CLAP_TRANSPORT_HAS_TEMPO) != 0)
            info.setBpm (current.tempo);

        if ((flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0)
        {
            info.setPpqPosition ((double) current.song_pos_beats / (double) CLAP_BEATTIME_FACTOR);
            info.setPpqPositionOfLastBarStart ((double) current.bar_start / (double) CLAP_BEATTIME_FACTOR);
            info.setBarCount (current.bar_number);
        }

        if ((flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) != 0)
            info.setTimeInSeconds ((double) current.song_pos_seconds / (double) CLAP_SECTIME_FACTOR);

        if ((flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) != 0)
            info.setTimeSignature (TimeSignature { current.tsig_num, current.tsig_denom });

        info.setIsPlaying ((flags & CLAP_TRANSPORT_IS_PLAYING) != 0);
        info.setIsRecording ((flags & CLAP_TRANSPORT_IS_RECORDING) != 0);
        info.setIsLooping ((flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE) != 0);
        return info;
    }

private:
    clap_event_transport_t current {};
    bool valid = false;
};

//==============================================================================
class Plugin final : private AudioProcessorListener
{
public:
    Plugin (const clap_host_t* h, const clap_plugin_descriptor_t* descriptor) : host (h)
    {
        plugin.desc = descriptor;
        plugin.plugin_data = this;
        plugin.init = [] (const clap_plugin_t* p) { return self (p).init(); };
        plugin.destroy = [] (const clap_plugin_t* p) { delete &self (p); };
        plugin.activate = [] (const clap_plugin_t* p, double sr, uint32_t minFrames, uint32_t maxFrames) { return self (p).activate (sr, minFrames, maxFrames); };
        plugin.deactivate = [] (const clap_plugin_t* p) { self (p).processor->releaseResources(); };
        plugin.start_processing = [] (const clap_plugin_t*) { return true; };
        plugin.stop_processing = [] (const clap_plugin_t*) {};
        plugin.reset = [] (const clap_plugin_t* p) { self (p).processor->reset(); };
        plugin.process = [] (const clap_plugin_t* p, const clap_process_t* process) { return self (p).process (*process); };
        plugin.get_extension = [] (const clap_plugin_t* p, const char* id) { return self (p).getExtension (id); };
        plugin.on_main_thread = [] (const clap_plugin_t* p) { self (p).onMainThread(); };
    }

    ~Plugin() override
    {
        if (processor != nullptr)
            processor->removeListener (this);
    }

    const clap_plugin_t* getClapPlugin() const { return &plugin; }

private:
    enum class Origin { plugin, host, page };

    struct OutEvent
    {
        uint16_t type;
        clap_id id;
        double value;
    };

    static Plugin& self (const clap_plugin_t* p) { return *static_cast<Plugin*> (p->plugin_data); }

    template <typename Extension>
    const Extension* hostExtension (const char* id) const
    {
        return host->get_extension != nullptr ? static_cast<const Extension*> (host->get_extension (host, id)) : nullptr;
    }

    //==============================================================================
    bool init()
    {
        MessageManager::getInstance();

        hostParams = hostExtension<clap_host_params_t> (CLAP_EXT_PARAMS);
        hostState = hostExtension<clap_host_state_t> (CLAP_EXT_STATE);
        hostGui = hostExtension<clap_host_gui_t> (CLAP_EXT_GUI);
        hostWebview = hostExtension<clap_host_webview_t> (CLAP_EXT_WEBVIEW);

        processor.reset (createPluginFilter());

        if (processor == nullptr)
            return false;

        processor->setPlayHead (&playHead);
        extension = createPageExtension();

        for (auto* parameter : processor->getParameters())
        {
            const auto id = clapIdFor (*parameter);
            parameters.push_back (parameter);
            parametersById[id] = parameter;
            idsByIndex.push_back (id);
        }

        const auto& info = getClapPluginInfo();
        editorWidth = (uint32_t) info.editorWidth;
        editorHeight = (uint32_t) info.editorHeight;

        processor->addListener (this);
        pumpMessages();
        return true;
    }

    bool activate (double sampleRate, uint32_t, uint32_t maxFrames)
    {
        const auto inputs = mainChannels (true), outputs = mainChannels (false);
        processor->setPlayConfigDetails (inputs, outputs, sampleRate, (int) maxFrames);
        processor->prepareToPlay (sampleRate, (int) maxFrames);

        channels.assign ((size_t) jmax (inputs, outputs), nullptr);
        scratch.setSize (jmax (inputs, outputs), (int) maxFrames);
        midi.ensureSize (4096);
        fallbackInterval = (int) (sampleRate / 30.0);
        extensionInterval = (int) (sampleRate / 30.0);
        return true;
    }

    int mainChannels (bool isInput) const
    {
        if (auto* bus = processor->getBus (isInput, 0))
            return bus->isEnabled() ? bus->getNumberOfChannels() : 0;

        return 0;
    }

    //==============================================================================
    clap_process_status process (const clap_process_t& process)
    {
        const auto frames = (int) process.frames_count;

        playHead.set (process.transport);
        midi.clear();

        if (process.in_events != nullptr)
            readEvents (*process.in_events, true);

        // Notes from the editor's keyboard, at the start of the block
        if (! pageMidi.isEmpty())
        {
            midi.addEvents (pageMidi, 0, -1, 0);
            pageMidi.clear();
        }

        // Audio buffers: the processor works in place on the outputs (extra input channels on scratch).
        const auto numOut = process.audio_outputs_count > 0 ? (int) process.audio_outputs[0].channel_count : 0;
        const auto numIn = process.audio_inputs_count > 0 ? (int) process.audio_inputs[0].channel_count : 0;
        const auto numChannels = jmin ((int) channels.size(), jmax (numIn, numOut));

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* out = ch < numOut ? process.audio_outputs[0].data32[ch] : scratch.getWritePointer (ch);
            const float* in = ch < numIn ? process.audio_inputs[0].data32[ch] : nullptr;

            if (in != nullptr && in != out)
                std::memcpy (out, in, sizeof (float) * (size_t) frames);
            else if (in == nullptr)
                std::fill (out, out + frames, 0.0f);

            channels[(size_t) ch] = out;
        }

        AudioBuffer<float> buffer (channels.data(), numChannels, frames);
        processor->processBlock (buffer, midi);

        pumpMessages();

        if (process.out_events != nullptr)
            writeEvents (*process.out_events);

        samplesSinceSend = jmin (samplesSinceSend + (int) frames, extensionInterval);

        if (extension != nullptr && pageOpen)
        {
            samplesSinceExtensionUpdate += frames;

            if (samplesSinceExtensionUpdate >= extensionInterval)
            {
                samplesSinceExtensionUpdate = 0;
                extensionUpdateDue = true;
                requestPageUpdate();
            }
        }

        // Fallback for hosts that do not call on_main_thread: send page updates from here.
        if (pageUpdateRequested)
        {
            samplesSinceRequest += frames;

            if (samplesSinceRequest > fallbackInterval)
                sendPageUpdates();
        }

        return CLAP_PROCESS_CONTINUE;
    }

    void readEvents (const clap_input_events_t& events, bool inProcess)
    {
        const auto count = events.size (&events);

        for (uint32_t i = 0; i < count; ++i)
        {
            const auto* header = events.get (&events, i);

            if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID)
                continue;

            const auto time = (int) header->time;

            switch (header->type)
            {
                case CLAP_EVENT_PARAM_VALUE:
                {
                    const auto* event = reinterpret_cast<const clap_event_param_value_t*> (header);
                    setParameter (event->param_id, event->value, Origin::host);
                    break;
                }

                case CLAP_EVENT_NOTE_ON:
                case CLAP_EVENT_NOTE_OFF:
                case CLAP_EVENT_NOTE_CHOKE:
                {
                    if (! inProcess)
                        break;

                    const auto* event = reinterpret_cast<const clap_event_note_t*> (header);
                    const auto channel = jlimit (1, 16, event->channel + 1);
                    const auto key = jlimit (0, 127, (int) event->key);
                    const auto velocity = (float) jlimit (0.0, 1.0, event->velocity);

                    if (event->key < 0)
                        midi.addEvent (MidiMessage::allNotesOff (channel), time);
                    else if (header->type == CLAP_EVENT_NOTE_ON)
                        midi.addEvent (MidiMessage::noteOn (channel, key, jmax (velocity, 1.0f / 127.0f)), time);
                    else
                        midi.addEvent (MidiMessage::noteOff (channel, key, velocity), time);

                    break;
                }

                case CLAP_EVENT_MIDI:
                {
                    if (! inProcess)
                        break;

                    const auto* event = reinterpret_cast<const clap_event_midi_t*> (header);
                    const auto size = MidiMessage::getMessageLengthFromFirstByte (event->data[0]);
                    midi.addEvent (event->data, jlimit (1, 3, size), time);
                    break;
                }

                default:
                    break;
            }
        }
    }

    void writeEvents (const clap_output_events_t& events)
    {
        for (const auto& out : outEvents)
        {
            if (out.type == CLAP_EVENT_PARAM_VALUE)
            {
                clap_event_param_value_t event {};
                event.header = { sizeof (event), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
                event.param_id = out.id;
                event.note_id = -1;
                event.port_index = -1;
                event.channel = -1;
                event.key = -1;
                event.value = out.value;
                events.try_push (&events, &event.header);
            }
            else
            {
                clap_event_param_gesture_t event {};
                event.header = { sizeof (event), 0, CLAP_CORE_EVENT_SPACE_ID, out.type, 0 };
                event.param_id = out.id;
                events.try_push (&events, &event.header);
            }
        }

        outEvents.clear();
    }

    /** Sets a parameter the way JUCE's plugin wrappers apply host automation, remembering who changed it. */
    void setParameter (clap_id id, double value, Origin from)
    {
        if (auto* parameter = findParameter (id))
        {
            const auto v = (float) jlimit (0.0, 1.0, value);

            // Compared with what the host was told (hostValue): a value outside 0..1 that comes back clamped is
            // the host repeating it, not a change
            if (approximatelyEqual (hostValue (*parameter), v))
                return;

            const ScopedValueSetter<Origin> scope (origin, from);
            parameter->setValue (v);
            parameter->sendValueChangedMessageToListeners (v);
        }
    }

    /** A parameter's value as the host sees it. JUCE asks for 0..1, but some plugins step outside (Surge XT's
        bipolar macros go down to -1); CLAP hosts get the range they were promised. */
    static float hostValue (const AudioProcessorParameter& parameter)
    {
        return jlimit (0.0f, 1.0f, parameter.getValue());
    }

    AudioProcessorParameter* findParameter (clap_id id) const
    {
        const auto it = parametersById.find (id);
        return it != parametersById.end() ? it->second : nullptr;
    }

    void requestHostFlush()
    {
        if (hostParams != nullptr && hostParams->request_flush != nullptr)
            hostParams->request_flush (host);
    }

    //==============================================================================
    // AudioProcessorListener

    void audioProcessorParameterChanged (AudioProcessor*, int index, float value) override
    {
        if (! isPositiveAndBelow (index, (int) idsByIndex.size()))
            return;

        const auto id = idsByIndex[(size_t) index];

        if (origin != Origin::host)
            outEvents.push_back ({ CLAP_EVENT_PARAM_VALUE, id, (double) jlimit (0.0f, 1.0f, value) });

        if (origin != Origin::page)
        {
            pageValues.insert (id);
            requestPageUpdate();
        }

        if (origin == Origin::plugin)
            requestHostFlush();
    }

    void audioProcessorParameterChangeGestureBegin (AudioProcessor*, int index) override { gesture (index, CLAP_EVENT_PARAM_GESTURE_BEGIN); }
    void audioProcessorParameterChangeGestureEnd (AudioProcessor*, int index) override   { gesture (index, CLAP_EVENT_PARAM_GESTURE_END); }

    void gesture (int index, uint16_t type)
    {
        if (origin == Origin::plugin && isPositiveAndBelow (index, (int) idsByIndex.size()))
            outEvents.push_back ({ type, idsByIndex[(size_t) index], 0.0 });
    }

    void audioProcessorChanged (AudioProcessor*, const ChangeDetails& details) override
    {
        // The page is not told: it changes the same state on its own stand-in processor, and a state echo
        // would make both sides reload each other's patches. The host is, unless it loaded the state itself.
        if (details.nonParameterStateChanged && origin != Origin::host && hostState != nullptr)
            hostState->mark_dirty (host);
    }

    //==============================================================================
    // Page (clap.gui with the webview API, clap.webview)

    bool receive (const void* data, uint32_t size)
    {
        protocol::Reader r (data, size);
        const auto type = r.get<uint8>();
        r.get<uint8>();
        r.get<uint16>();

        if (! r.ok)
            return false;

        switch (type)
        {
            case protocol::hello:
                // A page that says hello is there, whether or not the host called gui.create first: hosts that
                // only relay the webview frames (WebCLAP's own browser-test-host and its clap-audionode) never do,
                // and without this the page would get neither the first snapshot nor any later parameter change.
                guiCreated = true;
                pageOpen = true;
                pageSnapshot = true;

                if (extension != nullptr)
                    extension->opened();

                sendPageUpdates();
                break;

            case protocol::midi:
            {
                const auto size = (int) (r.end - r.p);

                if (size >= 1 && size <= 3)
                    pageMidi.addEvent (r.p, size, 0);

                break;
            }

            case protocol::param:
            {
                const auto id = r.get<uint32>();
                const auto value = r.get<double>();

                if (r.ok)
                {
                    setParameter (id, value, Origin::page);
                    requestHostFlush();
                }

                break;
            }

            case protocol::gesture:
            {
                const auto id = r.get<uint32>();
                const auto begin = r.get<uint8>();

                if (r.ok && findParameter (id) != nullptr)
                {
                    outEvents.push_back ({ (uint16_t) (begin != 0 ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END), id, 0.0 });
                    requestHostFlush();
                }

                break;
            }

            case protocol::state:
            {
                const ScopedValueSetter<Origin> scope (origin, Origin::page);
                processor->setStateInformation (r.p, (int) (r.end - r.p));

                if (hostState != nullptr)
                    hostState->mark_dirty (host);

                requestHostFlush();
                break;
            }

            case protocol::resize:
            {
                const auto width = r.get<uint32>();
                const auto height = r.get<uint32>();

                if (r.ok && width > 0 && height > 0 && (width != editorWidth || height != editorHeight))
                {
                    editorWidth = width;
                    editorHeight = height;

                    if (hostGui != nullptr && hostGui->request_resize != nullptr)
                        hostGui->request_resize (host, width, height);
                }

                break;
            }

            default:
                if (type >= protocol::firstPluginType && extension != nullptr)
                {
                    // What a port frame changes, the page changed on its stand-in already (a program or patch
                    // load): parameters go to the host only, as for page edits
                    const ScopedValueSetter<Origin> scope (origin, Origin::page);
                    extension->receive (*processor, type, r.p, (size_t) (r.end - r.p));
                    requestHostFlush();
                }

                break;
        }

        pumpMessages();
        return true;
    }

    void requestPageUpdate()
    {
        if (! guiCreated || pageUpdateRequested)
            return;

        pageUpdateRequested = true;
        samplesSinceRequest = 0;

        if (host->request_callback != nullptr)
            host->request_callback (host);
    }

    void onMainThread()
    {
        pumpMessages();

        // hosts that call back after every block would get a frame per block, the process fallback sends the rest
        if (pageUpdateRequested && (pageSnapshot || samplesSinceSend == 0 || samplesSinceSend >= extensionInterval))
            sendPageUpdates();
    }

    /** Sends what the page does not know yet: everything after hello or a state load, else changed values. */
    void sendPageUpdates()
    {
        pageUpdateRequested = false;
        samplesSinceSend = 0;

        if (! guiCreated || hostWebview == nullptr || hostWebview->send == nullptr)
            return;

        sendExtensionUpdate();

        if (std::exchange (pageSnapshot, false))
        {
            MemoryBlock state;
            processor->getStateInformation (state);

            protocol::Writer w (protocol::snapshot);
            w.u32 ((uint32) parameters.size());

            for (auto* parameter : parameters)
            {
                w.u32 (clapIdFor (*parameter));
                w.f64 (parameter->getValue());
            }

            w.u32 ((uint32) state.getSize());
            w.put (state.getData(), state.getSize());
            hostWebview->send (host, w.bytes.data(), (uint32_t) w.bytes.size());
            pageValues.clear();
            return;
        }

        for (const auto id : pageValues)
        {
            if (auto* parameter = findParameter (id))
            {
                protocol::Writer w (protocol::param);
                w.u32 (id);
                w.f64 (parameter->getValue());
                hostWebview->send (host, w.bytes.data(), (uint32_t) w.bytes.size());
            }
        }

        pageValues.clear();
    }

    void sendExtensionUpdate()
    {
        if (extension == nullptr || ! pageOpen || ! std::exchange (extensionUpdateDue, false))
            return;

        extension->update (*processor, [this] (uint8_t type, const void* payload, size_t size)
        {
            protocol::Writer w (type);
            w.put (payload, size);
            hostWebview->send (host, w.bytes.data(), (uint32_t) w.bytes.size());
        });
    }

    int32_t getUri (char* uri, uint32_t capacity) const
    {
        const auto* page = getClapPluginInfo().webviewUri;
        const auto length = (uint32_t) std::strlen (page) + 1;

        if (uri != nullptr && capacity > 0)
        {
            const auto n = jmin (length, capacity) - 1;
            std::memcpy (uri, page, n);
            uri[n] = 0;
        }

        return (int32_t) length;
    }

    //==============================================================================
    // clap.params

    bool getParamInfo (uint32_t index, clap_param_info_t* info) const
    {
        if (index >= parameters.size())
            return false;

        auto* parameter = parameters[index];
        *info = {};
        info->id = idsByIndex[index];
        info->flags = CLAP_PARAM_REQUIRES_PROCESS;

        if (parameter->isAutomatable())
            info->flags |= CLAP_PARAM_IS_AUTOMATABLE;

        if (parameter->isDiscrete() && parameter->getNumSteps() < AudioProcessor::getDefaultNumParameterSteps())
            info->flags |= CLAP_PARAM_IS_STEPPED;

        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = parameter->getDefaultValue();
        parameter->getName (CLAP_NAME_SIZE - 1).copyToUTF8 (info->name, CLAP_NAME_SIZE);

        if (const auto* group = processor->getParameterTree().getGroupsForParameter (parameter).getLast())
            group->getName().copyToUTF8 (info->module, CLAP_PATH_SIZE);

        return true;
    }

    bool valueToText (clap_id id, double value, char* display, uint32_t size) const
    {
        auto* parameter = findParameter (id);

        if (parameter == nullptr || size == 0)
            return false;

        // Text only, without getLabel(): plugins often put the unit into the text already, in its own scale
        parameter->getText ((float) value, (int) size - 1).copyToUTF8 (display, size);
        return true;
    }

    bool textToValue (clap_id id, const char* display, double* value) const
    {
        if (auto* parameter = findParameter (id))
        {
            *value = parameter->getValueForText (String::fromUTF8 (display));
            return true;
        }

        return false;
    }

    void flush (const clap_input_events_t* in, const clap_output_events_t* out)
    {
        if (in != nullptr)
            readEvents (*in, false);

        pumpMessages();

        if (out != nullptr)
            writeEvents (*out);
    }

    //==============================================================================
    // clap.state

    bool save (const clap_ostream_t* stream)
    {
        MemoryBlock block;
        processor->getStateInformation (block);

        auto* data = static_cast<const char*> (block.getData());
        auto remaining = (int64_t) block.getSize();

        while (remaining > 0)
        {
            const auto written = stream->write (stream, data, (uint64_t) remaining);

            if (written <= 0)
                return false;

            data += written;
            remaining -= written;
        }

        return true;
    }

    bool load (const clap_istream_t* stream)
    {
        MemoryOutputStream block;
        char chunk[4096];

        for (;;)
        {
            const auto read = stream->read (stream, chunk, sizeof (chunk));

            if (read < 0)
                return false;

            if (read == 0)
                break;

            block.write (chunk, (size_t) read);
        }

        {
            const ScopedValueSetter<Origin> scope (origin, Origin::host);
            processor->setStateInformation (block.getData(), (int) block.getDataSize());
            pumpMessages();
        }

        if (hostParams != nullptr && hostParams->rescan != nullptr)
            hostParams->rescan (host, CLAP_PARAM_RESCAN_VALUES);

        pageSnapshot = true;
        requestPageUpdate();
        return true;
    }

    //==============================================================================
    const void* getExtension (const char* id)
    {
        static const clap_plugin_params_t params {
            [] (const clap_plugin_t* p) { return (uint32_t) self (p).parameters.size(); },
            [] (const clap_plugin_t* p, uint32_t index, clap_param_info_t* info) { return self (p).getParamInfo (index, info); },
            [] (const clap_plugin_t* p, clap_id id, double* value)
            {
                auto* parameter = self (p).findParameter (id);

                if (parameter != nullptr)
                    *value = hostValue (*parameter);

                return parameter != nullptr;
            },
            [] (const clap_plugin_t* p, clap_id id, double value, char* display, uint32_t size) { return self (p).valueToText (id, value, display, size); },
            [] (const clap_plugin_t* p, clap_id id, const char* display, double* value) { return self (p).textToValue (id, display, value); },
            [] (const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) { self (p).flush (in, out); }
        };

        static const clap_plugin_state_t state {
            [] (const clap_plugin_t* p, const clap_ostream_t* stream) { return self (p).save (stream); },
            [] (const clap_plugin_t* p, const clap_istream_t* stream) { return self (p).load (stream); }
        };

        static const clap_plugin_audio_ports_t audioPorts {
            [] (const clap_plugin_t* p, bool isInput) { return self (p).mainChannels (isInput) > 0 ? 1u : 0u; },
            [] (const clap_plugin_t* p, uint32_t index, bool isInput, clap_audio_port_info_t* info)
            {
                const auto channelCount = self (p).mainChannels (isInput);

                if (index != 0 || channelCount == 0)
                    return false;

                *info = {};
                info->id = isInput ? 0 : 1;
                std::strncpy (info->name, isInput ? "Input" : "Output", CLAP_NAME_SIZE - 1);
                info->flags = CLAP_AUDIO_PORT_IS_MAIN;
                info->channel_count = (uint32_t) channelCount;
                info->port_type = channelCount == 2 ? CLAP_PORT_STEREO : channelCount == 1 ? CLAP_PORT_MONO : nullptr;
                info->in_place_pair = isInput ? 1 : 0;
                return true;
            }
        };

        static const clap_plugin_note_ports_t notePorts {
            [] (const clap_plugin_t* p, bool isInput) { return isInput && self (p).processor->acceptsMidi() ? 1u : 0u; },
            [] (const clap_plugin_t* p, uint32_t index, bool isInput, clap_note_port_info_t* info)
            {
                if (index != 0 || ! isInput || ! self (p).processor->acceptsMidi())
                    return false;

                *info = {};
                info->id = 0;
                info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
                info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
                std::strncpy (info->name, "Notes", CLAP_NAME_SIZE - 1);
                return true;
            }
        };

        static const clap_plugin_tail_t tail {
            [] (const clap_plugin_t* p)
            {
                const auto seconds = self (p).processor->getTailLengthSeconds();
                return std::isinf (seconds) ? std::numeric_limits<uint32_t>::max()
                                            : (uint32_t) (seconds * self (p).processor->getSampleRate());
            }
        };

        static const clap_plugin_gui_t gui {
            [] (const clap_plugin_t*, const char* api, bool isFloating) { return ! isFloating && std::strcmp (api, CLAP_WINDOW_API_WEBVIEW) == 0; },
            [] (const clap_plugin_t*, const char** api, bool* isFloating)
            {
                *api = CLAP_WINDOW_API_WEBVIEW;
                *isFloating = false;
                return true;
            },
            [] (const clap_plugin_t* p, const char* api, bool isFloating)
            {
                if (isFloating || std::strcmp (api, CLAP_WINDOW_API_WEBVIEW) != 0)
                    return false;

                self (p).guiCreated = true;
                return true;
            },
            [] (const clap_plugin_t* p)
            {
                auto& s = self (p);
                s.guiCreated = false;
                s.pageOpen = false;
                s.pageUpdateRequested = false;
                s.pageValues.clear();
            },
            [] (const clap_plugin_t*, double) { return false; },
            [] (const clap_plugin_t* p, uint32_t* width, uint32_t* height)
            {
                *width = self (p).editorWidth;
                *height = self (p).editorHeight;
                return true;
            },
            [] (const clap_plugin_t*) { return false; }, // the editor resizes itself (zoom) and asks for the size
            [] (const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; },
            [] (const clap_plugin_t* p, uint32_t* width, uint32_t* height)
            {
                *width = self (p).editorWidth;
                *height = self (p).editorHeight;
                return true;
            },
            [] (const clap_plugin_t* p, uint32_t width, uint32_t height) { return width == self (p).editorWidth && height == self (p).editorHeight; },
            [] (const clap_plugin_t*, const clap_window_t*) { return true; },
            [] (const clap_plugin_t*, const clap_window_t*) { return false; },
            [] (const clap_plugin_t*, const char*) {},
            [] (const clap_plugin_t*) { return true; },
            [] (const clap_plugin_t*) { return true; }
        };

        static const clap_plugin_webview_t webview {
            [] (const clap_plugin_t* p, char* uri, uint32_t capacity) { return self (p).getUri (uri, capacity); },
            [] (const clap_plugin_t*, const char*, char*, uint32_t, const clap_ostream_t*) { return false; }, // hosts serve the bundle
            [] (const clap_plugin_t* p, const void* buffer, uint32_t size) { return self (p).receive (buffer, size); }
        };

        if (std::strcmp (id, CLAP_EXT_PARAMS) == 0)      return &params;
        if (std::strcmp (id, CLAP_EXT_STATE) == 0)       return &state;
        if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &audioPorts;
        if (std::strcmp (id, CLAP_EXT_NOTE_PORTS) == 0)  return &notePorts;
        if (std::strcmp (id, CLAP_EXT_TAIL) == 0)        return &tail;
        if (std::strcmp (id, CLAP_EXT_GUI) == 0)         return &gui;
        if (std::strcmp (id, CLAP_EXT_WEBVIEW) == 0)     return &webview;
        return nullptr;
    }

    //==============================================================================
    clap_plugin_t plugin {};
    const clap_host_t* host;
    const clap_host_params_t* hostParams = nullptr;
    const clap_host_state_t* hostState = nullptr;
    const clap_host_gui_t* hostGui = nullptr;
    const clap_host_webview_t* hostWebview = nullptr;

    std::unique_ptr<AudioProcessor> processor;
    std::vector<AudioProcessorParameter*> parameters;
    std::vector<clap_id> idsByIndex;
    std::unordered_map<clap_id, AudioProcessorParameter*> parametersById;

    ClapPlayHead playHead;
    MidiBuffer midi;
    std::vector<float*> channels;
    AudioBuffer<float> scratch;

    Origin origin = Origin::plugin;
    std::vector<OutEvent> outEvents;

    bool guiCreated = false, pageOpen = false, pageSnapshot = false, pageUpdateRequested = false;
    std::unique_ptr<PageExtension> extension;
    MidiBuffer pageMidi;
    bool extensionUpdateDue = false;
    int samplesSinceExtensionUpdate = 0, extensionInterval = 1600;
    std::unordered_set<clap_id> pageValues;
    int samplesSinceRequest = 0, fallbackInterval = 1600, samplesSinceSend = 0;
    uint32_t editorWidth = 0, editorHeight = 0;
};

//==============================================================================
const clap_plugin_descriptor_t* descriptor()
{
    static const clap_plugin_descriptor_t d = []
    {
        const auto& info = getClapPluginInfo();
        clap_plugin_descriptor_t result {};
        result.clap_version = CLAP_VERSION_INIT;
        result.id = info.id;
        result.name = info.name;
        result.vendor = info.vendor;
        result.url = info.url;
        result.manual_url = "";
        result.support_url = "";
        result.version = info.version;
        result.description = info.description;
        result.features = info.features;
        return result;
    }();

    return &d;
}

const clap_plugin_factory_t factory {
    [] (const clap_plugin_factory_t*) { return 1u; },
    [] (const clap_plugin_factory_t*, uint32_t index) { return index == 0 ? descriptor() : nullptr; },
    [] (const clap_plugin_factory_t*, const clap_host_t* host, const char* id) -> const clap_plugin_t*
    {
        if (! clap_version_is_compatible (host->clap_version) || std::strcmp (id, descriptor()->id) != 0)
            return nullptr;

        return (new Plugin (host, descriptor()))->getClapPlugin();
    }
};

} // namespace
} // namespace juce::webclap

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry {
    CLAP_VERSION_INIT,
    [] (const char*) { return true; },
    [] {},
    [] (const char* id) -> const void* { return std::strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &juce::webclap::factory : nullptr; }
};
