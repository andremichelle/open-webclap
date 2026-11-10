// OpenRB: the TB-303 and the four effects of OpenRB-338 (pingw33n/openrb-338, GPL-3.0-only), the reimplementation of
// ReBirth RB-338 2.01's engine, as five CLAP plugins in one module: the 303 an instrument, Distortion, PCF, Compressor
// and Delay audio effects. The engine (external/openrb-338/crates/orb-engine) is Rust, linked unchanged as a static
// library behind a small C API (engine/src/lib.rs). It runs at its own rate (44.1 kHz) in its own blocks (64 frames),
// as in the original: every control value, every note and every pattern step lands on a block. The host's audio is
// resampled to that rate and back.
//
// The 303 plays its own 32 patterns on the host's transport (Play: Pattern, the default) or notes (Play: Notes:
// velocity above 100 accents, a legato note slides), as the original's pattern mode: a new pattern starts at the next bar line, shuffle
// delays every second 16th. The PCF's patterns follow the host's beat position while it plays and run on at its
// tempo while it stops; the delay's length follows the tempo.
//
// Every device is its page in openDAW's device panel (feature "opendaw:device-panel", extension
// "opendaw.device-panel/1"), the 303's pattern editor included; there is no window. The shared messages and gestures
// come from modules/opendaw-clap/opendaw_page.h, the panel's controls from modules/opendaw-page.

#include <clap/clap.h>
#include "opendaw_page.h"
#include "resampler.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" void emscripten_notify_memory_growth(size_t index) {(void) index;}
// Rust's std asks for the working directory (a panic's message); a WebCLAP host has none and offers no such import
extern "C" int __syscall_getcwd(intptr_t buffer, size_t size) {(void) buffer; (void) size; return -52;}

// --- the engine (engine/src/lib.rs) --------------------------------------------------------------------------------

extern "C" {
struct Bass;
struct Dist;
struct Filter;
struct Comp;
struct Echo;
void orb_init();
float orb_level(uint8_t value);
Bass *orb_bass_new();
void orb_bass_free(Bass *bass);
void orb_bass_set(Bass *bass, uint8_t ctrl, uint8_t value);
void orb_bass_set_vintage(Bass *bass, bool vintage);
void orb_bass_note_on(Bass *bass, int32_t semis, bool accent, bool glide);
void orb_bass_note_off(Bass *bass);
void orb_bass_start(Bass *bass, int32_t tick);
void orb_bass_stop(Bass *bass);
void orb_bass_step(Bass *bass, int32_t tick, uint8_t shuffle, uint32_t tempo);
void orb_bass_process(Bass *bass, float *out);
void orb_bass_reset(Bass *bass);
uint8_t *orb_bass_patterns(Bass *bass);
void orb_bass_position(const Bass *bass, uint8_t *pattern, uint8_t *step);
bool orb_bass_gate(const Bass *bass);
Dist *orb_dist_new();
void orb_dist_free(Dist *dist);
void orb_dist_set(Dist *dist, uint8_t ctrl, uint8_t value);
void orb_dist_process(Dist *dist, float *bus);
uint8_t orb_dist_meter(const Dist *dist);
void orb_dist_transfer(uint8_t amount, uint8_t shape, uint32_t n, float *out);
Filter *orb_pcf_new();
void orb_pcf_free(Filter *pcf);
void orb_pcf_set(Filter *pcf, uint8_t ctrl, uint8_t value);
void orb_pcf_process(Filter *pcf, float *bus, int32_t tick);
void orb_pcf_status(const Filter *pcf, float *env, int32_t *step);
void orb_pcf_wave(uint8_t wave, uint16_t *length, uint16_t *resolution, uint8_t *steps);
Comp *orb_comp_new();
void orb_comp_free(Comp *comp);
void orb_comp_set(Comp *comp, uint8_t ctrl, uint8_t value);
void orb_comp_process(Comp *comp, float *l, float *r);
void orb_comp_meters(const Comp *comp, uint8_t *gain, uint8_t *peak);
Echo *orb_delay_new();
void orb_delay_free(Echo *delay);
void orb_delay_set(Echo *delay, uint8_t ctrl, uint8_t value);
void orb_delay_process(Echo *delay, const float *send, float *l, float *r, uint32_t tempo);
uint32_t orb_delay_length(uint32_t tempo, uint8_t steps, uint8_t mode);
void orb_delay_clear(Echo *delay);
uint8_t orb_delay_meter(const Echo *delay);
struct Kit;
struct Drums;
Kit *orb_kit_new();
void orb_kit_free(Kit *kit);
bool orb_kit_load_dll(Kit *kit, const uint8_t *bytes, uint32_t size);
void orb_kit_set(Kit *kit, uint16_t id, const int16_t *audio, uint32_t frames);
const int16_t *orb_kit_get(const Kit *kit, uint16_t id, uint32_t *frames);
Drums *orb_drums_new(uint8_t machine, const Kit *kit);
void orb_drums_free(Drums *drums);
void orb_drums_set_kit(Drums *drums, const Kit *kit);
void orb_drums_set(Drums *drums, uint8_t ctrl, uint8_t value);
void orb_drums_hit(Drums *drums, uint8_t column, bool accent, bool strong);
void orb_drums_start(Drums *drums, int32_t tick);
void orb_drums_stop(Drums *drums);
void orb_drums_step(Drums *drums, int32_t tick, uint8_t shuffle, uint32_t tempo);
void orb_drums_process(Drums *drums, float *out);
void orb_drums_reset(Drums *drums);
uint8_t *orb_drums_patterns(Drums *drums);
void orb_drums_position(const Drums *drums, uint8_t *pattern, uint8_t *step);
}

#define ENGINE_RATE 44100.0
#define BLOCK 64
#define TICKS_PER_BEAT 192 // 768 to a bar, 48 to a 16th
#define PATTERNS 32
#define PATTERN_BYTES 98 // the 303's: shuffle, length, 16 steps of note, slide, accent, up, down, rest
#define DRUM_PATTERN_BYTES 194 // the 808's and 909's: shuffle, length, 16 steps of 12 columns (accent, 11 instruments)
#define DRUM_COLUMNS 12
#define MAX_PATTERN_BYTES DRUM_PATTERN_BYTES
#define STEPS 16
#define FIFO 1024
#define MAX_PARAMS 32
#define TRANSFER_POINTS 65
#define ACCENT_VELOCITY (100.5 / 127.0) // openDAW's default velocity is 100: above it accents
#define STATE_VERSION 1
#define FIRST_SAMPLE 1000 // the kit's slots are ReBirth.dll's resource ids, 1000..1045

// --- the plugins and their parameters ------------------------------------------------------------------------------

enum Kind : uint8_t {KIND_BASS = 0, KIND_DIST = 1, KIND_PCF = 2, KIND_COMP = 3, KIND_DELAY = 4, KIND_TR808 = 5, KIND_TR909 = 6,
                     KIND_COUNT = 7};

static bool is_drums(uint8_t kind) {return kind == KIND_TR808 || kind == KIND_TR909;}
static bool is_instrument(uint8_t kind) {return kind == KIND_BASS || is_drums(kind);}

enum Format : uint8_t {
    F_PERCENT,  // 0..127 as 0..100 %
    F_TUNE,     // 0..127 as the tune table's semitones
    F_CHOICE,   // labels
    F_PATTERN,  // A1..D8
    F_NUMBER,   // as it is
    F_PAN,      // L63..C..R63
    F_DB,       // whole decibels
    F_SHAPE     // the distortion's shape: Clip at 0, then the 32 curves (shape >> 2) as 1..32
};

struct Param {
    const char *name;
    double min, max, def;
    uint8_t ctrl;  // the device's controller, 255: the plugin's own
    Format format;
    const char *labels[4];
};

// the 303: the tune table (tb303.rs TUNE_TABLE), indexed by tune >> 2
static const int8_t tune_table[32] = {-12, -12, -11, -10, -9, -8, -7, -6, -5, -4, -3, -2, -1, 0, 0, 0,
                                      0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 12};

enum : clap_id {B_TUNE, B_CUTOFF, B_RESONANCE, B_ENVMOD, B_DECAY, B_ACCENT, B_WAVE, B_SOUND, B_PLAY, B_PATTERN,
                B_SHUFFLE, B_GAIN, B_COUNT};
static const Param bass_params[B_COUNT] = {
    {"Tune", 0, 127, 64, 2, F_TUNE, {}},
    {"Cutoff", 0, 127, 20, 3, F_PERCENT, {}},
    {"Resonance", 0, 127, 90, 4, F_PERCENT, {}},
    {"Env Mod", 0, 127, 20, 5, F_PERCENT, {}},
    {"Decay", 0, 127, 100, 6, F_PERCENT, {}},
    {"Accent", 0, 127, 64, 7, F_PERCENT, {}},
    {"Wave", 0, 1, 0, 8, F_CHOICE, {"Saw", "Square"}},
    {"Sound", 0, 1, 0, 255, F_CHOICE, {"2.0", "Vintage"}},
    {"Play", 0, 1, 1, 255, F_CHOICE, {"Notes", "Pattern"}},
    {"Pattern", 0, PATTERNS - 1, 0, 1, F_PATTERN, {}},
    {"Shuffle", 0, 127, 0, 255, F_PERCENT, {}},
    {"Gain", -24, 6, 0, 255, F_DB, {}},
};

enum : clap_id {D_AMOUNT, D_SHAPE, D_COUNT};
static const Param dist_params[D_COUNT] = {
    {"Amount", 0, 127, 70, 1, F_PERCENT, {}},
    {"Shape", 0, 127, 0, 2, F_SHAPE, {}},
};

enum : clap_id {P_FREQUENCY, P_RESONANCE, P_AMOUNT, P_WAVE, P_DECAY, P_MODE, P_COUNT};
static const Param pcf_params[P_COUNT] = {
    {"Frequency", 0, 127, 70, 1, F_PERCENT, {}},
    {"Resonance", 0, 127, 64, 2, F_PERCENT, {}},
    {"Amount", 0, 127, 40, 3, F_PERCENT, {}},
    {"Wave", 0, 54, 24, 4, F_NUMBER, {}},
    {"Decay", 0, 127, 90, 5, F_PERCENT, {}},
    {"Mode", 0, 1, 0, 6, F_CHOICE, {"Low Pass", "Band Pass"}},
};

enum : clap_id {C_AMOUNT, C_THRESHOLD, C_COUNT};
static const Param comp_params[C_COUNT] = {
    {"Amount", 0, 127, 70, 1, F_PERCENT, {}},
    {"Threshold", 0, 127, 64, 2, F_PERCENT, {}},
};

enum : clap_id {E_STEPS, E_GRID, E_FEEDBACK, E_PAN, E_SEND, E_COUNT};
static const Param delay_params[E_COUNT] = {
    {"Steps", 1, 32, 3, 1, F_NUMBER, {}},
    {"Grid", 0, 1, 0, 2, F_CHOICE, {"1/16", "1/8 T"}},
    {"Feedback", 0, 127, 80, 3, F_PERCENT, {}},
    {"Pan", 0, 127, 64, 4, F_PAN, {}},
    {"Send", 0, 127, 100, 255, F_PERCENT, {}},
};

// The 808: the controllers of the machine (tr808.rs, RBS order) after the plugin's own Play, Pattern, Shuffle and
// Gain. A tom's or the rim shot's and clap's switch picks the other sample of its pair (conga, claves, maracas).
enum : clap_id {R_PLAY, R_PATTERN, R_SHUFFLE, R_GAIN};
static const Param tr808_params[] = {
    {"Play", 0, 1, 1, 255, F_CHOICE, {"Notes", "Pattern"}},
    {"Pattern", 0, PATTERNS - 1, 0, 1, F_PATTERN, {}},
    {"Shuffle", 0, 127, 0, 255, F_PERCENT, {}},
    {"Gain", -24, 6, 0, 255, F_DB, {}},
    {"Accent", 0, 127, 40, 2, F_PERCENT, {}},
    {"BD Level", 0, 127, 100, 3, F_PERCENT, {}},
    {"BD Tone", 0, 127, 64, 4, F_PERCENT, {}},
    {"BD Decay", 0, 127, 64, 5, F_PERCENT, {}},
    {"SD Level", 0, 127, 100, 6, F_PERCENT, {}},
    {"SD Tone", 0, 127, 64, 7, F_PERCENT, {}},
    {"SD Snappy", 0, 127, 64, 8, F_PERCENT, {}},
    {"LT Level", 0, 127, 100, 9, F_PERCENT, {}},
    {"LT Tuning", 0, 127, 64, 10, F_PERCENT, {}},
    {"LT Select", 0, 1, 0, 11, F_CHOICE, {"Conga", "Tom"}},
    {"MT Level", 0, 127, 100, 12, F_PERCENT, {}},
    {"MT Tuning", 0, 127, 64, 13, F_PERCENT, {}},
    {"MT Select", 0, 1, 0, 14, F_CHOICE, {"Conga", "Tom"}},
    {"HT Level", 0, 127, 100, 15, F_PERCENT, {}},
    {"HT Tuning", 0, 127, 64, 16, F_PERCENT, {}},
    {"HT Select", 0, 1, 0, 17, F_CHOICE, {"Conga", "Tom"}},
    {"RS Level", 0, 127, 100, 18, F_PERCENT, {}},
    {"RS Select", 0, 1, 0, 19, F_CHOICE, {"Claves", "Rim Shot"}},
    {"CP Level", 0, 127, 100, 20, F_PERCENT, {}},
    {"CP Select", 0, 1, 0, 21, F_CHOICE, {"Maracas", "Clap"}},
    {"CB Level", 0, 127, 100, 22, F_PERCENT, {}},
    {"CY Level", 0, 127, 100, 23, F_PERCENT, {}},
    {"CY Tone", 0, 127, 0, 24, F_PERCENT, {}},
    {"CY Decay", 0, 127, 100, 25, F_PERCENT, {}},
    {"OH Level", 0, 127, 100, 26, F_PERCENT, {}},
    {"OH Decay", 0, 127, 100, 27, F_PERCENT, {}},
    {"CH Level", 0, 127, 100, 28, F_PERCENT, {}},
};

// The 909: its controllers (tr909.rs, RBS order: 20 rim shot, 21 clap, 22 hi-hat level) after the plugin's own.
static const Param tr909_params[] = {
    {"Play", 0, 1, 1, 255, F_CHOICE, {"Notes", "Pattern"}},
    {"Pattern", 0, PATTERNS - 1, 0, 1, F_PATTERN, {}},
    {"Shuffle", 0, 127, 0, 255, F_PERCENT, {}},
    {"Gain", -24, 6, 0, 255, F_DB, {}},
    {"Accent", 0, 127, 32, 2, F_PERCENT, {}},
    {"BD Level", 0, 127, 100, 3, F_PERCENT, {}},
    {"BD Tune", 0, 127, 64, 4, F_PERCENT, {}},
    {"BD Attack", 0, 127, 64, 5, F_PERCENT, {}},
    {"BD Decay", 0, 127, 127, 6, F_PERCENT, {}},
    {"SD Level", 0, 127, 100, 7, F_PERCENT, {}},
    {"SD Tune", 0, 127, 64, 8, F_PERCENT, {}},
    {"SD Tone", 0, 127, 64, 9, F_PERCENT, {}},
    {"SD Snappy", 0, 127, 64, 10, F_PERCENT, {}},
    {"LT Level", 0, 127, 100, 11, F_PERCENT, {}},
    {"LT Tune", 0, 127, 64, 12, F_PERCENT, {}},
    {"LT Decay", 0, 127, 127, 13, F_PERCENT, {}},
    {"MT Level", 0, 127, 100, 14, F_PERCENT, {}},
    {"MT Tune", 0, 127, 64, 15, F_PERCENT, {}},
    {"MT Decay", 0, 127, 127, 16, F_PERCENT, {}},
    {"HT Level", 0, 127, 100, 17, F_PERCENT, {}},
    {"HT Tune", 0, 127, 64, 18, F_PERCENT, {}},
    {"HT Decay", 0, 127, 127, 19, F_PERCENT, {}},
    {"RS Level", 0, 127, 100, 20, F_PERCENT, {}},
    {"CP Level", 0, 127, 100, 21, F_PERCENT, {}},
    {"HH Level", 0, 127, 100, 22, F_PERCENT, {}},
    {"CH Decay", 0, 127, 32, 23, F_PERCENT, {}},
    {"OH Decay", 0, 127, 127, 24, F_PERCENT, {}},
    {"Crash Level", 0, 127, 100, 25, F_PERCENT, {}},
    {"Crash Tune", 0, 127, 64, 26, F_PERCENT, {}},
    {"Ride Level", 0, 127, 100, 27, F_PERCENT, {}},
    {"Ride Tune", 0, 127, 64, 28, F_PERCENT, {}},
    {"Flam", 0, 127, 64, 29, F_PERCENT, {}},
};
#define TR808_COUNT (sizeof(tr808_params) / sizeof(tr808_params[0]))
#define TR909_COUNT (sizeof(tr909_params) / sizeof(tr909_params[0]))

struct KindInfo {
    const Param *params;
    uint32_t count;
    const char *page; // ?device=
    double panel_width;
    // the sequencing instruments' own parameters, -1 none
    int play, pattern, shuffle, gain;
};

// the panels' widths at 176 px, as page/index.html lays them out (its preview measures them, WIDTHS there)
#define BASS_PANEL_WIDTH 734
#define DIST_PANEL_WIDTH 230
#define PCF_PANEL_WIDTH 504
#define COMP_PANEL_WIDTH 214
#define DELAY_PANEL_WIDTH 412
#define TR808_PANEL_WIDTH 1042
#define TR909_PANEL_WIDTH 1282

static const KindInfo kinds[KIND_COUNT] = {
    {bass_params, B_COUNT, "bass", BASS_PANEL_WIDTH, B_PLAY, B_PATTERN, B_SHUFFLE, B_GAIN},
    {dist_params, D_COUNT, "distortion", DIST_PANEL_WIDTH, -1, -1, -1, -1},
    {pcf_params, P_COUNT, "pcf", PCF_PANEL_WIDTH, -1, -1, -1, -1},
    {comp_params, C_COUNT, "compressor", COMP_PANEL_WIDTH, -1, -1, -1, -1},
    {delay_params, E_COUNT, "delay", DELAY_PANEL_WIDTH, -1, -1, -1, -1},
    {tr808_params, TR808_COUNT, "tr808", TR808_PANEL_WIDTH, R_PLAY, R_PATTERN, R_SHUFFLE, R_GAIN},
    {tr909_params, TR909_COUNT, "tr909", TR909_PANEL_WIDTH, R_PLAY, R_PATTERN, R_SHUFFLE, R_GAIN},
};

// the drum machines' keys: General MIDI's drum map, to the pattern's columns (1 BD .. 11)
static uint8_t drum_column(uint8_t kind, int key) {
    switch (key) {
        case 35: case 36: return 1;           // bass drum
        case 38: case 40: return 2;           // snare
        case 41: case 43: return 3;           // low tom
        case 45: case 47: return 4;           // mid tom
        case 48: case 50: return 5;           // high tom
        case 37: return 6;                    // rim shot
        case 39: return 7;                    // clap
        case 42: case 44: return kind == KIND_TR808 ? 11 : 8; // closed hi-hat
        case 46: return kind == KIND_TR808 ? 10 : 9;          // open hi-hat
        case 49: case 57: return kind == KIND_TR808 ? 9 : 10; // cymbal / crash
        case 56: return kind == KIND_TR808 ? 8 : 0;           // cowbell (808)
        case 51: case 59: return kind == KIND_TR909 ? 11 : 0; // ride (909)
        default: return 0;
    }
}

// --- the page's messages -------------------------------------------------------------------------------------------
// The shared ones (poll, parameters, hover) are opendaw_page.h's. From the page: hello (asks for the snapshot),
// a step of a pattern, a pattern's header (length and shuffle), a whole pattern, commit (an edit is done:
// the state is dirty). To the page: the status (the poll's answer) and the snapshot (the status with the patterns).
//   status    u8 type, u8 kind, u8 flags (1 playing, 2 gate), u8 pattern playing, u8 step (255 none), u8 meter, u8
//             meter, u8 0, f64 values[count], then the kind's own: the distortion's transfer curve (65 f32 from -1 to
//             1), the PCF's envelope (f32, 0..511), step (i32), its wave's length and resolution (u16 each) and 32
//             step velocities, the delay's length in samples (u32)
//   snapshot  the status of a 303, 808 or 909, then its 32 patterns (98 or 194 bytes each); a drum machine's status
//             has the number of its kit's slots that hold a sample at byte 5
//   kit       from the page: u8 type, 3 bytes 0, then ReBirth.dll's bytes (PAGE_KIT_DLL); or u8 type, u8 0, u16 slot
//             id (1000..1045), u32 frames, then the 16-bit samples at 44.1 kHz (PAGE_KIT_SLOT, 0 frames clears it)

#define PAGE_HELLO 8
#define PAGE_STEP 10
#define PAGE_HEADER 11
#define PAGE_PATTERN 12
#define PAGE_COMMIT 13
#define PAGE_KIT_DLL 14
#define PAGE_KIT_SLOT 15
#define PLUGIN_STATUS 1
#define PLUGIN_SNAPSHOT 2
#define FLAG_PLAYING 1
#define FLAG_GATE 2

// --- the plugin ----------------------------------------------------------------------------------------------------

struct Plugin {
    clap_plugin_t plugin;
    const clap_host_t *host;
    const clap_host_latency_t *host_latency;
    opendaw::PageHost page;
    opendaw::OutEvents<128> out_events;
    Kind kind;
    const KindInfo *info;
    double values[MAX_PARAMS];
    int applied[MAX_PARAMS]; // the byte the device has, -1 none

    Bass *bass;
    Dist *dist[2];
    Filter *pcf[2];
    Comp *comp;
    Echo *delay;
    Drums *drums;
    Kit *kit;

    // the rates: the host's, the resamplers to the engine's and back, a block in, the output's queue
    double host_rate;
    bool active;
    openrb::Resampler down, up;
    float in[2][BLOCK], out[2][BLOCK];
    uint32_t fill;
    float fifo[2][FIFO];
    uint32_t fifo_read, fifo_write;
    uint32_t latency;

    // the transport, seen at the frame the engine runs a block
    bool transport_playing;
    double beat0, beats_per_frame, tempo; // at the process call's first frame
    bool was_playing;
    int32_t last_tick;
    double free_tick; // the PCF's ticks while the transport stops

    // the 303's notes: the keys held, the last on top
    uint8_t held[128];
    uint32_t held_count;
    int sounding; // the key that plays, -1 none
    bool pattern_mode;

    bool snapshot_due;
    float gain; // an instrument's output gain at the end of the last block
};

static Plugin *plugin_of(const clap_plugin_t *plugin) {return static_cast<Plugin *>(plugin->plugin_data);}

static double clamp_value(const Param &p, double value) {
    if (!(value == value)) {value = p.def;}
    value = value < p.min ? p.min : value > p.max ? p.max : value;
    return std::round(value);
}

// a parameter's value as the device's controller byte
static int byte_of(const Plugin *self, clap_id id) {
    const int v = static_cast<int>(self->values[id]);
    if (self->kind == KIND_PCF && id == P_MODE) {return v == 0 ? 1 : 0;} // the device: 1 low pass, else band pass
    if (self->kind == KIND_DELAY && id == E_GRID) {return v == 0 ? 1 : 0;} // the device: 0 triplets, else 16ths
    return v;
}

static void set_value(Plugin *self, clap_id id, double value) {
    if (id >= self->info->count) {return;}
    self->values[id] = clamp_value(self->info->params[id], value);
}

// the parameters into the devices, before a block
static void sync_params(Plugin *self) {
    for (clap_id id = 0; id < self->info->count; id++) {
        const int byte = byte_of(self, id);
        if (byte == self->applied[id]) {continue;}
        self->applied[id] = byte;
        const uint8_t ctrl = self->info->params[id].ctrl;
        switch (self->kind) {
            case KIND_BASS:
                if (id == B_SOUND) {orb_bass_set_vintage(self->bass, byte != 0);}
                else if (ctrl != 255) {orb_bass_set(self->bass, ctrl, static_cast<uint8_t>(byte));}
                break;
            case KIND_DIST:
                for (Dist *d : self->dist) {orb_dist_set(d, ctrl, static_cast<uint8_t>(byte));}
                break;
            case KIND_PCF:
                for (Filter *f : self->pcf) {orb_pcf_set(f, ctrl, static_cast<uint8_t>(byte));}
                break;
            case KIND_COMP:
                orb_comp_set(self->comp, ctrl, static_cast<uint8_t>(byte));
                break;
            case KIND_DELAY:
                if (ctrl != 255) {orb_delay_set(self->delay, ctrl, static_cast<uint8_t>(byte));}
                break;
            case KIND_TR808:
            case KIND_TR909:
                if (ctrl != 255) {orb_drums_set(self->drums, ctrl, static_cast<uint8_t>(byte));}
                break;
            default: break;
        }
    }
}

// the patterns of a 303, 808 or 909
static uint8_t *patterns_of(Plugin *self) {
    return self->kind == KIND_BASS ? orb_bass_patterns(self->bass) : orb_drums_patterns(self->drums);
}

static uint32_t pattern_bytes(const Plugin *self) {return self->kind == KIND_BASS ? PATTERN_BYTES : DRUM_PATTERN_BYTES;}

// A pattern's shuffle flag is always on: the Shuffle knob alone decides (at 0 nothing moves).
static void sanitize_pattern(uint8_t *pattern) {
    pattern[0] = 1;
    if (pattern[1] < 1 || pattern[1] > STEPS) {pattern[1] = STEPS;}
    for (int step = 0; step < STEPS; step++) {
        uint8_t *s = pattern + 2 + step * 6;
        if (s[0] > 12) {s[0] = 12;}
        for (int i = 1; i < 6; i++) {s[i] = s[i] != 0 ? 1 : 0;}
    }
}

// A drum pattern: the accent column 0 or 1; an 808's instruments 0 or 1, a 909's 0..4 (1 on, 2 strong, 3 and 4 the
// same with a flam).
static void sanitize_drum_pattern(uint8_t kind, uint8_t *pattern) {
    pattern[0] = 1;
    if (pattern[1] < 1 || pattern[1] > STEPS) {pattern[1] = STEPS;}
    const uint8_t top = kind == KIND_TR909 ? 4 : 1;
    for (int step = 0; step < STEPS; step++) {
        uint8_t *s = pattern + 2 + step * DRUM_COLUMNS;
        s[0] = s[0] != 0 ? 1 : 0;
        for (int i = 1; i < DRUM_COLUMNS; i++) {if (s[i] > top) {s[i] = top;}}
    }
}

static void sanitize_any(Plugin *self, uint8_t *pattern) {
    if (self->kind == KIND_BASS) {sanitize_pattern(pattern);} else {sanitize_drum_pattern(self->kind, pattern);}
}

// --- the factory patterns ------------------------------------------------------------------------------------------
// Bank A, written for this port. A step: a note name (C .. B, then c for the C above), "+" an octave up, "-" down,
// "a" accented, "s" sliding into the next step; "." a rest. "/n" ends it after n steps.

static const char *factory_patterns[] = {
    "C C+a . C D#s F C+ . C . A# Ga C . D# Fs",
    "C . C+a C . G-s A#- C Ca . C+ D# . C Fa C",
    "A- A-a A . A- E-s G- A- A-a . C A- G-s A- . Da",
    "C+a C+s G F D#a Ds C . C+a A#s G . Fa D# Cs G-",
    "/12 F#- F#-a . F# F#- C#s D# F#- . E-a F#- Bs",
    "C . D# . Ca . G-s A#- C C+as C . A#- . Ga Fs",
    "D-a D- D . D-a D- Cs D . D-a D- F . D-as A- C",
    "Ca C+s Cs C+ G-a G-s G . A#-a A#- A#-s C . D#a Ds C",
};

static int note_index(char letter) {
    switch (letter) {
        case 'C': return 0; case 'D': return 2; case 'E': return 4; case 'F': return 5;
        case 'G': return 7; case 'A': return 9; case 'B': return 11; case 'c': return 12;
        default: return -1;
    }
}

static void parse_pattern(const char *text, uint8_t *pattern) {
    std::memset(pattern, 0, PATTERN_BYTES);
    pattern[1] = STEPS;
    for (int step = 0; step < STEPS; step++) {pattern[2 + step * 6 + 5] = 1;}
    int step = 0;
    const char *at = text;
    while (*at != 0 && step < STEPS) {
        while (*at == ' ') {at++;}
        if (*at == 0) {break;}
        const char *end = at;
        while (*end != 0 && *end != ' ') {end++;}
        if (*at == '/') {
            pattern[1] = static_cast<uint8_t>(std::atoi(at + 1));
        } else {
            uint8_t *s = pattern + 2 + step * 6;
            if (*at != '.') {
                int note = note_index(*at);
                for (const char *c = at + 1; c < end; c++) {
                    if (*c == '#') {note++;}
                    else if (*c == '+') {s[3] = 1;}
                    else if (*c == '-') {s[4] = 1;}
                    else if (*c == 'a') {s[2] = 1;}
                    else if (*c == 's') {s[1] = 1;}
                }
                s[0] = static_cast<uint8_t>(note < 0 ? 0 : note > 12 ? 12 : note);
                s[5] = 0;
            }
            step++;
        }
        at = end;
    }
    sanitize_pattern(pattern);
}

// The drum machines' bank A, written for this port: a row per instrument, a character per step ("x" on, "o" the
// 909's strong hit, "f" a flam, "." off), the accent row first. Rows: AC BD SD LT MT HT RS CP, then the 808's CB CY OH
// CH or the 909's CH OH CC RC (the pattern's columns).
struct DrumPattern {
    const char *rows[DRUM_COLUMNS];
};

static const DrumPattern factory_808[] = {
    {{"x...x...x...x...", "x.....x...x.....", "....x.......x...", "", "", "", "", "", "", "", "", "x.x.x.x.x.x.x.x."}},
    {{"x.......x.......", "x..x..x...x.x...", "....x.......x..x", "", "", "", "..x....x..x.....", "", "", "", "", "xxxxxxxxxxxxxxxx"}},
    {{"....x.......x...", "x.........x.....", "....x.......x...", "", "", "", "", "............x...", "..x.....x.....x.", "x...............", "......x.......x.",
      "x.x.x...x.x.x..."}},
    {{"x...............", "x..x......xx....", "....x..x....x...", "......x...x.....", "....x....x......", "..x.....x.......", "", "", "", "", "", "x.xxx.xxx.xxx.xx"}},
};

static const DrumPattern factory_909[] = {
    {{"x...x...x...x...", "x...x...x...x...", "....x.......x...", "", "", "", "", "", "x.x.x.x.x.x.x.x.", "..x...x...x...x.", "", ""}},
    {{"....x.......x...", "x...x...x...x...", "....o.......o..x", "", "", "", "", "....x.......x...", "x.xxx.xxx.xxx.xx", "", "x...............", ""}},
    {{"x.......x.......", "x...x...x...x...", "....x.....x.f...", "", "", "", "", "", "", "..o...o...o...o.", "", "x.x.x.x.x.x.x.x."}},
    {{"x...x...x...x...", "x.....x...x.....", "....x.......x...", "..........x.....", "........x.....x.", "......x.....x...", "", "", "x.x.x.x.x.x.x.x.", "", "", ""}},
};

static void parse_drum_pattern(uint8_t kind, const DrumPattern &source, uint8_t *pattern) {
    std::memset(pattern, 0, DRUM_PATTERN_BYTES);
    pattern[1] = STEPS;
    for (int column = 0; column < DRUM_COLUMNS; column++) {
        const char *row = source.rows[column];
        for (int step = 0; step < STEPS && row != nullptr && row[step] != 0; step++) {
            const char c = row[step];
            uint8_t v = c == 'x' ? 1 : c == 'o' ? 2 : c == 'f' ? 3 : 0;
            if (kind == KIND_TR808 && v > 1) {v = 1;}
            pattern[2 + step * DRUM_COLUMNS + column] = v;
        }
    }
    sanitize_drum_pattern(kind, pattern);
}

static void load_factory_patterns(Plugin *self) {
    uint8_t *patterns = patterns_of(self);
    if (self->kind == KIND_BASS) {
        const int count = static_cast<int>(sizeof(factory_patterns) / sizeof(factory_patterns[0]));
        for (int p = 0; p < count; p++) {parse_pattern(factory_patterns[p], patterns + p * PATTERN_BYTES);}
        for (int p = 0; p < PATTERNS; p++) {sanitize_pattern(patterns + p * PATTERN_BYTES);}
        return;
    }
    const DrumPattern *bank = self->kind == KIND_TR808 ? factory_808 : factory_909;
    const int count = 4;
    for (int p = 0; p < PATTERNS; p++) {
        uint8_t *pattern = patterns + p * DRUM_PATTERN_BYTES;
        if (p < count) {parse_drum_pattern(self->kind, bank[p], pattern);} else {sanitize_drum_pattern(self->kind, pattern);}
    }
}

// --- the transport and the notes -----------------------------------------------------------------------------------

static double tempo_of(const Plugin *self) {return self->tempo > 0.0 ? self->tempo : 120.0;}

static uint32_t engine_tempo(const Plugin *self) {return static_cast<uint32_t>(std::lround(tempo_of(self) * 1000.0));}

// the song's tick at the host's frame, as the original's sequencer counts them (trunc of the position)
static int32_t tick_at(const Plugin *self, uint32_t frame) {
    const double beat = self->beat0 + frame * self->beats_per_frame;
    return static_cast<int32_t>(std::floor(beat * TICKS_PER_BEAT));
}

static void release_notes(Plugin *self) {
    self->held_count = 0;
    if (self->kind != KIND_BASS) {return;}
    if (self->sounding >= 0 && self->bass != nullptr) {orb_bass_note_off(self->bass);}
    self->sounding = -1;
}

static void play_key(Plugin *self, int key, double velocity, bool glide) {
    self->sounding = key;
    orb_bass_note_on(self->bass, key - 36, velocity > ACCENT_VELOCITY, glide);
}

static void note_on(Plugin *self, int key, double velocity) {
    if (key < 0 || key > 127) {return;}
    if (is_drums(self->kind)) {
        // a drum machine plays notes in both modes: velocity above 100 accents (the 909: its strong hit)
        const uint8_t column = drum_column(self->kind, key);
        if (column == 0) {return;}
        const bool loud = velocity > ACCENT_VELOCITY;
        if (self->kind == KIND_TR808) {orb_drums_hit(self->drums, column, loud, false);}
        else {orb_drums_hit(self->drums, column, false, loud);}
        return;
    }
    if (self->pattern_mode) {return;}
    // a key held already moves to the top
    uint32_t at = 0;
    for (uint32_t i = 0; i < self->held_count; i++) {if (self->held[i] != key) {self->held[at++] = self->held[i];}}
    self->held_count = at;
    const bool legato = self->held_count > 0 && self->sounding >= 0;
    self->held[self->held_count++] = static_cast<uint8_t>(key);
    play_key(self, key, velocity, legato);
}

static void note_off(Plugin *self, int key) {
    if (key < 0 || key > 127 || self->kind != KIND_BASS || self->pattern_mode) {return;}
    uint32_t at = 0;
    for (uint32_t i = 0; i < self->held_count; i++) {if (self->held[i] != key) {self->held[at++] = self->held[i];}}
    self->held_count = at;
    if (key != self->sounding) {return;}
    if (self->held_count == 0) {
        orb_bass_note_off(self->bass);
        self->sounding = -1;
    } else {
        // back to the key held before, sliding, unaccented
        play_key(self, self->held[self->held_count - 1], 0.0, true);
    }
}

static void handle_event(Plugin *self, const clap_event_header_t *header) {
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) {return;}
    switch (header->type) {
        case CLAP_EVENT_PARAM_VALUE: {
            const clap_event_param_value_t *param = reinterpret_cast<const clap_event_param_value_t *>(header);
            set_value(self, param->param_id, param->value);
            break;
        }
        case CLAP_EVENT_NOTE_ON: {
            const clap_event_note_t *note = reinterpret_cast<const clap_event_note_t *>(header);
            if (is_instrument(self->kind)) {
                if (note->velocity <= 0.0) {note_off(self, note->key);} else {note_on(self, note->key, note->velocity);}
            }
            break;
        }
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE: {
            const clap_event_note_t *note = reinterpret_cast<const clap_event_note_t *>(header);
            if (is_instrument(self->kind)) {
                if (note->key < 0) {release_notes(self);} else {note_off(self, note->key);}
            }
            break;
        }
        case CLAP_EVENT_MIDI: {
            const clap_event_midi_t *midi = reinterpret_cast<const clap_event_midi_t *>(header);
            if (!is_instrument(self->kind)) {break;}
            const uint8_t status = midi->data[0] & 0xF0;
            if (status == 0x90 && midi->data[2] > 0) {note_on(self, midi->data[1], midi->data[2] / 127.0);}
            else if (status == 0x80 || status == 0x90) {note_off(self, midi->data[1]);}
            else if (status == 0xB0 && (midi->data[1] == 123 || midi->data[1] == 120)) {release_notes(self);}
            break;
        }
        default: break;
    }
}

// --- the engine's blocks -------------------------------------------------------------------------------------------

static void push_out(Plugin *self, float l, float r) {
    if (self->fifo_write - self->fifo_read >= FIFO) {return;}
    self->fifo[0][self->fifo_write % FIFO] = l;
    self->fifo[1][self->fifo_write % FIFO] = r;
    self->fifo_write++;
}

// The ticks of the block the engine runs at the host's frame: the transport's while it plays; a jump (a start, a
// loop, a locate) relocates the patterns. While it stops the 303's patterns stop, the PCF runs on.
static void run_transport(Plugin *self, uint32_t frame, int32_t *tick, bool *advanced) {
    *advanced = false;
    if (self->transport_playing) {
        const int32_t t = tick_at(self, frame);
        const bool jumped = !self->was_playing || t < self->last_tick || t > self->last_tick + TICKS_PER_BEAT;
        if (jumped && self->pattern_mode) {
            if (self->kind == KIND_BASS) {orb_bass_start(self->bass, t);}
            else if (is_drums(self->kind)) {orb_drums_start(self->drums, t);}
        }
        *advanced = jumped || t > self->last_tick;
        self->last_tick = t;
        self->free_tick = t;
        self->was_playing = true;
        *tick = t;
        return;
    }
    if (self->was_playing) {
        self->was_playing = false;
        if (self->pattern_mode) {
            if (self->kind == KIND_BASS) {orb_bass_stop(self->bass);}
            else if (is_drums(self->kind)) {orb_drums_stop(self->drums);}
        }
    }
    self->free_tick += tempo_of(self) / 60.0 * TICKS_PER_BEAT * BLOCK / ENGINE_RATE;
    *tick = static_cast<int32_t>(self->free_tick);
}

static void run_block(Plugin *self, uint32_t frame) {
    sync_params(self);
    int32_t tick;
    bool advanced;
    // an instrument's mode follows its parameter at a block
    if (is_instrument(self->kind)) {
        const bool pattern = self->values[self->info->play] != 0.0;
        if (pattern != self->pattern_mode) {
            release_notes(self);
            if (self->kind == KIND_BASS) {orb_bass_stop(self->bass);} else {orb_drums_stop(self->drums);}
            self->pattern_mode = pattern;
            if (pattern && self->transport_playing) {self->was_playing = false;} // starts at the next block
        }
    }
    run_transport(self, frame, &tick, &advanced);
    switch (self->kind) {
        case KIND_BASS:
            if (self->pattern_mode && self->transport_playing && advanced && tick >= 0) {
                orb_bass_step(self->bass, tick, static_cast<uint8_t>(self->values[B_SHUFFLE]), engine_tempo(self));
            }
            orb_bass_process(self->bass, self->out[0]);
            break;
        case KIND_TR808:
        case KIND_TR909:
            if (self->pattern_mode && self->transport_playing && advanced && tick >= 0) {
                orb_drums_step(self->drums, tick, static_cast<uint8_t>(self->values[R_SHUFFLE]), engine_tempo(self));
            }
            orb_drums_process(self->drums, self->out[0]);
            break;
        case KIND_DIST:
            for (int c = 0; c < 2; c++) {
                std::memcpy(self->out[c], self->in[c], sizeof(self->in[c]));
                orb_dist_process(self->dist[c], self->out[c]);
            }
            break;
        case KIND_PCF:
            for (int c = 0; c < 2; c++) {
                std::memcpy(self->out[c], self->in[c], sizeof(self->in[c]));
                orb_pcf_process(self->pcf[c], self->out[c], tick);
            }
            break;
        case KIND_COMP:
            std::memcpy(self->out, self->in, sizeof(self->in));
            orb_comp_process(self->comp, self->out[0], self->out[1]);
            break;
        case KIND_DELAY: {
            // the send as the mixer's send knob: the level curve's gain on the input, mono
            const float send_gain = orb_level(static_cast<uint8_t>(self->values[E_SEND]));
            float send[BLOCK];
            for (int i = 0; i < BLOCK; i++) {send[i] = (self->in[0][i] + self->in[1][i]) * 0.5f * send_gain;}
            std::memcpy(self->out, self->in, sizeof(self->in));
            orb_delay_process(self->delay, send, self->out[0], self->out[1], engine_tempo(self));
            break;
        }
        default: break;
    }
    if (is_instrument(self->kind)) {
        // the output gain, ramped over the block; mono to both sides
        const float target = static_cast<float>(std::pow(10.0, self->values[self->info->gain] / 20.0));
        const float step = (target - self->gain) / BLOCK;
        for (int i = 0; i < BLOCK; i++) {self->out[0][i] *= self->gain + step * (i + 1);}
        self->gain = target;
        std::memcpy(self->out[1], self->out[0], sizeof(self->out[0]));
    }
    for (int i = 0; i < BLOCK; i++) {
        self->up.push(self->out[0][i], self->out[1][i], [self](float l, float r) {push_out(self, l, r);});
    }
}

// The rates: the resamplers, the output queue ahead by what a block and the conversions need (effects), so it never
// runs dry. The 303 renders a block whenever the queue is empty.
static void configure(Plugin *self) {
    self->down.init(self->host_rate, ENGINE_RATE);
    self->up.init(ENGINE_RATE, self->host_rate);
    self->fill = 0;
    std::memset(self->fifo, 0, sizeof(self->fifo));
    self->fifo_read = 0;
    const double ratio = self->host_rate / ENGINE_RATE;
    self->latency = is_instrument(self->kind) ? 0
        : static_cast<uint32_t>(self->down.delay() + std::ceil(BLOCK * ratio) + std::ceil(self->up.delay() * ratio) + 2);
    self->fifo_write = self->latency;
}

static void reset_devices(Plugin *self) {
    release_notes(self);
    if (self->bass != nullptr) {orb_bass_reset(self->bass);}
    if (self->drums != nullptr) {orb_drums_reset(self->drums);}
    if (self->delay != nullptr) {orb_delay_clear(self->delay);}
    self->was_playing = false;
    self->last_tick = -1;
}

// --- the plugin's calls --------------------------------------------------------------------------------------------

static bool plugin_init(const clap_plugin_t *plugin) {
    Plugin *self = plugin_of(plugin);
    self->page.init(self->host);
    self->host_latency = static_cast<const clap_host_latency_t *>(self->host->get_extension(self->host, CLAP_EXT_LATENCY));
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin) {
    Plugin *self = plugin_of(plugin);
    if (self->bass != nullptr) {orb_bass_free(self->bass);}
    for (Dist *d : self->dist) {if (d != nullptr) {orb_dist_free(d);}}
    for (Filter *f : self->pcf) {if (f != nullptr) {orb_pcf_free(f);}}
    if (self->comp != nullptr) {orb_comp_free(self->comp);}
    if (self->delay != nullptr) {orb_delay_free(self->delay);}
    if (self->drums != nullptr) {orb_drums_free(self->drums);}
    if (self->kit != nullptr) {orb_kit_free(self->kit);}
    self->~Plugin();
    std::free(self);
}

static bool plugin_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames) {
    (void) min_frames; (void) max_frames;
    Plugin *self = plugin_of(plugin);
    const uint32_t before = self->latency;
    self->host_rate = sample_rate;
    configure(self);
    reset_devices(self);
    self->active = true;
    if (before != self->latency && self->host_latency != nullptr) {self->host_latency->changed(self->host);}
    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin) {plugin_of(plugin)->active = false;}

static bool plugin_start_processing(const clap_plugin_t *plugin) {(void) plugin; return true;}

static void plugin_stop_processing(const clap_plugin_t *plugin) {release_notes(plugin_of(plugin));}

static void plugin_reset(const clap_plugin_t *plugin) {
    Plugin *self = plugin_of(plugin);
    configure(self);
    reset_devices(self);
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process) {
    Plugin *self = plugin_of(plugin);
    const uint32_t frames = process->frames_count;
    self->out_events.drain(process->out_events);
    const clap_event_transport_t *transport = process->transport;
    const uint32_t needed = CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
    self->tempo = transport != nullptr && (transport->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0 ? transport->tempo : 0.0;
    self->transport_playing = transport != nullptr && (transport->flags & needed) == needed;
    if (self->transport_playing) {
        self->beat0 = static_cast<double>(transport->song_pos_beats) / CLAP_BEATTIME_FACTOR;
        self->beats_per_frame = tempo_of(self) / 60.0 / self->host_rate;
    }
    const clap_input_events_t *in_events = process->in_events;
    const uint32_t count = in_events->size(in_events);
    uint32_t event = 0;
    const bool effect = !is_instrument(self->kind);
    const float *in_l = nullptr, *in_r = nullptr;
    if (effect && process->audio_inputs_count > 0 && process->audio_inputs[0].data32 != nullptr) {
        in_l = process->audio_inputs[0].data32[0];
        in_r = process->audio_inputs[0].channel_count > 1 ? process->audio_inputs[0].data32[1] : in_l;
    }
    float *out_l = process->audio_outputs[0].data32[0];
    float *out_r = process->audio_outputs[0].channel_count > 1 ? process->audio_outputs[0].data32[1] : nullptr;
    for (uint32_t frame = 0; frame < frames; frame++) {
        for (; event < count; event++) {
            const clap_event_header_t *header = in_events->get(in_events, event);
            if (header->time > frame) {break;}
            handle_event(self, header);
        }
        if (effect) {
            const float l = in_l != nullptr ? in_l[frame] : 0.0f, r = in_r != nullptr ? in_r[frame] : 0.0f;
            self->down.push(l, r, [self, frame](float a, float b) {
                self->in[0][self->fill] = a;
                self->in[1][self->fill] = b;
                if (++self->fill == BLOCK) {
                    self->fill = 0;
                    run_block(self, frame);
                }
            });
        } else {
            // a block whenever the queue runs dry (the upsampler holds the first outputs back)
            for (int guard = 0; self->fifo_write == self->fifo_read && guard < 4; guard++) {run_block(self, frame);}
        }
        float l = 0.0f, r = 0.0f;
        if (self->fifo_read != self->fifo_write) {
            l = self->fifo[0][self->fifo_read % FIFO];
            r = self->fifo[1][self->fifo_read % FIFO];
            self->fifo_read++;
        }
        out_l[frame] = l;
        if (out_r != nullptr) {out_r[frame] = r;}
    }
    for (; event < count; event++) {handle_event(self, in_events->get(in_events, event));}
    return CLAP_PROCESS_CONTINUE;
}

// --- audio and note ports ------------------------------------------------------------------------------------------

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input) {
    return is_input && is_instrument(plugin_of(plugin)->kind) ? 0 : 1;
}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
    if (index != 0 || (is_input && is_instrument(plugin_of(plugin)->kind))) {return false;}
    std::memset(info, 0, sizeof(*info));
    info->id = is_input ? 0 : 1;
    std::snprintf(info->name, sizeof(info->name), "%s", is_input ? "Input" : "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t audio_ports = {audio_ports_count, audio_ports_get};

static uint32_t note_ports_count(const clap_plugin_t *plugin, bool is_input) {
    return is_input && is_instrument(plugin_of(plugin)->kind) ? 1 : 0;
}

static bool note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info) {
    if (index != 0 || !is_input || !is_instrument(plugin_of(plugin)->kind)) {return false;}
    std::memset(info, 0, sizeof(*info));
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "Notes");
    return true;
}

static const clap_plugin_note_ports_t note_ports = {note_ports_count, note_ports_get};

static uint32_t latency_get(const clap_plugin_t *plugin) {return plugin_of(plugin)->latency;}

static const clap_plugin_latency_t latency = {latency_get};

// --- parameters ----------------------------------------------------------------------------------------------------

static uint32_t params_count(const clap_plugin_t *plugin) {return plugin_of(plugin)->info->count;}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info) {
    const Plugin *self = plugin_of(plugin);
    if (index >= self->info->count) {return false;}
    const Param &p = self->info->params[index];
    std::memset(info, 0, sizeof(*info));
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
    if (p.format == F_CHOICE || p.format == F_PATTERN) {info->flags |= CLAP_PARAM_IS_ENUM;}
    std::snprintf(info->name, sizeof(info->name), "%s", p.name);
    info->min_value = p.min;
    info->max_value = p.max;
    info->default_value = p.def;
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value) {
    const Plugin *self = plugin_of(plugin);
    if (id >= self->info->count) {return false;}
    *value = self->values[id];
    return true;
}

static void pattern_name(int index, char *text, uint32_t size) {
    std::snprintf(text, size, "%c%d", 'A' + index / 8, index % 8 + 1);
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *text, uint32_t size) {
    const Plugin *self = plugin_of(plugin);
    if (id >= self->info->count || size == 0) {return false;}
    const Param &p = self->info->params[id];
    const int v = static_cast<int>(clamp_value(p, value));
    switch (p.format) {
        case F_PERCENT: std::snprintf(text, size, "%d %%", static_cast<int>(std::lround(v * 100.0 / 127.0))); break;
        case F_TUNE: {
            const int semis = tune_table[v >> 2];
            std::snprintf(text, size, semis == 0 ? "0 st" : "%+d st", semis);
            break;
        }
        case F_CHOICE: std::snprintf(text, size, "%s", p.labels[v]); break;
        case F_PATTERN: pattern_name(v, text, size); break;
        case F_DB: std::snprintf(text, size, v > 0 ? "+%d dB" : "%d dB", v); break;
        case F_SHAPE:
            if (v == 0) {std::snprintf(text, size, "Clip");} else {std::snprintf(text, size, "%d", (v >> 2) + 1);}
            break;
        case F_PAN:
            if (v == 64) {std::snprintf(text, size, "C");}
            else {std::snprintf(text, size, "%c%d", v < 64 ? 'L' : 'R', v < 64 ? 64 - v : v - 64);}
            break;
        default:
            if (self->kind == KIND_PCF && id == P_WAVE) {std::snprintf(text, size, "%d", v + 1);}
            else {std::snprintf(text, size, "%d", v);}
            break;
    }
    return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value) {
    const Plugin *self = plugin_of(plugin);
    if (id >= self->info->count || text == nullptr) {return false;}
    const Param &p = self->info->params[id];
    while (*text == ' ') {text++;}
    switch (p.format) {
        case F_CHOICE:
            for (int i = 0; i <= static_cast<int>(p.max); i++) {
                if (strncasecmp(text, p.labels[i], std::strlen(p.labels[i])) == 0) {*value = i; return true;}
            }
            *value = clamp_value(p, std::atof(text));
            return true;
        case F_PATTERN: {
            const char bank = static_cast<char>(text[0] & ~0x20);
            if (bank < 'A' || bank > 'D') {return false;}
            *value = clamp_value(p, (bank - 'A') * 8 + std::atoi(text + 1) - 1);
            return true;
        }
        case F_PERCENT: *value = clamp_value(p, std::atof(text) * 127.0 / 100.0); return true;
        case F_TUNE: {
            const int semis = std::atoi(text);
            int best = 64;
            for (int v = 0; v < 128; v++) {
                if (tune_table[v >> 2] == semis && std::abs(v - 64) < std::abs(best - 64)) {best = v;}
            }
            if (tune_table[best >> 2] != semis) {best = semis < 0 ? 0 : 127;}
            *value = best;
            return true;
        }
        case F_DB: *value = clamp_value(p, std::atof(text)); return true;
        case F_SHAPE: {
            // Clip, or a curve 1..32: its first value (curve 1 starts at 1, curve n at 4 * (n - 1))
            if ((text[0] & ~0x20) == 'C') {*value = 0; return true;}
            const int curve = std::atoi(text);
            if (curve < 1) {*value = 0; return true;}
            *value = curve == 1 ? 1 : clamp_value(p, 4.0 * (curve - 1));
            return true;
        }
        case F_PAN: {
            const char side = static_cast<char>(text[0] & ~0x20);
            const int amount = std::atoi(text + 1);
            *value = clamp_value(p, side == 'L' ? 64 - amount : side == 'R' ? 64 + amount : 64);
            return true;
        }
        default: {
            double parsed = std::atof(text);
            if (self->kind == KIND_PCF && id == P_WAVE) {parsed -= 1.0;}
            *value = clamp_value(p, parsed);
            return true;
        }
    }
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out) {
    Plugin *self = plugin_of(plugin);
    self->out_events.drain(out);
    const uint32_t count = in->size(in);
    for (uint32_t index = 0; index < count; index++) {
        const clap_event_header_t *header = in->get(in, index);
        if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_PARAM_VALUE) {
            handle_event(self, header);
        }
    }
}

static const clap_plugin_params_t params = {
    params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush
};

// --- state ---------------------------------------------------------------------------------------------------------
// "ORB1", the kind, the version, two bytes 0, the parameters (f64 each, in id order), then a 303's, 808's or 909's 32
// patterns (98 or 194 bytes each, as the device keeps them), then a drum machine's kit: for each of its slots (the 808
// 1000..1023, the 909 1025..1045) a u32 frame count and the 16-bit samples. The layout up to the kit never moves: a
// step's edit changes its own bytes.

#define STATE_HEADER 8

static uint16_t kit_first(const Plugin *self) {return self->kind == KIND_TR808 ? 1000 : 1025;}
static uint16_t kit_last(const Plugin *self) {return self->kind == KIND_TR808 ? 1023 : 1045;}

static uint32_t fixed_state_size(const Plugin *self) {
    const uint32_t patterns = self->kind == KIND_BASS || is_drums(self->kind) ? PATTERNS * pattern_bytes(self) : 0;
    return STATE_HEADER + self->info->count * 8 + patterns;
}

static bool write_all(const clap_ostream_t *stream, const uint8_t *bytes, uint64_t size) {
    uint64_t written = 0;
    while (written < size) {
        const int64_t result = stream->write(stream, bytes + written, size - written);
        if (result <= 0) {return false;}
        written += static_cast<uint64_t>(result);
    }
    return true;
}

static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    Plugin *self = plugin_of(plugin);
    uint8_t bytes[STATE_HEADER + MAX_PARAMS * 8 + PATTERNS * MAX_PATTERN_BYTES] = {};
    std::memcpy(bytes, "ORB1", 4);
    bytes[4] = self->kind;
    bytes[5] = STATE_VERSION;
    std::memcpy(bytes + STATE_HEADER, self->values, self->info->count * 8);
    if (self->kind == KIND_BASS || is_drums(self->kind)) {
        std::memcpy(bytes + STATE_HEADER + self->info->count * 8, patterns_of(self), PATTERNS * pattern_bytes(self));
    }
    if (!write_all(stream, bytes, fixed_state_size(self))) {return false;}
    if (!is_drums(self->kind)) {return true;}
    for (uint16_t id = kit_first(self); id <= kit_last(self); id++) {
        uint32_t frames = 0;
        const int16_t *audio = orb_kit_get(self->kit, id, &frames);
        if (!write_all(stream, reinterpret_cast<const uint8_t *>(&frames), 4)) {return false;}
        if (frames > 0 && !write_all(stream, reinterpret_cast<const uint8_t *>(audio), frames * 2ull)) {return false;}
    }
    return true;
}

static uint64_t read_all(const clap_istream_t *stream, uint8_t *bytes, uint64_t size) {
    uint64_t read = 0;
    while (read < size) {
        const int64_t result = stream->read(stream, bytes + read, size - read);
        if (result <= 0) {break;}
        read += static_cast<uint64_t>(result);
    }
    return read;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    Plugin *self = plugin_of(plugin);
    uint8_t bytes[STATE_HEADER + MAX_PARAMS * 8 + PATTERNS * MAX_PATTERN_BYTES];
    const uint64_t size = fixed_state_size(self);
    if (read_all(stream, bytes, size) < size || std::memcmp(bytes, "ORB1", 4) != 0 || bytes[4] != self->kind
        || bytes[5] != STATE_VERSION) {
        return false;
    }
    for (clap_id id = 0; id < self->info->count; id++) {
        double value;
        std::memcpy(&value, bytes + STATE_HEADER + id * 8, 8);
        set_value(self, id, value);
    }
    if (self->kind == KIND_BASS || is_drums(self->kind)) {
        uint8_t *patterns = patterns_of(self);
        const uint32_t each = pattern_bytes(self);
        std::memcpy(patterns, bytes + STATE_HEADER + self->info->count * 8, PATTERNS * each);
        for (int p = 0; p < PATTERNS; p++) {sanitize_any(self, patterns + p * each);}
    }
    if (is_drums(self->kind)) {
        // the kit: a slot the state lacks (an older or shorter one) stays empty
        for (uint16_t id = kit_first(self); id <= kit_last(self); id++) {
            uint32_t frames = 0;
            if (read_all(stream, reinterpret_cast<uint8_t *>(&frames), 4) < 4 || frames > 44100u * 60u) {frames = 0;}
            int16_t *audio = frames > 0 ? static_cast<int16_t *>(std::malloc(frames * 2ull)) : nullptr;
            if (audio != nullptr && read_all(stream, reinterpret_cast<uint8_t *>(audio), frames * 2ull) < frames * 2ull) {
                frames = 0;
            }
            orb_kit_set(self->kit, id, audio, audio != nullptr ? frames : 0);
            std::free(audio);
        }
        orb_drums_set_kit(self->drums, self->kit);
    }
    self->snapshot_due = true;
    return true;
}

static const clap_plugin_state_t state = {state_save, state_load};

// --- the page ------------------------------------------------------------------------------------------------------

static int32_t write_uri(const Plugin *self, char *uri, uint32_t capacity) {
    char page[96];
    const int length = std::snprintf(page, sizeof(page), "/index.html?device=%s", self->info->page);
    if (capacity > 0 && uri != nullptr) {std::snprintf(uri, capacity, "%s", page);}
    return length + 1;
}

static int32_t webview_get_uri(const clap_plugin_t *plugin, char *uri, uint32_t capacity) {
    return write_uri(plugin_of(plugin), uri, capacity);
}

static bool webview_get_resource(const clap_plugin_t *plugin, const char *path, char *mime, uint32_t capacity,
                                 const clap_ostream_t *stream) {
    (void) plugin; (void) path; (void) mime; (void) capacity; (void) stream;
    return false;
}

static uint32_t write_status(Plugin *self, uint8_t *message, uint8_t type) {
    std::memset(message, 0, 8);
    message[0] = type;
    message[1] = self->kind;
    message[4] = 255;
    uint32_t at = 8;
    for (clap_id id = 0; id < self->info->count; id++, at += 8) {std::memcpy(message + at, &self->values[id], 8);}
    switch (self->kind) {
        case KIND_BASS: {
            message[2] = static_cast<uint8_t>((self->was_playing ? FLAG_PLAYING : 0) | (orb_bass_gate(self->bass) ? FLAG_GATE : 0));
            orb_bass_position(self->bass, message + 3, message + 4);
            if (!self->pattern_mode || !self->was_playing) {message[4] = 255;}
            break;
        }
        case KIND_DIST: {
            message[5] = orb_dist_meter(self->dist[0]);
            message[6] = orb_dist_meter(self->dist[1]);
            float curve[TRANSFER_POINTS];
            orb_dist_transfer(static_cast<uint8_t>(self->values[D_AMOUNT]), static_cast<uint8_t>(self->values[D_SHAPE]),
                              TRANSFER_POINTS, curve);
            std::memcpy(message + at, curve, sizeof(curve));
            at += sizeof(curve);
            break;
        }
        case KIND_PCF: {
            message[2] = self->was_playing ? FLAG_PLAYING : 0;
            float env;
            int32_t step;
            orb_pcf_status(self->pcf[0], &env, &step);
            uint16_t length, resolution;
            orb_pcf_wave(static_cast<uint8_t>(self->values[P_WAVE]), &length, &resolution, message + at + 12);
            std::memcpy(message + at, &env, 4);
            std::memcpy(message + at + 4, &step, 4);
            std::memcpy(message + at + 8, &length, 2);
            std::memcpy(message + at + 10, &resolution, 2);
            at += 12 + 32;
            break;
        }
        case KIND_COMP:
            orb_comp_meters(self->comp, message + 5, message + 6);
            break;
        case KIND_TR808:
        case KIND_TR909: {
            message[2] = self->was_playing ? FLAG_PLAYING : 0;
            orb_drums_position(self->drums, message + 3, message + 4);
            if (!self->pattern_mode || !self->was_playing) {message[4] = 255;}
            uint8_t loaded = 0;
            for (uint16_t id = kit_first(self); id <= kit_last(self); id++) {
                uint32_t frames = 0;
                orb_kit_get(self->kit, id, &frames);
                if (frames > 0) {loaded++;}
            }
            message[5] = loaded;
            break;
        }
        case KIND_DELAY: {
            message[5] = orb_delay_meter(self->delay);
            const uint32_t length = orb_delay_length(engine_tempo(self), static_cast<uint8_t>(byte_of(self, E_STEPS)),
                                                     static_cast<uint8_t>(byte_of(self, E_GRID)));
            std::memcpy(message + at, &length, 4);
            at += 4;
            break;
        }
        default: break;
    }
    return at;
}

static void send_status(Plugin *self) {
    static uint8_t message[8 + MAX_PARAMS * 8 + TRANSFER_POINTS * 4 + PATTERNS * MAX_PATTERN_BYTES];
    if (self->snapshot_due && (self->kind == KIND_BASS || is_drums(self->kind))) {
        uint32_t at = write_status(self, message, PLUGIN_SNAPSHOT);
        std::memcpy(message + at, patterns_of(self), PATTERNS * pattern_bytes(self));
        at += PATTERNS * pattern_bytes(self);
        self->snapshot_due = false;
        self->page.send(message, at);
        return;
    }
    self->snapshot_due = false;
    const uint32_t at = write_status(self, message, PLUGIN_STATUS);
    self->page.send(message, at);
}

struct PageOf {
    Plugin *self;
    bool has(clap_id id) const {return id < self->info->count;}
    void set(clap_id id, double value) {set_value(self, id, value);}
    double value(clap_id id) const {return self->values[id];}
    void status() {send_status(self);}
};

static bool webview_receive(const clap_plugin_t *plugin, const void *buffer, uint32_t size) {
    Plugin *self = plugin_of(plugin);
    const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
    if (size < 1 || self->page.webview == nullptr) {return false;}
    switch (bytes[0]) {
        case opendaw::PAGE_POLL:
        case opendaw::PAGE_PARAM_BEGIN:
        case opendaw::PAGE_PARAM_SET:
        case opendaw::PAGE_PARAM_END:
        case opendaw::PAGE_HOVER: {
            PageOf page{self};
            return opendaw::receive(page, self->page, self->out_events, buffer, size);
        }
        case PAGE_HELLO:
            self->snapshot_due = true;
            send_status(self);
            return true;
        case PAGE_STEP: {
            // u8 type, u8 pattern, u8 step, u8 0, the step's bytes (6 for a 303, 12 for an 808 or 909)
            if (self->kind != KIND_BASS && !is_drums(self->kind)) {return false;}
            const uint32_t width = self->kind == KIND_BASS ? 6 : DRUM_COLUMNS;
            if (size < 4 + width || bytes[1] >= PATTERNS || bytes[2] >= STEPS) {return false;}
            uint8_t *pattern = patterns_of(self) + bytes[1] * pattern_bytes(self);
            std::memcpy(pattern + 2 + bytes[2] * width, bytes + 4, width);
            sanitize_any(self, pattern);
            return true;
        }
        case PAGE_HEADER: {
            // u8 type, u8 pattern, u8 0 (the shuffle switch that was), u8 length
            if ((self->kind != KIND_BASS && !is_drums(self->kind)) || size < 4 || bytes[1] >= PATTERNS) {return false;}
            uint8_t *pattern = patterns_of(self) + bytes[1] * pattern_bytes(self);
            pattern[1] = bytes[3];
            sanitize_any(self, pattern);
            return true;
        }
        case PAGE_PATTERN: {
            // u8 type, u8 pattern, u16 0, the pattern's bytes
            if (self->kind != KIND_BASS && !is_drums(self->kind)) {return false;}
            if (size < 4 + pattern_bytes(self) || bytes[1] >= PATTERNS) {return false;}
            uint8_t *pattern = patterns_of(self) + bytes[1] * pattern_bytes(self);
            std::memcpy(pattern, bytes + 4, pattern_bytes(self));
            sanitize_any(self, pattern);
            return true;
        }
        case PAGE_KIT_DLL: {
            // u8 type, 3 bytes 0, ReBirth.dll: every slot from its AIFF resources
            if (!is_drums(self->kind) || size < 5) {return false;}
            if (!orb_kit_load_dll(self->kit, bytes + 4, size - 4)) {return false;}
            orb_drums_set_kit(self->drums, self->kit);
            self->snapshot_due = true;
            if (self->page.state != nullptr) {self->page.state->mark_dirty(self->page.host);}
            return true;
        }
        case PAGE_KIT_SLOT: {
            // u8 type, u8 0, u16 slot id, u32 frames, the 16-bit samples
            if (!is_drums(self->kind) || size < 8) {return false;}
            uint16_t id;
            uint32_t frames;
            std::memcpy(&id, bytes + 2, 2);
            std::memcpy(&frames, bytes + 4, 4);
            if (id < kit_first(self) || id > kit_last(self) || size < 8 + frames * 2ull) {return false;}
            int16_t *audio = frames > 0 ? static_cast<int16_t *>(std::malloc(frames * 2ull)) : nullptr;
            if (audio != nullptr) {std::memcpy(audio, bytes + 8, frames * 2ull);}
            orb_kit_set(self->kit, id, audio, audio != nullptr ? frames : 0);
            std::free(audio);
            orb_drums_set_kit(self->drums, self->kit);
            self->snapshot_due = true;
            if (self->page.state != nullptr) {self->page.state->mark_dirty(self->page.host);}
            return true;
        }
        case PAGE_COMMIT:
            // another page of the plugin gets the patterns with the next poll
            self->snapshot_due = true;
            if (self->page.state != nullptr) {self->page.state->mark_dirty(self->page.host);}
            return true;
        default:
            return false;
    }
}

static const clap_plugin_webview_t webview = {webview_get_uri, webview_get_resource, webview_receive};

// --- the device panel: every device is its panel, there is no window ----------------------------------------------

static int32_t panel_get_uri(const clap_plugin_t *plugin, char *uri, uint32_t capacity) {
    return write_uri(plugin_of(plugin), uri, capacity);
}

static bool panel_get_width(const clap_plugin_t *plugin, uint32_t height, uint32_t *width) {
    *width = opendaw::panel_width_at(plugin_of(plugin)->info->panel_width, height);
    return true;
}

static const opendaw_plugin_device_panel_t device_panel = {panel_get_uri, panel_get_width};

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id) {
    const Plugin *self = plugin_of(plugin);
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {return &audio_ports;}
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0 && is_instrument(self->kind)) {return &note_ports;}
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) {return &latency;}
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {return &params;}
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {return &state;}
    if (std::strcmp(id, CLAP_EXT_WEBVIEW) == 0) {return &webview;}
    if (std::strcmp(id, OPENDAW_EXT_DEVICE_PANEL) == 0) {return &device_panel;}
    return nullptr;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin) {(void) plugin;}

// --- the descriptors -----------------------------------------------------------------------------------------------

#define URL "https://github.com/andremichelle/open-webclap/tree/main/ports/openrb"
#define PANEL_FEATURES(width) OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL, OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL_WIDTH(width)

static const char *bass_features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                      CLAP_PLUGIN_FEATURE_MONO, PANEL_FEATURES(BASS_PANEL_WIDTH), nullptr};
static const char *dist_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DISTORTION,
                                      CLAP_PLUGIN_FEATURE_STEREO, PANEL_FEATURES(DIST_PANEL_WIDTH), nullptr};
static const char *pcf_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_FILTER,
                                     CLAP_PLUGIN_FEATURE_STEREO, PANEL_FEATURES(PCF_PANEL_WIDTH), nullptr};
static const char *comp_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_COMPRESSOR,
                                      CLAP_PLUGIN_FEATURE_STEREO, PANEL_FEATURES(COMP_PANEL_WIDTH), nullptr};
static const char *delay_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DELAY,
                                       CLAP_PLUGIN_FEATURE_STEREO, PANEL_FEATURES(DELAY_PANEL_WIDTH), nullptr};
static const char *tr808_features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_DRUM_MACHINE,
                                       CLAP_PLUGIN_FEATURE_MONO, PANEL_FEATURES(TR808_PANEL_WIDTH), nullptr};
static const char *tr909_features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_DRUM_MACHINE,
                                       CLAP_PLUGIN_FEATURE_MONO, PANEL_FEATURES(TR909_PANEL_WIDTH), nullptr};

static const clap_plugin_descriptor_t descriptors[KIND_COUNT] = {
    {CLAP_VERSION_INIT, "studio.opendaw.openrb.bass", "OpenRB 303", "open-webclap", URL, URL, "", "0.1.0",
     "Bass line synth: OpenRB-338's TB-303, played by notes or by its own patterns", bass_features},
    {CLAP_VERSION_INIT, "studio.opendaw.openrb.distortion", "OpenRB Distortion", "open-webclap", URL, URL, "", "0.1.0",
     "OpenRB-338's distortion: a clipper and a family of waveshapers", dist_features},
    {CLAP_VERSION_INIT, "studio.opendaw.openrb.pcf", "OpenRB PCF", "open-webclap", URL, URL, "", "0.1.0",
     "OpenRB-338's pattern controlled filter: 55 rhythmic envelope patterns on a low or band pass", pcf_features},
    {CLAP_VERSION_INIT, "studio.opendaw.openrb.compressor", "OpenRB Compressor", "open-webclap", URL, URL, "", "0.1.0",
     "OpenRB-338's compressor, linked stereo as on its master", comp_features},
    {CLAP_VERSION_INIT, "studio.opendaw.openrb.delay", "OpenRB Delay", "open-webclap", URL, URL, "", "0.1.0",
     "OpenRB-338's tempo synced delay in 16ths or 8th triplets", delay_features},
    {CLAP_VERSION_INIT, "studio.opendaw.openrb.tr808", "OpenRB 808", "open-webclap", URL, URL, "", "0.1.0",
     "OpenRB-338's TR-808 on the samples you bring (ReBirth.dll), played by notes or by its own patterns", tr808_features},
    {CLAP_VERSION_INIT, "studio.opendaw.openrb.tr909", "OpenRB 909", "open-webclap", URL, URL, "", "0.1.0",
     "OpenRB-338's TR-909 on the samples you bring (ReBirth.dll), played by notes or by its own patterns", tr909_features},
};

static const clap_plugin_t *create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id) {
    (void) factory;
    int kind = -1;
    for (int k = 0; k < KIND_COUNT; k++) {if (std::strcmp(plugin_id, descriptors[k].id) == 0) {kind = k;}}
    if (kind < 0) {return nullptr;}
    orb_init();
    void *memory = std::calloc(1, sizeof(Plugin));
    if (memory == nullptr) {return nullptr;}
    Plugin *self = new(memory) Plugin();
    self->host = host;
    self->kind = static_cast<Kind>(kind);
    self->info = &kinds[kind];
    for (clap_id id = 0; id < self->info->count; id++) {
        self->values[id] = self->info->params[id].def;
        self->applied[id] = -1;
    }
    switch (self->kind) {
        case KIND_BASS: self->bass = orb_bass_new(); load_factory_patterns(self); break;
        case KIND_DIST: self->dist[0] = orb_dist_new(); self->dist[1] = orb_dist_new(); break;
        case KIND_PCF: self->pcf[0] = orb_pcf_new(); self->pcf[1] = orb_pcf_new(); break;
        case KIND_COMP: self->comp = orb_comp_new(); break;
        case KIND_DELAY: self->delay = orb_delay_new(); break;
        case KIND_TR808:
        case KIND_TR909:
            // no samples until the user brings them
            self->kit = orb_kit_new();
            self->drums = orb_drums_new(self->kind == KIND_TR808 ? 0 : 1, self->kit);
            load_factory_patterns(self);
            break;
        default: break;
    }
    self->sounding = -1;
    self->last_tick = -1;
    self->gain = 1.0f;
    self->host_rate = 48000.0;
    configure(self);
    self->plugin.desc = &descriptors[kind];
    self->plugin.plugin_data = self;
    self->plugin.init = plugin_init;
    self->plugin.destroy = plugin_destroy;
    self->plugin.activate = plugin_activate;
    self->plugin.deactivate = plugin_deactivate;
    self->plugin.start_processing = plugin_start_processing;
    self->plugin.stop_processing = plugin_stop_processing;
    self->plugin.reset = plugin_reset;
    self->plugin.process = plugin_process;
    self->plugin.get_extension = plugin_get_extension;
    self->plugin.on_main_thread = plugin_on_main_thread;
    return &self->plugin;
}

static uint32_t plugin_count(const clap_plugin_factory_t *factory) {(void) factory; return KIND_COUNT;}

static const clap_plugin_descriptor_t *plugin_descriptor(const clap_plugin_factory_t *factory, uint32_t index) {
    (void) factory;
    return index < KIND_COUNT ? &descriptors[index] : nullptr;
}

static const clap_plugin_factory_t factory = {plugin_count, plugin_descriptor, create};

static bool entry_init(const char *path) {(void) path; return true;}

static void entry_deinit(void) {}

static const void *entry_get_factory(const char *id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory};
