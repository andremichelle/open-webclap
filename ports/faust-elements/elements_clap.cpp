/*
    Elements (https://github.com/sletz/faust-elements, MIT; the Faust model of Mutable Instruments' Elements
    eurorack module) as module.wasm: a CLAP instrument. Plain CLAP, no JUCE: the pinned faust-rs compiler
    (ports/faust-elements/gen.py, grame-cncm/faust-rs — elements.lib needs its `ondemand` domains) turns this
    port's src/voice.dsp and src/body.dsp (the voice and the effect bus of upstream's dsp/elements_poly.dsp,
    verbatim) into the self-contained C++ classes ElementsVoice and ElementsBody, single precision, and this
    wrapper is the polyphonic host around them. No upstream file is modified; gen.py only runs the compiler.

    The model runs at 32 kHz — every table and delay of the module's firmware is computed for its codec rate —
    so the DSPs are always init'ed at 32000 and this wrapper resamples the host's output stream from 32 kHz to the
    host rate with a windowed-sinc resampler (the browser does the same for the web version's 32 kHz context).
    Internally the audio advances in chunks of 16 frames, the model's block: gate and control changes take
    effect at a block boundary, as on the module and in the web AudioWorklet.

    Voices (8, the web version's default): the allocation, restrike, release, culling, stealing, divergence
    guards and body sleep are a direct port of upstream's web/elements-processor.js:
    - a note reuses its key's voice, else a free one, else the oldest released one, else the oldest held one;
    - every note starts with one 16-frame block of low gate, so the voice sees a rising edge (a key struck
      again restrikes its own voice on the strings still sounding);
    - a release that arrives before the voice has computed a block with its gate high waits for that block, so
      a short click still strikes;
    - a released voice stops being computed once its outputs have stayed below 1e-5 for a second (three while
      its key is held); the body stops once its output has stayed below 1e-5 for a second with no voice active
      (a frozen reverb never goes quiet, so it never sleeps);
    - a voice whose output is not finite or passes 1e3, or a body whose output is not finite or passes 1e6, has
      its state cleared (instanceClear) — Elements does diverge (a lossless string under a strong bow), and the
      module resets its voice then;
    - the plugin sleeps (CLAP_PROCESS_SLEEP) after half a second of silence.
    The voices' three outputs (raw, center, side) are summed into the body's inputs, the body runs once per
    chunk; its outputs are main and aux, sent as right and left (elements.cc).

    Parameters: the 21 panel controls (ids 0..20, gen.py's table: the voice's exciter/resonator/unit/performance
    groups, then the body's space/diffusion/reverb_lp), as their native values — min/max/default/step, unit and
    menu come from the .dsp's metadata; the resonator model is a stepped enum (Modal/String/Strings). The
    pitch offset (modulation) additionally carries the pitch bend / tuning note expression. The state is a fixed
    96-byte blob: 'ELEM', version, count, 21 floats. Presets are the host's (or the page's) parameter sets;
    upstream's 24 web presets ship as the bundle's ui/patches.json.

    Editor: ui/index.html through clap.gui (the webview API) and clap.webview, adapted from upstream's web/
    page (controls, presets, an atlas tab with upstream's web/atlas.json, a playable keyboard). Its messages are
    ArrayBuffers over window.parent.postMessage, binary little endian, the types below:

      page -> plugin:   1 hello, 2 poll, 3 begin (u32 id@4), 4 set (u32 id@4, f64 value@8), 5 end (u32 id@4),
                        6 hover (u32 id@4, 0xffffffff none), 8 patch (u16 name length@2, name@4, 21 x f64 values),
                        9 defaults, 10 panic, 11 note on (u32 key@4, f64 velocity@8), 12 note off (u32 key@4)
      plugin -> page:   0x81 describe (the parameter table JSON from 4), 0x82 snapshot (21 x f32 from 4),
                        0x83 values (u16 count@2, then count x (u32 id, u32 0, f64 value)),
                        0x84 activity (u8 active voices@1, f32 output peak@4)

    The page says hello, gets the table and a snapshot, then polls at most every 33 ms. A drag is one gesture
    (begin, values, end); the hovered parameter is reported so the host's menu and value entry work.
*/

#include <clap/clap.h>
#include <clap/ext/draft/param-hovered.h>
#include <clap/ext/draft/webview.h>
#include <clap/ext/tail.h>

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

// --- the faust-rs generated DSPs ---------------------------------------------------------------------------
// The generated files are self-contained except for the architecture types (FAUSTFLOAT, Meta, UI, dsp), which
// the includer provides, and the faust_wrap_* helpers, which are not include-guarded: rename them per include so
// both classes compile in this one translation unit (their tables and SIG classes are already class-prefixed).
#ifndef FAUSTFLOAT
#define FAUSTFLOAT float
#endif

struct Meta
{
    virtual ~Meta() {}
    virtual void declare (const char*, const char*) {}
};

struct UI
{
    virtual ~UI() {}
    virtual void openTabBox (const char*) {}
    virtual void openHorizontalBox (const char*) {}
    virtual void openVerticalBox (const char*) {}
    virtual void closeBox() {}
    virtual void addButton (const char*, FAUSTFLOAT*) {}
    virtual void addCheckButton (const char*, FAUSTFLOAT*) {}
    virtual void addVerticalSlider (const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) {}
    virtual void addHorizontalSlider (const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) {}
    virtual void addNumEntry (const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) {}
    virtual void addHorizontalBargraph (const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) {}
    virtual void addVerticalBargraph (const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) {}
    virtual void addSoundfile (const char*, const char*, void**) {}
    virtual void declare (FAUSTFLOAT*, const char*, const char*) {}
};

struct dsp
{
    virtual ~dsp() {}
};

#define faust_wrap_add faust_wrap_add_voice
#define faust_wrap_sub faust_wrap_sub_voice
#define faust_wrap_mul faust_wrap_mul_voice
#include "voice_gen.cpp"
#undef faust_wrap_add
#undef faust_wrap_sub
#undef faust_wrap_mul
#define faust_wrap_add faust_wrap_add_body
#define faust_wrap_sub faust_wrap_sub_body
#define faust_wrap_mul faust_wrap_mul_body
#include "body_gen.cpp"
#undef faust_wrap_add
#undef faust_wrap_sub
#undef faust_wrap_mul

#include "params.inc"

namespace
{
constexpr const char* pluginId = "org.faust.elements";

// The model's rate: every table, delay length and coefficient of the module's firmware is computed for 32 kHz.
constexpr int kInternalRate = 32000;
// The model's block: the voices compute in multiples of 16 frames.
constexpr int kBlock = 16;

// 8 voices: the web version's default (its range is 1 to 16).
constexpr int kVoices = 8;

// The editor's design size; the page scales to its window, hosts resize at this aspect ratio
constexpr uint32_t editorWidth = 1024, editorHeight = 640;
constexpr double minEditorScale = 0.5, maxEditorScale = 2.0;

// The silence and divergence thresholds of web/elements-processor.js, at the internal rate.
constexpr float kQuiet = 1.0e-5f;      // -100 dBFS: a voice or the body counts as silent below this
constexpr uint32_t kQuietFrames = kInternalRate;        // a released voice is culled after 1 s below kQuiet
constexpr uint32_t kHeldQuietFrames = 3 * kInternalRate; // a held one after 3 s
constexpr float kVoiceGuard = 1.0e3f;  // a voice beyond this (or NaN) is cleared
constexpr float kBodyGuard = 1.0e6f;   // the body likewise

// The saved state, and the page's snapshot after its 4-byte header: fixed layout
struct State
{
    char magic[4];   // "ELEM"
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
    pagePatch = 8,   // u16 name length at 2, name bytes at 4, then paramCount x f64 values
    pageDefaults = 9,
    pagePanic = 10,
    pageNoteOn = 11, // u32 key at 4, f64 velocity at 8
    pageNoteOff = 12 // u32 key at 4
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
    void addSoundfile (const char*, const char*, void**) override {}
};

struct Voice
{
    ElementsVoice* dsp = nullptr;
    FAUSTFLOAT* zones[paramCount] {}; // this voice's zone per voice parameter
    FAUSTFLOAT *freq = nullptr, *gain = nullptr, *gate = nullptr;
    bool active = false, held = false, restrike = false, fresh = false, releaseNext = false;
    int key = -1;
    uint32_t started = 0, quiet = 0;
};

// The windowed-sinc resampler, 32 kHz -> host rate (a passthrough when the rates are equal). For an internal
// position p it sums the history around floor(p) with a Kaiser-windowed sinc whose cutoff is just under the lower
// Nyquist frequency.
struct Resampler
{
    static constexpr int kTaps = 16; // half the kernel length, in internal samples
    static constexpr double kBeta = 8.0;

    double cutoff = 0.47; // cycles per internal sample, set from the rates
    std::vector<float> histL, histR;
    size_t base = 0; // absolute internal index of hist[0]; produced == base + hist.size()

    static double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k)
        {
            const double t = x / (2.0 * k);
            term *= t * t;
            sum += term;
            if (term < 1e-12 * sum)
                break;
        }
        return sum;
    }

    void configure (double hostRate)
    {
        // Cutoff in cycles per internal sample: the internal Nyquist when upsampling, the host's Nyquist when
        // downsampling, with a transition band.
        cutoff = 0.5 * std::min (1.0, hostRate / (double) kInternalRate) * 0.94;
    }

    // The kernel h(d): d is the distance in internal samples from the tap to the read position.
    double kernel (double d) const
    {
        const double x = 2.0 * cutoff * d;
        const double sinc = x == 0.0 ? 1.0 : std::sin (M_PI * x) / (M_PI * x);
        const double w = d / kTaps;
        const double window = w >= 1.0 || w <= -1.0 ? 0.0 : besselI0 (kBeta * std::sqrt (1.0 - w * w)) / besselI0 (kBeta);
        return 2.0 * cutoff * sinc * window;
    }

    float sample (const std::vector<float>& hist, double p) const
    {
        const long b = (long) std::floor (p);
        const double f = p - b;
        double sum = 0.0;
        for (int k = -kTaps + 1; k <= kTaps; ++k)
        {
            const long idx = b + k;
            if (idx < (long) base || idx >= (long) (base + hist.size()))
                continue; // before the start of the stream: zero
            sum += (double) hist[(size_t) (idx - (long) base)] * kernel (k - f);
        }
        return (float) sum;
    }
};

struct Elements
{
    clap_plugin plugin {};
    const clap_host* host = nullptr;
    const clap_host_params* hostParams = nullptr;
    const clap_host_webview* hostWebview = nullptr;
    const clap_host_param_hovered* hostHovered = nullptr;

    double sampleRate = 0;
    bool active = false, guiCreated = false;
    uint32_t guiWidth = editorWidth, guiHeight = editorHeight;

    Voice voices[kVoices];
    ElementsBody* body = nullptr;
    FAUSTFLOAT* bodyZones[paramCount] {};
    uint32_t voiceCounter = 0;

    double values[paramCount];
    bool pageValues[paramCount] {}; // the host changed it: send on the next poll
    bool pageSnapshot = false;
    double bend = 0.0; // pitch bend / tuning, in semitones, added to the modulation parameter

    // The internal 32 kHz stream, produced chunk by chunk and consumed by the resampler. `produced` is the
    // absolute internal sample count; the resampler's history window covers [base, produced).
    Resampler rs;
    uint64_t produced = 0;
    uint64_t hostSamples = 0; // absolute host samples rendered, for the event positions
    double blockPeak = 0.0;

    bool sleeping = false;
    uint64_t quietSamples = 0;
    float meterPeak = 0.0f;
    uint8_t lastVoices = 0, lastVoicesSent = 0xff;
    float lastPeakSent = -1.0f;
    bool bodyAwake = false;
    uint32_t bodyQuiet = 0;

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

    bool activate (double rate, uint32_t, uint32_t)
    {
        sampleRate = rate;
        rs.configure (rate);
        rs.histL.clear();
        rs.histR.clear();
        rs.base = 0;
        produced = 0;
        hostSamples = 0;
        body = new (std::nothrow) ElementsBody();
        if (body == nullptr)
            return false;
        body->init (kInternalRate);
        bindZones (*body, bodyZones, true);
        for (int v = 0; v < kVoices; ++v)
        {
            voices[v].dsp = new (std::nothrow) ElementsVoice();
            if (voices[v].dsp == nullptr)
                return false;
            voices[v].dsp->init (kInternalRate);
            bindZones (*voices[v].dsp, voices[v].zones, false);
            Collector collector;
            voices[v].dsp->buildUserInterface (&collector);
            for (const auto& [address, zone] : collector.out)
            {
                if (address == "/elements_poly/freq") voices[v].freq = zone;
                else if (address == "/elements_poly/gain") voices[v].gain = zone;
                else if (address == "/elements_poly/gate") voices[v].gate = zone;
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
        delete body;
        body = nullptr;
        std::fill (std::begin (bodyZones), std::end (bodyZones), nullptr);
    }

    // Maps a DSP instance's zones to the parameter table by address: the voice params on the voices, the body
    // params on the body instance; anything missing is a gen/collector mismatch
    template <typename DSP>
    void bindZones (DSP& dsp, FAUSTFLOAT** zones, bool isBody)
    {
        Collector collector;
        dsp.buildUserInterface (&collector);
        for (clap_id id = 0; id < paramCount; ++id)
        {
            if (paramSpecs[id].body != isBody)
                continue;
            for (const auto& [address, zone] : collector.out)
                if (address == paramSpecs[id].key)
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
        // The pitch offset control carries the bend on top of its parameter value.
        const float v = (float) (values[id] + (id == modulationParamId ? bend : 0.0));
        for (int i = 0; i < kVoices; ++i)
            if (voices[i].zones[id] != nullptr)
                *voices[i].zones[id] = v;
        if (bodyZones[id] != nullptr)
            *bodyZones[id] = v;
    }

    void applyAll()
    {
        for (clap_id id = 0; id < paramCount; ++id)
            apply (id);
    }

    void setBend (double semitones)
    {
        if (bend == semitones)
            return;
        bend = semitones;
        apply (modulationParamId);
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
    // The allocation of web/elements-processor.js
    Voice* oldest (Voice** list, int count)
    {
        Voice* best = nullptr;
        for (int i = 0; i < count; ++i)
            if (list[i] != nullptr && (best == nullptr || list[i]->started < best->started))
                best = list[i];
        return best;
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
                if (! voices[i].active)
                {
                    v = &voices[i];
                    break;
                }
        if (v == nullptr)
        {
            Voice* released[kVoices];
            int nReleased = 0;
            for (int i = 0; i < kVoices; ++i)
                if (! voices[i].held)
                    released[nReleased++] = &voices[i];
            v = oldest (released, nReleased);
            if (v == nullptr)
            {
                Voice* all[kVoices];
                for (int i = 0; i < kVoices; ++i)
                    all[i] = &voices[i];
                v = oldest (all, kVoices);
            }
        }
        if (v == nullptr)
            return;
        *v->freq = (float) (440.0 * std::pow (2.0, (key - 69) / 12.0));
        *v->gain = (float) std::clamp (velocity, 0.0, 1.0);
        v->restrike = true;
        v->key = key;
        v->held = true;
        v->active = true;
        v->fresh = true;
        v->releaseNext = false;
        v->quiet = 0;
        v->started = ++voiceCounter;
        bodyAwake = true;
        wake();
    }

    void noteOff (int key)
    {
        for (int i = 0; i < kVoices; ++i)
        {
            Voice& v = voices[i];
            if (v.active && v.key == key && v.held)
            {
                v.held = false;
                if (v.fresh)
                    v.releaseNext = true; // the gate falls after the next block with it high
                else
                {
                    *v.gate = 0.0f;
                    v.restrike = false;
                }
            }
        }
    }

    void choke (int key)
    {
        if (key < 0)
        {
            panic();
            return;
        }
        for (int i = 0; i < kVoices; ++i)
        {
            Voice& v = voices[i];
            if (v.active && v.key == key)
            {
                v.dsp->instanceClear();
                *v.gate = 0.0f;
                v.active = false;
                v.held = false;
                v.key = -1;
            }
        }
    }

    // The page's panic and all-notes-off: every gate low, every voice stopped, the body let to sleep (its state
    // is kept, as upstream's — a reverb tail is silenced by not computing the body)
    void panic()
    {
        for (int i = 0; i < kVoices; ++i)
        {
            Voice& v = voices[i];
            if (v.gate != nullptr)
                *v.gate = 0.0f;
            v.held = false;
            v.active = false;
            v.restrike = v.fresh = v.releaseNext = false;
            v.key = -1;
        }
        bodyAwake = false;
        quietSamples = 0;
    }

    void reset()
    {
        for (int i = 0; i < kVoices; ++i)
            if (voices[i].dsp != nullptr)
                voices[i].dsp->instanceClear();
        if (body != nullptr)
            body->instanceClear();
        panic();
    }

    // --- the internal render ----------------------------------------------------------------------------------

    // One 16-frame chunk of the internal stream: the active voices summed into the body's inputs, then the
    // body; appends the stereo chunk (aux left, main right) to the resampler's history.
    void computeChunk()
    {
        float bin[3][kBlock] = {};
        float vout[3][kBlock];
        float* vin[1] = { nullptr };
        float* vouts[3] = { vout[0], vout[1], vout[2] };
        uint8_t activeVoices = 0;
        for (int i = 0; i < kVoices; ++i)
        {
            Voice& v = voices[i];
            if (! v.active)
                continue;
            activeVoices++;
            if (v.restrike)
            {
                // One block with the gate low, then the gate high: the voice sees a rising edge.
                *v.gate = 0.0f;
                v.dsp->compute (kBlock, vin, vouts);
                for (int c = 0; c < 3; ++c)
                    for (int s = 0; s < kBlock; ++s)
                        bin[c][s] += vout[c][s];
                *v.gate = 1.0f;
                v.restrike = false;
                continue; // the high gate is computed from the next chunk; a release meanwhile waits (fresh)
            }
            v.dsp->compute (kBlock, vin, vouts);
            float level = 0.0f;
            bool diverged = false;
            for (int c = 0; c < 3; ++c)
                for (int s = 0; s < kBlock; ++s)
                {
                    const float a = std::fabs (vout[c][s]);
                    if (! (a <= kVoiceGuard))
                        diverged = true;
                    else
                        level = std::max (level, a);
                }
            if (diverged)
            {
                v.dsp->instanceClear();
                level = 0.0f;
            }
            else
                for (int c = 0; c < 3; ++c)
                    for (int s = 0; s < kBlock; ++s)
                        bin[c][s] += vout[c][s];
            v.fresh = false;
            if (v.releaseNext)
            {
                *v.gate = 0.0f;
                v.releaseNext = false;
            }
            if (level < kQuiet)
            {
                v.quiet += kBlock;
                if (v.quiet > (v.held ? kHeldQuietFrames : kQuietFrames))
                {
                    v.active = false;
                    if (! v.held)
                        v.key = -1;
                }
            }
            else
                v.quiet = 0;
        }
        float* binp[3] = { bin[0], bin[1], bin[2] };
        float bout[2][kBlock];
        float* boutp[2] = { bout[0], bout[1] };
        if (activeVoices > 0 || bodyAwake)
        {
            body->compute (kBlock, binp, boutp);
            float peak = 0.0f;
            bool diverged = false;
            for (int c = 0; c < 2; ++c)
                for (int s = 0; s < kBlock; ++s)
                {
                    const float a = std::fabs (bout[c][s]);
                    if (! (a <= kBodyGuard))
                        diverged = true;
                    else
                        peak = std::max (peak, a);
                }
            if (diverged)
            {
                body->instanceClear();
                std::memset (bout, 0, sizeof bout);
                peak = 0.0f;
            }
            for (int s = 0; s < kBlock; ++s)
            {
                rs.histL.push_back (bout[1][s]); // aux on the left, main on the right (elements.cc)
                rs.histR.push_back (bout[0][s]);
            }
            blockPeak = std::max (blockPeak, (double) peak);
            if (activeVoices == 0 && peak < kQuiet)
            {
                bodyQuiet += kBlock;
                if (bodyQuiet > (uint32_t) kInternalRate)
                    bodyAwake = false;
            }
            else
            {
                bodyQuiet = 0;
                bodyAwake = true;
            }
        }
        else
        {
            for (int s = 0; s < kBlock; ++s)
            {
                rs.histL.push_back (0.0f);
                rs.histR.push_back (0.0f);
            }
        }
        produced += kBlock;
        lastVoices = activeVoices;
        // Keep the history window small; the resampler reads at most kTaps back.
        constexpr size_t kHistory = 4 * Resampler::kTaps + 16;
        while (rs.histL.size() > kHistory)
        {
            rs.histL.erase (rs.histL.begin());
            rs.histR.erase (rs.histR.begin());
            rs.base++;
        }
    }

    void wake() { sleeping = false; quietSamples = 0; }

    // --- events ---------------------------------------------------------------------------------------------

    struct PendingEvent
    {
        double pos; // internal sample position
        const clap_event_header* header;
    };
    std::vector<PendingEvent> pending;
    size_t pendingIndex = 0;

    void applyEvent (const clap_event_header* header)
    {
        if (header->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return;
        switch (header->type)
        {
            case CLAP_EVENT_NOTE_ON:
                noteOn (reinterpret_cast<const clap_event_note*> (header)->key,
                        reinterpret_cast<const clap_event_note*> (header)->velocity);
                break;
            case CLAP_EVENT_NOTE_OFF:
                noteOff (reinterpret_cast<const clap_event_note*> (header)->key);
                break;
            case CLAP_EVENT_NOTE_CHOKE:
                choke (reinterpret_cast<const clap_event_note*> (header)->key);
                break;
            case CLAP_EVENT_NOTE_EXPRESSION:
            {
                const auto* e = reinterpret_cast<const clap_event_note_expression*> (header);
                if (e->expression_id == CLAP_NOTE_EXPRESSION_TUNING)
                    setBend (e->value);
                break;
            }
            case CLAP_EVENT_MIDI:
            {
                const auto* e = reinterpret_cast<const clap_event_midi*> (header);
                const int status = e->data[0] & 0xf0;
                const int data1 = e->data[1] & 0x7f, data2 = e->data[2] & 0x7f;
                if (status == 0x90 && data2 > 0)
                    noteOn (data1, data2 / 127.0);
                else if (status == 0x80 || (status == 0x90 && data2 == 0))
                    noteOff (data1);
                else if (status == 0xb0 && (data1 == 120 || data1 == 123))
                    panic();
                else if (status == 0xe0)
                    setBend ((data2 * 128 + data1 - 8192) / 8192.0 * 2.0); // +/- 2 semitones, as the web page
                break;
            }
            case CLAP_EVENT_PARAM_VALUE:
            {
                const auto* e = reinterpret_cast<const clap_event_param_value*> (header);
                hostValue (e->param_id, e->value);
                break;
            }
            default:
                break;
        }
    }

    // Applies every pending event whose internal position falls before `chunkEnd`.
    void applyEventsBefore (double chunkEnd)
    {
        while (pendingIndex < pending.size() && pending[pendingIndex].pos < chunkEnd)
            applyEvent (pending[pendingIndex++].header);
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

        // The events of this block, at their internal positions, after the ones still pending from the previous
        // block (kept in the queue until their position is reached).
        pending.erase (pending.begin(), pending.begin() + (long) pendingIndex);
        pendingIndex = 0;
        const double ratio = (double) kInternalRate / sampleRate;
        const uint32_t count = process.in_events->size (process.in_events);
        for (uint32_t i = 0; i < count; ++i)
        {
            const clap_event_header* header = process.in_events->get (process.in_events, i);
            const size_t time = std::min<size_t> (header->time, frames > 0 ? frames - 1 : 0);
            pending.push_back ({ (hostSamples + time) * ratio, header });
        }

        const bool passthrough = sampleRate == kInternalRate;
        for (size_t j = 0; j < frames; ++j)
        {
            const double p = (hostSamples + j) * ratio;
            const long need = (long) std::floor (p) + Resampler::kTaps + 1;
            while ((long) produced < need)
            {
                applyEventsBefore ((double) produced + kBlock);
                computeChunk();
            }
            if (sleeping)
            {
                // The chunks were computed as silence; nothing to resample but zeros.
                left[j] = 0.0f;
                right[j] = 0.0f;
                continue;
            }
            if (passthrough)
            {
                const long idx = (long) std::floor (p) - (long) rs.base;
                left[j] = idx >= 0 && idx < (long) rs.histL.size() ? rs.histL[(size_t) idx] : 0.0f;
                right[j] = idx >= 0 && idx < (long) rs.histR.size() ? rs.histR[(size_t) idx] : 0.0f;
            }
            else
            {
                left[j] = rs.sample (rs.histL, p);
                right[j] = rs.sample (rs.histR, p);
            }
            const float peak = std::max (std::fabs (left[j]), std::fabs (right[j]));
            meterPeak = std::max (meterPeak, peak);
            if (! std::isfinite (left[j]))
                left[j] = 0.0f;
            if (! std::isfinite (right[j]))
                right[j] = 0.0f;
        }
        hostSamples += frames;
        applyEventsBefore ((double) produced + kBlock); // events at the very end of the block

        if (lastVoices == 0 && blockPeak < 1.0e-6)
            sleeping = (quietSamples += frames) > (uint64_t) (sampleRate / 2);
        else
            quietSamples = 0;
        blockPeak = 0.0;

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
            case pageNoteOn:
            {
                if (size < 16)
                    return false;
                noteOn ((int) u32 (4), f64 (8));
                return true;
            }
            case pageNoteOff:
            {
                if (size < 8)
                    return false;
                noteOff ((int) u32 (4));
                return true;
            }
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
        state.magic[0] = 'E'; state.magic[1] = 'L'; state.magic[2] = 'E'; state.magic[3] = 'M';
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
        if (read != sizeof (State) || std::memcmp (bytes, "ELEM", 4) != 0)
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

Elements& self (const clap_plugin* plugin) { return *static_cast<Elements*> (plugin->plugin_data); }

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
    if (id == modulationParamId)
    {
        std::snprintf (text, size, "%+.2f st", value);
        return true;
    }
    if (p.unit != nullptr)
    {
        std::snprintf (text, size, "%.3g %s", value, p.unit);
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
        else if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_NOTE_ON)
        {
            const auto* e = reinterpret_cast<const clap_event_note*> (header);
            p.noteOn (e->key, e->velocity);
        }
        else if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_NOTE_OFF)
            p.noteOff (reinterpret_cast<const clap_event_note*> (header)->key);
    }
    if (out != nullptr)
        p.writeEvents (*out);
}

const clap_plugin_params params { paramsCount, paramsInfo, paramsValue, paramsValueToText, paramsTextToValue,
                                  paramsFlush };

bool stateSave (const clap_plugin* plugin, const clap_ostream* stream) { return self (plugin).save (*stream); }
bool stateLoad (const clap_plugin* plugin, const clap_istream* stream) { return self (plugin).load (*stream); }

const clap_plugin_state state { stateSave, stateLoad };

uint32_t tailGet (const clap_plugin*) { return UINT32_MAX; } // the reverb tail rings for seconds

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
    "Elements",
    "Mutable Instruments (Faust port: sletz)",
    "https://github.com/sletz/faust-elements",
    "",
    "",
    ELEMENTS_VERSION,
    "Physical modelling voice (Mutable Instruments Elements) in Faust: bow, blow and strike exciters, a 52-mode "
    "resonator or one string or a five-string chord, and the module's stereo reverb",
    features
};

const clap_plugin* createPlugin (const clap_host* host)
{
    auto* p = new (std::nothrow) Elements();
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
