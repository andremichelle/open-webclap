// The openDAW WebCLAP kit: a whole CLAP plugin from a parameter table and a DSP struct, built for openDAW's device
// panel and window (docs/building-for-opendaw.md). A plugin is ONE .cpp file that includes this header once:
//
//     #include "opendaw_kit.h"
//     enum : clap_id {DRIVE, MIX, PARAM_COUNT};                         // ids are 0, 1, 2, ... in table order
//     static constexpr opendaw::Param PARAMS[PARAM_COUNT] = {
//         opendaw::Param::knob(DRIVE, "Drive", 0.0, 24.0, 6.0, opendaw::Unit::Decibel),
//         opendaw::Param::knob(MIX, "Mix", 0.0, 100.0, 100.0, opendaw::Unit::Percent)};
//     static constexpr opendaw::Info INFO = {"studio.opendaw.commons.my-drive", "My Drive", "Me", "1.0.0",
//         "A warm drive.", opendaw::Kind::AudioEffect, CLAP_PLUGIN_FEATURE_DISTORTION, 136};
//     struct Dsp : opendaw::DspBase {
//         void activate(double sampleRate, uint32_t maxFrames, const double *params) {...} // allocate here only
//         void process(opendaw::Block &block) {...}                    // block.params[DRIVE] in dB, as the table says
//     };
//     OPENDAW_PLUGIN(Dsp, INFO, PARAMS)
//
// The kit does the rest: the descriptor and features, parameters in the units the page shows (openDAW maps CLAP
// values linearly), texts both ways, every event at its sample (process is called once per span between events), a
// versioned fixed-layout state (an edit changes 8 bytes), gestures as one undo step, the hovered parameter for
// openDAW's menu and value entry, the device panel (opendaw.device-panel/1 and its width feature), the window (the
// same page zoomed, 100 to 300 %), latency and tail, the page's messages, and for note effects a well-formed note
// stream (a key never held twice, every note on ended). Everything runs on the audio thread, no threads, no locks.
#pragma once

#include <clap/clap.h>
#include "opendaw_page.h"
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" void emscripten_notify_memory_growth(size_t index) {(void) index;}

namespace opendaw {

// --- what the plugin is ---------------------------------------------------------------------------------------------

enum class Kind {AudioEffect, Instrument, MidiEffect};

// id: studio.opendaw.commons.<slug> for opendaw-commons; feature: one CLAP category (CLAP_PLUGIN_FEATURE_DELAY,
// _SYNTHESIZER, _ARPEGGIATOR, ...) or nullptr; panelWidth: the device panel's width at 176 px, as the page measures it
struct Info {
    const char *id;
    const char *name;
    const char *vendor;
    const char *version;
    const char *description;
    Kind kind;
    const char *feature;
    uint32_t panelWidth;
};

// --- parameters -----------------------------------------------------------------------------------------------------
// The host value IS the value in its unit (dB, Hz, ms, %): openDAW's automation, value entry and MIDI learn use it
// as it is. Curves only shape the page's knob (Exponential for frequencies, Squared for times).

enum class Unit {None, Percent, Decibel, Hertz, Milliseconds, Seconds, Semitones, Cents};
enum class Curve {Linear, Exponential, Squared};
enum class ParamKind {Knob, Integer, Choice};

struct Param {
    clap_id id;
    const char *name;
    ParamKind kind;
    double min;
    double max;
    double value; // the default
    Unit unit;
    Curve curve;
    const char *const *labels; // a choice's steps, max - min + 1 of them

    // a continuous value, min < value < max; Exponential needs min > 0
    static constexpr Param knob(clap_id id, const char *name, double min, double max, double value,
                                Unit unit = Unit::None, Curve curve = Curve::Linear) {
        return {id, name, ParamKind::Knob, min, max, value, unit, curve, nullptr};
    }
    // a whole number from min to max (Semitones, a count)
    static constexpr Param integer(clap_id id, const char *name, int min, int max, int value, Unit unit = Unit::None) {
        return {id, name, ParamKind::Integer, double(min), double(max), double(value), unit, Curve::Linear, nullptr};
    }
    // one of count steps, shown as its label; value is the step index
    static constexpr Param choice(clap_id id, const char *name, const char *const *labels, int count, int value) {
        return {id, name, ParamKind::Choice, 0.0, double(count - 1), double(value), Unit::None, Curve::Linear, labels};
    }
};

static constexpr const char *OFF_ON[] = {"Off", "On"};

constexpr bool paramsValid(const Param *params, uint32_t count) {
    for (uint32_t index = 0; index < count; index++) {
        const Param &param = params[index];
        if (param.id != index) {return false;}
        if (!(param.min < param.max) || param.value < param.min || param.value > param.max) {return false;}
        if (param.curve == Curve::Exponential && param.min <= 0.0) {return false;}
        if (param.kind == ParamKind::Choice && param.labels == nullptr) {return false;}
        if (param.name == nullptr || param.name[0] == '\0') {return false;}
    }
    return count > 0;
}

// --- helpers for the DSP ------------------------------------------------------------------------------------------

inline float dbToGain(double db) {return static_cast<float>(std::pow(10.0, db / 20.0));}
inline double gainToDb(double gain) {return 20.0 * std::log10(std::fmax(gain, 1e-10));}
// key 60 is C3 in openDAW's note names, 440 Hz is key 69
inline double keyToHz(double key) {return 440.0 * std::pow(2.0, (key - 69.0) / 12.0);}
// WebAssembly cannot flush denormals: call this on every value fed back (filter states, delay feedback, tails)
inline float flush(float value) {return std::fabs(value) < 1e-15f ? 0.0f : value;}

// a value that glides to its target in about `ms`, one step per sample, so a knob never clicks
struct Smoothed {
    float value = 0.0f, target = 0.0f, coefficient = 1.0f;
    void prepare(double sampleRate, double ms, float initial) {
        coefficient = static_cast<float>(1.0 - std::exp(-1000.0 / (ms * sampleRate)));
        value = target = initial;
    }
    void set(float next) {target = next;}
    float next() {
        value += (target - value) * coefficient;
        if (std::fabs(target - value) < 1e-7f) {value = target;}
        return value;
    }
};

// the loudest sample since the page last asked (read and reset in meters())
struct Peak {
    float peak = 0.0f;
    void feed(float sample) {peak = std::fmax(peak, std::fabs(sample));}
    float take() {
        const float result = peak;
        peak = 0.0f;
        return result;
    }
};

// --- what process gets ------------------------------------------------------------------------------------------

struct Transport {
    bool playing = false;
    double tempo = 120.0;        // beats per minute
    double beats = 0.0;          // the song position at the block's first frame, in quarter notes
    double barStart = 0.0;       // the current bar's start, in quarter notes
    int numerator = 4, denominator = 4;
};

struct Note {
    int16_t key;     // 0..127
    int16_t channel; // 0..15
    double velocity; // 0..1
    int32_t id;      // the host's note id, -1 when it has none
};

class Notes;

struct Block {
    uint32_t frames;         // 1 to 128 in openDAW (any number elsewhere), count time in these, never in calls
    const float *input[2];   // audio effects: the input, both channels always set
    float *output[2];        // audio effects write it, instruments ADD to it (it starts at zero)
    const double *params;    // every parameter's value in its unit, by id, constant for this block
    Transport transport;     // at the block's first frame
    Notes &notes;            // note effects: notes out, at a frame of this block
    double sampleRate;
};

// Notes a note effect sends. The kit keeps the stream well formed: a key that sounds already is ended first and
// struck again, and its note off goes out with the last of its sources (counted per key and channel).
class Notes {
public:
    static constexpr uint32_t CAPACITY = 1024, OFF_RESERVE = 256;

    void on(int key, double velocity, int channel = 0, uint32_t frame = 0) {
        if (key < 0 || key > 127 || channel < 0 || channel > 15) {return;}
        uint16_t &count = sources_[channel][key];
        // room for the note off it may need, and for every off still owed
        if (count_ + 2 > CAPACITY - OFF_RESERVE) {return;}
        if (count > 0) {push(false, key, 0.0, channel, frame);}
        if (count < 65535) {count++;}
        push(true, key, velocity, channel, frame);
    }
    void off(int key, int channel = 0, uint32_t frame = 0) {
        if (key < 0 || key > 127 || channel < 0 || channel > 15) {return;}
        uint16_t &count = sources_[channel][key];
        if (count == 0) {return;}
        if (--count == 0) {push(false, key, 0.0, channel, frame);}
    }
    // ends every sounding note (a mode change that makes held notes meaningless)
    void releaseAll(uint32_t frame = 0) {
        for (int channel = 0; channel < 16; channel++) {
            for (int key = 0; key < 128; key++) {
                if (sources_[channel][key] == 0) {continue;}
                sources_[channel][key] = 0;
                push(false, key, 0.0, channel, frame);
            }
        }
    }
    bool sounding(int key, int channel = 0) const {return sources_[channel & 15][key & 127] > 0;}

    // kit side
    struct Event {
        uint32_t time;
        bool on;
        int16_t key, channel;
        double velocity;
        uint8_t midi[3];
    };
    void begin(uint32_t base) {base_ = base;}
    void forget() {std::memset(sources_, 0, sizeof(sources_));}
    void drain(const clap_output_events_t *out, uint32_t frames) {
        for (uint32_t index = 0; index < count_; index++) {
            const Event &event = events_[index];
            const uint32_t time = event.time < frames ? event.time : frames - 1;
            if (event.midi[0] != 0) {
                clap_event_midi_t midi = {};
                midi.header = {sizeof(midi), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
                midi.port_index = 0;
                std::memcpy(midi.data, event.midi, 3);
                out->try_push(out, &midi.header);
                continue;
            }
            clap_event_note_t note = {};
            note.header = {sizeof(note), time, CLAP_CORE_EVENT_SPACE_ID,
                           static_cast<uint16_t>(event.on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
            note.note_id = -1;
            note.port_index = 0;
            note.channel = event.channel;
            note.key = event.key;
            note.velocity = event.velocity;
            out->try_push(out, &note.header);
        }
        count_ = 0;
    }
    // other MIDI passing through, in time order with the notes
    void midi(const uint8_t data[3], uint32_t frame = 0) {
        if (count_ + 1 > CAPACITY - OFF_RESERVE) {return;}
        insert(Event{base_ + frame, false, 0, 0, 0.0, {data[0], data[1], data[2]}});
    }
    uint32_t pending() const {return count_;}
    const Event &at(uint32_t index) const {return events_[index];}
    void dropAll() {count_ = 0;}

private:
    void push(bool on, int key, double velocity, int channel, uint32_t frame) {
        insert(Event{base_ + frame, on, static_cast<int16_t>(key), static_cast<int16_t>(channel),
                     velocity < 0.0 ? 0.0 : velocity > 1.0 ? 1.0 : velocity, {0, 0, 0}});
    }
    void insert(const Event &event) {
        if (count_ >= CAPACITY) {return;}
        uint32_t index = count_++;
        while (index > 0 && events_[index - 1].time > event.time) {
            events_[index] = events_[index - 1];
            index--;
        }
        events_[index] = event;
    }
    uint16_t sources_[16][128] = {};
    Event events_[CAPACITY];
    uint32_t count_ = 0;
    uint32_t base_ = 0;
};

// The DSP's defaults; a plugin's Dsp derives from it and hides what it needs.
struct DspBase {
    // params: every parameter's value now, start smoothers there
    void activate(double sampleRate, uint32_t maxFrames, const double *params) {(void) sampleRate; (void) maxFrames; (void) params;}
    void deactivate() {}
    void reset() {}                                      // clear delay lines, voices, filter states
    void process(Block &block) {(void) block;}
    void noteOn(const Note &note, Notes &notes) {(void) note; (void) notes;}
    void noteOff(const Note &note, Notes &notes) {(void) note; (void) notes;}
    void midi(const uint8_t data[3], Notes &notes) {(void) data; (void) notes;} // other MIDI (instruments)
    // a parameter changed (from automation, the page or a state load), before the next block; read block.params
    // in process instead when that is simpler
    void changed(clap_id id, double value) {(void) id; (void) value;}
    uint32_t latency() const {return 0;}                // frames, call nothing when it changes, the kit notices
    uint32_t tail() const {return 0;}                   // frames the output rings on, UINT32_MAX for endless
    // readings for the page's display (peaks since the last call, a gain), at most `capacity` floats
    uint32_t meters(float *out, uint32_t capacity) {(void) out; (void) capacity; return 0;}
};

// --- texts ----------------------------------------------------------------------------------------------------------

inline void format(const Param &param, double value, char *text, uint32_t size) {
    if (size == 0) {return;}
    const double magnitude = std::fabs(value);
    const auto plain = [&](const char *unit) {
        const int decimals = magnitude < 10.0 ? 2 : magnitude < 100.0 ? 1 : 0;
        std::snprintf(text, size, "%.*f%s", decimals, value, unit);
    };
    if (param.kind == ParamKind::Choice) {
        const long step = std::lround(value - param.min);
        const long last = std::lround(param.max - param.min);
        std::snprintf(text, size, "%s", param.labels[step < 0 ? 0 : step > last ? last : step]);
        return;
    }
    if (param.kind == ParamKind::Integer) {
        const long whole = std::lround(value);
        switch (param.unit) {
            case Unit::Semitones: std::snprintf(text, size, whole == 0 ? "0 st" : "%+ld st", whole); return;
            case Unit::Cents: std::snprintf(text, size, whole == 0 ? "0 ct" : "%+ld ct", whole); return;
            case Unit::Percent: std::snprintf(text, size, "%ld%%", whole); return;
            case Unit::Decibel: std::snprintf(text, size, "%ld dB", whole); return;
            case Unit::Hertz: std::snprintf(text, size, "%ld Hz", whole); return;
            case Unit::Milliseconds: std::snprintf(text, size, "%ld ms", whole); return;
            case Unit::Seconds: std::snprintf(text, size, "%ld s", whole); return;
            default: std::snprintf(text, size, "%ld", whole); return;
        }
    }
    switch (param.unit) {
        case Unit::None: plain(""); return;
        case Unit::Percent:
            std::snprintf(text, size, "%.*f%%", param.max - param.min < 10.0 ? 1 : 0, value);
            return;
        case Unit::Decibel:
            if (param.min <= -60.0 && value <= param.min) {std::snprintf(text, size, "-inf dB"); return;}
            std::snprintf(text, size, "%.1f dB", value);
            return;
        case Unit::Hertz:
            if (magnitude < 1000.0) {std::snprintf(text, size, "%.*f Hz", magnitude < 100.0 ? 1 : 0, value);}
            else {std::snprintf(text, size, "%.*f kHz", magnitude < 10000.0 ? 2 : 1, value / 1000.0);}
            return;
        case Unit::Milliseconds:
            if (magnitude < 1000.0) {std::snprintf(text, size, "%.*f ms", magnitude < 10.0 ? 2 : magnitude < 100.0 ? 1 : 0, value);}
            else {std::snprintf(text, size, "%.2f s", value / 1000.0);}
            return;
        case Unit::Seconds:
            if (magnitude < 1.0) {std::snprintf(text, size, "%.0f ms", value * 1000.0);}
            else {std::snprintf(text, size, "%.2f s", value);}
            return;
        case Unit::Semitones: std::snprintf(text, size, "%+.2f st", value); return;
        case Unit::Cents: std::snprintf(text, size, "%+.0f ct", value); return;
    }
}

inline bool startsWith(const char *text, const char *word) {
    for (; *word != '\0'; text++, word++) {
        if (*text == '\0' || (*text | 32) != (*word | 32)) {return false;}
    }
    return true;
}

inline bool contains(const char *text, const char *word) {
    for (; *text != '\0'; text++) {if (startsWith(text, word)) {return true;}}
    return false;
}

inline double clampTo(const Param &param, double value) {
    if (!(value >= param.min)) {value = param.min;}
    if (value > param.max) {value = param.max;}
    return param.kind == ParamKind::Knob ? value : std::round(value);
}

// a number in the units the texts show ("12 dB", "4k", "1.5 s", "-inf", a choice's label)
inline bool parse(const Param &param, const char *text, double *value) {
    while (*text == ' ') {text++;}
    if (param.kind == ParamKind::Choice) {
        const long steps = std::lround(param.max - param.min);
        for (long step = 0; step <= steps; step++) {
            if (*text != '\0' && startsWith(param.labels[step], text)) {
                *value = param.min + double(step);
                return true;
            }
        }
    }
    if (contains(text, "inf") && param.unit == Unit::Decibel) {
        *value = param.min;
        return true;
    }
    char *end = nullptr;
    double number = std::strtod(text, &end);
    if (end == text || !std::isfinite(number)) {return false;}
    switch (param.unit) {
        case Unit::Hertz: if (contains(end, "k")) {number *= 1000.0;} break;
        case Unit::Milliseconds: if (contains(end, "s") && !contains(end, "ms")) {number *= 1000.0;} break;
        case Unit::Seconds: if (contains(end, "ms")) {number /= 1000.0;} break;
        default: break;
    }
    *value = clampTo(param, number);
    return true;
}

// --- the plugin -----------------------------------------------------------------------------------------------------

// the page's own messages beside opendaw_page.h's: describe (answered with JSON once), show the window
constexpr uint8_t PAGE_DESCRIBE = 8, PAGE_SHOW_WINDOW = 9;
constexpr uint8_t PLUGIN_DESCRIBE = 2;
constexpr uint32_t TEXT_BYTES = 16, MAX_METERS = 64;
constexpr double WINDOW_ZOOM = 2.0, WINDOW_MIN_ZOOM = 1.0, WINDOW_MAX_ZOOM = 3.0;

template<class D, const Info &I, const Param *P, uint32_t N>
struct Plugin {
    static_assert(N <= 256, "at most 256 parameters");

    clap_plugin_t plugin;
    const clap_host_t *host = nullptr;
    PageHost page;
    const clap_host_latency_t *hostLatency = nullptr;
    const clap_host_tail_t *hostTail = nullptr;
    const clap_host_gui_t *hostGui = nullptr;
    D dsp;
    Notes notes;
    double values[N];
    bool changed[N];
    bool anyChanged = false;
    bool active = false;
    double sampleRate = 48000.0;
    uint32_t maxFrames = 128;
    uint32_t latency = 0, tail = 0;
    uint32_t guiWidth = 0, guiHeight = 0;
    Transport transport;
    uint32_t transportFrame = 0;
    float *silence = nullptr;
    float *scratch[2] = {nullptr, nullptr};
    OutEvents<256> outEvents;
    uint8_t message[8 + N * 8 + N * TEXT_BYTES + MAX_METERS * 4];

    // --- descriptor ---

    static char *widthFeature() {
        static char feature[64];
        return feature;
    }
    static const char **features() {
        static const char *list[8];
        return list;
    }
    static const clap_plugin_descriptor_t *descriptor() {
        static clap_plugin_descriptor_t descriptor;
        static bool ready = false;
        if (!ready) {
            std::snprintf(widthFeature(), 64, "opendaw:device-panel-width:%u", static_cast<unsigned>(I.panelWidth));
            const char **list = features();
            uint32_t count = 0;
            list[count++] = I.kind == Kind::Instrument ? CLAP_PLUGIN_FEATURE_INSTRUMENT
                : I.kind == Kind::MidiEffect ? CLAP_PLUGIN_FEATURE_NOTE_EFFECT : CLAP_PLUGIN_FEATURE_AUDIO_EFFECT;
            if (I.feature != nullptr) {list[count++] = I.feature;}
            if (I.kind != Kind::MidiEffect) {list[count++] = CLAP_PLUGIN_FEATURE_STEREO;}
            list[count++] = OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL;
            list[count++] = widthFeature();
            list[count] = nullptr;
            descriptor = {CLAP_VERSION_INIT, I.id, I.name, I.vendor, "", "", "", I.version, I.description, list};
            ready = true;
        }
        return &descriptor;
    }

    static Plugin *of(const clap_plugin_t *plugin) {return static_cast<Plugin *>(plugin->plugin_data);}
    static bool known(clap_id id) {return id < N;}

    // --- values ---

    void setValue(clap_id id, double value) {
        if (!known(id) || !std::isfinite(value)) {return;}
        const double next = clampTo(P[id], value);
        if (next == values[id]) {return;}
        values[id] = next;
        changed[id] = true;
        anyChanged = true;
    }

    void applyChanges() {
        if (!anyChanged) {return;}
        anyChanged = false;
        for (uint32_t id = 0; id < N; id++) {
            if (!changed[id]) {continue;}
            changed[id] = false;
            dsp.changed(id, values[id]);
        }
    }

    void checkLatency() {
        if (!active) {return;}
        const uint32_t next = dsp.latency();
        if (next != latency) {
            latency = next;
            if (hostLatency != nullptr) {hostLatency->changed(host);}
        }
        const uint32_t nextTail = dsp.tail();
        if (nextTail != tail) {
            tail = nextTail;
            if (hostTail != nullptr) {hostTail->changed(host);}
        }
    }

    // --- lifecycle ---

    static bool init(const clap_plugin_t *plugin) {
        Plugin *self = of(plugin);
        self->page.init(self->host);
        self->hostLatency = static_cast<const clap_host_latency_t *>(self->host->get_extension(self->host, CLAP_EXT_LATENCY));
        self->hostTail = static_cast<const clap_host_tail_t *>(self->host->get_extension(self->host, CLAP_EXT_TAIL));
        self->hostGui = static_cast<const clap_host_gui_t *>(self->host->get_extension(self->host, CLAP_EXT_GUI));
        return true;
    }

    void freeBuffers() {
        std::free(silence);
        std::free(scratch[0]);
        std::free(scratch[1]);
        silence = scratch[0] = scratch[1] = nullptr;
    }

    static void destroy(const clap_plugin_t *plugin) {
        Plugin *self = of(plugin);
        if (self->active) {self->dsp.deactivate();}
        self->freeBuffers();
        self->~Plugin();
        std::free(self);
    }

    static bool activate(const clap_plugin_t *plugin, double sampleRate, uint32_t minFrames, uint32_t maxFrames) {
        (void) minFrames;
        Plugin *self = of(plugin);
        self->sampleRate = sampleRate;
        self->maxFrames = maxFrames > 0 ? maxFrames : 1;
        self->freeBuffers();
        self->silence = static_cast<float *>(std::calloc(self->maxFrames, sizeof(float)));
        self->scratch[0] = static_cast<float *>(std::calloc(self->maxFrames, sizeof(float)));
        self->scratch[1] = static_cast<float *>(std::calloc(self->maxFrames, sizeof(float)));
        if (self->silence == nullptr || self->scratch[0] == nullptr || self->scratch[1] == nullptr) {return false;}
        for (uint32_t id = 0; id < N; id++) {self->changed[id] = false;}
        self->anyChanged = false;
        self->dsp.activate(sampleRate, self->maxFrames, self->values);
        self->notes.forget();
        self->notes.dropAll();
        self->latency = self->dsp.latency();
        self->tail = self->dsp.tail();
        self->active = true;
        return true;
    }

    static void deactivate(const clap_plugin_t *plugin) {
        Plugin *self = of(plugin);
        if (!self->active) {return;}
        self->active = false;
        self->dsp.deactivate();
        self->notes.forget();
    }

    static bool startProcessing(const clap_plugin_t *plugin) {(void) plugin; return true;}
    static void stopProcessing(const clap_plugin_t *plugin) {of(plugin)->notes.forget();}

    static void reset(const clap_plugin_t *plugin) {
        Plugin *self = of(plugin);
        self->notes.forget();
        self->dsp.reset();
    }

    // --- process ---

    // the transport at `frame` of the current call
    void readTransport(const clap_event_transport_t *event, uint32_t frame) {
        if (event == nullptr) {return;}
        transportFrame = frame;
        transport.playing = (event->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
        if ((event->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0 && event->tempo > 0.0) {transport.tempo = event->tempo;}
        if ((event->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0) {
            transport.beats = double(event->song_pos_beats) / double(CLAP_BEATTIME_FACTOR);
            transport.barStart = double(event->bar_start) / double(CLAP_BEATTIME_FACTOR);
        }
        if ((event->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) != 0 && event->tsig_num > 0 && event->tsig_denom > 0) {
            transport.numerator = event->tsig_num;
            transport.denominator = event->tsig_denom;
        }
    }

    void render(const clap_process_t *process, uint32_t from, uint32_t to) {
        if (to <= from) {return;}
        while (from < to) {
            const uint32_t frames = to - from < maxFrames ? to - from : maxFrames;
            Block block{frames, {silence, silence}, {scratch[0], scratch[1]}, values, transport, notes, sampleRate};
            if (transport.playing && from > transportFrame) {
                block.transport.beats = transport.beats + double(from - transportFrame) * transport.tempo / (60.0 * sampleRate);
            }
            if (I.kind == Kind::AudioEffect && process->audio_inputs_count > 0) {
                const clap_audio_buffer_t &input = process->audio_inputs[0];
                if (input.channel_count > 0 && input.data32 != nullptr) {
                    block.input[0] = input.data32[0] + from;
                    block.input[1] = input.data32[input.channel_count > 1 ? 1 : 0] + from;
                }
            }
            float *outputs[2] = {nullptr, nullptr};
            if (I.kind != Kind::MidiEffect && process->audio_outputs_count > 0) {
                const clap_audio_buffer_t &output = process->audio_outputs[0];
                if (output.channel_count > 0 && output.data32 != nullptr) {
                    outputs[0] = output.data32[0] + from;
                    outputs[1] = output.channel_count > 1 ? output.data32[1] + from : nullptr;
                }
            }
            // effects write into scratch first: the host may hand the same buffer in and out
            std::memset(scratch[0], 0, frames * sizeof(float));
            std::memset(scratch[1], 0, frames * sizeof(float));
            notes.begin(from);
            dsp.process(block);
            for (uint32_t channel = 0; channel < 2; channel++) {
                if (outputs[channel] == nullptr) {continue;}
                const float *source = scratch[channel];
                float *target = outputs[channel];
                for (uint32_t frame = 0; frame < frames; frame++) {
                    const float sample = source[frame];
                    target[frame] = std::isfinite(sample) ? (sample > 4.0f ? 4.0f : sample < -4.0f ? -4.0f : sample) : 0.0f;
                }
            }
            from += frames;
        }
    }

    void handle(const clap_event_header_t *header, uint32_t time) {
        if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) {return;}
        notes.begin(time);
        switch (header->type) {
            case CLAP_EVENT_PARAM_VALUE: {
                const clap_event_param_value_t *event = reinterpret_cast<const clap_event_param_value_t *>(header);
                setValue(event->param_id, event->value);
                applyChanges();
                return;
            }
            case CLAP_EVENT_TRANSPORT:
                readTransport(reinterpret_cast<const clap_event_transport_t *>(header), time);
                return;
            case CLAP_EVENT_NOTE_ON:
            case CLAP_EVENT_NOTE_OFF:
            case CLAP_EVENT_NOTE_CHOKE: {
                const clap_event_note_t *event = reinterpret_cast<const clap_event_note_t *>(header);
                if (event->key < 0 || event->key > 127) {return;}
                const Note note{event->key, static_cast<int16_t>(event->channel < 0 ? 0 : event->channel & 15),
                                event->velocity, event->note_id};
                if (header->type == CLAP_EVENT_NOTE_ON && event->velocity > 0.0) {dsp.noteOn(note, notes);}
                else {dsp.noteOff(note, notes);}
                return;
            }
            case CLAP_EVENT_MIDI: {
                const clap_event_midi_t *event = reinterpret_cast<const clap_event_midi_t *>(header);
                const uint8_t status = event->data[0] & 0xf0;
                const int16_t channel = event->data[0] & 0x0f;
                if (status == 0x90 && event->data[2] > 0) {
                    dsp.noteOn(Note{static_cast<int16_t>(event->data[1] & 127), channel, event->data[2] / 127.0, -1}, notes);
                } else if (status == 0x80 || status == 0x90) {
                    dsp.noteOff(Note{static_cast<int16_t>(event->data[1] & 127), channel, event->data[2] / 127.0, -1}, notes);
                } else if (I.kind == Kind::MidiEffect) {
                    notes.midi(event->data);
                } else {
                    dsp.midi(event->data, notes);
                }
                return;
            }
            default:
                return;
        }
    }

    static clap_process_status process(const clap_plugin_t *plugin, const clap_process_t *process) {
        Plugin *self = of(plugin);
        if (!self->active) {return CLAP_PROCESS_ERROR;}
        const uint32_t frames = process->frames_count;
        // values the page set since the last call
        self->applyChanges();
        self->readTransport(process->transport, 0);
        const uint32_t count = process->in_events != nullptr ? process->in_events->size(process->in_events) : 0;
        uint32_t position = 0;
        for (uint32_t index = 0; index < count; index++) {
            const clap_event_header_t *header = process->in_events->get(process->in_events, index);
            const uint32_t time = header->time < frames ? header->time : frames;
            if (time > position) {
                self->render(process, position, time);
                position = time;
            }
            self->handle(header, time);
        }
        self->render(process, position, frames);
        // CLAP wants the output sorted by time: the page's gestures and values (time 0) first, then the notes
        self->outEvents.drain(process->out_events);
        if (frames > 0) {self->notes.drain(process->out_events, frames);}
        self->checkLatency();
        return CLAP_PROCESS_CONTINUE;
    }

    // --- audio and note ports ---

    static uint32_t audioPortsCount(const clap_plugin_t *plugin, bool isInput) {
        (void) plugin;
        if (I.kind == Kind::MidiEffect) {return 0;}
        return isInput ? (I.kind == Kind::AudioEffect ? 1 : 0) : 1;
    }

    static bool audioPortsGet(const clap_plugin_t *plugin, uint32_t index, bool isInput, clap_audio_port_info_t *info) {
        if (index >= audioPortsCount(plugin, isInput)) {return false;}
        info->id = 0;
        std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Input" : "Output");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }

    static uint32_t notePortsCount(const clap_plugin_t *plugin, bool isInput) {
        (void) plugin;
        if (I.kind == Kind::MidiEffect) {return 1;}
        return I.kind == Kind::Instrument && isInput ? 1 : 0;
    }

    static bool notePortsGet(const clap_plugin_t *plugin, uint32_t index, bool isInput, clap_note_port_info_t *info) {
        if (index >= notePortsCount(plugin, isInput)) {return false;}
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Notes In" : "Notes Out");
        return true;
    }

    static uint32_t latencyGet(const clap_plugin_t *plugin) {
        const Plugin *self = of(plugin);
        return self->active ? self->dsp.latency() : 0;
    }

    static uint32_t tailGet(const clap_plugin_t *plugin) {
        const Plugin *self = of(plugin);
        return self->active ? self->dsp.tail() : 0;
    }

    // --- parameters ---

    static uint32_t paramsCount(const clap_plugin_t *plugin) {(void) plugin; return N;}

    static bool paramsGetInfo(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info) {
        (void) plugin;
        if (index >= N) {return false;}
        const Param &param = P[index];
        std::memset(info, 0, sizeof(*info));
        info->id = param.id;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        if (param.kind != ParamKind::Knob) {info->flags |= CLAP_PARAM_IS_STEPPED;}
        if (param.kind == ParamKind::Choice) {info->flags |= CLAP_PARAM_IS_ENUM;}
        info->min_value = param.min;
        info->max_value = param.max;
        info->default_value = param.value;
        std::snprintf(info->name, sizeof(info->name), "%s", param.name);
        return true;
    }

    static bool paramsGetValue(const clap_plugin_t *plugin, clap_id id, double *value) {
        if (!known(id)) {return false;}
        *value = of(plugin)->values[id];
        return true;
    }

    static bool paramsValueToText(const clap_plugin_t *plugin, clap_id id, double value, char *text, uint32_t size) {
        (void) plugin;
        if (!known(id) || text == nullptr || size == 0) {return false;}
        format(P[id], value, text, size);
        return true;
    }

    static bool paramsTextToValue(const clap_plugin_t *plugin, clap_id id, const char *text, double *value) {
        (void) plugin;
        if (!known(id) || text == nullptr) {return false;}
        return parse(P[id], text, value);
    }

    static void paramsFlush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out) {
        Plugin *self = of(plugin);
        const uint32_t count = in != nullptr ? in->size(in) : 0;
        for (uint32_t index = 0; index < count; index++) {
            const clap_event_header_t *header = in->get(in, index);
            if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_PARAM_VALUE) {
                const clap_event_param_value_t *event = reinterpret_cast<const clap_event_param_value_t *>(header);
                self->setValue(event->param_id, event->value);
            }
        }
        if (self->active) {self->applyChanges();}
        self->outEvents.drain(out);
        self->checkLatency();
    }

    // --- state: "ODKS", the kit's format version (1), the count, then (id u32, reserved u32, value f64) each ---

    static constexpr uint32_t STATE_VERSION = 1;

    static bool writeAll(const clap_ostream_t *stream, const uint8_t *bytes, uint32_t size) {
        while (size > 0) {
            const int64_t written = stream->write(stream, bytes, size);
            if (written <= 0) {return false;}
            bytes += written;
            size -= static_cast<uint32_t>(written);
        }
        return true;
    }

    static bool stateSave(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
        const Plugin *self = of(plugin);
        uint8_t bytes[12 + N * 16];
        std::memcpy(bytes, "ODKS", 4);
        const uint32_t header[2] = {STATE_VERSION, N};
        std::memcpy(bytes + 4, header, 8);
        for (uint32_t id = 0; id < N; id++) {
            const uint32_t entry[2] = {id, 0};
            std::memcpy(bytes + 12 + id * 16, entry, 8);
            std::memcpy(bytes + 20 + id * 16, &self->values[id], 8);
        }
        return writeAll(stream, bytes, sizeof(bytes));
    }

    static bool stateLoad(const clap_plugin_t *plugin, const clap_istream_t *stream) {
        Plugin *self = of(plugin);
        uint8_t bytes[12 + 1024 * 16];
        uint32_t size = 0;
        while (size < sizeof(bytes)) {
            const int64_t read = stream->read(stream, bytes + size, sizeof(bytes) - size);
            if (read <= 0) {break;}
            size += static_cast<uint32_t>(read);
        }
        if (size < 12 || std::memcmp(bytes, "ODKS", 4) != 0) {return false;}
        uint32_t header[2];
        std::memcpy(header, bytes + 4, 8);
        if (header[0] < 1) {return false;}
        const uint32_t count = header[1];
        if (count > (size - 12) / 16) {return false;}
        // a stored id this build does not know is skipped, a parameter the state does not hold keeps its default
        for (uint32_t index = 0; index < count; index++) {
            uint32_t id;
            double value;
            std::memcpy(&id, bytes + 12 + index * 16, 4);
            std::memcpy(&value, bytes + 20 + index * 16, 8);
            self->setValue(id, value);
        }
        if (self->active) {
            self->applyChanges();
            self->checkLatency();
        }
        return true;
    }

    // --- the page ---

    static int32_t pageUri(const char *uri, char *out, uint32_t capacity) {
        const int length = static_cast<int>(std::strlen(uri));
        if (out != nullptr && capacity > 0) {std::snprintf(out, capacity, "%s", uri);}
        return length + 1;
    }
    static int32_t windowUri(const clap_plugin_t *plugin, char *uri, uint32_t capacity) {
        (void) plugin;
        return pageUri("/index.html", uri, capacity);
    }
    static int32_t panelUri(const clap_plugin_t *plugin, char *uri, uint32_t capacity) {
        (void) plugin;
        return pageUri("/index.html?panel", uri, capacity);
    }

    static bool webviewGetResource(const clap_plugin_t *plugin, const char *path, char *mime, uint32_t capacity,
                                   const clap_ostream_t *stream) {
        (void) plugin; (void) path; (void) mime; (void) capacity; (void) stream;
        return false;
    }

    // STATUS: type 1, 0, the parameter count (u16), the meter count (u16), 0 (u16), every value (f64), every value's
    // text (16 bytes, zero padded), then the meters (f32)
    void sendStatus() {
        float meters[MAX_METERS];
        uint32_t meterCount = active ? dsp.meters(meters, MAX_METERS) : 0;
        if (meterCount > MAX_METERS) {meterCount = MAX_METERS;}
        message[0] = PLUGIN_STATUS;
        message[1] = 0;
        const uint16_t counts[3] = {static_cast<uint16_t>(N), static_cast<uint16_t>(meterCount), 0};
        std::memcpy(message + 2, counts, 6);
        std::memcpy(message + 8, values, N * 8);
        uint8_t *texts = message + 8 + N * 8;
        std::memset(texts, 0, N * TEXT_BYTES);
        for (uint32_t id = 0; id < N; id++) {
            char text[64];
            format(P[id], values[id], text, sizeof(text));
            std::memcpy(texts + id * TEXT_BYTES, text, std::strlen(text) < TEXT_BYTES - 1 ? std::strlen(text) : TEXT_BYTES - 1);
        }
        std::memcpy(texts + N * TEXT_BYTES, meters, meterCount * 4);
        page.send(message, 8 + N * 8 + N * TEXT_BYTES + meterCount * 4);
    }

    static void appendJson(char *out, uint32_t capacity, uint32_t &length, const char *text) {
        for (; *text != '\0' && length + 7 < capacity; text++) {
            const char letter = *text;
            if (letter == '"' || letter == '\\') {out[length++] = '\\'; out[length++] = letter;}
            else if (static_cast<unsigned char>(letter) < 0x20) {length += std::snprintf(out + length, capacity - length, "\\u%04x", letter);}
            else {out[length++] = letter;}
        }
        out[length] = '\0';
    }

    static void appendf(char *out, uint32_t capacity, uint32_t &length, const char *format, ...) {
        if (length >= capacity) {return;}
        va_list arguments;
        va_start(arguments, format);
        const int written = std::vsnprintf(out + length, capacity - length, format, arguments);
        va_end(arguments);
        if (written > 0) {length = length + static_cast<uint32_t>(written) < capacity ? length + written : capacity - 1;}
    }

    static const char *unitName(Unit unit) {
        switch (unit) {
            case Unit::Percent: return "%";
            case Unit::Decibel: return "dB";
            case Unit::Hertz: return "Hz";
            case Unit::Milliseconds: return "ms";
            case Unit::Seconds: return "s";
            case Unit::Semitones: return "st";
            case Unit::Cents: return "ct";
            default: return "";
        }
    }

    // the widest text a parameter shows, in characters, so the page reserves it and a value never moves the layout
    static uint32_t widestText(const Param &param) {
        char text[64];
        uint32_t widest = 0;
        const uint32_t steps = param.kind == ParamKind::Knob ? 200 : static_cast<uint32_t>(std::lround(param.max - param.min));
        for (uint32_t step = 0; step <= steps; step++) {
            const double value = param.min + (param.max - param.min) * double(step) / double(steps == 0 ? 1 : steps);
            format(param, value, text, sizeof(text));
            const uint32_t length = static_cast<uint32_t>(std::strlen(text));
            if (length > widest) {widest = length;}
        }
        return widest;
    }

    // DESCRIBE: type 2, then JSON {"name","kind","width","params":[{"id","name","kind","min","max","value","unit",
    // "curve","chars","labels"}]}; sent once when the page loads
    void sendDescription() {
        static char json[16384 + N * 2048];
        uint32_t length = 0;
        const uint32_t capacity = sizeof(json);
        json[length++] = static_cast<char>(PLUGIN_DESCRIBE);
        appendf(json, capacity, length, "{\"name\":\"");
        appendJson(json, capacity, length, I.name);
        appendf(json, capacity, length, "\",\"kind\":\"%s\",\"width\":%u,\"params\":[",
                I.kind == Kind::Instrument ? "instrument" : I.kind == Kind::MidiEffect ? "midi-effect" : "audio-effect",
                static_cast<unsigned>(I.panelWidth));
        for (uint32_t id = 0; id < N; id++) {
            const Param &param = P[id];
            appendf(json, capacity, length, "%s{\"id\":%u,\"name\":\"", id == 0 ? "" : ",", static_cast<unsigned>(id));
            appendJson(json, capacity, length, param.name);
            appendf(json, capacity, length, "\",\"kind\":\"%s\",\"min\":%.17g,\"max\":%.17g,\"value\":%.17g,\"unit\":\"%s\",\"curve\":\"%s\",\"chars\":%u",
                    param.kind == ParamKind::Knob ? "knob" : param.kind == ParamKind::Integer ? "integer" : "choice",
                    param.min, param.max, param.value, unitName(param.unit),
                    param.curve == Curve::Exponential ? "exponential" : param.curve == Curve::Squared ? "squared" : "linear",
                    static_cast<unsigned>(widestText(param)));
            if (param.kind == ParamKind::Choice) {
                appendf(json, capacity, length, ",\"labels\":[");
                const long steps = std::lround(param.max - param.min);
                for (long step = 0; step <= steps; step++) {
                    appendf(json, capacity, length, "%s\"", step == 0 ? "" : ",");
                    appendJson(json, capacity, length, param.labels[step]);
                    appendf(json, capacity, length, "\"");
                }
                appendf(json, capacity, length, "]");
            }
            appendf(json, capacity, length, "}");
        }
        appendf(json, capacity, length, "]}");
        page.send(json, length);
    }

    // the page's messages (opendaw_page.h): its values and gestures, the hovered parameter, polls answered by the status
    struct PageOf {
        Plugin *self;
        bool has(clap_id id) const {return known(id);}
        void set(clap_id id, double value) {self->setValue(id, value);}
        double value(clap_id id) const {return self->values[id];}
        void status() {self->sendStatus();}
    };

    static bool webviewReceive(const clap_plugin_t *plugin, const void *buffer, uint32_t size) {
        Plugin *self = of(plugin);
        const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
        if (size >= 1 && bytes[0] == PAGE_DESCRIBE) {
            self->sendDescription();
            self->sendStatus();
            return true;
        }
        if (size >= 1 && bytes[0] == PAGE_SHOW_WINDOW) {
            if (self->hostGui != nullptr) {self->hostGui->request_show(self->host);}
            return true;
        }
        PageOf page{self};
        return receive(page, self->page, self->outEvents, buffer, size);
    }

    // --- gui: the window shows the panel's page zoomed, 200 % to start, 100 to 300 % ---

    static bool guiGetSize(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
        const Plugin *self = of(plugin);
        *width = self->guiWidth;
        *height = self->guiHeight;
        return true;
    }
    static bool guiGetResizeHints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints) {
        (void) plugin;
        window_hints(I.panelWidth, hints);
        return true;
    }
    static bool guiAdjustSize(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
        (void) plugin;
        window_fit(I.panelWidth, WINDOW_MIN_ZOOM, WINDOW_MAX_ZOOM, width, height);
        return true;
    }
    static bool guiSetSize(const clap_plugin_t *plugin, uint32_t width, uint32_t height) {
        Plugin *self = of(plugin);
        guiAdjustSize(plugin, &width, &height);
        self->guiWidth = width;
        self->guiHeight = height;
        return true;
    }

    static bool panelGetWidth(const clap_plugin_t *plugin, uint32_t height, uint32_t *width) {
        (void) plugin;
        *width = panel_width_at(I.panelWidth, height);
        return true;
    }

    static const void *getExtension(const clap_plugin_t *plugin, const char *id) {
        (void) plugin;
        static const clap_plugin_audio_ports_t audioPorts = {audioPortsCount, audioPortsGet};
        static const clap_plugin_note_ports_t notePorts = {notePortsCount, notePortsGet};
        static const clap_plugin_params_t params = {
            paramsCount, paramsGetInfo, paramsGetValue, paramsValueToText, paramsTextToValue, paramsFlush};
        static const clap_plugin_state_t state = {stateSave, stateLoad};
        static const clap_plugin_latency_t latencyExtension = {latencyGet};
        static const clap_plugin_tail_t tailExtension = {tailGet};
        static const clap_plugin_webview_t webview = {windowUri, webviewGetResource, webviewReceive};
        static const clap_plugin_gui_t gui = {
            gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy, gui_set_scale, guiGetSize,
            gui_can_resize, guiGetResizeHints, guiAdjustSize, guiSetSize, gui_set_parent, gui_set_transient,
            gui_suggest_title, gui_show, gui_hide};
        static const opendaw_plugin_device_panel_t devicePanel = {panelUri, panelGetWidth};
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {return &audioPorts;}
        if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) {return &notePorts;}
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {return &params;}
        if (std::strcmp(id, CLAP_EXT_STATE) == 0) {return &state;}
        if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) {return &latencyExtension;}
        if (std::strcmp(id, CLAP_EXT_TAIL) == 0 && I.kind == Kind::AudioEffect) {return &tailExtension;}
        if (std::strcmp(id, CLAP_EXT_WEBVIEW) == 0) {return &webview;}
        if (std::strcmp(id, CLAP_EXT_GUI) == 0) {return &gui;}
        if (std::strcmp(id, OPENDAW_EXT_DEVICE_PANEL) == 0) {return &devicePanel;}
        return nullptr;
    }

    static void onMainThread(const clap_plugin_t *plugin) {(void) plugin;}

    // --- factory ---

    static const clap_plugin_t *create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *pluginId) {
        (void) factory;
        if (pluginId == nullptr || std::strcmp(pluginId, I.id) != 0) {return nullptr;}
        void *memory = std::calloc(1, sizeof(Plugin));
        if (memory == nullptr) {return nullptr;}
        Plugin *self = new(memory) Plugin();
        self->host = host;
        for (uint32_t id = 0; id < N; id++) {
            self->values[id] = P[id].value;
            self->changed[id] = false;
        }
        self->guiWidth = static_cast<uint32_t>(std::lround(I.panelWidth * WINDOW_ZOOM));
        self->guiHeight = static_cast<uint32_t>(std::lround(OPENDAW_PANEL_HEIGHT * WINDOW_ZOOM));
        self->plugin.desc = descriptor();
        self->plugin.plugin_data = self;
        self->plugin.init = init;
        self->plugin.destroy = destroy;
        self->plugin.activate = activate;
        self->plugin.deactivate = deactivate;
        self->plugin.start_processing = startProcessing;
        self->plugin.stop_processing = stopProcessing;
        self->plugin.reset = reset;
        self->plugin.process = process;
        self->plugin.get_extension = getExtension;
        self->plugin.on_main_thread = onMainThread;
        return &self->plugin;
    }

    static uint32_t pluginCount(const clap_plugin_factory_t *factory) {(void) factory; return 1;}
    static const clap_plugin_descriptor_t *pluginDescriptor(const clap_plugin_factory_t *factory, uint32_t index) {
        (void) factory;
        return index == 0 ? descriptor() : nullptr;
    }
    static const clap_plugin_factory_t *factory() {
        static const clap_plugin_factory_t instance = {pluginCount, pluginDescriptor, create};
        return &instance;
    }
    static bool entryInit(const char *path) {(void) path; return true;}
    static void entryDeinit() {}
    static const void *entryGetFactory(const char *id) {
        return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? factory() : nullptr;
    }
};

} // namespace opendaw

// The plugin's entry: the Dsp struct, the Info and the parameter table, all at namespace scope.
#define OPENDAW_PLUGIN(DSP, INFO, PARAMS)                                                                          \
    static_assert(opendaw::paramsValid(PARAMS, sizeof(PARAMS) / sizeof(PARAMS[0])),                              \
        "parameters: ids 0, 1, 2... in table order, min < max, the default inside, Exponential needs min > 0");     \
    using OpenDAWPlugin = opendaw::Plugin<DSP, INFO, PARAMS, sizeof(PARAMS) / sizeof(PARAMS[0])>;                  \
    extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {                                                \
        CLAP_VERSION_INIT, OpenDAWPlugin::entryInit, OpenDAWPlugin::entryDeinit, OpenDAWPlugin::entryGetFactory};
