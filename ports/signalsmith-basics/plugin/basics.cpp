// Signalsmith Basics (Geraint Luff, MIT) as WebCLAP audio effects: the effects from external/signalsmith-basics behind
// plain CLAP plugins with a webview, one module for all seven. Six take over Signalsmith's own WebCLAP build: its clap
// ids, its parameter ids and values (knob positions 0..1, stepped values as they are) and its state (CBOR, see below),
// so openDAW offers this build as their update and projects keep their settings. Dynamics is new.
//
// openDAW shows each effect's card in its device panel (feature "opendaw:device-panel", extension
// "opendaw.device-panel/1"), every host in the window. The page is page/index.html#<effect>, built on the shared
// controls in modules/opendaw-page; the panel, the page's messages and the window come from
// modules/opendaw-clap/opendaw_page.h.

#include <clap/clap.h>
#include "opendaw_page.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "signalsmith-basics/analyser.h"
#include "signalsmith-basics/chorus.h"
#include "signalsmith-basics/crunch.h"
#include "signalsmith-basics/dynamics.h"
#include "signalsmith-basics/freq-shifter.h"
#include "signalsmith-basics/limiter.h"
#include "signalsmith-basics/reverb.h"

extern "C" void emscripten_notify_memory_growth(size_t index) {(void) index;}

namespace basics = signalsmith::basics;

#define MAX_PARAMS 16
#define OUT_EVENTS 128
#define GUI_ZOOM 2.0
#define GUI_MIN_ZOOM 1.0
#define GUI_MAX_ZOOM 3.0
#define MAX_BANDS 1024

// --- parameters -------------------------------------------------------------------------------------------------
// A range parameter's host value is its knob position (0..1), mapped to the effect's value through STFX's three
// points: low at 0, mid at 0.5, high at 1. The frequency shifter's shift has its own control curve. A stepped
// parameter's host value is the step.

enum class Kind {Range, Shift, Stepped};
enum class Text {Percent, Gain, Db, DbOff, Hz, Ms, Seconds, Cents, Plain0, Plain1, Ratio, InverseRatio, Labels};

struct Param {
    clap_id id;
    const char *path; // the state key, "group/key" inside a group
    const char *name;
    Kind kind;
    double low, mid, high;
    double value; // the default, in the effect's units
    Text text;
    const char *const *labels;
};

static double db_to_gain(double db) {return std::pow(10.0, db * 0.05);}
static double gain_to_db(double gain) {return 20.0 * std::log10(std::fmax(gain, 1e-10));}

static double to_effect(const Param &param, double host) {
    switch (param.kind) {
        case Kind::Stepped: return std::round(std::fmin(param.high, std::fmax(param.low, host)));
        case Kind::Shift: {
            const double x = std::fmin(1.0, std::fmax(0.0, host)) * 2.0 - 1.0;
            return 100.0 * x / (1.1 - x * x);
        }
        default: {
            const double u = std::fmin(1.0, std::fmax(0.0, host));
            const double s = (param.mid - param.low) / (param.high - param.mid);
            return param.low + (param.high - param.low) * u * s / (1.0 - u + u * s);
        }
    }
}

static double to_host(const Param &param, double value) {
    switch (param.kind) {
        case Kind::Stepped: return std::round(std::fmin(param.high, std::fmax(param.low, value)));
        case Kind::Shift: {
            if (std::fabs(value) < 1e-9) {return 0.5;}
            const double x = (std::sqrt(2500.0 + 1.1 * value * value) - 50.0) / value;
            return std::fmin(1.0, std::fmax(0.0, (x + 1.0) * 0.5));
        }
        default: {
            const double s = (param.mid - param.low) / (param.high - param.mid);
            const double f = (value - param.low) / (param.high - param.low);
            const double u = f / (s - f * s + f);
            return std::isfinite(u) ? std::fmin(1.0, std::fmax(0.0, u)) : 0.0;
        }
    }
}

static const char *const REFLECT[] = {"no reflect/duplicate", "reflect below 0Hz", "duplicate above 0Hz",
                                      "always reflect/duplicate"};
static const char *const SMOOTHING[] = {"rect", "double"};
static const char *const OFF_ON[] = {"off", "on"};

// ids of the six from Signalsmith's build (hashes of the keys there), Dynamics' are FNV-1a hashes of its paths
static const double G_M12 = 0.25118864315095801, G_P24 = 15.848931924611133, G_P40 = 100.0, G_M24 = 0.063095734448019331;
static const Param CHORUS[] = {
    {1223226259u, "mix", "mix", Kind::Range, 0, 0.5, 1, 0.6, Text::Percent, nullptr},
    {1841279726u, "depthMs", "depth", Kind::Range, 2, 15, 50, 15, Text::Ms, nullptr},
    {2967225864u, "detune", "detune", Kind::Range, 1, 8, 50, 5, Text::Cents, nullptr},
    {3972183190u, "stereo", "stereo", Kind::Range, 0, 0.5, 1.5, 1, Text::Percent, nullptr},
};
static const Param CRUNCH[] = {
    {2943184073u, "drive", "drive", Kind::Range, G_M12, 4, G_P40, 4, Text::Gain, nullptr},
    {481700u, "fuzz", "fuzz", Kind::Range, 0, 0.5, 1, 0, Text::Percent, nullptr},
    {4249490896u, "toneHz", "tone", Kind::Range, 100, 4000, 20000, 2000, Text::Hz, nullptr},
    {1100269863u, "cutHz", "cut", Kind::Range, 20, 100, 500, 50, Text::Hz, nullptr},
    {2537258908u, "outGain", "out", Kind::Range, G_M12, 1, G_P24, 1, Text::Gain, nullptr},
};
static const Param FREQ_SHIFTER[] = {
    {1223226259u, "mix", "mix", Kind::Range, 0, 0.5, 1, 1, Text::Percent, nullptr},
    {2301226280u, "shiftHz", "shift", Kind::Shift, -1000, 0, 1000, 50, Text::Hz, nullptr},
    {3716977271u, "reflect", "reflect", Kind::Stepped, 0, 0, 3, 0, Text::Labels, REFLECT},
};
static const Param LIMITER[] = {
    {224108168u, "inputGain", "pre-gain", Kind::Range, G_M12, 1, G_P24, 1, Text::Gain, nullptr},
    {1951529716u, "outputLimit", "limit", Kind::Range, G_M24, 0.25118864315095801, 1, 0.70794578438413791, Text::Gain, nullptr},
    {274825181u, "attackMs", "attack", Kind::Range, 1, 10, 50, 20, Text::Ms, nullptr},
    {1212264141u, "holdMs", "hold", Kind::Range, 0, 10, 50, 0, Text::Ms, nullptr},
    {1417304825u, "releaseMs", "release", Kind::Range, 0, 10, 250, 0, Text::Ms, nullptr},
    {870994041u, "smoothingStages", "smoothing", Kind::Stepped, 1, 1, 2, 1, Text::Labels, SMOOTHING},
    {2439531228u, "linkChannels", "link", Kind::Range, 0, 0.5, 1, 0.5, Text::Percent, nullptr},
};
static const Param REVERB[] = {
    {1015595465u, "dry", "dry", Kind::Range, 0, 1, 4, 1, Text::Gain, nullptr},
    {3538153984u, "wet", "wet", Kind::Range, 0, 1, 4, 0.5, Text::Gain, nullptr},
    {2974242839u, "roomMs", "room", Kind::Range, 10, 100, 200, 80, Text::Ms, nullptr},
    {1506047092u, "rt20", "decay", Kind::Range, 0.01, 2, 30, 1, Text::Seconds, nullptr},
    {867170651u, "early", "early", Kind::Range, 0, 1, 2.5, 1.5, Text::Percent, nullptr},
    {2967225864u, "detune", "detune", Kind::Range, 0, 5, 50, 2, Text::Plain1, nullptr},
    {2342541086u, "lowCutHz", "low cut", Kind::Range, 10, 80, 500, 80, Text::Hz, nullptr},
    {1970510480u, "lowDampRate", "low damp", Kind::Range, 1, 2, 10, 1.5, Text::Plain1, nullptr},
    {1157094815u, "highCutHz", "high cut", Kind::Range, 1000, 5000, 20000, 12000, Text::Hz, nullptr},
    {2716295038u, "highDampRate", "high damp", Kind::Range, 1, 2, 10, 2.5, Text::Plain1, nullptr},
};
static const Param ANALYSER[] = {
    {2639851652u, "barkResolution", "res.", Kind::Range, 2, 10, 25, 10, Text::Plain0, nullptr},
};
static const Param DYNAMICS[] = {
    {0u, "compressor/limitDb", "threshold", Kind::Range, -60, -30, 0, -20, Text::Db, nullptr},
    {0u, "compressor/invRatio", "ratio", Kind::Range, 1, 0.5, 0, 0.5, Text::InverseRatio, nullptr},
    {0u, "compressor/autoGain", "auto gain", Kind::Stepped, 0, 0, 1, 1, Text::Labels, OFF_ON},
    {0u, "compressor/makeupDb", "makeup", Kind::Range, 0, 6, 20, 0, Text::DbOff, nullptr},
    {0u, "expander/limitDb", "expand", Kind::Range, -60, -30, 0, -40, Text::Db, nullptr},
    {0u, "expander/ratio", "exp. ratio", Kind::Range, 1, 2, 10, 1, Text::Ratio, nullptr},
    {0u, "gate/limitDb", "gate", Kind::Range, -80, -40, 0, -80, Text::Db, nullptr},
    {0u, "gate/hysteresis", "hysteresis", Kind::Range, 0, 3, 12, 3, Text::Db, nullptr},
    {0u, "kneeDb", "knee", Kind::Range, 0, 3, 12, 3, Text::Db, nullptr},
    {0u, "timing/lookaheadMs", "lookahead", Kind::Range, 0, 5, 20, 0, Text::Ms, nullptr},
    {0u, "timing/attackMs", "attack", Kind::Range, 1, 20, 50, 20, Text::Ms, nullptr},
    {0u, "timing/releaseMs", "release", Kind::Range, 20, 100, 250, 150, Text::Ms, nullptr},
    {1223226259u, "mix", "mix", Kind::Range, 0, 0.5, 1, 1, Text::Percent, nullptr},
};

static clap_id fnv1a(const char *text) {
    uint32_t hash = 2166136261u;
    for (; *text != '\0'; text++) {hash = (hash ^ static_cast<uint8_t>(*text)) * 16777619u;}
    return hash;
}

static clap_id param_id(const Param &param) {return param.id != 0 ? param.id : fnv1a(param.path);}

// --- engines ----------------------------------------------------------------------------------------------------
// One per effect: the STFX effect, its parameters in the order of its table, and the readings its display wants.

struct Readings {
    float phase; // chorus: the oscillator, 0..1
    float level_db, gain; // dynamics: the detector's loudest level; dynamics and limiter: the lowest gain
    uint32_t bands; // analyser: bands in hz and energy, 0 while no new spectrum is ready
    float hz[MAX_BANDS];
    float energy[2][MAX_BANDS];
};

struct Engine {
    virtual ~Engine() = default;
    virtual bool configure(double rate, uint32_t frames) = 0;
    virtual void reset() = 0;
    virtual void process(float *const *inputs, float *const *outputs, uint32_t frames) = 0;
    virtual void set(uint32_t index, double value) = 0;
    virtual int latency() = 0;
    virtual int tail() = 0;
    virtual void read(Readings &readings) {(void) readings;}
};

template<class Effect>
struct EngineOf : Engine {
    using Range = typename Effect::ParamRange;
    using Stepped = typename Effect::ParamStepped;
    struct Slot {
        Range *range;
        Stepped *stepped;
    };
    Effect effect;
    Slot slots[MAX_PARAMS] = {};
    uint32_t aux_channels = 0;

    void bind(uint32_t index, Range &param) {slots[index] = {&param, nullptr};}
    void bind(uint32_t index, Stepped &param) {slots[index] = {nullptr, &param};}

    bool configure(double rate, uint32_t frames) override {
        return effect.configure(rate, frames, 2, 2, aux_channels, 0);
    }
    void reset() override {effect.reset();}
    void process(float *const *inputs, float *const *outputs, uint32_t frames) override {
        effect.process(inputs, outputs, frames);
    }
    void set(uint32_t index, double value) override {
        const Slot &slot = slots[index];
        if (slot.range != nullptr) {*slot.range = value;}
        if (slot.stepped != nullptr) {*slot.stepped = static_cast<int>(std::lround(value));}
    }
    int latency() override {return static_cast<int>(effect.latencySamples());}
    int tail() override {return static_cast<int>(effect.tailSamples());}
};

struct ChorusEngine : EngineOf<basics::ChorusFloat> {
    ChorusEngine() {
        bind(0, effect.mix);
        bind(1, effect.depthMs);
        bind(2, effect.detune);
        bind(3, effect.stereo);
    }
    void read(Readings &readings) override {readings.phase = static_cast<float>(effect.lfoPhase());}
};

struct CrunchEngine : EngineOf<basics::CrunchFloat> {
    CrunchEngine() {
        bind(0, effect.drive);
        bind(1, effect.fuzz);
        bind(2, effect.toneHz);
        bind(3, effect.cutHz);
        bind(4, effect.outGain);
    }
};

struct FreqShifterEngine : EngineOf<basics::FreqShifterFloat> {
    FreqShifterEngine() {
        bind(0, effect.mix);
        bind(1, effect.shiftHz);
        bind(2, effect.reflect);
    }
};

struct LimiterEngine : EngineOf<basics::LimiterFloat> {
    LimiterEngine() {
        bind(0, effect.inputGain);
        bind(1, effect.outputLimit);
        bind(2, effect.attackMs);
        bind(3, effect.holdMs);
        bind(4, effect.releaseMs);
        bind(5, effect.smoothingStages);
        bind(6, effect.linkChannels);
    }
    void read(Readings &readings) override {
        readings.gain = static_cast<float>(effect.meterGain);
        effect.meterGain = 1;
    }
};

struct ReverbEngine : EngineOf<basics::ReverbFloat> {
    ReverbEngine() {
        bind(0, effect.dry);
        bind(1, effect.wet);
        bind(2, effect.roomMs);
        bind(3, effect.rt20);
        bind(4, effect.early);
        bind(5, effect.detune);
        bind(6, effect.lowCutHz);
        bind(7, effect.lowDampRate);
        bind(8, effect.highCutHz);
        bind(9, effect.highDampRate);
    }
};

struct AnalyserEngine : EngineOf<basics::AnalyserFloat> {
    AnalyserEngine() {bind(0, effect.barkResolution);}
    bool configure(double rate, uint32_t frames) override {
        const bool configured = EngineOf::configure(rate, frames);
        effect.wantsMeters(true);
        return configured;
    }
    // the spectrum once the effect has one ready, then it fills the next
    void read(Readings &readings) override {
        readings.bands = 0;
        if (!effect.hasMeters()) {return;}
        const auto &hz = effect.spectrumHz();
        const uint32_t bands = static_cast<uint32_t>(hz.size() < MAX_BANDS ? hz.size() : MAX_BANDS);
        for (uint32_t band = 0; band < bands; band++) {readings.hz[band] = hz[band];}
        for (uint32_t channel = 0; channel < 2; channel++) {
            const auto &energy = effect.spectrumEnergy(channel);
            for (uint32_t band = 0; band < bands; band++) {
                readings.energy[channel][band] = band < energy.size() ? energy[band] : 0.0f;
            }
        }
        readings.bands = bands;
        effect.wantsMeters(true);
    }
};

// with a sidechain: the detector listens to the second input when the host connects it
struct DynamicsEngine : EngineOf<basics::DynamicsFloat> {
    DynamicsEngine() {
        aux_channels = 2;
        bind(0, effect.compressor.limitDb);
        bind(1, effect.compressor.invRatio);
        bind(2, effect.compressor.autoGain);
        bind(3, effect.compressor.makeupDb);
        bind(4, effect.expander.limitDb);
        bind(5, effect.expander.ratio);
        bind(6, effect.gate.limitDb);
        bind(7, effect.gate.hysteresis);
        bind(8, effect.kneeDb);
        bind(9, effect.timing.lookaheadMs);
        bind(10, effect.timing.attackMs);
        bind(11, effect.timing.releaseMs);
        bind(12, effect.mix);
    }
    void read(Readings &readings) override {
        readings.level_db = static_cast<float>(effect.meterLevelDb);
        readings.gain = static_cast<float>(effect.meterGain);
        effect.meterLevelDb = -300;
        effect.meterGain = 1;
    }
};

// --- descriptors ------------------------------------------------------------------------------------------------

struct Effect {
    const char *page; // the page's #fragment
    clap_plugin_descriptor_t descriptor;
    const Param *params;
    uint32_t param_count;
    int version; // the state's
    uint32_t width; // the card at the panel height
    bool sidechain;
    Engine *(*create)();
};

template<class T> static Engine *create_engine() {return new(std::nothrow) T();}

// the card's width at openDAW's 176 px as "opendaw:device-panel-width:<width>", read from its cloud index before the
// plugin loads; it is the effect's width below
#define FEATURES(width, ...) \
    (const char *[]) {__VA_ARGS__, OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL, OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL_WIDTH(width), nullptr}
#define DESCRIPTOR(id, name, description, features) \
    {CLAP_VERSION_INIT, id, name, "Signalsmith Audio", "https://signalsmith-audio.co.uk/code/basics/", \
     "https://github.com/andremichelle/open-webclap/tree/main/ports/signalsmith-basics", "", "1.1.0", description, \
     features}
#define COUNT(array) static_cast<uint32_t>(sizeof(array) / sizeof(array[0]))

static const Effect EFFECTS[] = {
    {"chorus", DESCRIPTOR("uk.co.signalsmith.basics.chorus", "[Basics] Chorus", "Six-voice stereo chorus",
        FEATURES(260, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_CHORUS)),
        CHORUS, COUNT(CHORUS), 0, 260, false, create_engine<ChorusEngine>},
    {"crunch", DESCRIPTOR("uk.co.signalsmith.basics.crunch", "[Basics] Crunch", "Oversampled distortion and saturation",
        FEATURES(284, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DISTORTION)),
        CRUNCH, COUNT(CRUNCH), 0, 284, false, create_engine<CrunchEngine>},
    {"freq-shifter", DESCRIPTOR("uk.co.signalsmith.basics.freq-shifter", "[Basics] Frequency Shifter",
        "Hilbert / Bode single-sideband frequency shifter",
        FEATURES(246, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_FREQUENCY_SHIFTER)),
        FREQ_SHIFTER, COUNT(FREQ_SHIFTER), 0, 246, false, create_engine<FreqShifterEngine>},
    {"limiter", DESCRIPTOR("uk.co.signalsmith.basics.limiter", "[Basics] Limiter", "Lookahead limiter",
        FEATURES(322, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_LIMITER)),
        LIMITER, COUNT(LIMITER), 4, 322, false, create_engine<LimiterEngine>},
    {"reverb", DESCRIPTOR("uk.co.signalsmith.basics.reverb", "[Basics] Reverb", "Feedback delay network reverb",
        FEATURES(382, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_REVERB)),
        REVERB, COUNT(REVERB), 5, 382, false, create_engine<ReverbEngine>},
    {"analyser", DESCRIPTOR("uk.co.signalsmith.basics.analyser", "[Basics] Analyser", "Bark-scale spectrum analyser",
        FEATURES(338, CLAP_PLUGIN_FEATURE_ANALYZER)),
        ANALYSER, COUNT(ANALYSER), 0, 338, false, create_engine<AnalyserEngine>},
    {"dynamics", DESCRIPTOR("uk.co.signalsmith.basics.dynamics", "[Basics] Dynamics",
        "Compressor, expander and gate with lookahead and a sidechain",
        FEATURES(374, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_COMPRESSOR, CLAP_PLUGIN_FEATURE_EXPANDER,
                 CLAP_PLUGIN_FEATURE_GATE)),
        DYNAMICS, COUNT(DYNAMICS), 0, 374, true, create_engine<DynamicsEngine>},
};

// --- the plugin ---------------------------------------------------------------------------------------------------

struct Basics {
    clap_plugin_t plugin;
    const clap_host_t *host;
    opendaw::PageHost page; // the host's params, state, webview and hovered parameter
    const clap_host_latency_t *host_latency;
    const Effect *effect;
    Engine *engine;
    double values[MAX_PARAMS]; // host values
    double effect_values[MAX_PARAMS]; // the same in the effect's units: exact for defaults and loaded states
    bool changed[MAX_PARAMS];
    bool any_changed;
    bool active;
    int latency;
    uint32_t max_frames;
    float *buffers[6]; // the effect's inputs (2, and 2 for a sidechain), its outputs
    float peak_in, peak_out;
    uint32_t sidechain_hold; // frames the sidechain stays the detector's after its last sound
    uint32_t sidechain_frames;
    uint32_t gui_width, gui_height;
    opendaw::OutEvents<OUT_EVENTS> out_events; // the page's gestures and values for the host
    Readings readings;
    uint8_t message[16 + MAX_PARAMS * 8 + 24 + MAX_BANDS * 12];
};

static Basics *basics_of(const clap_plugin_t *plugin) {return static_cast<Basics *>(plugin->plugin_data);}

static int32_t index_of(const Basics *self, clap_id id) {
    for (uint32_t index = 0; index < self->effect->param_count; index++) {
        if (param_id(self->effect->params[index]) == id) {return static_cast<int32_t>(index);}
    }
    return -1;
}

static void set_value(Basics *self, uint32_t index, double host) {
    const Param &param = self->effect->params[index];
    const double clamped = param.kind == Kind::Stepped ? to_host(param, host) : std::fmin(1.0, std::fmax(0.0, host));
    if (clamped == self->values[index]) {return;}
    self->values[index] = clamped;
    self->effect_values[index] = to_effect(param, clamped);
    self->changed[index] = true;
    self->any_changed = true;
}

static void apply_values(Basics *self) {
    if (!self->any_changed || self->engine == nullptr) {return;}
    self->any_changed = false;
    for (uint32_t index = 0; index < self->effect->param_count; index++) {
        if (!self->changed[index]) {continue;}
        self->changed[index] = false;
        self->engine->set(index, self->effect_values[index]);
    }
}

// a changed attack or lookahead changes the latency, openDAW reads it again on clap.host_latency.changed
static void check_latency(Basics *self) {
    if (!self->active || self->engine == nullptr) {return;}
    const int latency = self->engine->latency();
    if (latency == self->latency) {return;}
    self->latency = latency;
    if (self->host_latency != nullptr) {self->host_latency->changed(self->host);}
}

static void handle_event(Basics *self, const clap_event_header_t *header) {
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID || header->type != CLAP_EVENT_PARAM_VALUE) {return;}
    const clap_event_param_value_t *event = reinterpret_cast<const clap_event_param_value_t *>(header);
    const int32_t index = index_of(self, event->param_id);
    if (index >= 0) {set_value(self, static_cast<uint32_t>(index), event->value);}
}

static bool basics_init(const clap_plugin_t *plugin) {
    Basics *self = basics_of(plugin);
    const clap_host_t *host = self->host;
    self->page.init(host);
    self->host_latency = static_cast<const clap_host_latency_t *>(host->get_extension(host, CLAP_EXT_LATENCY));
    return true;
}

static void free_buffers(Basics *self) {
    for (float *&buffer : self->buffers) {
        std::free(buffer);
        buffer = nullptr;
    }
}

static void basics_destroy(const clap_plugin_t *plugin) {
    Basics *self = basics_of(plugin);
    delete self->engine;
    free_buffers(self);
    self->~Basics();
    std::free(self);
}

// the engine is built here: STFX allocates while it configures
static bool basics_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames) {
    (void) min_frames;
    Basics *self = basics_of(plugin);
    delete self->engine;
    self->engine = self->effect->create();
    if (self->engine == nullptr) {return false;}
    free_buffers(self);
    self->max_frames = max_frames > 0 ? max_frames : 1;
    for (float *&buffer : self->buffers) {
        buffer = static_cast<float *>(std::calloc(self->max_frames, sizeof(float)));
        if (buffer == nullptr) {return false;}
    }
    for (uint32_t index = 0; index < self->effect->param_count; index++) {
        self->engine->set(index, self->effect_values[index]);
        self->changed[index] = false;
    }
    self->any_changed = false;
    if (!self->engine->configure(sample_rate, self->max_frames)) {return false;}
    self->latency = self->engine->latency();
    self->peak_in = self->peak_out = 0.0f;
    self->sidechain_hold = 0;
    self->sidechain_frames = static_cast<uint32_t>(sample_rate * 0.5);
    self->active = true;
    return true;
}

static void basics_deactivate(const clap_plugin_t *plugin) {basics_of(plugin)->active = false;}

static bool basics_start_processing(const clap_plugin_t *plugin) {(void) plugin; return true;}

static void basics_stop_processing(const clap_plugin_t *plugin) {(void) plugin;}

static void basics_reset(const clap_plugin_t *plugin) {
    Basics *self = basics_of(plugin);
    if (self->engine != nullptr) {self->engine->reset();}
}

static float peak_of(const float *samples, uint32_t frames, float peak) {
    for (uint32_t frame = 0; frame < frames; frame++) {peak = std::fmax(peak, std::fabs(samples[frame]));}
    return peak;
}

// A host hands an unconnected sidechain over as silence (openDAW does), indistinguishable from a quiet one, so the
// detector follows the sidechain while it sounds, and half a second after, otherwise the main input.
static bool sidechain_sounds(Basics *self, const clap_process_t *process, uint32_t from, uint32_t to) {
    if (!self->effect->sidechain || process->audio_inputs_count < 2) {return false;}
    const clap_audio_buffer_t &aux = process->audio_inputs[1];
    if (aux.channel_count == 0 || aux.data32 == nullptr) {return false;}
    bool sounds = false;
    for (uint32_t channel = 0; channel < aux.channel_count && channel < 2 && !sounds; channel++) {
        sounds = peak_of(aux.data32[channel] + from, to - from, 0.0f) > 0.0f;
    }
    if (sounds) {self->sidechain_hold = self->sidechain_frames;}
    else {self->sidechain_hold = self->sidechain_hold > to - from ? self->sidechain_hold - (to - from) : 0;}
    return self->sidechain_hold > 0;
}

// the host's channels into the effect's own buffers (STFX wants inputs apart from outputs), in pieces of max_frames
static void render(Basics *self, const clap_process_t *process, uint32_t from, uint32_t to) {
    const clap_audio_buffer_t &input = process->audio_inputs[0];
    const clap_audio_buffer_t &output = process->audio_outputs[0];
    if (to <= from) {return;}
    const bool sidechain = sidechain_sounds(self, process, from, to);
    while (from < to) {
        const uint32_t frames = to - from < self->max_frames ? to - from : self->max_frames;
        for (uint32_t channel = 0; channel < 2; channel++) {
            const float *source = input.data32[channel < input.channel_count ? channel : 0];
            std::memcpy(self->buffers[channel], source + from, frames * sizeof(float));
            self->peak_in = peak_of(source + from, frames, self->peak_in);
            if (self->effect->sidechain) {
                const clap_audio_buffer_t &aux = process->audio_inputs[sidechain ? 1 : 0];
                const float *key = aux.data32[channel < aux.channel_count ? channel : 0];
                std::memcpy(self->buffers[2 + channel], key + from, frames * sizeof(float));
            }
        }
        self->engine->process(self->buffers, self->buffers + 4, frames);
        for (uint32_t channel = 0; channel < output.channel_count && channel < 2; channel++) {
            std::memcpy(output.data32[channel] + from, self->buffers[4 + channel], frames * sizeof(float));
            self->peak_out = peak_of(self->buffers[4 + channel], frames, self->peak_out);
        }
        from += frames;
    }
}

static clap_process_status basics_process(const clap_plugin_t *plugin, const clap_process_t *process) {
    Basics *self = basics_of(plugin);
    if (self->engine == nullptr || process->audio_inputs_count < 1 || process->audio_outputs_count < 1) {
        return CLAP_PROCESS_ERROR;
    }
    if (process->audio_inputs[0].channel_count < 1 || process->audio_outputs[0].channel_count < 1) {
        return CLAP_PROCESS_ERROR;
    }
    const uint32_t frames = process->frames_count;
    const uint32_t count = process->in_events->size(process->in_events);
    uint32_t position = 0;
    for (uint32_t index = 0; index < count; index++) {
        const clap_event_header_t *header = process->in_events->get(process->in_events, index);
        const uint32_t time = header->time < frames ? header->time : frames;
        if (time > position) {
            apply_values(self);
            render(self, process, position, time);
            position = time;
        }
        handle_event(self, header);
    }
    apply_values(self);
    render(self, process, position, frames);
    self->out_events.drain(process->out_events);
    check_latency(self);
    return CLAP_PROCESS_CONTINUE;
}

// --- audio ports, latency, tail -----------------------------------------------------------------------------------

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input) {
    return is_input && basics_of(plugin)->effect->sidechain ? 2 : 1;
}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
    if (index >= audio_ports_count(plugin, is_input)) {return false;}
    info->id = index;
    std::snprintf(info->name, sizeof(info->name), "%s", index == 1 ? "Sidechain" : "main");
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t audio_ports = {audio_ports_count, audio_ports_get};

static uint32_t latency_get(const clap_plugin_t *plugin) {
    const Basics *self = basics_of(plugin);
    return self->engine != nullptr ? static_cast<uint32_t>(self->engine->latency()) : 0;
}

static const clap_plugin_latency_t latency = {latency_get};

static uint32_t tail_get(const clap_plugin_t *plugin) {
    const Basics *self = basics_of(plugin);
    return self->engine != nullptr ? static_cast<uint32_t>(self->engine->tail()) : 0;
}

static const clap_plugin_tail_t tail = {tail_get};

// --- parameters ---------------------------------------------------------------------------------------------------

static uint32_t params_count(const clap_plugin_t *plugin) {return basics_of(plugin)->effect->param_count;}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info) {
    const Effect *effect = basics_of(plugin)->effect;
    if (index >= effect->param_count) {return false;}
    const Param &param = effect->params[index];
    std::memset(info, 0, sizeof(*info));
    info->id = param_id(param);
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (param.kind == Kind::Stepped) {
        info->flags |= CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM;
        info->min_value = param.low;
        info->max_value = param.high;
    } else {
        info->min_value = 0.0;
        info->max_value = 1.0;
    }
    info->default_value = to_host(param, param.value);
    std::snprintf(info->name, sizeof(info->name), "%s", param.name);
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value) {
    const Basics *self = basics_of(plugin);
    const int32_t index = index_of(self, id);
    if (index < 0) {return false;}
    *value = self->values[index];
    return true;
}

static void format(const Param &param, double value, char *text, uint32_t size) {
    const double magnitude = std::fabs(value);
    switch (param.text) {
        case Text::Percent: std::snprintf(text, size, "%.0f%%", value * 100.0); break;
        case Text::Gain:
            if (value <= 0.0) {std::snprintf(text, size, "off"); break;}
            std::snprintf(text, size, "%.1fdB", gain_to_db(value));
            break;
        case Text::Db: std::snprintf(text, size, "%.1fdB", value); break;
        case Text::DbOff:
            if (value <= 0.0) {std::snprintf(text, size, "off"); break;}
            std::snprintf(text, size, "%.1fdB", value);
            break;
        case Text::Hz:
            if (magnitude >= 1000.0) {std::snprintf(text, size, "%.*fkHz", magnitude >= 10000.0 ? 1 : 2, value / 1000.0);}
            else {std::snprintf(text, size, "%.*fHz", magnitude < 10.0 ? 1 : 0, value);}
            break;
        case Text::Ms:
            if (magnitude >= 999.5) {std::snprintf(text, size, "%.2f sec", value / 1000.0);}
            else {std::snprintf(text, size, "%.*fms", magnitude < 10.0 ? 1 : 0, value);}
            break;
        case Text::Seconds:
            if (magnitude < 0.9995) {std::snprintf(text, size, "%.0fms", value * 1000.0);}
            else {std::snprintf(text, size, "%.*f sec", magnitude < 10.0 ? 2 : 1, value);}
            break;
        case Text::Cents: std::snprintf(text, size, "%.*f cents", magnitude < 10.0 ? 1 : 0, value); break;
        case Text::Plain0: std::snprintf(text, size, "%.0f", value); break;
        case Text::Plain1: std::snprintf(text, size, "%.1f", value); break;
        case Text::Ratio: std::snprintf(text, size, "%.1f:1", value); break;
        case Text::InverseRatio:
            if (value <= 1e-6) {std::snprintf(text, size, "inf:1"); break;}
            std::snprintf(text, size, "%.1f:1", 1.0 / value);
            break;
        case Text::Labels: {
            const int step = static_cast<int>(std::lround(value - param.low));
            std::snprintf(text, size, "%s", param.labels[step < 0 ? 0 : step]);
            break;
        }
    }
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *text, uint32_t size) {
    const Basics *self = basics_of(plugin);
    const int32_t index = index_of(self, id);
    if (index < 0 || size == 0) {return false;}
    const Param &param = self->effect->params[index];
    format(param, to_effect(param, value), text, size);
    return true;
}

static bool contains(const char *text, const char *word) {
    for (; *text != '\0'; text++) {
        const char *a = text, *b = word;
        while (*a != '\0' && *b != '\0' && (*a | 32) == (*b | 32)) {a++; b++;}
        if (*b == '\0') {return true;}
    }
    return false;
}

// a number in the units the texts show ("12dB", "4k", "1.5 sec", "2:1", "off", a step's label)
static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value) {
    const Basics *self = basics_of(plugin);
    const int32_t index = index_of(self, id);
    if (index < 0 || text == nullptr) {return false;}
    const Param &param = self->effect->params[index];
    if (param.kind == Kind::Stepped) {
        for (int step = 0; step <= static_cast<int>(param.high - param.low); step++) {
            if (contains(param.labels[step], text) || contains(text, param.labels[step])) {
                *value = param.low + step;
                return true;
            }
        }
    }
    char *end = nullptr;
    double number = std::strtod(text, &end);
    if (end == text) {
        if (!contains(text, "off")) {return false;}
        *value = to_host(param, param.low);
        return true;
    }
    switch (param.text) {
        case Text::Percent: number /= 100.0; break;
        case Text::Gain: number = contains(end, "%") ? number / 100.0 : contains(end, "x") ? number : db_to_gain(number); break;
        case Text::Hz: if (contains(end, "k")) {number *= 1000.0;} break;
        case Text::Ms: if (contains(end, "s") && !contains(end, "ms")) {number *= 1000.0;} break;
        case Text::Seconds: if (contains(end, "ms")) {number /= 1000.0;} break;
        case Text::InverseRatio: number = number > 0.0 ? 1.0 / number : 1.0; break;
        default: break;
    }
    *value = to_host(param, number);
    return true;
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out) {
    Basics *self = basics_of(plugin);
    const uint32_t count = in->size(in);
    for (uint32_t index = 0; index < count; index++) {handle_event(self, in->get(in, index));}
    apply_values(self);
    self->out_events.drain(out);
    check_latency(self);
}

static const clap_plugin_params_t params = {
    params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush
};

// --- state ----------------------------------------------------------------------------------------------------------
// STFX's CBOR, as Signalsmith's build writes it: an indefinite map with "version", then per parameter its key and a
// map {"value": float64 or the step as an integer}. Parameters inside a group (Dynamics' compressor, expander, gate,
// timing) sit in a map of the group's, which has a "version" of its own. Every value keeps its length, so an edit
// changes 8 bytes (a step 1).

struct Writer {
    const clap_ostream_t *stream;
    bool ok = true;
    void bytes(const void *data, uint32_t size) {
        const uint8_t *at = static_cast<const uint8_t *>(data);
        while (ok && size > 0) {
            const int64_t written = stream->write(stream, at, size);
            if (written <= 0) {ok = false; return;}
            at += written;
            size -= static_cast<uint32_t>(written);
        }
    }
    void byte(uint8_t value) {bytes(&value, 1);}
    void head(uint8_t major, uint64_t value) {
        if (value < 24) {byte(static_cast<uint8_t>(major << 5 | value));}
        else if (value < 256) {byte(static_cast<uint8_t>(major << 5 | 24)); byte(static_cast<uint8_t>(value));}
        else {
            byte(static_cast<uint8_t>(major << 5 | 26));
            for (int shift = 24; shift >= 0; shift -= 8) {byte(static_cast<uint8_t>(value >> shift));}
        }
    }
    void text(const char *value, uint32_t length) {head(3, length); bytes(value, length);}
    void text(const char *value) {text(value, static_cast<uint32_t>(std::strlen(value)));}
    void integer(int64_t value) {value >= 0 ? head(0, static_cast<uint64_t>(value)) : head(1, static_cast<uint64_t>(-1 - value));}
    void real(double value) {
        uint64_t bits;
        std::memcpy(&bits, &value, 8);
        byte(0xfb);
        for (int shift = 56; shift >= 0; shift -= 8) {byte(static_cast<uint8_t>(bits >> shift));}
    }
};

static uint32_t group_length(const char *path) {
    const char *slash = std::strchr(path, '/');
    return slash != nullptr ? static_cast<uint32_t>(slash - path) : 0;
}

static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    const Basics *self = basics_of(plugin);
    const Effect *effect = self->effect;
    Writer writer{stream};
    writer.byte(0xbf);
    writer.text("version");
    writer.integer(effect->version);
    const char *group = nullptr;
    uint32_t group_size = 0;
    for (uint32_t index = 0; index < effect->param_count; index++) {
        const Param &param = effect->params[index];
        const uint32_t length = group_length(param.path);
        const bool same = length > 0 && group != nullptr && length == group_size && std::strncmp(group, param.path, length) == 0;
        if (group != nullptr && !same) {
            writer.byte(0xff);
            group = nullptr;
        }
        if (length > 0 && group == nullptr) {
            group = param.path;
            group_size = length;
            writer.text(param.path, length);
            writer.byte(0xbf);
            writer.text("version");
            writer.integer(0);
        }
        writer.text(length > 0 ? param.path + length + 1 : param.path);
        writer.byte(0xbf);
        writer.text("value");
        const double value = self->effect_values[index];
        if (param.kind == Kind::Stepped) {writer.integer(std::lround(value));} else {writer.real(value);}
        writer.byte(0xff);
    }
    if (group != nullptr) {writer.byte(0xff);}
    writer.byte(0xff);
    return writer.ok;
}

// A reader for that CBOR, lenient: definite or indefinite maps, any order, numbers as integers or floats of any
// width. Unknown keys and types are skipped.
struct Reader {
    const uint8_t *data;
    uint32_t size, at;
    bool ok = true;
    uint8_t next() {
        if (at >= size) {ok = false; return 0xff;}
        return data[at++];
    }
    uint64_t argument(uint8_t info) {
        if (info < 24) {return info;}
        const int bytes = info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : 0;
        if (bytes == 0) {ok = false; return 0;}
        uint64_t value = 0;
        for (int index = 0; index < bytes; index++) {value = value << 8 | next();}
        return value;
    }
    static double half(uint16_t bits) {
        const int exponent = bits >> 10 & 31, mantissa = bits & 1023;
        const double value = exponent == 0 ? std::ldexp(mantissa, -24)
            : exponent == 31 ? (mantissa != 0 ? NAN : INFINITY) : std::ldexp(mantissa + 1024, exponent - 25);
        return bits & 0x8000 ? -value : value;
    }
    // a number, or false (and skipped) for anything else
    bool number(double &value) {
        const uint8_t initial = next();
        const uint8_t major = initial >> 5, info = initial & 31;
        if (major == 0) {value = static_cast<double>(argument(info)); return ok;}
        if (major == 1) {value = -1.0 - static_cast<double>(argument(info)); return ok;}
        if (major == 7 && info >= 25 && info <= 27) {
            const uint64_t bits = argument(info);
            if (info == 25) {value = half(static_cast<uint16_t>(bits));}
            else if (info == 26) {float single; const uint32_t b = static_cast<uint32_t>(bits); std::memcpy(&single, &b, 4); value = single;}
            else {std::memcpy(&value, &bits, 8);}
            return ok;
        }
        at--;
        skip();
        return false;
    }
    void skip() {
        const uint8_t initial = next();
        const uint8_t major = initial >> 5, info = initial & 31;
        if (!ok) {return;}
        if (info == 31 && (major == 2 || major == 3 || major == 4 || major == 5)) {
            while (ok && at < size && data[at] != 0xff) {
                skip();
                if (major == 5) {skip();}
            }
            next();
            return;
        }
        const uint64_t length = argument(info);
        switch (major) {
            case 2: case 3: at = length <= size - at ? at + static_cast<uint32_t>(length) : (ok = false, size); break;
            case 4: for (uint64_t index = 0; ok && index < length; index++) {skip();} break;
            case 5: for (uint64_t index = 0; ok && index < length; index++) {skip(); skip();} break;
            case 6: skip(); break;
            default: break;
        }
    }
    // a map's entries: calls entry(key, length) with the reader at the value, which entry reads or skips
    template<class Entry>
    void map(Entry entry) {
        const uint8_t initial = next();
        if ((initial >> 5) != 5) {at--; skip(); return;}
        const bool indefinite = (initial & 31) == 31;
        const uint64_t count = indefinite ? 0 : argument(initial & 31);
        for (uint64_t index = 0; ok && (indefinite ? at < size && data[at] != 0xff : index < count); index++) {
            const uint8_t key = next();
            if ((key >> 5) != 3 || (key & 31) == 31) {at--; skip(); skip(); continue;}
            const uint64_t length = argument(key & 31);
            if (length > size - at) {ok = false; return;}
            const char *text = reinterpret_cast<const char *>(data + at);
            at += static_cast<uint32_t>(length);
            const uint32_t before = at;
            entry(text, static_cast<uint32_t>(length));
            if (at == before) {skip();}
        }
        if (indefinite) {next();}
    }
};

static bool key_is(const char *key, uint32_t length, const char *word, uint32_t word_length) {
    return length == word_length && std::strncmp(key, word, length) == 0;
}

// the parameters whose path starts with the group (empty: outside any group)
static void read_params(Basics *self, Reader &reader, const char *group, uint32_t group_size, bool &any) {
    reader.map([&](const char *key, uint32_t length) {
        for (uint32_t index = 0; index < self->effect->param_count; index++) {
            const Param &param = self->effect->params[index];
            const uint32_t prefix = group_length(param.path);
            if (prefix != group_size || std::strncmp(param.path, group, group_size) != 0) {continue;}
            const char *name = prefix > 0 ? param.path + prefix + 1 : param.path;
            if (!key_is(key, length, name, static_cast<uint32_t>(std::strlen(name)))) {continue;}
            reader.map([&](const char *field, uint32_t size) {
                double value;
                if (key_is(field, size, "value", 5) && reader.number(value) && std::isfinite(value)) {
                    const double low = std::fmin(param.low, param.high), high = std::fmax(param.low, param.high);
                    self->effect_values[index] = param.kind == Kind::Stepped ? to_host(param, value)
                        : std::fmin(high, std::fmax(low, value));
                    self->values[index] = to_host(param, self->effect_values[index]);
                    self->changed[index] = true;
                    any = true;
                }
            });
            return;
        }
        if (group_size > 0) {return;}
        // a group of parameters
        for (uint32_t index = 0; index < self->effect->param_count; index++) {
            const uint32_t prefix = group_length(self->effect->params[index].path);
            if (prefix > 0 && key_is(key, length, self->effect->params[index].path, prefix)) {
                read_params(self, reader, self->effect->params[index].path, prefix, any);
                return;
            }
        }
    });
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    Basics *self = basics_of(plugin);
    static uint8_t data[8192];
    uint32_t size = 0;
    while (size < sizeof(data)) {
        const int64_t read = stream->read(stream, data + size, sizeof(data) - size);
        if (read <= 0) {break;}
        size += static_cast<uint32_t>(read);
    }
    if (size == 0) {return false;}
    Reader reader{data, size, 0};
    bool any = false;
    read_params(self, reader, "", 0, any);
    if (any) {self->any_changed = true;}
    if (self->active) {
        apply_values(self);
        check_latency(self);
    }
    return reader.ok || any;
}

static const clap_plugin_state_t state = {state_save, state_load};

// --- page -----------------------------------------------------------------------------------------------------------

static int32_t page_uri(const clap_plugin_t *plugin, char *uri, uint32_t capacity) {
    char page[64];
    const int length = std::snprintf(page, sizeof(page), "/index.html#%s", basics_of(plugin)->effect->page);
    if (capacity > 0 && uri != nullptr) {std::snprintf(uri, capacity, "%s", page);}
    return length + 1;
}

static bool webview_get_resource(const clap_plugin_t *plugin, const char *path, char *mime, uint32_t capacity,
                                 const clap_ostream_t *stream) {
    (void) plugin; (void) path; (void) mime; (void) capacity; (void) stream;
    return false;
}

static void put_f32(uint8_t *at, float value) {std::memcpy(at, &value, 4);}

// STATUS: type, the parameter count, the band count (u16 at 2), the host values (f64 from 8), then the readings
// (f32): input and output peak since the last poll, the chorus phase, the dynamics level (dB), the gain, then the
// spectrum's bands, hz and both channels' energy
static void send_status(Basics *self) {
    const uint32_t count = self->effect->param_count;
    std::memset(&self->readings, 0, offsetof(Readings, hz));
    self->readings.gain = 1.0f;
    self->readings.level_db = -300.0f;
    if (self->engine != nullptr && self->active) {self->engine->read(self->readings);}
    uint8_t *message = self->message;
    const uint32_t bands = self->readings.bands;
    message[0] = opendaw::PLUGIN_STATUS;
    message[1] = static_cast<uint8_t>(count);
    message[2] = static_cast<uint8_t>(bands & 0xff);
    message[3] = static_cast<uint8_t>(bands >> 8);
    for (uint32_t index = 0; index < 4; index++) {message[4 + index] = 0;}
    std::memcpy(message + 8, self->values, count * 8);
    uint8_t *at = message + 8 + count * 8;
    put_f32(at, self->peak_in);
    put_f32(at + 4, self->peak_out);
    put_f32(at + 8, self->readings.phase);
    put_f32(at + 12, self->readings.level_db);
    put_f32(at + 16, self->readings.gain);
    at += 20;
    std::memcpy(at, self->readings.hz, bands * 4);
    std::memcpy(at + bands * 4, self->readings.energy[0], bands * 4);
    std::memcpy(at + bands * 8, self->readings.energy[1], bands * 4);
    at += bands * 12;
    self->peak_in = self->peak_out = 0.0f;
    self->page.send(message, static_cast<uint32_t>(at - message));
}

// the page's messages (opendaw_page.h): its values and gestures, the hovered parameter, polls answered by the status
struct PageOf {
    Basics *self;
    bool has(clap_id id) const {return index_of(self, id) >= 0;}
    void set(clap_id id, double value) {set_value(self, static_cast<uint32_t>(index_of(self, id)), value);}
    double value(clap_id id) const {return self->values[index_of(self, id)];}
    void status() {send_status(self);}
};

static bool webview_receive(const clap_plugin_t *plugin, const void *buffer, uint32_t size) {
    Basics *self = basics_of(plugin);
    PageOf page{self};
    return opendaw::receive(page, self->page, self->out_events, buffer, size);
}

static const clap_plugin_webview_t webview = {page_uri, webview_get_resource, webview_receive};

// gui: the webview, the card at its aspect ratio, 100 to 300 %, opening at 200 %
static bool gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    const Basics *self = basics_of(plugin);
    *width = self->gui_width;
    *height = self->gui_height;
    return true;
}

static bool gui_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints) {
    opendaw::window_hints(basics_of(plugin)->effect->width, hints);
    return true;
}

static bool gui_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    opendaw::window_fit(basics_of(plugin)->effect->width, GUI_MIN_ZOOM, GUI_MAX_ZOOM, width, height);
    return true;
}

static bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height) {
    Basics *self = basics_of(plugin);
    gui_adjust_size(plugin, &width, &height);
    self->gui_width = width;
    self->gui_height = height;
    return true;
}

static const clap_plugin_gui_t gui = {
    opendaw::gui_is_api_supported, opendaw::gui_get_preferred_api, opendaw::gui_create, opendaw::gui_destroy,
    opendaw::gui_set_scale, gui_get_size, opendaw::gui_can_resize, gui_get_resize_hints, gui_adjust_size, gui_set_size,
    opendaw::gui_set_parent, opendaw::gui_set_transient, opendaw::gui_suggest_title, opendaw::gui_show, opendaw::gui_hide
};

// device panel: the same page, the card at 1:1 for the panel's 176 px
static bool panel_get_width(const clap_plugin_t *plugin, uint32_t height, uint32_t *width) {
    *width = opendaw::panel_width_at(basics_of(plugin)->effect->width, height);
    return true;
}

static const opendaw_plugin_device_panel_t device_panel = {page_uri, panel_get_width};

static const void *basics_get_extension(const clap_plugin_t *plugin, const char *id) {
    (void) plugin;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {return &audio_ports;}
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {return &params;}
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {return &state;}
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) {return &latency;}
    if (std::strcmp(id, CLAP_EXT_TAIL) == 0) {return &tail;}
    if (std::strcmp(id, CLAP_EXT_WEBVIEW) == 0) {return &webview;}
    if (std::strcmp(id, CLAP_EXT_GUI) == 0) {return &gui;}
    if (std::strcmp(id, OPENDAW_EXT_DEVICE_PANEL) == 0) {return &device_panel;}
    return nullptr;
}

static void basics_on_main_thread(const clap_plugin_t *plugin) {(void) plugin;}

// --- factory --------------------------------------------------------------------------------------------------------

static const clap_plugin_t *create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id) {
    (void) factory;
    const Effect *effect = nullptr;
    for (const Effect &candidate : EFFECTS) {
        if (std::strcmp(plugin_id, candidate.descriptor.id) == 0) {effect = &candidate;}
    }
    if (effect == nullptr) {return nullptr;}
    void *memory = std::calloc(1, sizeof(Basics));
    if (memory == nullptr) {return nullptr;}
    Basics *self = new(memory) Basics();
    self->host = host;
    self->effect = effect;
    for (uint32_t index = 0; index < effect->param_count; index++) {
        self->values[index] = to_host(effect->params[index], effect->params[index].value);
        self->effect_values[index] = effect->params[index].value;
    }
    self->gui_width = static_cast<uint32_t>(std::lround(effect->width * GUI_ZOOM));
    self->gui_height = static_cast<uint32_t>(std::lround(OPENDAW_PANEL_HEIGHT * GUI_ZOOM));
    self->plugin.desc = &effect->descriptor;
    self->plugin.plugin_data = self;
    self->plugin.init = basics_init;
    self->plugin.destroy = basics_destroy;
    self->plugin.activate = basics_activate;
    self->plugin.deactivate = basics_deactivate;
    self->plugin.start_processing = basics_start_processing;
    self->plugin.stop_processing = basics_stop_processing;
    self->plugin.reset = basics_reset;
    self->plugin.process = basics_process;
    self->plugin.get_extension = basics_get_extension;
    self->plugin.on_main_thread = basics_on_main_thread;
    return &self->plugin;
}

static uint32_t plugin_count(const clap_plugin_factory_t *factory) {(void) factory; return COUNT(EFFECTS);}

static const clap_plugin_descriptor_t *plugin_descriptor(const clap_plugin_factory_t *factory, uint32_t index) {
    (void) factory;
    return index < COUNT(EFFECTS) ? &EFFECTS[index].descriptor : nullptr;
}

static const clap_plugin_factory_t factory = {plugin_count, plugin_descriptor, create};

static bool entry_init(const char *path) {(void) path; return true;}

static void entry_deinit(void) {}

static const void *entry_get_factory(const char *id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory};
