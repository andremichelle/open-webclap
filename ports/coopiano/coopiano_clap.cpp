/*
    coopiano as module.wasm: Peter Cooper's physically modelled piano (https://github.com/peterc/coopiano, MIT) as a
    CLAP instrument. Plain CLAP, no framework: the engine is upstream's instrument::Engine behind
    its own host-rate adapter (piano_host::Adapter, 44.1, 48, 88.2 or 96 kHz), unchanged but for
    patches/coopiano-webclap.patch.

    Strikes. Every note starts from the hammer's contact with the strings, a nonlinear solve that costs up to a few
    milliseconds per key and velocity natively, over 10 ms in WebAssembly (soft bass is the most expensive). Upstream's live host solves it on its MIDI
    thread or builds all 88 x 127 responses up front (about 10 s, 72 MiB). A WebCLAP module has the audio thread
    only, so the strike cache starts empty and fills here, every solve in slices of bounded work (the patch makes
    the solver resumable, with the same results), the budgets below converted with the measured solver speed:

      - a note whose response is cached starts at its sample, as in upstream's host
      - a note whose response is missing starts at its sample all the same with the nearest cached velocity of
        its key, up to 8 MIDI steps away; its exact response is solved next in the background
      - a note without even that waits in a queue; each process call solves queued responses for a quarter of
        its own duration, then starts those notes at the top of the block, a block or a few late, the first time
        only. Later events for a waiting key wait behind it, so a note-off never overtakes
        its note-on, and it follows a block later at the earliest
      - blocks without waiting notes fill the cache in the background, for a sixth of their real time: the
        responses notes asked for first, then all keys at velocity 64, 32 and 96, 16, 48, 80, 112 and 8, so
        every key has a neighbour within 8 for every velocity (about 5 s after loading on a fast core, half a
        minute on an efficiency core, where browsers may run the audio thread). The others are solved when
        played, so the fill ends and costs nothing after

    Parameters: Volume (the engine's output gain, before its soft limiter), Reverb (the room's wet level) and
    Sustain (the damper pedal, continuous: half-pedalling works, for hosts without CC64 as in openDAW). Notes
    arrive as CLAP notes or MIDI (note on/off, CC64). The state is the three values.

    Editor: ui/index.html through clap.gui (the webview API) and clap.webview, the three parameters as openDAW
    Studio's knobs. Its messages are text (see receive); its edits reach the host as gesture and value events.
*/

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>

#include "piano_host.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>

namespace
{
constexpr const char* pluginId = "com.peterc.coopiano";

enum ParamId : clap_id
{
    volumeId = 0,
    reverbId = 1,
    sustainId = 2,
    paramCount = 3
};

struct ParamSpec
{
    clap_id id;
    const char* name;
    double min, max, value;
};

constexpr ParamSpec paramSpecs[paramCount] = {
    { volumeId, "Volume", -24.0, 12.0, 0.0 },  // dB on the engine's calibrated gain
    { reverbId, "Reverb", 0.0, 1.0, 0.3 },     // upstream's default room mix
    { sustainId, "Sustain", 0.0, 1.0, 0.0 },   // pedal position, 0 up .. 1 down
};

constexpr double baseGain = instrument::Settings {}.gain;
constexpr size_t maxEventsPerCall = 1024;
constexpr size_t maxWaiting = 512;
constexpr int nearestVelocity = 8;  // how far a stand-in velocity may be from the one played
constexpr size_t maxWanted = 64;    // exact responses stand-ins asked for, solved first by the background fill
constexpr int backgroundVelocities = 8; // fillOrder's first: 8 to 120 in steps of 16, a neighbour within 8 for all

// MIDI velocities 1..127, coarse to fine: 64, 32, 96, 16, 48, 80, 112, 8, ...
struct FillOrder
{
    int velocities[127] {};
    FillOrder()
    {
        bool taken[128] {};
        int n = 0;
        for (int step = 64; step >= 1; step /= 2)
            for (int v = step; v <= 127; v += step)
                if (! taken[v])
                {
                    taken[v] = true;
                    velocities[n++] = v;
                }
    }
};

const FillOrder fillOrder;

using Clock = std::chrono::steady_clock;

double millisecondsSince (Clock::time_point start)
{
    return std::chrono::duration<double, std::milli> (Clock::now() - start).count();
}

struct Coopiano
{
    clap_plugin plugin {};
    const clap_host* host = nullptr;

    std::unique_ptr<instrument::Engine> engine;
    std::unique_ptr<piano_host::Adapter> adapter;
    double sampleRate = 0;
    size_t maxFrames = 0;
    bool active = false;

    double values[paramCount] { paramSpecs[0].value, paramSpecs[1].value, paramSpecs[2].value };
    double midiPedal[16] {}; // CC64 per channel, combined with the Sustain parameter (whichever is further down)
    bool pedalDirty = false;  // Sustain changed outside process (flush, state): sent with the next block

    std::vector<instrument::MidiEvent> events;  // one adapter call's
    std::vector<instrument::MidiEvent> waiting; // FIFO behind a missing strike
    size_t waitingHead = 0;
    uint64_t noteSequence = 1;

    // background fill: what stand-ins asked for (a ring), then fillOrder's velocity index and key
    std::pair<int, int> wanted[maxWanted] {};
    size_t wantedHead = 0, wantedCount = 0;
    int fillVelocityIndex = 0, fillKey = 21;
    bool filled = false;
    std::pair<int, int> fillTarget {}; // key 0: none yet
    std::pair<int, int> failed {};     // a response the solver gave up on (not representable)

    // solver speed in mode-steps per ms, measured: about 7e5 to 9e5 in WebAssembly on an M1, the start is lower
    double workPerMs = 3.0e5, measuredWork = 0, measuredMs = 0;

    // editor: what the page did, for the host (output events), and what the host did, for the page
    struct OutEvent
    {
        uint16_t type;
        clap_id id;
        double value;
    };
    std::vector<OutEvent> outEvents;
    const clap_host_params* hostParams = nullptr;
    const clap_host_webview* hostWebview = nullptr;
    bool guiCreated = false, pageUpdateRequested = false;
    bool pageValues[paramCount] {};
    size_t samplesSinceRequest = 0, samplesSinceSend = 0;

    // --- setup ----------------------------------------------------------------------------------------------

    bool init()
    {
        try
        {
            instrument::Settings settings;
            engine = std::make_unique<instrument::Engine> (settings);
            engine->build_strike_cache (1.0, true);
            adapter = std::make_unique<piano_host::Adapter> (*engine);
            events.reserve (maxEventsPerCall);
            waiting.reserve (maxWaiting);
            outEvents.reserve (64);
            hostParams = static_cast<const clap_host_params*> (host->get_extension (host, CLAP_EXT_PARAMS));
            hostWebview = static_cast<const clap_host_webview*> (host->get_extension (host, CLAP_EXT_WEBVIEW));
            return true;
        }
        catch (const std::exception& e)
        {
            std::fprintf (stderr, "coopiano: %s\n", e.what());
            return false;
        }
    }

    bool activate (double rate, uint32_t minFrames, uint32_t maximumFrames)
    {
        (void) minFrames;
        if (! piano_host::supported_rate (rate))
        {
            std::fprintf (stderr, "coopiano: %g Hz is not supported (44100, 48000, 88200 or 96000)\n", rate);
            return false;
        }
        try
        {
            maxFrames = std::clamp<size_t> (maximumFrames, 32, 4096);
            adapter->prepare (rate, maxFrames, maxEventsPerCall);
            sampleRate = rate;
            applyValues();
            active = true;
            return true;
        }
        catch (const std::exception& e)
        {
            std::fprintf (stderr, "coopiano: %s\n", e.what());
            return false;
        }
    }

    void deactivate() { active = false; }

    void reset()
    {
        if (adapter != nullptr)
            adapter->reset();
        waiting.clear();
        waitingHead = 0;
        std::fill (std::begin (midiPedal), std::end (midiPedal), 0.0);
        applyValues();
    }

    // The engine's gain and room follow the values; the pedal goes in as events (pedalEvents)
    void applyValues()
    {
        engine->set_gain (baseGain * std::pow (10.0, values[volumeId] / 20.0));
        engine->set_reverb_mix (values[reverbId]);
    }

    // --- events ---------------------------------------------------------------------------------------------

    static int clapVelocity (double velocity)
    {
        return std::clamp ((int) std::lround (velocity * 127.0), 1, 127);
    }

    uint64_t nextSeed (int key)
    {
        // as upstream's live host: a sequence number and the key
        return 0x9e3779b97f4a7c15ULL ^ (noteSequence++ * 0x100000001b3ULL) ^ ((uint64_t) key << 32);
    }

    bool keyIsWaiting (int key, int channel) const
    {
        for (size_t i = waitingHead; i < waiting.size(); ++i)
            if (waiting[i].type != instrument::MidiEvent::Type::pedal && waiting[i].key == key
                && waiting[i].channel == channel)
                return true;
        return false;
    }

    void wait (const instrument::MidiEvent& event)
    {
        if (waiting.size() - waitingHead >= maxWaiting)
            return; // a flood the cache cannot keep up with: drop
        if (waitingHead > 0 && waiting.size() == waiting.capacity())
        {
            waiting.erase (waiting.begin(), waiting.begin() + (std::ptrdiff_t) waitingHead);
            waitingHead = 0;
        }
        waiting.push_back (event);
    }

    // A note event at offset (in this adapter call's frames): played now, or queued behind a missing strike
    void note (instrument::MidiEvent event)
    {
        event.channel = std::clamp (event.channel, 0, 15);
        if (event.key < 0 || event.key > 127)
            return;
        const bool on = event.type == instrument::MidiEvent::Type::note_on && event.velocity > 0;
        if (on)
            event.seed = nextSeed (event.key);
        else
            event.type = instrument::MidiEvent::Type::note_off;
        if (keyIsWaiting (event.key, event.channel))
            wait (event);
        else if (! on || engine->has_strike (event.key, event.velocity))
            push (event);
        else if (const int standIn = nearestCached (event.key, event.velocity); standIn > 0)
        {
            want (event.key, event.velocity);
            event.velocity = standIn;
            push (event);
        }
        else
            wait (event);
    }

    int nearestCached (int key, int velocity) const
    {
        for (int distance = 1; distance <= nearestVelocity; ++distance)
            for (const int v : { velocity - distance, velocity + distance })
                if (v >= 1 && v <= 127 && engine->has_strike (key, v))
                    return v;
        return 0;
    }

    void want (int key, int velocity)
    {
        for (size_t i = 0; i < wantedCount; ++i)
            if (wanted[(wantedHead + i) % maxWanted] == std::make_pair (key, velocity))
                return;
        if (wantedCount == maxWanted) // the oldest wish goes, the order fills it later anyway
        {
            wantedHead = (wantedHead + 1) % maxWanted;
            --wantedCount;
        }
        wanted[(wantedHead + wantedCount++) % maxWanted] = { key, velocity };
    }

    void push (const instrument::MidiEvent& event)
    {
        if (events.size() < maxEventsPerCall)
            events.push_back (event);
    }

    void pedalEvents (size_t offset, int channel)
    {
        const auto add = [this, offset] (int ch)
        {
            instrument::MidiEvent event;
            event.type = instrument::MidiEvent::Type::pedal;
            event.offset = offset;
            event.channel = ch;
            event.position = std::clamp (std::max (values[sustainId], midiPedal[ch]), 0.0, 1.0);
            push (event);
        };
        if (channel < 0)
            for (int ch = 0; ch < 16; ++ch)
                add (ch);
        else
            add (channel);
    }

    void midi (const clap_event_midi& event, size_t offset)
    {
        const int status = event.data[0] & 0xf0, channel = event.data[0] & 0x0f;
        if (status == 0x90 || status == 0x80)
        {
            instrument::MidiEvent e;
            e.offset = offset;
            e.key = event.data[1];
            e.velocity = status == 0x90 ? event.data[2] : 0;
            e.channel = channel;
            e.type = e.velocity > 0 ? instrument::MidiEvent::Type::note_on : instrument::MidiEvent::Type::note_off;
            note (e);
        }
        else if (status == 0xb0 && event.data[1] == 64)
        {
            midiPedal[channel] = event.data[2] / 127.0;
            pedalEvents (offset, channel);
        }
    }

    void setValue (clap_id id, double value)
    {
        if (id >= paramCount)
            return;
        values[id] = std::clamp (value, paramSpecs[id].min, paramSpecs[id].max);
    }

    // --- strikes --------------------------------------------------------------------------------------------

    // Solves for about work mode-steps on one response, in slices (patches/coopiano-webclap.patch). Measures
    // the speed: the clock may tick in whole milliseconds (Date.now in a worklet), so over 50 ms at a time.
    long solve (int key, int velocity, long work, bool measure = true)
    {
        const auto start = Clock::now();
        long spent = 0;
        try
        {
            spent = engine->build_strike (key, velocity, work);
        }
        catch (const std::exception& e)
        {
            std::fprintf (stderr, "coopiano: strike %d/%d: %s\n", key, velocity, e.what());
            failed = { key, velocity };
        }
        if (! measure)
            return std::max (spent, 1L);
        measuredWork += spent;
        measuredMs += millisecondsSince (start);
        if (measuredMs >= 50.0)
        {
            // a coarse clock under- or over-counts a window: a quarter at most per window, within fixed bounds
            const double measured = measuredWork / measuredMs;
            workPerMs = std::clamp (std::clamp (measured, 0.8 * workPerMs, 1.25 * workPerMs), 5.0e4, 3.0e6);
            measuredWork = 0;
            measuredMs = 0;
        }
        return std::max (spent, 1L);
    }

    bool solvable (int key, int velocity) const { return failed != std::make_pair (key, velocity); }

    // Solves the strikes queued notes need, for about budget ms, then moves the playable front of the queue to
    // this call's events at offset 0. A note-off whose note starts in this call stays for the next.
    void serveWaiting (double budget)
    {
        long work = (long) (budget * workPerMs);
        uint32_t started[128 / 32] {};
        while (waitingHead < waiting.size())
        {
            auto event = waiting[waitingHead];
            const uint32_t bit = 1u << (event.key % 32);
            if (event.type == instrument::MidiEvent::Type::note_off && (started[event.key / 32] & bit) != 0)
                break;
            if (event.type == instrument::MidiEvent::Type::note_on)
            {
                while (! engine->has_strike (event.key, event.velocity) && solvable (event.key, event.velocity)
                       && work > 0)
                    work -= solve (event.key, event.velocity, work);
                if (! solvable (event.key, event.velocity))
                    event.type = instrument::MidiEvent::Type::note_off; // the note cannot sound
                else if (! engine->has_strike (event.key, event.velocity))
                    break; // more next block
                else
                    started[event.key / 32] |= bit;
            }
            event.offset = 0;
            push (event);
            ++waitingHead;
        }
        if (waitingHead == waiting.size())
        {
            waiting.clear();
            waitingHead = 0;
        }
    }

    // The response the background fill works on: what a stand-in asked for, else the next in fillOrder
    bool nextFillTarget()
    {
        while (wantedCount > 0)
        {
            fillTarget = wanted[wantedHead];
            wantedHead = (wantedHead + 1) % maxWanted;
            --wantedCount;
            if (! engine->has_strike (fillTarget.first, fillTarget.second))
                return true;
        }
        while (! filled)
        {
            fillTarget = { fillKey, fillOrder.velocities[fillVelocityIndex] };
            if (++fillKey > 108)
            {
                fillKey = 21;
                filled = ++fillVelocityIndex >= backgroundVelocities;
                if (filled)
                    std::fprintf (stderr, "coopiano: background strikes ready (%d velocities per key)\n", backgroundVelocities);
            }
            if (! engine->has_strike (fillTarget.first, fillTarget.second))
                return true;
        }
        return false;
    }

    void fill (long work)
    {
        const long budget = work;
        while (work > 0)
        {
            const auto [key, velocity] = fillTarget;
            if (key == 0 || engine->has_strike (key, velocity) || ! solvable (key, velocity))
            {
                if (! nextFillTarget())
                {
                    fillTarget = {};
                    return;
                }
                continue;
            }
            if (! engine->building_strike (key, velocity))
            {
                // a solve's setup (copying the key's strings) costs about a slice: it gets a block of its own
                if (work == budget)
                    solve (key, velocity, 1, false);
                return;
            }
            work -= solve (key, velocity, work);
        }
    }

    // --- process --------------------------------------------------------------------------------------------

    // Renders count frames with this range's events (offsets from its start), in pieces of at most maxFrames
    bool render (float* left, float* right, size_t count)
    {
        size_t done = 0, next = 0;
        bool ok = true;
        do
        {
            const size_t end = std::min (done + maxFrames, count), first = next;
            while (next < events.size() && (events[next].offset < end || (end == count && events[next].offset <= end)))
                events[next++].offset -= done;
            ok &= adapter->process (left + done, right + done, end - done, events.data() + first, next - first);
            done = end;
        } while (done < count);
        events.clear();
        return ok;
    }

    clap_process_status process (const clap_process& process)
    {
        const size_t frames = process.frames_count;
        if (process.audio_outputs_count < 1 || process.audio_outputs[0].channel_count < 2)
            return CLAP_PROCESS_CONTINUE;
        float* left = process.audio_outputs[0].data32[0];
        float* right = process.audio_outputs[0].data32[1];
        const double blockMs = 1000.0 * (double) frames / sampleRate;

        events.clear();
        serveWaiting (0.25 * blockMs);
        if (pedalDirty)
        {
            pedalDirty = false;
            pedalEvents (0, -1);
        }

        // events in time order; a Volume or Reverb change ends the range rendered with the old value
        size_t start = 0;
        bool ok = true;
        const uint32_t count = process.in_events->size (process.in_events);
        for (uint32_t i = 0; i < count; ++i)
        {
            const clap_event_header* header = process.in_events->get (process.in_events, i);
            if (header->space_id != CLAP_CORE_EVENT_SPACE_ID)
                continue;
            const size_t time = std::clamp<size_t> (header->time, start, frames);
            const size_t offset = time - start;
            switch (header->type)
            {
                case CLAP_EVENT_NOTE_ON:
                case CLAP_EVENT_NOTE_OFF:
                case CLAP_EVENT_NOTE_CHOKE:
                {
                    const auto* e = reinterpret_cast<const clap_event_note*> (header);
                    instrument::MidiEvent event;
                    event.offset = offset;
                    event.key = e->key;
                    event.channel = e->channel;
                    event.type = header->type == CLAP_EVENT_NOTE_ON ? instrument::MidiEvent::Type::note_on
                                                                    : instrument::MidiEvent::Type::note_off;
                    event.velocity = header->type == CLAP_EVENT_NOTE_ON ? clapVelocity (e->velocity) : 0;
                    note (event);
                    break;
                }
                case CLAP_EVENT_MIDI:
                    midi (*reinterpret_cast<const clap_event_midi*> (header), offset);
                    break;
                case CLAP_EVENT_PARAM_VALUE:
                {
                    const auto* e = reinterpret_cast<const clap_event_param_value*> (header);
                    hostChanged (e->param_id, e->value);
                    if (e->param_id == sustainId)
                    {
                        setValue (e->param_id, e->value);
                        pedalEvents (offset, -1);
                    }
                    else if (e->param_id < paramCount)
                    {
                        ok &= render (left + start, right + start, time - start);
                        start = time;
                        setValue (e->param_id, e->value);
                        applyValues();
                    }
                    break;
                }
                default:
                    break;
            }
        }
        ok &= render (left + start, right + start, frames - start);
        if (! ok)
        {
            std::fill (left, left + frames, 0.0f);
            std::fill (right, right + frames, 0.0f);
        }

        if (process.out_events != nullptr)
            writeEvents (*process.out_events);
        samplesSinceSend = std::min (samplesSinceSend + frames, sendInterval());
        // a host that never calls on_main_thread gets the page's updates from here, 30 times a second at most
        if (pageUpdateRequested && (samplesSinceRequest += frames) > (size_t) (sampleRate / 30.0))
            sendPageUpdates();

        // the background fill spends a sixth of the block's real time
        if ((! filled || wantedCount > 0 || fillTarget.first != 0) && waitingHead == waiting.size())
            fill ((long) (blockMs / 6.0 * workPerMs));
        return CLAP_PROCESS_CONTINUE;
    }

    // --- editor ---------------------------------------------------------------------------------------------

    // The page (ui/index.html) sends "hello", "begin <id>", "set <id> <value>" and "end <id>" and gets
    // "value <id> <value> <text>". Its edits go to the host as output events with the next process or flush; the
    // host's changes go to the page from on_main_thread. One thread, so that is between process calls.
    bool receive (const void* buffer, uint32_t size)
    {
        const std::string message (static_cast<const char*> (buffer), size);
        char type[8] {};
        unsigned id = 0;
        double value = 0;
        const int fields = std::sscanf (message.c_str(), "%7s %u %lf", type, &id, &value);
        if (fields >= 1 && std::strcmp (type, "hello") == 0)
        {
            std::fill (std::begin (pageValues), std::end (pageValues), true);
            sendPageUpdates();
            return true;
        }
        if (fields < 2 || id >= paramCount)
            return false;
        if (std::strcmp (type, "begin") == 0 || std::strcmp (type, "end") == 0)
            outEvents.push_back ({ (uint16_t) (type[0] == 'b' ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                              : CLAP_EVENT_PARAM_GESTURE_END), id, 0.0 });
        else if (std::strcmp (type, "set") == 0 && fields == 3 && std::isfinite (value))
        {
            setValue (id, value);
            if (id == sustainId)
                pedalDirty = true;
            else if (active)
                applyValues();
            outEvents.push_back ({ CLAP_EVENT_PARAM_VALUE, id, values[id] });
            sendValue (id); // the page shows the plugin's text for it
        }
        else
            return false;
        if (hostParams != nullptr && hostParams->request_flush != nullptr)
            hostParams->request_flush (host);
        return true;
    }

    void writeEvents (const clap_output_events& out)
    {
        for (const auto& e : outEvents)
        {
            if (e.type == CLAP_EVENT_PARAM_VALUE)
            {
                clap_event_param_value event {};
                event.header = { sizeof event, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
                event.param_id = e.id;
                event.note_id = -1;
                event.port_index = -1;
                event.channel = -1;
                event.key = -1;
                event.value = e.value;
                out.try_push (&out, &event.header);
            }
            else
            {
                clap_event_param_gesture event {};
                event.header = { sizeof event, 0, CLAP_CORE_EVENT_SPACE_ID, e.type, 0 };
                event.param_id = e.id;
                out.try_push (&out, &event.header);
            }
        }
        outEvents.clear();
    }

    // A value from the host (events, state), before setValue: the page follows if it changes
    void hostChanged (clap_id id, double value)
    {
        if (id >= paramCount || ! guiCreated
            || std::clamp (value, paramSpecs[id].min, paramSpecs[id].max) == values[id])
            return;
        pageValues[id] = true;
        if (pageUpdateRequested)
            return;
        pageUpdateRequested = true;
        samplesSinceRequest = 0;
        host->request_callback (host);
    }

    size_t sendInterval() const { return (size_t) (sampleRate / 30.0); }

    // hosts that call back after every block would get a frame per block, process sends the rest
    void sendPageUpdatesWhenDue()
    {
        if (pageUpdateRequested && (samplesSinceSend == 0 || samplesSinceSend >= sendInterval()))
            sendPageUpdates();
    }

    void sendPageUpdates()
    {
        pageUpdateRequested = false;
        samplesSinceSend = 0;
        for (clap_id id = 0; id < paramCount; ++id)
            if (std::exchange (pageValues[id], false))
                sendValue (id);
    }

    void sendValue (clap_id id);

    // --- state ----------------------------------------------------------------------------------------------

    static constexpr uint32_t stateMagic = 0x504f4f43; // "COOP"
    static constexpr uint32_t stateVersion = 1;

    bool save (const clap_ostream& stream) const
    {
        uint32_t header[2] { stateMagic, stateVersion };
        if (stream.write (&stream, header, sizeof header) != (int64_t) sizeof header)
            return false;
        return stream.write (&stream, values, sizeof values) == (int64_t) sizeof values;
    }

    bool load (const clap_istream& stream)
    {
        uint32_t header[2] {};
        double loaded[paramCount] {};
        if (stream.read (&stream, header, sizeof header) != (int64_t) sizeof header || header[0] != stateMagic
            || header[1] != stateVersion
            || stream.read (&stream, loaded, sizeof loaded) != (int64_t) sizeof loaded)
            return false;
        for (clap_id id = 0; id < paramCount; ++id)
        {
            hostChanged (id, loaded[id]);
            setValue (id, loaded[id]);
        }
        if (active)
            applyValues();
        pedalDirty = true;
        return true;
    }
};

Coopiano& self (const clap_plugin* plugin) { return *static_cast<Coopiano*> (plugin->plugin_data); }

// --- extensions ---------------------------------------------------------------------------------------------

uint32_t audioPortsCount (const clap_plugin*, bool isInput) { return isInput ? 0 : 1; }

bool audioPortsGet (const clap_plugin*, uint32_t index, bool isInput, clap_audio_port_info* info)
{
    if (isInput || index != 0)
        return false;
    info->id = 0;
    std::snprintf (info->name, sizeof info->name, "%s", "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports audioPorts { audioPortsCount, audioPortsGet };

uint32_t notePortsCount (const clap_plugin*, bool isInput) { return isInput ? 1 : 0; }

bool notePortsGet (const clap_plugin*, uint32_t index, bool isInput, clap_note_port_info* info)
{
    if (! isInput || index != 0)
        return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf (info->name, sizeof info->name, "%s", "Notes");
    return true;
}

const clap_plugin_note_ports notePorts { notePortsCount, notePortsGet };

uint32_t paramsCount (const clap_plugin*) { return paramCount; }

bool paramsInfo (const clap_plugin*, uint32_t index, clap_param_info* info)
{
    if (index >= paramCount)
        return false;
    const auto& spec = paramSpecs[index];
    std::memset (info, 0, sizeof *info);
    info->id = spec.id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf (info->name, sizeof info->name, "%s", spec.name);
    info->min_value = spec.min;
    info->max_value = spec.max;
    info->default_value = spec.value;
    return true;
}

bool paramsValue (const clap_plugin* plugin, clap_id id, double* value)
{
    if (id >= paramCount)
        return false;
    *value = self (plugin).values[id];
    return true;
}

bool paramsValueToText (const clap_plugin*, clap_id id, double value, char* text, uint32_t size)
{
    switch (id)
    {
        case volumeId: std::snprintf (text, size, "%.1f dB", value); return true;
        case reverbId: std::snprintf (text, size, "%.0f %%", value * 100.0); return true;
        case sustainId:
            if (value <= 0.0)
                std::snprintf (text, size, "Up");
            else if (value >= 1.0)
                std::snprintf (text, size, "Down");
            else
                std::snprintf (text, size, "%.0f %%", value * 100.0);
            return true;
        default: return false;
    }
}

void Coopiano::sendValue (clap_id id)
{
    if (! guiCreated || hostWebview == nullptr || hostWebview->send == nullptr)
        return;
    char text[64] {}, message[128] {};
    paramsValueToText (&plugin, id, values[id], text, sizeof text);
    const int length = std::snprintf (message, sizeof message, "value %u %.17g %s", id, values[id], text);
    hostWebview->send (host, message, (uint32_t) length);
}

bool paramsTextToValue (const clap_plugin*, clap_id id, const char* text, double* value)
{
    if (id >= paramCount)
        return false;
    if (id == sustainId && (std::strcmp (text, "Up") == 0 || std::strcmp (text, "Down") == 0))
    {
        *value = text[0] == 'D' ? 1.0 : 0.0;
        return true;
    }
    char* end = nullptr;
    const double number = std::strtod (text, &end);
    if (end == text)
        return false;
    *value = id == volumeId ? number : number / 100.0;
    return true;
}

void paramsFlush (const clap_plugin* plugin, const clap_input_events* in, const clap_output_events* out)
{
    auto& p = self (plugin);
    const uint32_t count = in->size (in);
    for (uint32_t i = 0; i < count; ++i)
    {
        const clap_event_header* header = in->get (in, i);
        if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto* e = reinterpret_cast<const clap_event_param_value*> (header);
            p.hostChanged (e->param_id, e->value);
            p.setValue (e->param_id, e->value);
        }
    }
    if (p.active)
        p.applyValues();
    p.pedalDirty = true;
    if (out != nullptr)
        p.writeEvents (*out);
}

const clap_plugin_params params { paramsCount, paramsInfo, paramsValue, paramsValueToText, paramsTextToValue,
                                  paramsFlush };

bool stateSave (const clap_plugin* plugin, const clap_ostream* stream) { return self (plugin).save (*stream); }
bool stateLoad (const clap_plugin* plugin, const clap_istream* stream) { return self (plugin).load (*stream); }

const clap_plugin_state state { stateSave, stateLoad };

uint32_t latencyGet (const clap_plugin* plugin)
{
    const auto& p = self (plugin);
    return p.adapter != nullptr ? (uint32_t) p.adapter->latency_samples() : 0;
}

const clap_plugin_latency latency { latencyGet };

// The editor: ui/index.html in the bundle, a fixed size (the studio's 16px layout at twice the size)
constexpr uint32_t editorWidth = 352, editorHeight = 144;

bool guiApiSupported (const clap_plugin*, const char* api, bool isFloating)
{
    return ! isFloating && std::strcmp (api, CLAP_WINDOW_API_WEBVIEW) == 0;
}

bool guiPreferredApi (const clap_plugin*, const char** api, bool* isFloating)
{
    *api = CLAP_WINDOW_API_WEBVIEW;
    *isFloating = false;
    return true;
}

bool guiCreate (const clap_plugin* plugin, const char* api, bool isFloating)
{
    if (! guiApiSupported (plugin, api, isFloating))
        return false;
    self (plugin).guiCreated = true;
    return true;
}

void guiDestroy (const clap_plugin* plugin)
{
    auto& p = self (plugin);
    p.guiCreated = false;
    p.pageUpdateRequested = false;
}

bool guiSize (const clap_plugin*, uint32_t* width, uint32_t* height)
{
    *width = editorWidth;
    *height = editorHeight;
    return true;
}

const clap_plugin_gui gui {
    guiApiSupported,
    guiPreferredApi,
    guiCreate,
    guiDestroy,
    [] (const clap_plugin*, double) { return false; },
    guiSize,
    [] (const clap_plugin*) { return false; },
    [] (const clap_plugin*, clap_gui_resize_hints*) { return false; },
    guiSize,
    [] (const clap_plugin*, uint32_t width, uint32_t height) { return width == editorWidth && height == editorHeight; },
    [] (const clap_plugin*, const clap_window*) { return true; },
    [] (const clap_plugin*, const clap_window*) { return false; },
    [] (const clap_plugin*, const char*) {},
    [] (const clap_plugin*) { return true; },
    [] (const clap_plugin*) { return true; }
};

int32_t webviewUri (const clap_plugin*, char* uri, uint32_t capacity)
{
    constexpr const char page[] = "/ui/index.html";
    if (uri != nullptr && capacity > 0)
        std::snprintf (uri, capacity, "%s", page);
    return (int32_t) sizeof page;
}

const clap_plugin_webview webview {
    webviewUri,
    [] (const clap_plugin*, const char*, char*, uint32_t, const clap_ostream*) { return false; }, // hosts serve the bundle
    [] (const clap_plugin* plugin, const void* buffer, uint32_t size) { return self (plugin).receive (buffer, size); }
};

// --- plugin -------------------------------------------------------------------------------------------------

const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                 "piano", CLAP_PLUGIN_FEATURE_STEREO, "physical modelling",
                                 "free and open source", nullptr };

const clap_plugin_descriptor descriptor {
    CLAP_VERSION_INIT,
    pluginId,
    "coopiano",
    "Peter Cooper",
    "https://github.com/peterc/coopiano",
    "",
    "",
    COOPIANO_VERSION,
    "A physically modelled piano: hammer, strings, soundboard, sympathetic resonance and room",
    features
};

const clap_plugin* createPlugin (const clap_host* host)
{
    auto* p = new (std::nothrow) Coopiano();
    if (p == nullptr)
        return nullptr;
    p->host = host;
    p->plugin.desc = &descriptor;
    p->plugin.plugin_data = p;
    p->plugin.init = [] (const clap_plugin* plugin) { return self (plugin).init(); };
    p->plugin.destroy = [] (const clap_plugin* plugin) { delete &self (plugin); };
    p->plugin.activate = [] (const clap_plugin* plugin, double rate, uint32_t minFrames, uint32_t maxFrames)
    { return self (plugin).activate (rate, minFrames, maxFrames); };
    p->plugin.deactivate = [] (const clap_plugin* plugin) { self (plugin).deactivate(); };
    p->plugin.start_processing = [] (const clap_plugin*) { return true; };
    p->plugin.stop_processing = [] (const clap_plugin*) {};
    p->plugin.reset = [] (const clap_plugin* plugin) { self (plugin).reset(); };
    p->plugin.process = [] (const clap_plugin* plugin, const clap_process* process)
    { return self (plugin).process (*process); };
    p->plugin.get_extension = [] (const clap_plugin*, const char* id) -> const void*
    {
        if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &audioPorts;
        if (std::strcmp (id, CLAP_EXT_NOTE_PORTS) == 0) return &notePorts;
        if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &params;
        if (std::strcmp (id, CLAP_EXT_STATE) == 0) return &state;
        if (std::strcmp (id, CLAP_EXT_LATENCY) == 0) return &latency;
        if (std::strcmp (id, CLAP_EXT_GUI) == 0) return &gui;
        if (std::strcmp (id, CLAP_EXT_WEBVIEW) == 0) return &webview;
        return nullptr;
    };
    p->plugin.on_main_thread = [] (const clap_plugin* plugin)
    {
        self (plugin).sendPageUpdatesWhenDue();
    };
    return &p->plugin;
}

uint32_t factoryCount (const clap_plugin_factory*) { return 1; }

const clap_plugin_descriptor* factoryDescriptor (const clap_plugin_factory*, uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}

const clap_plugin* factoryCreate (const clap_plugin_factory*, const clap_host* host, const char* id)
{
    return std::strcmp (id, pluginId) == 0 ? createPlugin (host) : nullptr;
}

const clap_plugin_factory factory { factoryCount, factoryDescriptor, factoryCreate };
} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry clap_entry {
    CLAP_VERSION_INIT,
    [] (const char*) { return true; },
    [] {},
    [] (const char* id) -> const void* { return std::strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr; }
};
