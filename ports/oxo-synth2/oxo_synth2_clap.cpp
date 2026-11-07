/*
    Oxo Synth2 as module.wasm: chab's Synth1 recreation (https://github.com/chab/oxo_synth2, MIT) as a CLAP
    instrument. Plain CLAP, no JUCE: upstream's header-only engine (external/oxo-synth2/EngineCore, C++20) behind
    this wrapper, unchanged (no patches needed: the engine is portable as it is — no threads, no files, no
    exceptions/RTTI on the render path; the only heap is the wave/filter table build in SetSampleRate, done in
    activate()).

    Parameters. The 99 patch parameters are CLAP parameters with ids 0..98 in Synth1's own index order (.sy1 line
    index = the original AU's parameter id, so patches and automation line up) and native integer values 0..127 (or
    their measured ranges): the value a host maps linearly between min and max and shows as a number is exactly the
    value Synth1's panel shows, and .sy1 files hold. Id 99 is the master volume (not part of a patch, kept in the
    state, like the original's chunk word 651). Ids 86..89 (the MIDI ctrl src/assign words) are hidden, not
    automatable, as in the original. value_to_text/text_to_value use the engine's own ParamFormat (the LCD's texts:
    wave names, filter types, note divisions, dB for the volume, ...), so every host shows what the panel shows.

    Processing. One Synth instance, driven like the VST3 processor drives it: BeginBlock(frames) per host block,
    SetTransport from the transport each block, events merged in time order — note on/off snapped to the original's
    16-sample sub-block grid (NoteEventOffset), everything else (parameters, CC, bend) at its exact sample, the
    block split where an event lands. MIDI dialects CLAP and MIDI. After half a second with no voice and below
    -120 dB the plugin sleeps (zeros, CLAP_PROCESS_SLEEP) and wakes on a note or a value change. The engine has no
    latency; the tail is unbounded (the delay line rings with high feedback), so clap.tail answers UINT32_MAX.

    State: a fixed layout of 472 bytes (State below): 'OS2W' + version + 99 x int32 + volume + the patch name, so an
    edit changes 4 bytes and openDAW's undo steps stay tiny. Loading also accepts the macOS plugin's own 'OXS2'
    chunks (v1..v4: volume + optional program/theme headers + .sy1 text), so a session saved by the VST3/AU loads
    here. A patch load (from the page, or a state) goes through the engine's patch-switch fade (BeginPatchChange +
    ClearDelay), like the editor's loads.

    Editor: ui/index.html through clap.gui (the webview API) and clap.webview/3, at the panel's 770 x 560 design
    size, resizable at that aspect ratio (0.5..2x). Messages are binary, little endian, see the Page and Plugin
    enums: the page says hello and gets the parameter table (JSON, once, prebuilt in init) and a snapshot, then
    polls at most every 33 ms and gets only what changed. The page's factory patches (the bundle's patches.json,
    Synth2's own factory bank as .sy1 values) load as one gesture around all changed values: one undo step.
*/

#include <clap/clap.h>
#include <clap/ext/draft/param-hovered.h>
#include <clap/ext/draft/webview.h>
#include <clap/ext/tail.h>

#include "EngineCore/InitPatch.hpp"
#include "EngineCore/ParamFormat.hpp"
#include "EngineCore/Synth.hpp"
#include "EngineCore/Sy1Codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace
{
using namespace oxo::s2;

constexpr const char* pluginId = "com.oxo.music.oxosynth2";
constexpr uint32_t paramCount = kParamCount + 1; // 0..98 the patch, 99 the master volume

// The editor's design size (SynthPanelView's 770 x 560 canvas); the page scales to its window, hosts resize at
// this aspect ratio (openDAW's zoom levels), from half to twice the size
constexpr uint32_t editorWidth = 770, editorHeight = 560;
constexpr double minEditorScale = 0.5, maxEditorScale = 2.0;

constexpr size_t nameSize = 64;

// The saved state, and the page's snapshot after its 4-byte header: fixed layout, so an edit changes a few bytes
struct State
{
    char magic[4];        // "OS2W"
    uint32_t version;     // 1
    int32_t values[kParamCount];
    int32_t volume;
    char name[nameSize];
};
static_assert (sizeof (State) == 4 + 4 + 99 * 4 + 4 + nameSize);

constexpr uint32_t stateMagic = 0x57533253; // "OS2W"
constexpr uint32_t stateVersion = 1;

// Message types, the first byte of every message. The page's parameter messages follow the shared numbering
// (modules/opendaw-clap/opendaw_page.h): poll 1, gestures 2..4, hover 7 would be 6 here — we keep 6 for hover to
// stay near it; the page and this file define them together (see ui/index.html)
enum Page : uint8_t
{
    pageHello = 1,
    pagePoll = 2,
    pageBegin = 3,  // u32 id at 4
    pageSet = 4,    // u32 id at 4, f64 value at 8 (the native value)
    pageEnd = 5,    // u32 id at 4
    pageHover = 6,  // u32 id at 4, 0xffffffff for none
    pagePatch = 8,  // u16 name length at 2, name bytes at 4, then kParamCount x int32 values, optional u16 .sy1 version
    pageInit = 9,   // the INIT patch
    pagePanic = 10  // all notes off
};

enum Plugin : uint8_t
{
    pluginDescribe = 0x81, // JSON from 4 on: the parameter table (built once in init)
    pluginSnapshot = 0x82, // a State from 4 on (values, volume, name)
    pluginValues = 0x83,   // u16 count at 2, then count x (u32 id, u32 0, f64 value), 16 bytes each
    pluginActivity = 0x84  // u8 gated voices at 1, u64 voice states at 8, f32 peak L at 16, peak R at 20
};

// The parameters with named choices (CLAP_PARAM_IS_ENUM, and the page's labels come from ParamFormat)
bool isEnum (int id)
{
    switch (id)
    {
        case kOsc1Shape: case kOsc2Shape: case kSubShape: case kSubOctave:
        case kFilterType: case kLfo1Type: case kLfo2Type: case kLfo1Dest: case kLfo2Dest:
        case kModEnvDest: case kArpType: case kArpOctRange: case kArpBeat:
        case kDelayTime: case kDelayType: case kPlayMode: case kChorusType: case kEffectType:
            return true;
        default: return false;
    }
}

bool isHidden (int id) { return id >= kCtrlSrc1 && id <= kCtrlAssign2; }

// Host parameter groups, the panel's sections
const char* paramModule (int id)
{
    if (id == kVolumeParamId) return "Master";
    if (id <= kModEnvDecay || id == kOsc1FM || id == kSubGain || id == kSubShape || id == kSubOctave
        || id == kOsc1Detune || id == kModEnvDest || id == kOscFineTune || id == kOscPhase)
        return "Oscillators";
    if (id >= kLfo1Dest && id <= kLfo2KeySync) return "LFO";
    if (id == kBendRange || id >= kWheelSpeedSens && id <= kCtrlAssign2) return "Wheel / MIDI";
    if (id >= kAmpAttack && id <= kAmpVelSens) return "Amplifier";
    if (id >= kFilterType && id <= kFilterVelSwitch) return "Filter";
    if (id == kArpOn || id >= kArpType && id <= kArpGate) return "Arpeggiator";
    if (id >= kEffectOn && id <= kEffectLevel) return "Effect";
    if (id == kEqTone || id >= kEqFreq && id <= kEqQ || id == kPan) return "Equalizer";
    if (id == kDelayOn || id == kDelayTime || id == kDelayFeedback || id == kDelayDryWet || id == kDelayType
        || id == kDelaySpread || id == kDelayTone)
        return "Delay";
    if (id == kChorusOn || id >= kChorusTime && id <= kChorusType) return "Chorus";
    return "Voice";
}

struct OxoSynth2
{
    clap_plugin plugin {};
    const clap_host* host = nullptr;
    const clap_host_params* hostParams = nullptr;
    const clap_host_state* hostState = nullptr;
    const clap_host_webview* hostWebview = nullptr;
    const clap_host_param_hovered* hostHovered = nullptr;

    Synth synth;
    int32_t volume = kVolumeParam.def;
    char patchName[nameSize] = "INIT";

    double sampleRate = 44100.0;
    bool active = false;
    size_t quietSamples = 0;
    bool sleeping = false;

    // editor: what the page did, for the host (output events), and what the host did, for the page
    struct OutEvent
    {
        uint16_t type;
        clap_id id;
        double value;
    };
    OutEvent outEvents[256];
    uint32_t outRead = 0, outWrite = 0;
    bool guiCreated = false;
    uint32_t guiWidth = editorWidth, guiHeight = editorHeight;
    std::string describe;         // the JSON parameter table, built once in init
    std::vector<uint8_t> message; // scratch for the answers
    bool pageValues[paramCount] {};
    bool pageSnapshot = false;
    uint8_t lastActivity[24] {};
    float meterPeaks[2] {};

    // --- setup ----------------------------------------------------------------------------------------------

    bool init()
    {
        hostParams = static_cast<const clap_host_params*> (host->get_extension (host, CLAP_EXT_PARAMS));
        hostState = static_cast<const clap_host_state*> (host->get_extension (host, CLAP_EXT_STATE));
        hostWebview = static_cast<const clap_host_webview*> (host->get_extension (host, CLAP_EXT_WEBVIEW));
        hostHovered = static_cast<const clap_host_param_hovered*> (host->get_extension (host, CLAP_EXT_PARAM_HOVERED));
        message.reserve (8 + sizeof (State));
        buildDescribe();
        setName ("INIT");
        return true;
    }

    // The parameter table for the page, once: every parameter with its range, flags, section, display name and —
    // for the stepped ones in a small range — every value's label as ParamFormat gives it (the LCD's texts). The
    // LFO speeds also carry their tempo-synced labels (the page picks by the sync switch); the effect level's
    // percent labels for the phasers the page formats from the effect type.
    void buildDescribe()
    {
        std::string json = "{\"version\":\"" OXO_SYNTH2_VERSION "\",\"params\":[";
        char buffer[1024];
        for (uint32_t id = 0; id < paramCount; ++id)
        {
            const ParamInfo& p = Info ((int) id);
            const bool hidden = isHidden ((int) id);
            std::snprintf (buffer, sizeof buffer,
                           "%s{\"id\":%u,\"key\":\"%s\",\"name\":\"%s\",\"displayName\":\"%s\",\"module\":\"%s\","
                           "\"min\":%d,\"max\":%d,\"def\":%d,\"automatable\":%s,\"hidden\":%s,\"enum\":%s",
                           id == 0 ? "" : ",", id, p.key, p.name, DisplayName ((int) id, -1), paramModule ((int) id),
                           p.min, p.max, p.def, p.automatable ? "true" : "false", hidden ? "true" : "false",
                           isEnum ((int) id) ? "true" : "false");
            json += buffer;
            const int range = p.max - p.min;
            if (! hidden && range <= 128)
            {
                json += ",\"labels\":[";
                for (int v = p.min; v <= p.max; ++v)
                    json += (v == p.min ? "\"" : ",\"") + escape (FormatParam ((int) id, v)) + "\"";
                json += "]";
                if (id == kLfo1Speed || id == kLfo2Speed)
                {
                    json += ",\"syncLabels\":[";
                    for (int v = p.min; v <= p.max; ++v)
                        json += (v == p.min ? "\"" : ",\"") + escape (FormatParam ((int) id, v, true)) + "\"";
                    json += "]";
                }
            }
            json += "}";
        }
        json += "],\"effectKnobNames\":[";
        for (int t = 0; t < 10; ++t)
        {
            std::snprintf (buffer, sizeof buffer, "%s[\"%s\",\"%s\",\"%s\"]", t == 0 ? "" : ",",
                           DisplayName (kEffectCtrl1, t), DisplayName (kEffectCtrl2, t), DisplayName (kEffectLevel, t));
            json += buffer;
        }
        json += "],\"init\":[";
        const PatchValues init = InitPatch().values;
        for (int i = 0; i < kParamCount; ++i)
        {
            std::snprintf (buffer, sizeof buffer, "%s%d", i == 0 ? "" : ",", init[i]);
            json += buffer;
        }
        json += "]}";
        describe = std::move (json);
    }

    static std::string escape (const std::string& text)
    {
        std::string out;
        out.reserve (text.size());
        for (char c : text)
        {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out;
    }

    void setName (const char* text)
    {
        std::memset (patchName, 0, nameSize);
        std::strncpy (patchName, text, nameSize - 1);
    }

    bool activate (double rate, uint32_t, uint32_t)
    {
        sampleRate = rate;
        synth.SetSampleRate (rate); // builds the wave/filter tables (the only heap, once per rate)
        synth.SetVolume (volume);
        quietSamples = 0;
        sleeping = false;
        active = true;
        return true;
    }

    void deactivate() { active = false; }

    void reset()
    {
        synth.Reset();
        sleeping = true;
        quietSamples = 0;
        meterPeaks[0] = meterPeaks[1] = 0.0f;
    }

    // --- values ---------------------------------------------------------------------------------------------

    // A native value (the CLAP values are the native ones): applies to the engine, tells the page if it changed
    void setParamNative (clap_id id, int32_t native, bool wakeEngine = true)
    {
        if (id < 0 || id >= (clap_id) paramCount)
            return;
        native = ClampValue ((int) id, native);
        if (id == kVolumeParamId)
        {
            if (volume == native)
                return;
            volume = native;
            synth.SetVolume (volume);
        }
        else
        {
            if (synth.Param ((int) id) == native)
                return;
            synth.SetParam ((int) id, native);
        }
        pageValues[id] = true;
        if (wakeEngine)
            wake();
    }

    // A value from the host (events, flush): the CLAP value is the native one
    void hostValue (clap_id id, double value)
    {
        if (! std::isfinite (value))
            return;
        setParamNative (id, (int32_t) std::llround (value));
    }

    double clapValue (clap_id id) const
    {
        if (id < 0 || id >= (clap_id) paramCount)
            return 0.0;
        return (double) (id == kVolumeParamId ? volume : synth.Param ((int) id));
    }

    void wake()
    {
        sleeping = false;
        quietSamples = 0;
    }

    // --- patch loads ----------------------------------------------------------------------------------------

    // A whole patch (the page's patch list, a state load): the engine's patch-switch fade, the name, and every
    // changed value to the host inside one gesture, so the load is one undo step
    void applyPatch (const PatchValues& values, const char* name, bool notifyHost)
    {
        bool changed[kParamCount] {};
        for (int i = 0; i < kParamCount; ++i)
            changed[i] = ClampValue (i, values[i]) != synth.Param (i);
        synth.ClearDelay();
        synth.BeginPatchChange();
        for (int i = 0; i < kParamCount; ++i)
            synth.SetParam (i, ClampValue (i, values[i]));
        setName (name);
        if (notifyHost)
        {
            for (int i = 0; i < kParamCount; ++i)
                if (changed[i])
                    pushOut (CLAP_EVENT_PARAM_GESTURE_BEGIN, i, 0.0);
            for (int i = 0; i < kParamCount; ++i)
                if (changed[i])
                    pushOut (CLAP_EVENT_PARAM_VALUE, i, clapValue (i));
            for (int i = 0; i < kParamCount; ++i)
                if (changed[i])
                    pushOut (CLAP_EVENT_PARAM_GESTURE_END, i, 0.0);
            markDirty();
            requestFlush();
        }
        for (int i = 0; i < kParamCount; ++i)
            pageValues[i] = true;
        pageValues[kVolumeParamId] = true;
        pageSnapshot = true;
        wake();
    }

    void markDirty()
    {
        if (hostState != nullptr && hostState->mark_dirty != nullptr)
            hostState->mark_dirty (host);
    }

    void requestFlush()
    {
        if (hostParams != nullptr && hostParams->request_flush != nullptr)
            hostParams->request_flush (host);
    }

    void pushOut (uint16_t type, clap_id id, double value)
    {
        if (outWrite - outRead >= 256)
            return;
        outEvents[outWrite % 256] = OutEvent { type, id, value };
        outWrite++;
    }

    void writeEvents (const clap_output_events& out)
    {
        while (outRead != outWrite)
        {
            const OutEvent& e = outEvents[outRead % 256];
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
            outRead++;
        }
    }

    // --- events ---------------------------------------------------------------------------------------------

    // An event at `time`: what comes before it is rendered first, the event lands at its sample (notes snap to
    // their 16-sample sub-block at the call site)
    void eventTime (size_t& start, size_t time, float* left, float* right)
    {
        if (left != nullptr && time > start)
        {
            render (left, right, start, time);
            start = time;
        }
        if (sleeping)
            wake();
    }

    void midi (const clap_event_midi& event, size_t& start, size_t time, float* left, float* right)
    {
        const int status = event.data[0] & 0xf0;
        const int data1 = event.data[1] & 0x7f, data2 = event.data[2] & 0x7f;
        if (status == 0x90 && data2 > 0)
        {
            eventTime (start, time, left, right);
            synth.NoteOn (data1, data2);
        }
        else if (status == 0x80 || status == 0x90)
        {
            eventTime (start, time, left, right);
            synth.NoteOff (data1);
        }
        else if (status == 0xb0)
        {
            eventTime (start, time, left, right);
            synth.ControlChange (data1, data2); // 64 = sustain, 120/123 = all notes off, inside the engine
        }
        else if (status == 0xe0)
        {
            eventTime (start, time, left, right);
            synth.PitchBend ((data2 << 7) | data1);
        }
        else if (status == 0xd0)
        {
            eventTime (start, time, left, right);
            synth.ChannelPressure (data1);
        }
    }

    // --- process --------------------------------------------------------------------------------------------

    // Renders frames [from, to) of this call; tracks the output peak and the sleep
    void render (float* left, float* right, size_t from, size_t to)
    {
        if (to <= from)
            return;
        const int count = (int) (to - from);
        if (sleeping)
        {
            std::fill (left + from, left + to, 0.0f);
            std::fill (right + from, right + to, 0.0f);
            return;
        }
        synth.Render (left + from, right + from, count);
        double peakLeft = 0.0, peakRight = 0.0;
        for (int i = 0; i < count; ++i)
        {
            peakLeft = std::max (peakLeft, (double) std::fabs (left[from + i]));
            peakRight = std::max (peakRight, (double) std::fabs (right[from + i]));
        }
        meterPeaks[0] = std::max (meterPeaks[0], (float) peakLeft);
        meterPeaks[1] = std::max (meterPeaks[1], (float) peakRight);
        // asleep after half a second without a voice and below -120 dB (the delay tail counts in the peak)
        if (synth.VoiceStates() == 0 && peakLeft < 1.0e-6 && peakRight < 1.0e-6)
            sleeping = (quietSamples += (size_t) count) > (size_t) (sampleRate / 2);
        else
            quietSamples = 0;
    }

    clap_process_status process (const clap_process& process)
    {
        const size_t frames = process.frames_count;

        if (const auto* transport = process.transport; transport != nullptr)
        {
            const double bpm = (transport->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0 && transport->tempo > 0.0
                                   ? transport->tempo
                                   : 120.0;
            const double ppq = (transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0
                                   ? (double) transport->song_pos_beats / (double) CLAP_BEATTIME_FACTOR
                                   : 0.0;
            synth.SetTransport (bpm, ppq, (transport->flags & CLAP_TRANSPORT_IS_PLAYING) != 0);
        }

        synth.BeginBlock ((int) frames); // the engine's voice-end grid counts from the host block's start

        float* left = nullptr;
        float* right = nullptr;
        if (process.audio_outputs_count >= 1 && process.audio_outputs[0].channel_count >= 2)
        {
            left = process.audio_outputs[0].data32[0];
            right = process.audio_outputs[0].data32[1];
        }

        size_t start = 0;
        const uint32_t count = process.in_events->size (process.in_events);
        for (uint32_t i = 0; i < count; ++i)
        {
            const clap_event_header* header = process.in_events->get (process.in_events, i);
            if (header->space_id != CLAP_CORE_EVENT_SPACE_ID)
                continue;
            const size_t time = std::min<size_t> (header->time, frames > 0 ? frames - 1 : 0);
            switch (header->type)
            {
                case CLAP_EVENT_NOTE_ON:
                {
                    const auto* e = reinterpret_cast<const clap_event_note*> (header);
                    if (e->key < 0 || e->key > 127)
                        break;
                    const size_t at = std::clamp<size_t> ((size_t) NoteEventOffset ((int) time), start, frames);
                    eventTime (start, at, left, right);
                    synth.NoteOn (e->key, std::clamp<int> ((int) std::lround (e->velocity * 127.0), 1, 127));
                    break;
                }
                case CLAP_EVENT_NOTE_OFF:
                {
                    const auto* e = reinterpret_cast<const clap_event_note*> (header);
                    if (e->key < 0 || e->key > 127)
                        break;
                    const size_t at = std::clamp<size_t> ((size_t) NoteEventOffset ((int) time), start, frames);
                    eventTime (start, at, left, right);
                    synth.NoteOff (e->key);
                    break;
                }
                case CLAP_EVENT_NOTE_CHOKE:
                {
                    const auto* e = reinterpret_cast<const clap_event_note*> (header);
                    const size_t at = std::clamp<size_t> ((size_t) NoteEventOffset ((int) time), start, frames);
                    eventTime (start, at, left, right);
                    if (e->key >= 0 && e->key <= 127)
                        synth.NoteOff (e->key);
                    else
                        synth.AllNotesOff (true);
                    break;
                }
                case CLAP_EVENT_NOTE_EXPRESSION:
                {
                    const auto* e = reinterpret_cast<const clap_event_note_expression*> (header);
                    if (e->expression_id == CLAP_NOTE_EXPRESSION_PRESSURE)
                        synth.ChannelPressure (std::clamp<int> ((int) std::lround (e->value * 127.0), 0, 127));
                    break;
                }
                case CLAP_EVENT_MIDI:
                    midi (*reinterpret_cast<const clap_event_midi*> (header), start, time, left, right);
                    break;
                case CLAP_EVENT_PARAM_VALUE:
                {
                    const auto* e = reinterpret_cast<const clap_event_param_value*> (header);
                    eventTime (start, time, left, right);
                    hostValue (e->param_id, e->value);
                    break;
                }
                default:
                    break;
            }
        }
        if (left != nullptr)
            render (left, right, start, frames);

        if (process.out_events != nullptr)
            writeEvents (*process.out_events);
        return sleeping ? CLAP_PROCESS_SLEEP : CLAP_PROCESS_CONTINUE;
    }

    // --- editor ---------------------------------------------------------------------------------------------

    bool receive (const uint8_t* data, uint32_t size)
    {
        if (size < 1)
            return false;
        const auto u32 = [data] (size_t at) { return (uint32_t) (data[at] | data[at + 1] << 8 | data[at + 2] << 16 | (uint32_t) data[at + 3] << 24); };
        const auto f64 = [data] (size_t at)
        {
            double value;
            std::memcpy (&value, data + at, sizeof value);
            return value;
        };
        switch (data[0])
        {
            case pageHello:
                sendDescribe();
                pageSnapshot = true;
                std::fill (std::begin (lastActivity), std::end (lastActivity), 0xff);
                sendPageUpdates();
                return true;
            case pagePoll:
                sendPageUpdates();
                return true;
            case pageBegin:
            case pageEnd:
            {
                if (size < 8)
                    return false;
                const clap_id id = (clap_id) u32 (4);
                if (id >= paramCount)
                    return false;
                pushOut ((uint16_t) (data[0] == pageBegin ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END),
                         id, 0.0);
                if (data[0] == pageEnd)
                    markDirty();
                requestFlush();
                return true;
            }
            case pageSet:
            {
                if (size < 16)
                    return false;
                const clap_id id = (clap_id) u32 (4);
                if (id >= paramCount)
                    return false;
                setParamNative (id, (int32_t) std::llround (f64 (8)));
                pushOut (CLAP_EVENT_PARAM_VALUE, id, clapValue (id));
                requestFlush();
                return true;
            }
            case pageHover:
            {
                if (size < 8 || hostHovered == nullptr || hostHovered->update == nullptr)
                    return false;
                const clap_id id = (clap_id) u32 (4);
                hostHovered->update (host, id < paramCount ? id : CLAP_INVALID_ID);
                return true;
            }
            case pagePatch:
            {
                if (size < 6)
                    return false;
                const uint32_t nameLength = data[2] | data[3] << 8;
                if (size < 4 + nameLength + kParamCount * 4)
                    return false;
                PatchValues values = DefaultValues();
                const uint8_t* at = data + 4 + nameLength;
                for (int i = 0; i < kParamCount; ++i)
                {
                    int32_t v;
                    std::memcpy (&v, at + 4 * i, 4);
                    values[i] = ClampValue (i, v);
                }
                // optional u16 .sy1 version after the values: legacy versions convert exactly like the original
                if (size >= 4 + nameLength + kParamCount * 4 + 2)
                {
                    const int version = at[kParamCount * 4] | at[kParamCount * 4 + 1] << 8;
                    if (version > 0 && version < 112)
                        ConvertLegacy (values, version);
                }
                std::string name (reinterpret_cast<const char*> (data + 4), nameLength);
                applyPatch (values, name.c_str(), true);
                return true;
            }
            case pageInit:
                applyPatch (InitPatch().values, "INIT", true);
                return true;
            case pagePanic:
                synth.Panic();
                return true;
            default:
                return false;
        }
    }

    void send (const uint8_t* bytes, size_t size)
    {
        if (guiCreated && hostWebview != nullptr && hostWebview->send != nullptr)
            hostWebview->send (host, bytes, (uint32_t) size);
    }

    void sendDescribe()
    {
        message.assign (4, 0);
        message[0] = pluginDescribe;
        message.insert (message.end(), describe.begin(), describe.end());
        send (message.data(), message.size());
    }

    // What changed since the last answer: a snapshot after a hello or a load, else the values the host changed,
    // and the activity when it moved. Nothing when nothing did.
    void sendPageUpdates()
    {
        if (pageSnapshot)
        {
            pageSnapshot = false;
            std::fill (std::begin (pageValues), std::end (pageValues), false);
            message.assign (4, 0);
            message[0] = pluginSnapshot;
            State state {};
            fillState (state);
            const auto* bytes = reinterpret_cast<const uint8_t*> (&state);
            message.insert (message.end(), bytes + 8, bytes + sizeof (State));
            send (message.data(), message.size());
        }
        uint16_t changed = 0;
        message.assign (4, 0);
        for (clap_id id = 0; id < paramCount; ++id)
        {
            if (! pageValues[id])
                continue;
            pageValues[id] = false;
            uint8_t entry[16] {};
            std::memcpy (entry, &id, 4);
            const double value = clapValue (id);
            std::memcpy (entry + 8, &value, 8);
            message.insert (message.end(), entry, entry + 16);
            ++changed;
        }
        if (changed > 0)
        {
            message[0] = pluginValues;
            message[2] = (uint8_t) changed;
            message[3] = (uint8_t) (changed >> 8);
            send (message.data(), message.size());
        }
        sendActivity();
    }

    void sendActivity()
    {
        uint8_t activity[24] {};
        activity[0] = pluginActivity;
        activity[1] = (uint8_t) std::clamp (synth.GatedVoices(), 0, 255);
        const uint64_t voices = synth.VoiceStates();
        std::memcpy (activity + 8, &voices, 8);
        std::memcpy (activity + 16, &meterPeaks[0], 4);
        std::memcpy (activity + 20, &meterPeaks[1], 4);
        meterPeaks[0] = meterPeaks[1] = 0.0f;
        if (std::memcmp (activity, lastActivity, sizeof activity) == 0)
            return;
        std::memcpy (lastActivity, activity, sizeof activity);
        send (activity, sizeof activity);
    }

    // --- state ----------------------------------------------------------------------------------------------

    void fillState (State& state) const
    {
        state.magic[0] = 'O'; state.magic[1] = 'S'; state.magic[2] = '2'; state.magic[3] = 'W';
        state.version = stateVersion;
        const PatchValues& values = synth.Values();
        for (int i = 0; i < kParamCount; ++i)
            state.values[i] = values[i];
        state.volume = volume;
        std::memcpy (state.name, patchName, nameSize);
    }

    bool save (const clap_ostream& stream) const
    {
        State state {};
        fillState (state);
        const auto* bytes = reinterpret_cast<const uint8_t*> (&state);
        size_t written = 0;
        while (written < sizeof (State))
        {
            const int64_t n = stream.write (&stream, bytes + written, sizeof (State) - written);
            if (n <= 0)
                return false;
            written += (size_t) n;
        }
        return true;
    }

    bool load (const clap_istream& stream)
    {
        uint8_t bytes[sizeof (State)];
        size_t read = 0;
        while (read < sizeof bytes)
        {
            const int64_t n = stream.read (&stream, bytes + read, sizeof bytes - read);
            if (n <= 0)
                break;
            read += (size_t) n;
        }
        if (read < 8)
            return false;
        if (std::memcmp (bytes, "OS2W", 4) == 0)
        {
            if (read != sizeof (State))
                return false;
            State state;
            std::memcpy (&state, bytes, sizeof state);
            if (state.version != stateVersion)
                return false;
            PatchValues values;
            for (int i = 0; i < kParamCount; ++i)
                values[i] = ClampValue (i, state.values[i]);
            applyPatch (values, state.name, false);
            setParamNative (kVolumeParamId, ClampValue (kVolumeParamId, state.volume), false);
            return true;
        }
        // the macOS plugin's own chunk ('OXS2' + version + volume + [program] + [theme] + .sy1 text), v1..v4
        if (std::memcmp (bytes, "OXS2", 4) == 0)
        {
            const auto le32 = [&] (size_t at) { return (uint32_t) (bytes[at] | bytes[at + 1] << 8 | bytes[at + 2] << 16 | (uint32_t) bytes[at + 3] << 24); };
            const uint32_t version = le32 (4);
            if (version < 1 || version > 4 || read < 8)
                return false;
            size_t at = 8;
            uint32_t volumeWord = 64;
            if (version >= 2)
            {
                if (read < at + 4) return false;
                volumeWord = le32 (at);
                at += 4;
            }
            if (version >= 3)
            {
                if (read < at + 8) return false;
                const uint32_t length = le32 (at + 4);
                if (read < at + 8 + length) return false;
                at += 8 + length;
            }
            if (version >= 4)
            {
                if (read < at + 4) return false;
                const uint32_t length = le32 (at);
                if (read < at + 4 + length) return false;
                at += 4 + length;
            }
            const Sy1Patch patch = ParseSy1 (std::string_view (reinterpret_cast<const char*> (bytes) + at, read - at));
            applyPatch (patch.values, patch.name.c_str(), false);
            setParamNative (kVolumeParamId, ClampValue (kVolumeParamId, (int32_t) volumeWord), false);
            return true;
        }
        return false;
    }
};

OxoSynth2& self (const clap_plugin* plugin) { return *static_cast<OxoSynth2*> (plugin->plugin_data); }

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
    const ParamInfo& p = Info ((int) index);
    std::memset (info, 0, sizeof *info);
    info->id = (clap_id) index;
    info->flags = CLAP_PARAM_IS_STEPPED;
    if (p.automatable)
        info->flags |= CLAP_PARAM_IS_AUTOMATABLE;
    if (isEnum ((int) index))
        info->flags |= CLAP_PARAM_IS_ENUM;
    if (isHidden ((int) index))
        info->flags |= CLAP_PARAM_IS_HIDDEN;
    std::snprintf (info->name, sizeof info->name, "%s", p.name);
    std::snprintf (info->module, sizeof info->module, "%s", paramModule ((int) index));
    info->min_value = p.min;
    info->max_value = p.max;
    info->default_value = p.def;
    return true;
}

bool paramsValue (const clap_plugin* plugin, clap_id id, double* value)
{
    if (id < 0 || id >= (clap_id) paramCount)
        return false;
    *value = self (plugin).clapValue (id);
    return true;
}

// The LCD's text for a value: ParamFormat with the current tempo-sync switch and effect type
bool paramsValueToText (const clap_plugin* plugin, clap_id id, double clapValue, char* text, uint32_t size)
{
    if (id < 0 || id >= (clap_id) paramCount || size == 0)
        return false;
    auto& p = self (plugin);
    const int native = ClampValue ((int) id, (int32_t) std::llround (clapValue));
    const int syncSwitch = TempoSyncSwitch ((int) id);
    const bool tempoSync = syncSwitch >= 0 && p.synth.Param (syncSwitch) != 0;
    const int effectType = p.synth.Param (kEffectType);
    const std::string label = FormatParam ((int) id, native, tempoSync, effectType);
    std::snprintf (text, size, "%s", label.c_str());
    return true;
}

bool paramsTextToValue (const clap_plugin* plugin, clap_id id, const char* text, double* value)
{
    if (id < 0 || id >= (clap_id) paramCount || text == nullptr)
        return false;
    const ParamInfo& p = Info ((int) id);
    char* end = nullptr;
    const double number = std::strtod (text, &end);
    if (end != text)
    {
        *value = (double) ClampValue ((int) id, (int32_t) std::llround (number));
        return true;
    }
    // a choice's label (what value_to_text gave)
    if (p.max - p.min <= 128)
    {
        auto& self_ = self (plugin);
        const int syncSwitch = TempoSyncSwitch ((int) id);
        const bool tempoSync = syncSwitch >= 0 && self_.synth.Param (syncSwitch) != 0;
        for (int v = p.min; v <= p.max; ++v)
            if (FormatParam ((int) id, v, tempoSync, self_.synth.Param (kEffectType)) == text)
            {
                *value = v;
                return true;
            }
    }
    return false;
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
            p.hostValue (e->param_id, e->value);
        }
    }
    if (out != nullptr)
        p.writeEvents (*out);
}

const clap_plugin_params params { paramsCount, paramsInfo, paramsValue, paramsValueToText, paramsTextToValue,
                                  paramsFlush };

bool stateSave (const clap_plugin* plugin, const clap_ostream* stream) { return self (plugin).save (*stream); }
bool stateLoad (const clap_plugin* plugin, const clap_istream* stream) { return self (plugin).load (*stream); }

const clap_plugin_state state { stateSave, stateLoad };

uint32_t tailGet (const clap_plugin*) { return UINT32_MAX; } // the delay line rings with high feedback

const clap_plugin_tail tail { tailGet };

// The editor: ui/index.html in the bundle, resizable at its aspect ratio

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

void guiDestroy (const clap_plugin* plugin) { self (plugin).guiCreated = false; }

bool guiSize (const clap_plugin* plugin, uint32_t* width, uint32_t* height)
{
    *width = self (plugin).guiWidth;
    *height = self (plugin).guiHeight;
    return true;
}

bool guiResizeHints (const clap_plugin*, clap_gui_resize_hints* hints)
{
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = true;
    hints->aspect_ratio_width = editorWidth;
    hints->aspect_ratio_height = editorHeight;
    return true;
}

// The size of the editor's aspect ratio closest to the request (the smaller of its two scales), within the limits
bool guiAdjustSize (const clap_plugin*, uint32_t* width, uint32_t* height)
{
    const double scale = std::clamp (std::min ((double) *width / editorWidth, (double) *height / editorHeight),
                                      minEditorScale, maxEditorScale);
    *width = (uint32_t) std::lround (editorWidth * scale);
    *height = (uint32_t) std::lround (editorHeight * scale);
    return true;
}

bool guiSetSize (const clap_plugin* plugin, uint32_t width, uint32_t height)
{
    uint32_t w = width, h = height;
    guiAdjustSize (plugin, &w, &h);
    if (w != width || h != height)
        return false;
    self (plugin).guiWidth = width;
    self (plugin).guiHeight = height;
    return true;
}

const clap_plugin_gui gui {
    guiApiSupported,
    guiPreferredApi,
    guiCreate,
    guiDestroy,
    [] (const clap_plugin*, double) { return false; },
    guiSize,
    [] (const clap_plugin*) { return true; },
    guiResizeHints,
    guiAdjustSize,
    guiSetSize,
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
    [] (const clap_plugin* plugin, const void* buffer, uint32_t size)
    { return self (plugin).receive (static_cast<const uint8_t*> (buffer), size); }
};

// --- plugin -------------------------------------------------------------------------------------------------

const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                 CLAP_PLUGIN_FEATURE_STEREO, "free and open source", nullptr };

const clap_plugin_descriptor descriptor {
    CLAP_VERSION_INIT,
    pluginId,
    "Oxo Synth2",
    "chab (Oxo Music)",
    "https://github.com/chab/oxo_synth2",
    "",
    "",
    OXO_SYNTH2_VERSION,
    "A recreation of Daichi Laboratory's Synth1: same 99 parameters, loads .sy1 patches, same sound",
    features
};

const clap_plugin* createPlugin (const clap_host* host)
{
    auto* p = new (std::nothrow) OxoSynth2();
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
        if (std::strcmp (id, CLAP_EXT_TAIL) == 0) return &tail;
        if (std::strcmp (id, CLAP_EXT_GUI) == 0) return &gui;
        if (std::strcmp (id, CLAP_EXT_WEBVIEW) == 0) return &webview;
        return nullptr;
    };
    p->plugin.on_main_thread = [] (const clap_plugin*) {};
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
