/*
    juce_webclap: the UI side of the split-UI message protocol (see PLAN.md, "Message protocol").

    The editor talks to a stand-in AudioProcessor in the UI module. ProcessorBridge listens to that processor
    and turns parameter changes, gestures and state changes into binary frames for the DSP side, and applies
    frames from the DSP side (automation, preset loads) back to the stand-in, so attachments update the editor.

    The frames are defined in juce_webclap_protocol.h.

    Editors with an on-screen keyboard play into their processor's MidiKeyboardState; forwardKeyboard() sends
    those notes to the DSP side. What else a port's editor exchanges goes through sendPluginFrame() and
    onPluginFrame (the DSP side is a PageExtension, see juce_webclap_clap.h).

    Parameter ids are CLAP ids. For JUCE parameters with an id this is the id's String::hashCode(), which is
    what clap-juce-extensions uses, so a host that automates CLAP ids talks about the same parameters.

    Depends on juce_audio_processors_headless. Header-only, include it in the app.
*/

#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include "juce_webclap_protocol.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace juce::webclap
{

/** The CLAP id of a JUCE parameter, the same on the UI and the DSP side. */
inline uint32 clapIdFor (const AudioProcessorParameter& parameter)
{
    if (const auto* withId = dynamic_cast<const AudioProcessorParameterWithID*> (&parameter))
        return (uint32) withId->paramID.hashCode();

    return (uint32) parameter.getParameterIndex();
}

//==============================================================================
class ProcessorBridge final : private AudioProcessorListener,
                              private MidiKeyboardState::Listener
{
public:
    using Sender = std::function<void (const void* data, size_t size)>;

    ProcessorBridge (AudioProcessor& p, Sender sender)
        : processor (p), send (std::move (sender))
    {
        for (auto* parameter : processor.getParameters())
            parametersById[clapIdFor (*parameter)] = parameter;

        processor.addListener (this);
    }

    ~ProcessorBridge() override
    {
        if (keyboard != nullptr)
            keyboard->removeListener (this);

        processor.removeListener (this);
    }

    /** Sends the notes played on this keyboard state (the editor's MidiKeyboardComponent) to the DSP side. */
    void forwardKeyboard (MidiKeyboardState& state)
    {
        jassert (keyboard == nullptr);
        keyboard = &state;
        keyboard->addListener (this);
    }

    /** A frame of a port type (protocol::firstPluginType and up), for the port's PageExtension. */
    void sendPluginFrame (uint8 type, const void* payload, size_t size)
    {
        jassert (type >= protocol::firstPluginType);
        protocol::Writer w (type);
        w.put (payload, size);
        emit (w);
    }

    static uint32 clapIdFor (const AudioProcessorParameter& parameter) { return webclap::clapIdFor (parameter); }

    AudioProcessorParameter* findParameter (uint32 clapId) const
    {
        const auto it = parametersById.find (clapId);
        return it != parametersById.end() ? it->second : nullptr;
    }

    /** Starts the session: the DSP side answers with a snapshot. */
    void sendHello()
    {
        protocol::Writer w (protocol::hello);
        w.u32 (protocol::version);
        emit (w);
    }

    /** Asks the host for the plugin's window (from a device panel's page). */
    void requestWindow() { emit (protocol::Writer (protocol::show)); }

    void sendResize (int width, int height)
    {
        protocol::Writer w (protocol::resize);
        w.u32 ((uint32) width);
        w.u32 ((uint32) height);
        emit (w);
    }

    /** A frame from the DSP side. */
    void receive (const void* data, size_t size)
    {
        protocol::Reader r (data, size);
        const auto type = r.get<uint8>();
        r.get<uint8>();
        r.get<uint16>();

        if (! r.ok)
            return;

        const ScopedValueSetter<bool> applying (applyingRemote, true);

        switch (type)
        {
            case protocol::param:
            {
                const auto id = r.get<uint32>();
                const auto value = r.get<double>();

                if (r.ok)
                    applyValue (id, value);

                break;
            }

            case protocol::state:
                applyState (r.p, (size_t) (r.end - r.p));
                break;

            case protocol::snapshot:
            {
                const auto count = r.get<uint32>();

                for (uint32 i = 0; i < count && r.ok; ++i)
                {
                    const auto id = r.get<uint32>();
                    const auto value = r.get<double>();

                    if (r.ok)
                        applyValue (id, value);
                }

                const auto stateSize = r.get<uint32>();

                if (r.ok && stateSize > 0 && r.has (stateSize))
                    applyState (r.p, stateSize);

                if (count == 0 && stateSize == 0)
                    needsSync = true; // the DSP side knows nothing yet: seed it from the stand-in

                ++snapshotsReceived;
                break;
            }

            default:
                if (type >= protocol::firstPluginType && onPluginFrame)
                    onPluginFrame (type, r.p, (size_t) (r.end - r.p));

                break;
        }
    }

    /** Sends what changed since the last call. Call once per UI frame: at most one value per parameter goes
        out per frame, gestures keep their order relative to the values. */
    void flush()
    {
        if (std::exchange (needsSync, false))
        {
            sendState (protocol::flagSync);

            for (auto& [id, parameter] : parametersById)
            {
                remoteValues[id] = parameter->getValue();
                protocol::Writer w (protocol::param, protocol::flagSync);
                w.u32 (id);
                w.f64 (parameter->getValue());
                emit (w);
            }
        }

        // After a remote state, the plugin settles: deferred patch application re-sets values (rounded) and
        // reports a state change. That is the state the DSP side just sent, so it is not sent back. Values the
        // user drags meanwhile (inside a gesture) still go out.
        const auto settling = settleUntil != 0 && (int32) (Time::getMillisecondCounter() - settleUntil) < 0;

        for (const auto& event : pending)
        {
            if (event.isGesture)
            {
                if (event.begin)
                    activeGestures.insert (event.id);
                else
                    activeGestures.erase (event.id);

                protocol::Writer w (protocol::gesture);
                w.u32 (event.id);
                w.u8 (event.begin ? 1 : 0);
                emit (w);
            }
            else
            {
                if (settling && activeGestures.count (event.id) == 0)
                    continue;

                // Plugins often re-apply values they were just given (deferred patch application, attachments
                // syncing). Values the DSP side already has are not news.
                if (const auto known = remoteValues.find (event.id);
                    known != remoteValues.end() && approximatelyEqual ((float) known->second, (float) event.value))
                    continue;

                remoteValues[event.id] = event.value;
                protocol::Writer w (protocol::param);
                w.u32 (event.id);
                w.f64 (event.value);
                emit (w);
            }
        }

        pending.clear();
        pendingValue.clear();

        if (settling)
        {
            stateChanged = false;
            return;
        }

        if (std::exchange (settleUntil, 0u) != 0)
        {
            // Settled: what the stand-in holds now is what the DSP side has.
            for (auto& [id, parameter] : parametersById)
                remoteValues[id] = parameter->getValue();

            processor.getStateInformation (remoteState);
            stateChanged = false;
        }

        if (std::exchange (stateChanged, false))
            sendState (0);
    }

    /** The DSP side has the values the stand-in has now: the port had it load the same thing (a patch frame).
        Values changed since the last flush are not sent; gestures are. */
    void assumeRemoteHasCurrentValues()
    {
        for (auto& [id, parameter] : parametersById)
            remoteValues[id] = parameter->getValue();

        pending.erase (std::remove_if (pending.begin(), pending.end(), [] (const Event& e) { return ! e.isGesture; }),
                       pending.end());
        pendingValue.clear();
    }

    int getSnapshotsReceived() const noexcept { return snapshotsReceived; }

    std::function<void (uint32 id, double value)> onRemoteValue;
    std::function<void()> onRemoteState;

    /** A port frame from the DSP side (its PageExtension). */
    std::function<void (uint8 type, const uint8* payload, size_t size)> onPluginFrame;

private:
    struct Event
    {
        uint32 id;
        double value;
        bool isGesture, begin;
    };

    void emit (const protocol::Writer& w)
    {
        if (send)
            send (w.bytes.data(), w.bytes.size());
    }

    void sendState (uint8 flags)
    {
        MemoryBlock block;
        processor.getStateInformation (block);

        if (block == remoteState)
            return;

        remoteState = block;

        protocol::Writer w (protocol::state, flags);
        w.put (block.getData(), block.getSize());
        emit (w);
    }

    void applyValue (uint32 id, double value)
    {
        // The DSP side tells every page of every change, also this page's own: a value this page is dragging or
        // has not sent yet is newer than what comes back
        if (activeGestures.count (id) > 0 || pendingValue.count (id) > 0)
            return;

        if (auto* parameter = findParameter (id))
        {
            const auto v = (float) jlimit (0.0, 1.0, value);
            remoteValues[id] = v;

            if (! approximatelyEqual (parameter->getValue(), v))
            {
                // What JUCE's plugin wrappers do for host automation: set, then tell listeners (attachments).
                parameter->setValue (v);
                parameter->sendValueChangedMessageToListeners (v);
            }

            if (onRemoteValue)
                onRemoteValue (id, value);
        }
    }

    void applyState (const void* data, size_t size)
    {
        remoteState = MemoryBlock (data, size);
        processor.setStateInformation (data, (int) size);
        settleUntil = jmax (1u, Time::getMillisecondCounter() + settleMilliseconds);

        if (onRemoteState)
            onRemoteState();
    }

    //==============================================================================
    void audioProcessorParameterChanged (AudioProcessor*, int index, float newValue) override
    {
        if (applyingRemote)
            return;

        if (auto* parameter = processor.getParameters()[index])
        {
            const auto id = clapIdFor (*parameter);
            const auto it = pendingValue.find (id);

            if (it != pendingValue.end())
            {
                pending[it->second].value = newValue;
            }
            else
            {
                pendingValue[id] = pending.size();
                pending.push_back ({ id, newValue, false, false });
            }
        }
    }

    void gesture (int index, bool begin)
    {
        if (applyingRemote)
            return;

        if (auto* parameter = processor.getParameters()[index])
        {
            const auto id = clapIdFor (*parameter);
            pendingValue.erase (id); // later values must follow this gesture event
            pending.push_back ({ id, 0.0, true, begin });
        }
    }

    void audioProcessorParameterChangeGestureBegin (AudioProcessor*, int index) override { gesture (index, true); }
    void audioProcessorParameterChangeGestureEnd (AudioProcessor*, int index) override   { gesture (index, false); }

    //==============================================================================
    void sendMidi (const MidiMessage& message)
    {
        protocol::Writer w (protocol::midi);
        w.put (message.getRawData(), (size_t) message.getRawDataSize());
        emit (w);
    }

    void handleNoteOn (MidiKeyboardState*, int channel, int note, float velocity) override
    {
        sendMidi (MidiMessage::noteOn (channel, note, velocity));
    }

    void handleNoteOff (MidiKeyboardState*, int channel, int note, float velocity) override
    {
        sendMidi (MidiMessage::noteOff (channel, note, velocity));
    }

    void audioProcessorChanged (AudioProcessor*, const ChangeDetails& details) override
    {
        if (! applyingRemote && details.nonParameterStateChanged)
            stateChanged = true;
    }

    AudioProcessor& processor;
    Sender send;
    MidiKeyboardState* keyboard = nullptr;
    std::unordered_map<uint32, AudioProcessorParameter*> parametersById;
    std::vector<Event> pending;
    std::map<uint32, size_t> pendingValue;
    std::unordered_map<uint32, double> remoteValues; // what the DSP side has, as far as we know
    MemoryBlock remoteState;
    std::set<uint32> activeGestures;
    uint32 settleUntil = 0; // millisecond counter, 0 = not settling
    static constexpr uint32 settleMilliseconds = 300;
    bool applyingRemote = false, stateChanged = false, needsSync = false;
    int snapshotsReceived = 0;
};

} // namespace juce::webclap
