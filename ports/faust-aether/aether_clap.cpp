/*
    Aether (https://github.com/sletz/faust-aether, MIT) as module.wasm: a CLAP instrument. Plain CLAP, no JUCE:
    the pinned Faust compiler (ports/faust-aether/gen.py) turns upstream's aether.dsp into the C++ class
    AetherVoice and effects.dsp into AetherFX, both single precision like the published web build. This wrapper
    is the polyphonic host the DSP expects: it allocates voices, routes notes (freq/gain/gate per voice), sums
    them and runs the shared stereo effect bus once per block — the same split upstream's web, JUCE and test
    hosts use. No upstream file is modified; gen.py only runs the compiler.

    Voices (8, like the web version's middle setting): a note reuses its key's voice, else a free one, else the
    quietest released voice, else the oldest; a stolen voice fades over one block (pending) before the new note
    starts, and a note-off never overtakes its note-on. A released voice sleeps after 200 ms below 1e-5; the
    plugin sleeps (CLAP_PROCESS_SLEEP) after half a second of silence, which the effect tails reset. Panic
    (instanceClear) stops everything.

    Parameters: the 96 Faust widgets (ids 0..95, gen.py's table order: the voice's groups, then the effect
    bus's), as their native values — min/max/default/step, log scale, unit and menu names come from the .dsp's
    metadata. Menus and checkboxes are stepped. The state is a fixed 396-byte blob: 'AETH', version, count,
    96 floats. Presets are the host's (or the page's) parameter sets; upstream's six ship as the bundle's
    ui/patches.json.

    Editor: ui/index.html through clap.gui (the webview API) and clap.webview, adapted from upstream's web/
    page. Its messages are ArrayBuffers over window.parent.postMessage, binary little endian, the types below:

      page -> plugin:   1 hello, 2 poll, 3 begin (u32 id@4), 4 set (u32 id@4, f64 value@8), 5 end (u32 id@4),
                        6 hover (u32 id@4, 0xffffffff none), 8 patch (u16 name length@2, name@4, 96 x f64 values),
                        9 defaults, 10 panic
      plugin -> page:   0x81 describe (the parameter table JSON from 4), 0x82 snapshot (96 x f32 from 4),
                        0x83 values (u16 count@2, then count x (u32 id, u32 0, f64 value)),
                        0x84 activity (u8 active voices@1, f32 output peak@4)

    The page says hello, gets the table and a snapshot, then polls at most every 33 ms. A drag is one gesture
    (begin, values, end); the hovered parameter is reported so the host's menu and value entry work.
*/

#include <clap/clap.h>
#include <clap/ext/draft/param-hovered.h>
#include <clap/ext/draft/webview.h>
#include <clap/ext/tail.h>

#include "faust/dsp/dsp.h"
#include "faust/gui/meta.h"
#include "faust/gui/UI.h"

#include "voice_gen.cpp"
#include "fx_gen.cpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "params.inc"

namespace
{
constexpr const char* pluginId = "org.faust.aether";

// 8 voices: the middle of the web version's 4..12. The DSP is demanding (the bus alone keeps computing), so the
// plugin sleeps in silence instead of idling.
constexpr int kVoices = 8;

// The editor's design size; the page scales to its window, hosts resize at this aspect ratio
constexpr uint32_t editorWidth = 1024, editorHeight = 640;
constexpr double minEditorScale = 0.5, maxEditorScale = 2.0;

// The saved state, and the page's snapshot after its 4-byte header: fixed layout
struct State
{
    char magic[4];   // "AETH"
    uint32_t version;
    uint32_t count;
    float values[paramCount];
};
static_assert (sizeof (State) == 12 + paramCount * 4);

constexpr uint32_t stateVersion = 1;

enum Page : uint8_t
{
    pageHello = 1,
    pagePoll = 2,
    pageBegin = 3,
    pageSet = 4,
    pageEnd = 5,
    pageHover = 6,
    pagePatch = 8,  // u16 name length at 2, name bytes at 4, then paramCount x f64 values
    pageDefaults = 9,
    pagePanic = 10
};

enum Plugin : uint8_t
{
    pluginDescribe = 0x81, // the parameter table JSON from 4 on
    pluginSnapshot = 0x82, // State from 4 on
    pluginValues = 0x83,   // u16 count at 2, then count x (u32 id, u32 0, f64 value)
    pluginActivity = 0x84  // u8 active voices at 1, f32 peak at 4
};

// Collects a generated DSP's widget zones with their full Faust addresses, mirroring the JSON gen.py reads
struct Collector : UI
{
    std::vector<std::string> stack;
    std::vector<std::pair<std::string, FAUSTFLOAT*>> out;

    void add (const char* label, FAUSTFLOAT* zone)
    {
        std::string address;
        for (const auto& box : stack)
            address += "/" + box;
        address += "/";
        address += label;
        for (auto& c : address)
            if (c == ' ')
                c = '_';
        out.push_back ({ address, zone });
    }

    void openHorizontalBox (const char* label) override { stack.push_back (label); }
    void openVerticalBox (const char* label) override { stack.push_back (label); }
    void openTabBox (const char* label) override { stack.push_back (label); }
    void closeBox() override { stack.pop_back(); }
    void addHorizontalSlider (const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT,
                              FAUSTFLOAT) override { add (label, zone); }
    void addVerticalSlider (const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT,
                            FAUSTFLOAT) override { add (label, zone); }
    void addButton (const char* label, FAUSTFLOAT* zone) override { add (label, zone); }
    void addCheckButton (const char* label, FAUSTFLOAT* zone) override { add (label, zone); }
    void addNumEntry (const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override
    { add (label, zone); }
    void addHorizontalBargraph (const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
    void addVerticalBargraph (const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
    void addSoundfile (const char*, const char*, Soundfile**) override {}
};

struct Voice
{
    AetherVoice* dsp = nullptr;
    FAUSTFLOAT* zones[paramCount] {}; // this voice's zone per parameter (non-fx params)
    FAUSTFLOAT *freq = nullptr, *gain = nullptr, *gate = nullptr;
    bool active = false, held = false, justStarted = false, releaseAfterBlock = false;
    int key = -1;
    uint32_t age = 0, quiet = 0;
    float level = 0.0f;
    struct Pending { int key; float velocity; bool release; } pending { 0, 0.0f, false };
    bool hasPending = false;
};

struct Aether
{
    clap_plugin plugin {};
    const clap_host* host = nullptr;
    const clap_host_params* hostParams = nullptr;
    const clap_host_webview* hostWebview = nullptr;
    const clap_host_param_hovered* hostHovered = nullptr;

    double sampleRate = 0;
    size_t maxFrames = 0;
    bool active = false, guiCreated = false;
    uint32_t guiWidth = editorWidth, guiHeight = editorHeight;

    Voice voices[kVoices];
    AetherFX* fx = nullptr;
    FAUSTFLOAT* fxZones[paramCount] {}; // fx params and bpm on the bus instance
    uint32_t voiceCounter = 0;

    double values[paramCount];
    bool pageValues[paramCount] {}; // the host changed it: send on the next poll
    bool pageSnapshot = false;

    // render buffers, sized in activate
    std::vector<float> mixL, mixR, outL, outR;
    std::vector<float> voiceOut; // kVoices x 2 x maxFrames

    bool sleeping = false;
    size_t quietSamples = 0;
    float meterPeak = 0.0f;
    uint8_t lastVoices = 0, lastVoicesSent = 0xff;
    float lastPeakSent = -1.0f;

    struct OutEvent { uint16_t type; clap_id id; double value; };
    OutEvent outEvents[256];
    uint32_t outRead = 0, outWrite = 0;
    std::vector<uint8_t> message;

    // --- setup ----------------------------------------------------------------------------------------------

    bool init()
    {
        hostParams = static_cast<const clap_host_params*> (host->get_extension (host, CLAP_EXT_PARAMS));
        hostWebview = static_cast<const clap_host_webview*> (host->get_extension (host, CLAP_EXT_WEBVIEW));
        hostHovered = static_cast<const clap_host_param_hovered*> (host->get_extension (host, CLAP_EXT_PARAM_HOVERED));
        for (clap_id id = 0; id < paramCount; ++id)
            values[id] = paramSpecs[id].defaultValue;
        message.reserve (4 + sizeof (State));
        return true;
    }

    bool activate (double rate, uint32_t, uint32_t maxFrames_)
    {
        sampleRate = rate;
        maxFrames = maxFrames_;
        mixL.assign (maxFrames, 0.0f);
        mixR.assign (maxFrames, 0.0f);
        outL.assign (maxFrames, 0.0f);
        outR.assign (maxFrames, 0.0f);
        voiceOut.assign ((size_t) kVoices * 2 * maxFrames, 0.0f);
        fx = new (std::nothrow) AetherFX();
        if (fx == nullptr)
            return false;
        fx->init ((int) std::lround (rate));
        bindZones (*fx, fxZones, true);
        for (int v = 0; v < kVoices; ++v)
        {
            voices[v].dsp = new (std::nothrow) AetherVoice();
            if (voices[v].dsp == nullptr)
                return false;
            voices[v].dsp->init ((int) std::lround (rate));
            bindZones (*voices[v].dsp, voices[v].zones, false);
            Collector collector;
            voices[v].dsp->buildUserInterface (&collector);
            for (const auto& [address, zone] : collector.out)
            {
                if (address == "/Aether/freq") voices[v].freq = zone;
                else if (address == "/Aether/gain") voices[v].gain = zone;
                else if (address == "/Aether/gate") voices[v].gate = zone;
            }
        }
        applyAll();
        active = true;
        return true;
    }

    void deactivate()
    {
        active = false;
        for (int v = 0; v < kVoices; ++v)
        {
            delete voices[v].dsp;
            voices[v] = Voice {};
        }
        delete fx;
        fx = nullptr;
        std::fill (std::begin (fxZones), std::end (fxZones), nullptr);
    }

    // Maps a DSP instance's zones to the parameter table by address. Voice params bind on the voices, fx params
    // (and the bus's copy of bpm) on the effect instance; anything missing is a gen/collector mismatch
    template <typename DSP>
    void bindZones (DSP& dsp, FAUSTFLOAT** zones, bool isFx)
    {
        Collector collector;
        dsp.buildUserInterface (&collector);
        for (clap_id id = 0; id < paramCount; ++id)
        {
            const char* key = paramSpecs[id].key;
            if (isFx)
            {
                if (! paramSpecs[id].fx && id != bpmParamId)
                    continue;
                if (id == bpmParamId)
                    key = "/Aether_Effects/Voice/Sequencer/bpm";
            }
            else if (paramSpecs[id].fx)
                continue;
            for (const auto& [address, zone] : collector.out)
                if (address == key)
                {
                    zones[id] = zone;
                    break;
                }
        }
    }

    // --- parameters -----------------------------------------------------------------------------------------

    static double clamp (clap_id id, double value)
    {
        const ParamSpec& p = paramSpecs[id];
        return std::clamp (value, p.min, p.max);
    }

    void setParam (clap_id id, double value)
    {
        if (id >= paramCount)
            return;
        value = clamp (id, value);
        if (values[id] == value)
            return;
        values[id] = value;
        apply (id);
    }

    void apply (clap_id id)
    {
        const float v = (float) values[id];
        for (int i = 0; i < kVoices; ++i)
            if (voices[i].zones[id] != nullptr)
                *voices[i].zones[id] = v;
        if (fxZones[id] != nullptr)
            *fxZones[id] = v;
    }

    void applyAll()
    {
        for (clap_id id = 0; id < paramCount; ++id)
            apply (id);
    }

    // From the host (process/flush): also tell the page
    void hostValue (clap_id id, double value)
    {
        if (id >= paramCount)
            return;
        setParam (id, value);
        pageValues[id] = true;
    }

    // --- notes ----------------------------------------------------------------------------------------------

    void start (Voice& v, int key, double velocity, bool release)
    {
        *v.freq = (float) (440.0 * std::pow (2.0, (key - 69) / 12.0));
        *v.gain = (float) std::clamp (velocity, 0.01, 1.0);
        *v.gate = 1.0f;
        v.key = key;
        v.held = true;
        v.active = true;
        v.age = voiceCounter++;
        v.quiet = 0;
        v.hasPending = false;
        v.justStarted = true;
        v.releaseAfterBlock = release;
    }

    void noteOn (int key, double velocity)
    {
        if (key < 0 || key > 127)
            return;
        if (velocity <= 0.0)
        {
            noteOff (key);
            return;
        }
        Voice* v = nullptr;
        for (int i = 0; i < kVoices; ++i)
            if (voices[i].active && voices[i].key == key)
            {
                v = &voices[i];
                break;
            }
        if (v == nullptr)
            for (int i = 0; i < kVoices; ++i)
                if (! voices[i].active && ! voices[i].hasPending)
                {
                    v = &voices[i];
                    break;
                }
        if (v == nullptr)
        {
            Voice* best = nullptr;
            bool bestReleased = false;
            for (int i = 0; i < kVoices; ++i)
            {
                if (voices[i].hasPending)
                    continue;
                const bool released = ! voices[i].held;
                if (best == nullptr || (released && ! bestReleased)
                    || (released == bestReleased && (released ? voices[i].level < best->level : voices[i].age < best->age)))
                {
                    best = &voices[i];
                    bestReleased = released;
                }
            }
            v = best;
        }
        if (v == nullptr)
            return;
        if (v->active)
            v->pending = { key, (float) velocity, false }, v->hasPending = true;
        else
            start (*v, key, velocity, false);
    }

    void noteOff (int key)
    {
        for (int i = 0; i < kVoices; ++i)
        {
            Voice& v = voices[i];
            if (v.hasPending && v.pending.key == key)
                v.pending.release = true;
            else if (v.active && v.key == key && v.held)
            {
                if (v.justStarted)
                    v.releaseAfterBlock = true;
                else
                {
                    v.held = false;
                    *v.gate = 0.0f;
                }
            }
        }
    }

    void panic()
    {
        for (int i = 0; i < kVoices; ++i)
        {
            Voice& v = voices[i];
            if (v.dsp != nullptr)
            {
                v.dsp->instanceClear();
                if (v.gate != nullptr)
                    *v.gate = 0.0f;
            }
            v.active = v.held = v.justStarted = v.releaseAfterBlock = v.hasPending = false;
            v.key = -1;
            v.age = v.quiet = 0;
            v.level = 0.0f;
        }
        if (fx != nullptr)
            fx->instanceClear();
        quietSamples = 0;
    }

    // --- render ---------------------------------------------------------------------------------------------

    // Renders frames [from, to) of this call into left/right; tracks the meter and the sleep
    void render (float* left, float* right, size_t from, size_t to)
    {
        if (to <= from)
            return;
        const int count = (int) (to - from);
        if (sleeping || ! active)
        {
            std::fill (left + from, left + to, 0.0f);
            std::fill (right + from, right + to, 0.0f);
            return;
        }
        std::fill (mixL.data(), mixL.data() + count, 0.0f);
        std::fill (mixR.data(), mixR.data() + count, 0.0f);
        float* noInputs[1] = { nullptr };
        uint8_t activeVoices = 0;
        for (int i = 0; i < kVoices; ++i)
        {
            Voice& v = voices[i];
            if (! v.active)
                continue;
            activeVoices++;
            float* out[2] = { voiceOut.data() + ((size_t) i * 2) * maxFrames + from,
                              voiceOut.data() + ((size_t) i * 2 + 1) * maxFrames + from };
            v.dsp->compute (count, noInputs, out);
            float level = 0.0f;
            for (int s = 0; s < count; ++s)
            {
                const float fade = v.hasPending ? 1.0f - (float) s / (float) std::max (1, count - 1) : 1.0f;
                mixL[s] += out[0][s] * fade;
                mixR[s] += out[1][s] * fade;
                level = std::max (level, std::max (std::fabs (out[0][s]), std::fabs (out[1][s])));
            }
            v.level = level;
            if (v.hasPending)
            {
                const Voice::Pending next = v.pending;
                v.dsp->instanceClear();
                start (v, next.key, next.velocity, next.release);
                continue;
            }
            v.justStarted = false;
            if (v.releaseAfterBlock)
            {
                v.releaseAfterBlock = false;
                v.held = false;
                *v.gate = 0.0f;
            }
            if (! v.held)
            {
                v.quiet = level < 1.0e-5f ? v.quiet + (uint32_t) count : 0;
                if (v.quiet > (uint32_t) (sampleRate * 0.2))
                    v.active = false;
            }
        }
        float* fxIn[2] = { mixL.data(), mixR.data() };
        float* fxOut[2] = { outL.data(), outR.data() };
        fx->compute (count, fxIn, fxOut);
        double peak = 0.0;
        for (int s = 0; s < count; ++s)
        {
            const float l = std::isfinite (outL[s]) ? outL[s] : 0.0f;
            const float r = std::isfinite (outR[s]) ? outR[s] : 0.0f;
            left[from + s] = l;
            right[from + s] = r;
            peak = std::max (peak, (double) std::max (std::fabs (l), std::fabs (r)));
        }
        meterPeak = std::max (meterPeak, (float) peak);
        if (activeVoices == 0 && peak < 1.0e-6)
            sleeping = (quietSamples += (size_t) count) > (size_t) (sampleRate / 2);
        else
            quietSamples = 0;
        lastVoices = activeVoices;
    }

    void wake() { sleeping = false; quietSamples = 0; }

    // --- events ---------------------------------------------------------------------------------------------

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
            noteOn (data1, data2 / 127.0);
        }
        else if (status == 0x80 || (status == 0x90 && data2 == 0))
        {
            eventTime (start, time, left, right);
            noteOff (data1);
        }
        else if (status == 0xb0 && (data1 == 120 || data1 == 123))
        {
            eventTime (start, time, left, right);
            panic();
        }
    }

    // --- process --------------------------------------------------------------------------------------------

    clap_process_status process (const clap_process& process)
    {
        const size_t frames = process.frames_count;
        float* left = nullptr;
        float* right = nullptr;
        if (process.audio_outputs_count >= 1 && process.audio_outputs[0].channel_count >= 2)
        {
            left = process.audio_outputs[0].data32[0];
            right = process.audio_outputs[0].data32[1];
        }
        if (left == nullptr)
            return CLAP_PROCESS_CONTINUE;

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
                    eventTime (start, time, left, right);
                    noteOn (e->key, e->velocity);
                    break;
                }
                case CLAP_EVENT_NOTE_OFF:
                {
                    const auto* e = reinterpret_cast<const clap_event_note*> (header);
                    eventTime (start, time, left, right);
                    noteOff (e->key);
                    break;
                }
                case CLAP_EVENT_NOTE_CHOKE:
                {
                    const auto* e = reinterpret_cast<const clap_event_note*> (header);
                    eventTime (start, time, left, right);
                    if (e->key >= 0 && e->key <= 127)
                    {
                        for (int v = 0; v < kVoices; ++v)
                            if (voices[v].active && voices[v].key == e->key)
                            {
                                voices[v].dsp->instanceClear();
                                *voices[v].gate = 0.0f;
                                voices[v].active = false;
                                voices[v].held = false;
                            }
                    }
                    else
                    {
                        panic();
                    }
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
        render (left, right, start, frames);

        if (process.out_events != nullptr)
            writeEvents (*process.out_events);
        return sleeping ? CLAP_PROCESS_SLEEP : CLAP_PROCESS_CONTINUE;
    }

    // --- page -----------------------------------------------------------------------------------------------

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

    void requestFlush()
    {
        if (hostParams != nullptr && hostParams->request_flush != nullptr)
            hostParams->request_flush (host);
    }

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
                std::fill (std::begin (pageValues), std::end (pageValues), true);
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
                setParam (id, f64 (8));
                pushOut (CLAP_EVENT_PARAM_VALUE, id, values[id]);
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
                if (size < 4 + nameLength + paramCount * 8)
                    return false;
                const uint8_t* at = data + 4 + nameLength;
                for (clap_id id = 0; id < paramCount; ++id)
                {
                    setParam (id, f64 ((size_t) (at - data) + 8 * id));
                    pushOut (CLAP_EVENT_PARAM_VALUE, id, values[id]);
                    pageValues[id] = true;
                }
                requestFlush();
                return true;
            }
            case pageDefaults:
                for (clap_id id = 0; id < paramCount; ++id)
                {
                    setParam (id, paramSpecs[id].defaultValue);
                    pushOut (CLAP_EVENT_PARAM_VALUE, id, values[id]);
                    pageValues[id] = true;
                }
                requestFlush();
                return true;
            case pagePanic:
                panic();
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
        const char* json = paramTableJson;
        message.insert (message.end(), json, json + std::strlen (json));
        send (message.data(), message.size());
    }

    void sendPageUpdates()
    {
        if (pageSnapshot)
        {
            pageSnapshot = false;
            State state {};
            fillState (state);
            message.assign (4, 0);
            message[0] = pluginSnapshot;
            const auto* bytes = reinterpret_cast<const uint8_t*> (&state);
            message.insert (message.end(), bytes + 12, bytes + sizeof (State)); // the values, after magic+version+count
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
            const double value = values[id];
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
        uint8_t activity[8] {};
        activity[0] = pluginActivity;
        activity[1] = lastVoices;
        const float peak = meterPeak;
        meterPeak = 0.0f;
        std::memcpy (activity + 4, &peak, 4);
        if (activity[1] == lastVoicesSent && std::memcmp (activity + 4, &lastPeakSent, 4) == 0)
            return;
        lastVoicesSent = activity[1];
        lastPeakSent = peak;
        send (activity, sizeof activity);
    }

    // --- state ----------------------------------------------------------------------------------------------

    void fillState (State& state) const
    {
        state.magic[0] = 'A'; state.magic[1] = 'E'; state.magic[2] = 'T'; state.magic[3] = 'H';
        state.version = stateVersion;
        state.count = paramCount;
        for (clap_id id = 0; id < paramCount; ++id)
            state.values[id] = (float) values[id];
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
        if (read != sizeof (State) || std::memcmp (bytes, "AETH", 4) != 0)
            return false;
        State state;
        std::memcpy (&state, bytes, sizeof state);
        if (state.version != stateVersion || state.count != paramCount)
            return false;
        for (clap_id id = 0; id < paramCount; ++id)
            setParam (id, state.values[id]);
        pageSnapshot = true;
        return true;
    }
};

Aether& self (const clap_plugin* plugin) { return *static_cast<Aether*> (plugin->plugin_data); }

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
    const ParamSpec& p = paramSpecs[index];
    std::memset (info, 0, sizeof *info);
    info->id = (clap_id) index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (p.menu != nullptr || p.step >= 1.0 || std::strcmp (p.type, "checkbox") == 0)
    {
        info->flags |= CLAP_PARAM_IS_STEPPED;
        if (p.menu != nullptr)
            info->flags |= CLAP_PARAM_IS_ENUM;
    }
    std::snprintf (info->name, sizeof info->name, "%s", p.name);
    std::snprintf (info->module, sizeof info->module, "%s", p.group);
    info->min_value = p.min;
    info->max_value = p.max;
    info->default_value = p.defaultValue;
    return true;
}

bool paramsValue (const clap_plugin* plugin, clap_id id, double* value)
{
    if (id < 0 || id >= (clap_id) paramCount)
        return false;
    *value = self (plugin).values[id];
    return true;
}

bool paramsValueToText (const clap_plugin* plugin, clap_id id, double value, char* text, uint32_t size)
{
    if (id < 0 || id >= (clap_id) paramCount || size == 0)
        return false;
    const ParamSpec& p = paramSpecs[id];
    if (std::strcmp (p.type, "checkbox") == 0)
    {
        std::snprintf (text, size, "%s", value >= 0.5 ? "On" : "Off");
        return true;
    }
    if (p.menu != nullptr)
    {
        const MenuEntry* best = &p.menu[0];
        for (uint32_t i = 1; i < p.menuCount; ++i)
            if (std::fabs (p.menu[i].value - value) < std::fabs (best->value - value))
                best = &p.menu[i];
        std::snprintf (text, size, "%s", best->name);
        return true;
    }
    if (std::strcmp (p.name, "filter_type") == 0)
    {
        std::snprintf (text, size, "%s", value < 0.5 ? "Ladder" : value < 1.5 ? "Band-pass" : "High-pass");
        return true;
    }
    return false;
}

bool paramsTextToValue (const clap_plugin*, clap_id id, const char* text, double* value)
{
    if (id < 0 || id >= (clap_id) paramCount || text == nullptr)
        return false;
    const ParamSpec& p = paramSpecs[id];
    char* end = nullptr;
    const double number = std::strtod (text, &end);
    if (end != text)
    {
        *value = std::clamp (number, p.min, p.max);
        return true;
    }
    if (p.menu != nullptr)
        for (uint32_t i = 0; i < p.menuCount; ++i)
            if (std::strcmp (p.menu[i].name, text) == 0)
            {
                *value = p.menu[i].value;
                return true;
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

uint32_t tailGet (const clap_plugin*) { return UINT32_MAX; } // the reverb and delay tails ring for seconds

const clap_plugin_tail tail { tailGet };

// --- editor -------------------------------------------------------------------------------------------------

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
    "Aether",
    "sletz (Faust)",
    "https://github.com/sletz/faust-aether",
    "",
    "",
    AETHER_VERSION,
    "Hybrid synthesizer in Faust: subtractive, 4-operator FM, additive, a modulation matrix, a 16-step sequencer "
    "and a stereo effects bus",
    features
};

const clap_plugin* createPlugin (const clap_host* host)
{
    auto* p = new (std::nothrow) Aether();
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
    p->plugin.reset = [] (const clap_plugin* plugin) { self (plugin).panic(); };
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
