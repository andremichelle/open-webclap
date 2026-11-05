// Pearls: a granular texture effect, Mutable Instruments' Beads (Emilie Gillet, MIT) as a plain CLAP plugin with a
// webview, no JUCE. Beads' DSP (external/eurorack/beads/dsp) runs as on the module: at the rate its quality setting
// gives the codec (48, 32 or 24 kHz), in the module's blocks (a 4 kHz block rate), with the
// pots' smoothing. The host's audio is resampled to that rate and back; the latency is the resampling's and a block's.
// The CV inputs and their attenuverters are gone, the attenuverters as random amounts (as on the module with nothing
// patched) stay. The automatic input gain is a gain parameter, the soft clipping of the tape settings stays. The
// module's wavetable synth (when nothing is patched) is left out: Pearls is an audio effect.
//
// openDAW shows the page in its device panel (feature "opendaw:device-panel", extension "opendaw.device-panel/1"),
// every host in the window. The feature "opendaw:device-panel-width:636" names the panel's width, which openDAW reads
// from its cloud index to open the panel at its size before the plugin loads.

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>
#include <clap/ext/draft/param-hovered.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "beads/dsp/processor.h"

extern "C" void emscripten_notify_memory_growth(size_t index) {(void) index;}

#define OUT_EVENTS 128
#define PANEL_HEIGHT 176
#define PANEL_WIDTH 636
#define GUI_ZOOM 1.5
#define GUI_MIN_ZOOM 1.0
#define GUI_MAX_ZOOM 3.0

// messages, see page/index.html
#define PAGE_POLL 1
#define PAGE_PARAM_BEGIN 2
#define PAGE_PARAM_SET 3
#define PAGE_PARAM_END 4
#define PAGE_HOVER 7
#define PLUGIN_STATUS 1

#define OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL "opendaw:device-panel"
#define STRING(value) #value
#define STRINGIFY(value) STRING(value)
#define OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL_WIDTH "opendaw:device-panel-width:" STRINGIFY(PANEL_WIDTH)
#define OPENDAW_EXT_DEVICE_PANEL "opendaw.device-panel/1"

typedef struct opendaw_plugin_device_panel {
    int32_t (*get_uri)(const clap_plugin_t *plugin, char *uri, uint32_t capacity);
    bool (*get_width)(const clap_plugin_t *plugin, uint32_t height, uint32_t *width);
} opendaw_plugin_device_panel_t;

// --- parameters -------------------------------------------------------------------------------------------------
// Values are what the module's controls give its DSP: knob positions 0..1, the pitch in semitones, the random amounts
// -1..1 (the attenuverters), gains in dB, steps. Ids are FNV-1a hashes of the keys, below 2^31 (see fnv1a).

enum class Text {Percent, Signed, Semitones, Db, Labels};

struct Param {
    const char *key;
    const char *name;
    double low, high, value;
    bool stepped;
    Text text;
    const char *const *labels;
};

static const char *const QUALITY[] = {"Dirty tape", "Clean tape", "Lo-fi digital", "Hi-fi digital"};
static const char *const RECORD[] = {"Stereo", "Mono"};
static const char *const OFF_ON[] = {"Off", "On"};
static const char *const SYNC[] = {"Off", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar"};
// the clock's period per sync step, in beats
static const double SYNC_BEATS[] = {0.0, 0.125, 0.25, 0.5, 1.0, 2.0, 4.0};

enum {
    P_TIME, P_DENSITY, P_PITCH, P_SIZE, P_SHAPE,
    P_TIME_RANDOM, P_PITCH_RANDOM, P_SIZE_RANDOM, P_SHAPE_RANDOM,
    P_FEEDBACK, P_DRY_WET, P_REVERB, P_INPUT, P_OUTPUT,
    P_QUALITY, P_RECORD, P_FREEZE, P_QUANTIZE, P_SYNC,
    P_COUNT
};

static const Param PARAMS[P_COUNT] = {
    {"time", "Time", 0, 1, 0.25, false, Text::Percent, nullptr},
    {"density", "Density", 0, 1, 0.8, false, Text::Percent, nullptr},
    {"pitch", "Pitch", -24, 24, 0, false, Text::Semitones, nullptr},
    {"size", "Size", 0, 1, 0.6, false, Text::Percent, nullptr},
    {"shape", "Shape", 0, 1, 0.5, false, Text::Percent, nullptr},
    {"timeRandom", "Time Random", -1, 1, 0, false, Text::Signed, nullptr},
    {"pitchRandom", "Pitch Random", -1, 1, 0, false, Text::Signed, nullptr},
    {"sizeRandom", "Size Random", -1, 1, 0, false, Text::Signed, nullptr},
    {"shapeRandom", "Shape Random", -1, 1, 0, false, Text::Signed, nullptr},
    {"feedback", "Feedback", 0, 1, 0.1, false, Text::Percent, nullptr},
    {"dryWet", "Dry/Wet", 0, 1, 0.5, false, Text::Percent, nullptr},
    {"reverb", "Reverb", 0, 1, 0.25, false, Text::Percent, nullptr},
    {"input", "Input", -24, 24, 0, false, Text::Db, nullptr},
    {"output", "Output", -24, 24, 0, false, Text::Db, nullptr},
    {"quality", "Quality", 0, 3, 3, true, Text::Labels, QUALITY},
    {"record", "Record", 0, 1, 0, true, Text::Labels, RECORD},
    {"freeze", "Freeze", 0, 1, 0, true, Text::Labels, OFF_ON},
    {"quantize", "Quantize", 0, 1, 0, true, Text::Labels, OFF_ON},
    {"sync", "Sync", 0, 6, 0, true, Text::Labels, SYNC},
};

// the key's FNV-1a hash with the top bit cleared: openDAW reads a hovered parameter's id as a signed number, so an
// id above 2^31 would match none of its parameters (no menu, no value entry on it)
static clap_id fnv1a(const char *text) {
    uint32_t hash = 0x811c9dc5u;
    for (; *text != '\0'; text++) {hash = (hash ^ static_cast<uint8_t>(*text)) * 16777619u;}
    return hash & 0x7fffffffu;
}

static double clamp(double value, double low, double high) {return value < low ? low : value > high ? high : value;}

static double constrain(const Param &param, double value) {
    value = clamp(value, param.low, param.high);
    return param.stepped ? std::round(value) : value;
}

// --- resampling ---------------------------------------------------------------------------------------------------
// From one rate to another, stereo, with a windowed sinc (8 zero crossings, cut below the lower Nyquist). Output
// sample k sits at input time k * step, it comes out as soon as the samples after it are in. Equal rates pass through.

#define SINC_ZEROS 8
#define SINC_RESOLUTION 256
#define RING 512

static float sinc_table[SINC_ZEROS * SINC_RESOLUTION + 2];

static void init_sinc_table() {
    for (int index = 0; index < SINC_ZEROS * SINC_RESOLUTION + 2; index++) {
        const double t = static_cast<double>(index) / SINC_RESOLUTION;
        const double x = M_PI * t;
        const double sinc = index == 0 ? 1.0 : std::sin(x) / x;
        // Blackman-Harris over the zero crossings
        const double w = t >= SINC_ZEROS ? 0.0 : 0.5 + 0.5 * t / SINC_ZEROS;
        const double a = 2.0 * M_PI * w;
        const double window = 0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a);
        sinc_table[index] = static_cast<float>(t >= SINC_ZEROS ? 0.0 : sinc * window);
    }
}

static inline float kernel(double t) {
    t = std::fabs(t) * SINC_RESOLUTION;
    const int index = static_cast<int>(t);
    if (index >= SINC_ZEROS * SINC_RESOLUTION) {return 0.0f;}
    const float fraction = static_cast<float>(t - index);
    return sinc_table[index] + (sinc_table[index + 1] - sinc_table[index]) * fraction;
}

struct Resampler {
    bool identity;
    double step, cutoff, position;
    int half; // input samples either side of an output
    int64_t written;
    float ring[2][RING];

    void init(double in_rate, double out_rate) {
        identity = in_rate == out_rate;
        step = in_rate / out_rate;
        cutoff = (out_rate < in_rate ? out_rate / in_rate : 1.0) * 0.9;
        half = static_cast<int>(std::ceil(SINC_ZEROS / cutoff));
        position = 0.0;
        written = 0;
        std::memset(ring, 0, sizeof(ring));
    }

    // input samples an output waits for after its own time
    int delay() const {return identity ? 0 : half;}

    template<class Emit> void push(float l, float r, Emit emit) {
        if (identity) {
            emit(l, r);
            return;
        }
        ring[0][written & (RING - 1)] = l;
        ring[1][written & (RING - 1)] = r;
        written++;
        while (position + half <= static_cast<double>(written - 1)) {
            const int64_t base = static_cast<int64_t>(position);
            float left = 0.0f, right = 0.0f;
            for (int64_t index = base - half + 1; index <= base + half; index++) {
                if (index < 0) {continue;}
                const float w = kernel((position - static_cast<double>(index)) * cutoff);
                left += ring[0][index & (RING - 1)] * w;
                right += ring[1][index & (RING - 1)] * w;
            }
            emit(left * static_cast<float>(cutoff), right * static_cast<float>(cutoff));
            position += step;
        }
    }
};

// --- the module ---------------------------------------------------------------------------------------------------

#define MAX_BLOCK 12
#define FIFO 4096

struct Module {
    beads::Processor processor;
    uint8_t delay_memory[768 * 1024];
    uint16_t reverb_memory[32768];
};

struct OutEvent {
    uint16_t type;
    clap_id id;
    double value;
};

struct Pearls {
    clap_plugin_t plugin;
    const clap_host_t *host;
    const clap_host_params_t *host_params;
    const clap_host_state_t *host_state;
    const clap_host_latency_t *host_latency;
    const clap_host_webview_t *host_webview;
    const clap_host_param_hovered_t *host_hovered;
    Module *module;
    double values[P_COUNT];
    bool active;
    double host_rate;

    // the running configuration, a change of quality or record restarts the module
    int media, mono;
    bool restart;
    double rate; // the module's
    uint32_t block;
    float fade; // the output's, down before a restart and up after
    uint32_t latency;

    Resampler down, up;
    beads::FloatFrame block_in[MAX_BLOCK], block_out[MAX_BLOCK];
    uint32_t block_fill;
    float fifo[2][FIFO];
    uint32_t fifo_read, fifo_write;
    uint32_t underruns;

    // Sync: the host's beats as the clock in the module's SEED jack, a gate high for the first half of each period.
    // While the transport stops nothing is patched.
    bool playing;
    double beat, beats_per_frame; // at the process call's first frame
    bool clock[MAX_BLOCK], clocked[MAX_BLOCK];
    stmlib::GateFlags previous_clock;

    // the pots' smoothing, at the block rate
    float smooth[P_COUNT];
    float input_gain, output_gain, soft_clip;

    float peak_in, peak_out;
    uint32_t gui_width, gui_height;
    OutEvent out_events[OUT_EVENTS];
    uint32_t out_read, out_write;
    uint8_t message[16 + P_COUNT * 8 + 16];
};

static Pearls *pearls_of(const clap_plugin_t *plugin) {return static_cast<Pearls *>(plugin->plugin_data);}

static int32_t index_of(clap_id id) {
    for (uint32_t index = 0; index < P_COUNT; index++) {
        if (fnv1a(PARAMS[index].key) == id) {return static_cast<int32_t>(index);}
    }
    return -1;
}

static int wanted_media(const Pearls *self) {return static_cast<int>(self->values[P_QUALITY]);}
static int wanted_mono(const Pearls *self) {return static_cast<int>(self->values[P_RECORD]);}

static void set_value(Pearls *self, uint32_t index, double value) {
    self->values[index] = constrain(PARAMS[index], value);
    if (index == P_QUALITY || index == P_RECORD) {
        self->restart = wanted_media(self) != self->media || wanted_mono(self) != self->mono;
    }
}

static double db_to_gain(double db) {return std::pow(10.0, db * 0.05);}

// The module at the settings: its rate and block, the resamplers, the output delayed by what the conversions and a
// block need, so the queue between them never runs dry. The latency is that delay (and the tape's on average).
static void configure(Pearls *self) {
    self->media = wanted_media(self);
    self->mono = wanted_mono(self);
    self->restart = false;
    beads::Processor &processor = self->module->processor;
    processor.Configure(beads::Processor::Media(self->media), self->mono != 0);
    self->rate = processor.sample_rate();
    self->block = static_cast<uint32_t>(self->rate / 4000.0);
    self->down.init(self->host_rate, self->rate);
    self->up.init(self->rate, self->host_rate);
    const double ratio = self->host_rate / self->rate;
    const uint32_t delay = static_cast<uint32_t>(self->down.delay() + std::ceil(self->block * ratio)
                                                 + std::ceil(self->up.delay() * ratio) + 2);
    // the dirty tape's wow and flutter delay everything by 1 to 123 samples, by 50 on average
    const uint32_t tape = self->media == beads::Processor::MEDIA_DIRTY_TAPE
        ? static_cast<uint32_t>(std::lround(50.0 * ratio)) : 0;
    std::memset(self->fifo, 0, sizeof(self->fifo));
    self->fifo_read = 0;
    self->fifo_write = delay;
    self->block_fill = 0;
    const bool changed = delay + tape != self->latency;
    self->latency = delay + tape;
    if (changed && self->active && self->host_latency != nullptr) {self->host_latency->changed(self->host);}
}

static void push_out_event(Pearls *self, uint16_t type, clap_id id, double value) {
    if (self->out_write - self->out_read >= OUT_EVENTS) {return;}
    self->out_events[self->out_write % OUT_EVENTS] = OutEvent{type, id, value};
    self->out_write++;
}

static void drain_out_events(Pearls *self, const clap_output_events_t *out) {
    while (self->out_read != self->out_write) {
        const OutEvent &event = self->out_events[self->out_read % OUT_EVENTS];
        if (event.type == CLAP_EVENT_PARAM_VALUE) {
            clap_event_param_value_t value = {};
            value.header = {sizeof(value), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            value.param_id = event.id;
            value.note_id = -1;
            value.port_index = -1;
            value.channel = -1;
            value.key = -1;
            value.value = event.value;
            out->try_push(out, &value.header);
        } else {
            clap_event_param_gesture_t gesture = {};
            gesture.header = {sizeof(gesture), 0, CLAP_CORE_EVENT_SPACE_ID, event.type, 0};
            gesture.param_id = event.id;
            out->try_push(out, &gesture.header);
        }
        self->out_read++;
    }
}

static void handle_event(Pearls *self, const clap_event_header_t *header) {
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID || header->type != CLAP_EVENT_PARAM_VALUE) {return;}
    const clap_event_param_value_t *event = reinterpret_cast<const clap_event_param_value_t *>(header);
    const int32_t index = index_of(event->param_id);
    if (index >= 0) {set_value(self, static_cast<uint32_t>(index), event->value);}
}

// One block of the module, as beads.cc runs it: the pots smoothed (one pole, 0.05 a block), the input's gain and its
// clipping (soft on tape, hard on digital), then the processor.
static void run_block(Pearls *self) {
    const uint32_t size = self->block;
    for (uint32_t index = 0; index < P_COUNT; index++) {
        if (PARAMS[index].stepped) {continue;}
        double target = self->values[index];
        if (index == P_PITCH && self->values[P_QUANTIZE] > 0.5) {target = std::round(target);}
        self->smooth[index] += (static_cast<float>(target) - self->smooth[index]) * 0.05f;
    }
    const float *s = self->smooth;

    const float input_gain = static_cast<float>(db_to_gain(s[P_INPUT]));
    // the clean tape clips softly as the gain pushes, the dirty tape always
    const float soft = self->media == beads::Processor::MEDIA_DIRTY_TAPE ? 1.0f
        : self->media == beads::Processor::MEDIA_CLEAN_TAPE ? clamp(2.0f * (input_gain - 0.75f), 0.0f, 1.0f) : 0.0f;
    for (uint32_t index = 0; index < size; index++) {
        const float gain = self->input_gain + (input_gain - self->input_gain) * (index + 1) / size;
        const float clip = self->soft_clip + (soft - self->soft_clip) * (index + 1) / size;
        float *channel = &self->block_in[index].l;
        for (int c = 0; c < 2; c++) {
            float x = channel[c] * gain;
            if (clip > 0.0f) {
                const float bias = 0.05f;
                const float softened = (1.0f - bias) * stmlib::SoftClip(x + bias) - bias;
                x = clamp(x, -1.0f, 1.0f);
                x += (softened - x) * clip;
            } else {
                x = clamp(x, -1.0f, 1.0f);
            }
            channel[c] = x;
        }
    }
    self->input_gain = input_gain;
    self->soft_clip = soft;

    beads::Parameters p;
    p.time = {s[P_TIME], s[P_TIME_RANDOM], 0.0f, false};
    p.size = {s[P_SIZE], s[P_SIZE_RANDOM], 0.0f, false};
    p.shape = {s[P_SHAPE], s[P_SHAPE_RANDOM], 0.0f, false};
    p.pitch = {s[P_PITCH], s[P_PITCH_RANDOM], 0.0f, false};
    p.monophonic_mode = false;
    beads::ModulatedParameter density = {s[P_DENSITY], 0.0f};
    beads::Processor::MixParameters m;
    m.feedback = clamp(s[P_FEEDBACK] * 1.005f, 0.0f, 1.0f);
    m.dry_wet = clamp(s[P_DRY_WET] * 1.005f, 0.0f, 1.0f);
    m.reverb_amount = clamp(s[P_REVERB] * 1.005f, 0.0f, 1.0f);
    m.output_gate = false;
    // the Seed button latched (the module's long press), its state at power-up: the density runs the grains
    const beads::SeedButtonState seed = beads::SEED_BUTTON_STATE_LOCKED;
    bool freeze[MAX_BLOCK];
    for (uint32_t index = 0; index < size; index++) {freeze[index] = self->values[P_FREEZE] > 0.5;}
    // the clock in the SEED jack: with it the density divides it (left of noon) or skips its pulses (right of noon),
    // the delay follows its period
    stmlib::GateFlags clock[MAX_BLOCK];
    bool patched = false;
    for (uint32_t index = 0; index < size; index++) {
        patched |= self->clocked[index];
        clock[index] = self->previous_clock = stmlib::ExtractGateFlags(self->previous_clock, self->clock[index]);
    }
    if (!patched) {self->previous_clock = stmlib::GATE_FLAG_LOW;}
    self->module->processor.Process(seed, patched ? clock : nullptr, freeze, density, p, m, self->block_in,
                                    self->block_out, size);
}

static void queue(Pearls *self, float l, float r) {
    if (self->fifo_write - self->fifo_read >= FIFO) {return;}
    self->fifo[0][self->fifo_write & (FIFO - 1)] = l;
    self->fifo[1][self->fifo_write & (FIFO - 1)] = r;
    self->fifo_write++;
}

// host frames in, the module's blocks in between, host frames out
static void render(Pearls *self, const clap_process_t *process, uint32_t from, uint32_t to) {
    const clap_audio_buffer_t &input = process->audio_inputs[0];
    const clap_audio_buffer_t &output = process->audio_outputs[0];
    const float *in_l = input.data32[0], *in_r = input.data32[input.channel_count > 1 ? 1 : 0];
    float *out_l = output.data32[0], *out_r = output.data32[output.channel_count > 1 ? 1 : 0];
    const float output_gain = static_cast<float>(db_to_gain(self->values[P_OUTPUT]));
    const float fade_step = static_cast<float>(1.0 / (0.01 * self->host_rate));
    for (uint32_t frame = from; frame < to; frame++) {
        if (self->restart && self->fade <= 0.0f) {configure(self);}
        const float l = in_l[frame], r = in_r[frame];
        self->peak_in = std::fmax(self->peak_in, std::fmax(std::fabs(l), std::fabs(r)));
        const int sync = static_cast<int>(self->values[P_SYNC]);
        const bool clocked = self->playing && sync > 0;
        bool gate = false;
        if (clocked) {
            const double phase = (self->beat + frame * self->beats_per_frame) / SYNC_BEATS[sync];
            gate = phase - std::floor(phase) < 0.5;
        }
        self->down.push(l, r, [self, clocked, gate](float a, float b) {
            self->clock[self->block_fill] = gate;
            self->clocked[self->block_fill] = clocked;
            self->block_in[self->block_fill] = {a, b};
            if (++self->block_fill < self->block) {return;}
            self->block_fill = 0;
            run_block(self);
            for (uint32_t index = 0; index < self->block; index++) {
                self->up.push(self->block_out[index].l, self->block_out[index].r,
                              [self](float x, float y) {queue(self, x, y);});
            }
        });
        float a = 0.0f, b = 0.0f;
        if (self->fifo_read != self->fifo_write) {
            a = self->fifo[0][self->fifo_read & (FIFO - 1)];
            b = self->fifo[1][self->fifo_read & (FIFO - 1)];
            self->fifo_read++;
        } else {
            self->underruns++;
        }
        self->fade = self->restart ? std::fmax(0.0f, self->fade - fade_step) : std::fmin(1.0f, self->fade + fade_step);
        self->output_gain += (output_gain - self->output_gain) * 0.002f;
        const float gain = self->output_gain * self->fade;
        out_l[frame] = a * gain;
        out_r[frame] = b * gain;
        self->peak_out = std::fmax(self->peak_out, std::fmax(std::fabs(a * gain), std::fabs(b * gain)));
    }
    (void) out_r;
}

static bool pearls_init(const clap_plugin_t *plugin) {
    Pearls *self = pearls_of(plugin);
    const clap_host_t *host = self->host;
    self->host_params = static_cast<const clap_host_params_t *>(host->get_extension(host, CLAP_EXT_PARAMS));
    self->host_state = static_cast<const clap_host_state_t *>(host->get_extension(host, CLAP_EXT_STATE));
    self->host_latency = static_cast<const clap_host_latency_t *>(host->get_extension(host, CLAP_EXT_LATENCY));
    self->host_webview = static_cast<const clap_host_webview_t *>(host->get_extension(host, CLAP_EXT_WEBVIEW));
    self->host_hovered = static_cast<const clap_host_param_hovered_t *>(host->get_extension(host, CLAP_EXT_PARAM_HOVERED));
    return true;
}

static void pearls_destroy(const clap_plugin_t *plugin) {
    Pearls *self = pearls_of(plugin);
    std::free(self->module);
    self->~Pearls();
    std::free(self);
}

static void reset_smoothing(Pearls *self) {
    for (uint32_t index = 0; index < P_COUNT; index++) {
        self->smooth[index] = static_cast<float>(self->values[index]);
    }
    self->input_gain = static_cast<float>(db_to_gain(self->values[P_INPUT]));
    self->output_gain = static_cast<float>(db_to_gain(self->values[P_OUTPUT]));
    self->soft_clip = 0.0f;
}

// the module's memory comes with the first activation, Beads' Init at every one (it clears the buffers)
static bool pearls_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames) {
    (void) min_frames; (void) max_frames;
    Pearls *self = pearls_of(plugin);
    if (self->module == nullptr) {
        self->module = static_cast<Module *>(std::calloc(1, sizeof(Module)));
        if (self->module == nullptr) {return false;}
    }
    Module *module = self->module;
    uint8_t *buffers[3] = {module->delay_memory, module->delay_memory + 256 * 1024, module->delay_memory + 512 * 1024};
    module->processor.Init(buffers, module->reverb_memory);
    self->host_rate = sample_rate;
    reset_smoothing(self);
    configure(self);
    self->fade = 1.0f;
    self->peak_in = self->peak_out = 0.0f;
    self->active = true;
    return true;
}

static void pearls_deactivate(const clap_plugin_t *plugin) {pearls_of(plugin)->active = false;}

static bool pearls_start_processing(const clap_plugin_t *plugin) {(void) plugin; return true;}

static void pearls_stop_processing(const clap_plugin_t *plugin) {(void) plugin;}

static void pearls_reset(const clap_plugin_t *plugin) {
    Pearls *self = pearls_of(plugin);
    if (self->module == nullptr) {return;}
    configure(self);
}

static clap_process_status pearls_process(const clap_plugin_t *plugin, const clap_process_t *process) {
    Pearls *self = pearls_of(plugin);
    if (self->module == nullptr || process->audio_inputs_count < 1 || process->audio_outputs_count < 1) {
        return CLAP_PROCESS_ERROR;
    }
    if (process->audio_inputs[0].channel_count < 1 || process->audio_outputs[0].channel_count < 1) {
        return CLAP_PROCESS_ERROR;
    }
    const uint32_t frames = process->frames_count;
    const clap_event_transport_t *transport = process->transport;
    const uint32_t needed = CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TEMPO;
    self->playing = transport != nullptr && (transport->flags & needed) == needed && transport->tempo > 0.0;
    if (self->playing) {
        self->beat = static_cast<double>(transport->song_pos_beats) / CLAP_BEATTIME_FACTOR;
        self->beats_per_frame = transport->tempo / 60.0 / self->host_rate;
    }
    const uint32_t count = process->in_events->size(process->in_events);
    uint32_t position = 0;
    for (uint32_t index = 0; index < count; index++) {
        const clap_event_header_t *header = process->in_events->get(process->in_events, index);
        const uint32_t time = header->time < frames ? header->time : frames;
        if (time > position) {
            render(self, process, position, time);
            position = time;
        }
        handle_event(self, header);
    }
    render(self, process, position, frames);
    drain_out_events(self, process->out_events);
    return CLAP_PROCESS_CONTINUE;
}

// --- audio ports, latency, tail -----------------------------------------------------------------------------------

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input) {(void) plugin; (void) is_input; return 1;}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
    (void) plugin; (void) is_input;
    if (index != 0) {return false;}
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t audio_ports = {audio_ports_count, audio_ports_get};

static uint32_t latency_get(const clap_plugin_t *plugin) {return pearls_of(plugin)->latency;}

static const clap_plugin_latency_t latency = {latency_get};

// grains, feedback, the frozen buffer and the reverb go on after the input stops
static uint32_t tail_get(const clap_plugin_t *plugin) {(void) plugin; return UINT32_MAX;}

static const clap_plugin_tail_t tail = {tail_get};

// --- parameters ---------------------------------------------------------------------------------------------------

static uint32_t params_count(const clap_plugin_t *plugin) {(void) plugin; return P_COUNT;}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info) {
    (void) plugin;
    if (index >= P_COUNT) {return false;}
    const Param &param = PARAMS[index];
    std::memset(info, 0, sizeof(*info));
    info->id = fnv1a(param.key);
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (param.stepped) {info->flags |= CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM;}
    info->min_value = param.low;
    info->max_value = param.high;
    info->default_value = param.value;
    std::snprintf(info->name, sizeof(info->name), "%s", param.name);
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value) {
    const int32_t index = index_of(id);
    if (index < 0) {return false;}
    *value = pearls_of(plugin)->values[index];
    return true;
}

static void format(const Param &param, double value, char *text, uint32_t size) {
    switch (param.text) {
        case Text::Percent: std::snprintf(text, size, "%.1f%%", value * 100.0); break;
        case Text::Signed: std::snprintf(text, size, "%+.0f%%", value * 100.0); break;
        case Text::Semitones: std::snprintf(text, size, "%+.2f st", value); break;
        case Text::Db: std::snprintf(text, size, "%+.1f dB", value); break;
        case Text::Labels: {
            const int step = static_cast<int>(std::lround(value - param.low));
            std::snprintf(text, size, "%s", param.labels[step < 0 ? 0 : step]);
            break;
        }
    }
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *text, uint32_t size) {
    (void) plugin;
    const int32_t index = index_of(id);
    if (index < 0 || size == 0) {return false;}
    format(PARAMS[index], value, text, size);
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

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value) {
    (void) plugin;
    const int32_t index = index_of(id);
    if (index < 0 || text == nullptr) {return false;}
    const Param &param = PARAMS[index];
    if (param.stepped) {
        for (int step = 0; step <= static_cast<int>(param.high - param.low); step++) {
            if (contains(param.labels[step], text) || contains(text, param.labels[step])) {
                *value = param.low + step;
                return true;
            }
        }
    }
    char *end = nullptr;
    double number = std::strtod(text, &end);
    if (end == text) {return false;}
    if (param.text == Text::Percent || param.text == Text::Signed) {number /= 100.0;}
    *value = constrain(param, number);
    return true;
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out) {
    Pearls *self = pearls_of(plugin);
    const uint32_t count = in->size(in);
    for (uint32_t index = 0; index < count; index++) {handle_event(self, in->get(in, index));}
    drain_out_events(self, out);
}

static const clap_plugin_params_t params = {
    params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush
};

// --- state ----------------------------------------------------------------------------------------------------------
// "PRL1", the count (u32), then per parameter its id (u32) and value (f64), little endian. Read by id: unknown ids are
// skipped, missing ones keep their defaults.

static const char MAGIC[4] = {'P', 'R', 'L', '1'};

static bool write_all(const clap_ostream_t *stream, const void *data, uint32_t size) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    while (size > 0) {
        const int64_t written = stream->write(stream, bytes, size);
        if (written <= 0) {return false;}
        bytes += written;
        size -= static_cast<uint32_t>(written);
    }
    return true;
}

static bool read_all(const clap_istream_t *stream, void *data, uint32_t size) {
    uint8_t *bytes = static_cast<uint8_t *>(data);
    while (size > 0) {
        const int64_t read = stream->read(stream, bytes, size);
        if (read <= 0) {return false;}
        bytes += read;
        size -= static_cast<uint32_t>(read);
    }
    return true;
}

static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    const Pearls *self = pearls_of(plugin);
    uint8_t data[8 + P_COUNT * 12];
    uint32_t count = 0;
    uint8_t *at = data + 8;
    for (uint32_t index = 0; index < P_COUNT; index++) {
        const clap_id id = fnv1a(PARAMS[index].key);
        std::memcpy(at, &id, 4);
        std::memcpy(at + 4, &self->values[index], 8);
        at += 12;
        count++;
    }
    std::memcpy(data, MAGIC, 4);
    std::memcpy(data + 4, &count, 4);
    return write_all(stream, data, static_cast<uint32_t>(at - data));
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    Pearls *self = pearls_of(plugin);
    uint8_t header[8];
    if (!read_all(stream, header, 8) || std::memcmp(header, MAGIC, 4) != 0) {return false;}
    uint32_t count;
    std::memcpy(&count, header + 4, 4);
    if (count > 4096) {return false;}
    for (uint32_t index = 0; index < P_COUNT; index++) {self->values[index] = PARAMS[index].value;}
    for (uint32_t entry = 0; entry < count; entry++) {
        uint8_t bytes[12];
        if (!read_all(stream, bytes, 12)) {return false;}
        clap_id id;
        double value;
        std::memcpy(&id, bytes, 4);
        std::memcpy(&value, bytes + 4, 8);
        const int32_t index = index_of(id);
        if (index >= 0 && std::isfinite(value)) {self->values[index] = constrain(PARAMS[index], value);}
    }
    set_value(self, P_QUALITY, self->values[P_QUALITY]);
    if (!self->active) {reset_smoothing(self);}
    if (self->host_params != nullptr) {self->host_params->rescan(self->host, CLAP_PARAM_RESCAN_VALUES);}
    return true;
}

static const clap_plugin_state_t state = {state_save, state_load};

// --- page -----------------------------------------------------------------------------------------------------------

static int32_t page_uri(const clap_plugin_t *plugin, char *uri, uint32_t capacity) {
    (void) plugin;
    const char *page = "/index.html";
    if (capacity > 0 && uri != nullptr) {std::snprintf(uri, capacity, "%s", page);}
    return static_cast<int32_t>(std::strlen(page) + 1);
}

static bool webview_get_resource(const clap_plugin_t *plugin, const char *path, char *mime, uint32_t capacity,
                                 const clap_ostream_t *stream) {
    (void) plugin; (void) path; (void) mime; (void) capacity; (void) stream;
    return false;
}

static void put_f32(uint8_t *at, float value) {std::memcpy(at, &value, 4);}

// STATUS: type, the parameter count, the values (f64 from 8), then f32: input and output peak since the last poll
static void send_status(Pearls *self) {
    uint8_t *message = self->message;
    std::memset(message, 0, 8);
    message[0] = PLUGIN_STATUS;
    message[1] = P_COUNT;
    std::memcpy(message + 8, self->values, P_COUNT * 8);
    uint8_t *at = message + 8 + P_COUNT * 8;
    put_f32(at, self->peak_in);
    put_f32(at + 4, self->peak_out);
    at += 8;
    self->peak_in = self->peak_out = 0.0f;
    self->host_webview->send(self->host, message, static_cast<uint32_t>(at - message));
}

static void page_param(Pearls *self, uint16_t type, const uint8_t *bytes, uint32_t size) {
    clap_id id;
    if (size < 8) {return;}
    std::memcpy(&id, bytes + 4, 4);
    const int32_t index = index_of(id);
    if (index < 0) {return;}
    if (type == CLAP_EVENT_PARAM_VALUE) {
        double value;
        if (size < 16) {return;}
        std::memcpy(&value, bytes + 8, 8);
        set_value(self, static_cast<uint32_t>(index), value);
    }
    // a drag is one undo step
    if (type == CLAP_EVENT_PARAM_GESTURE_END && self->host_state != nullptr) {
        self->host_state->mark_dirty(self->host);
    }
    push_out_event(self, type, id, self->values[index]);
    if (self->host_params != nullptr) {self->host_params->request_flush(self->host);}
}

static bool webview_receive(const clap_plugin_t *plugin, const void *buffer, uint32_t size) {
    Pearls *self = pearls_of(plugin);
    const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
    if (size < 1 || self->host_webview == nullptr) {return false;}
    switch (bytes[0]) {
        case PAGE_POLL:
            send_status(self);
            return true;
        case PAGE_PARAM_BEGIN:
            page_param(self, CLAP_EVENT_PARAM_GESTURE_BEGIN, bytes, size);
            return true;
        case PAGE_PARAM_SET:
            page_param(self, CLAP_EVENT_PARAM_VALUE, bytes, size);
            return true;
        case PAGE_PARAM_END:
            page_param(self, CLAP_EVENT_PARAM_GESTURE_END, bytes, size);
            return true;
        case PAGE_HOVER: {
            clap_id id;
            if (size < 8 || self->host_hovered == nullptr) {return false;}
            std::memcpy(&id, bytes + 4, 4);
            self->host_hovered->update(self->host, index_of(id) >= 0 ? id : CLAP_INVALID_ID);
            return true;
        }
        default:
            return false;
    }
}

static const clap_plugin_webview_t webview = {page_uri, webview_get_resource, webview_receive};

// gui: the webview, the panel at its aspect ratio, 100 to 300 %, opening at 150 %
static bool gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    (void) plugin; (void) is_floating;
    return std::strcmp(api, CLAP_WINDOW_API_WEBVIEW) == 0;
}

static bool gui_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *is_floating) {
    (void) plugin;
    *api = CLAP_WINDOW_API_WEBVIEW;
    *is_floating = false;
    return true;
}

static bool gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    return gui_is_api_supported(plugin, api, is_floating);
}

static void gui_destroy(const clap_plugin_t *plugin) {(void) plugin;}

static bool gui_set_scale(const clap_plugin_t *plugin, double scale) {(void) plugin; (void) scale; return false;}

static bool gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    const Pearls *self = pearls_of(plugin);
    *width = self->gui_width;
    *height = self->gui_height;
    return true;
}

static bool gui_can_resize(const clap_plugin_t *plugin) {(void) plugin; return true;}

static bool gui_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints) {
    (void) plugin;
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = true;
    hints->aspect_ratio_width = PANEL_WIDTH;
    hints->aspect_ratio_height = PANEL_HEIGHT;
    return true;
}

static bool gui_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    (void) plugin;
    const double wanted = std::fmin(*width / static_cast<double>(PANEL_WIDTH), static_cast<double>(*height) / PANEL_HEIGHT);
    const double zoom = std::fmin(GUI_MAX_ZOOM, std::fmax(GUI_MIN_ZOOM, wanted));
    *width = static_cast<uint32_t>(std::lround(PANEL_WIDTH * zoom));
    *height = static_cast<uint32_t>(std::lround(PANEL_HEIGHT * zoom));
    return true;
}

static bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height) {
    Pearls *self = pearls_of(plugin);
    gui_adjust_size(plugin, &width, &height);
    self->gui_width = width;
    self->gui_height = height;
    return true;
}

static bool gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window) {(void) plugin; (void) window; return true;}

static bool gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window) {(void) plugin; (void) window; return false;}

static void gui_suggest_title(const clap_plugin_t *plugin, const char *title) {(void) plugin; (void) title;}

static bool gui_show(const clap_plugin_t *plugin) {(void) plugin; return true;}

static bool gui_hide(const clap_plugin_t *plugin) {(void) plugin; return true;}

static const clap_plugin_gui_t gui = {
    gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy, gui_set_scale, gui_get_size, gui_can_resize,
    gui_get_resize_hints, gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient, gui_suggest_title, gui_show,
    gui_hide
};

// device panel: the same page at 1:1 for the panel's 176 px
static bool panel_get_width(const clap_plugin_t *plugin, uint32_t height, uint32_t *width) {
    (void) plugin;
    *width = static_cast<uint32_t>(std::lround(static_cast<double>(height) * PANEL_WIDTH / PANEL_HEIGHT));
    return true;
}

static const opendaw_plugin_device_panel_t device_panel = {page_uri, panel_get_width};

static const void *pearls_get_extension(const clap_plugin_t *plugin, const char *id) {
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

static void pearls_on_main_thread(const clap_plugin_t *plugin) {(void) plugin;}

// --- factory --------------------------------------------------------------------------------------------------------

static const char *const FEATURES[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_GRANULAR,
                                       CLAP_PLUGIN_FEATURE_DELAY, CLAP_PLUGIN_FEATURE_REVERB,
                                       OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL, OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL_WIDTH, nullptr};

static const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT,
    "studio.opendaw.pearls",
    "Pearls",
    "open-webclap",
    "https://github.com/andremichelle/open-webclap/tree/main/ports/pearls",
    "https://github.com/andremichelle/open-webclap/tree/main/ports/pearls",
    "",
    "1.0.0",
    "Granular texture effect after Beads by Emilie Gillet",
    FEATURES
};

static const clap_plugin_t *create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id) {
    (void) factory;
    if (std::strcmp(plugin_id, descriptor.id) != 0) {return nullptr;}
    void *memory = std::calloc(1, sizeof(Pearls));
    if (memory == nullptr) {return nullptr;}
    Pearls *self = new(memory) Pearls();
    self->host = host;
    for (uint32_t index = 0; index < P_COUNT; index++) {self->values[index] = PARAMS[index].value;}
    self->media = wanted_media(self);
    self->mono = wanted_mono(self);
    self->gui_width = static_cast<uint32_t>(std::lround(PANEL_WIDTH * GUI_ZOOM));
    self->gui_height = static_cast<uint32_t>(std::lround(PANEL_HEIGHT * GUI_ZOOM));
    self->plugin.desc = &descriptor;
    self->plugin.plugin_data = self;
    self->plugin.init = pearls_init;
    self->plugin.destroy = pearls_destroy;
    self->plugin.activate = pearls_activate;
    self->plugin.deactivate = pearls_deactivate;
    self->plugin.start_processing = pearls_start_processing;
    self->plugin.stop_processing = pearls_stop_processing;
    self->plugin.reset = pearls_reset;
    self->plugin.process = pearls_process;
    self->plugin.get_extension = pearls_get_extension;
    self->plugin.on_main_thread = pearls_on_main_thread;
    return &self->plugin;
}

static uint32_t plugin_count(const clap_plugin_factory_t *factory) {(void) factory; return 1;}

static const clap_plugin_descriptor_t *plugin_descriptor(const clap_plugin_factory_t *factory, uint32_t index) {
    (void) factory;
    return index == 0 ? &descriptor : nullptr;
}

static const clap_plugin_factory_t factory = {plugin_count, plugin_descriptor, create};

static bool entry_init(const char *path) {
    (void) path;
    init_sinc_table();
    return true;
}

static void entry_deinit(void) {}

static const void *entry_get_factory(const char *id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory};
