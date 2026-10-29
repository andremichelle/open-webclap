// Note Lab: the reference WebCLAP note effect (CLAP feature "note-effect"). Notes in, notes out, no audio. Four modes
// cover what a MIDI effect does to a note stream: pass it, add notes, add notes later, drop note offs. openDAW shows
// the page in its device panel (feature "opendaw:device-panel", extension "opendaw.device-panel/1").

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>
#include <clap/ext/draft/param-hovered.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

extern "C" void emscripten_notify_memory_growth(size_t index) {(void) index;}

enum : clap_id {PARAM_PRESET = 0, PARAM_MODE = 1, PARAM_DELAY = 2, PARAM_REPEATS = 3, PARAM_TRANSPOSE = 4, PARAM_COUNT = 5};
enum : uint8_t {MODE_THRU = 0, MODE_HARMONY = 1, MODE_ECHO = 2, MODE_HOLD = 3, MODE_COUNT = 4};

#define PRESET_COUNT 8
#define CHANNELS 16
#define KEYS 128
#define HARMONY_VOICES 3
#define DELAY_MIN 10.0
#define DELAY_MAX 2000.0
#define REPEATS_MIN 1
#define REPEATS_MAX 8
#define TRANSPOSE_RANGE 24
#define OUT_EVENTS 64
#define ACTIVITY 256
#define GUI_WIDTH 640
#define GUI_HEIGHT 400
#define STATE_VERSION 1
#define STATE_BYTES (8 + PRESET_COUNT * 8)

// message types, see page/index.html
#define PAGE_HELLO 1
#define PAGE_POLL 2
#define PAGE_PARAM_BEGIN 3
#define PAGE_PARAM_SET 4
#define PAGE_PARAM_END 5
#define PAGE_HOVER 6
#define PAGE_SHOW_WINDOW 7
#define PLUGIN_SNAPSHOT 1
#define PLUGIN_DELTA 2
#define ACTIVITY_OUTPUT 1
#define ACTIVITY_ON 2

#define OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL "opendaw:device-panel"
#define OPENDAW_EXT_DEVICE_PANEL "opendaw.device-panel/1"

typedef struct opendaw_plugin_device_panel {
    int32_t (*get_uri)(const clap_plugin_t *plugin, char *uri, uint32_t capacity);
    bool (*get_width)(const clap_plugin_t *plugin, uint32_t height, uint32_t *width);
} opendaw_plugin_device_panel_t;

// one preset, also its layout in the saved state (8 bytes, delay as float32 little endian)
struct Slot {
    uint8_t mode;
    uint8_t repeats;
    int8_t transpose;
    uint8_t reserved;
    float delay;
};

// what an input note produced, so its note off releases exactly that, whatever the parameters say by then
struct InputNote {
    bool active;
    uint8_t mode;
    uint8_t count;
    uint8_t repeats;
    int16_t keys[HARMONY_VOICES];
    uint32_t delay;
};

struct Scheduled {
    uint64_t time;
    bool on;
    int16_t channel;
    int16_t key;
    double velocity;
};

struct OutEvent {
    uint16_t type;
    clap_id id;
    double value;
};

struct Activity {
    uint8_t flags;
    uint8_t key;
    uint8_t velocity;
};

struct Lab {
    clap_plugin_t plugin;
    const clap_host_t *host;
    const clap_host_params_t *host_params;
    const clap_host_state_t *host_state;
    const clap_host_webview_t *host_webview;
    const clap_host_param_hovered_t *host_hovered;
    const clap_host_gui_t *host_gui;
    Slot slots[PRESET_COUNT];
    uint8_t preset;
    double sample_rate;
    uint64_t now;
    InputNote inputs[CHANNELS][KEYS];
    uint8_t held[CHANNELS][KEYS];
    uint64_t started[CHANNELS][KEYS];
    std::vector<Scheduled> scheduled;
    OutEvent out_events[OUT_EVENTS];
    uint32_t out_read, out_write;
    Activity activity[ACTIVITY];
    uint32_t activity_count;
    uint32_t changed;
    bool snapshot_due;
    bool release_due;
};

static const char *features[] = {
    CLAP_PLUGIN_FEATURE_NOTE_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL, nullptr
};

static const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT,
    "studio.opendaw.note-lab",
    "Note Lab",
    "open-webclap",
    "https://github.com/andremichelle/open-webclap/tree/main/ports/note-lab",
    "https://github.com/andremichelle/open-webclap/tree/main/ports/note-lab",
    "",
    "1.0.0",
    "The reference note effect: passes, harmonizes, echoes or holds the notes it gets",
    features
};

static const Slot default_slots[PRESET_COUNT] = {
    {MODE_THRU, 3, 0, 0, 250.0f}, {MODE_HARMONY, 3, 0, 0, 250.0f}, {MODE_ECHO, 3, 0, 0, 250.0f},
    {MODE_HOLD, 3, 0, 0, 250.0f}, {MODE_HARMONY, 3, 12, 0, 250.0f}, {MODE_ECHO, 6, 0, 0, 125.0f},
    {MODE_HARMONY, 3, -12, 0, 250.0f}, {MODE_ECHO, 2, 7, 0, 500.0f}
};

static const char *mode_names[MODE_COUNT] = {"Thru", "Harmony", "Echo", "Hold"};

static Lab *lab_of(const clap_plugin_t *plugin) {return static_cast<Lab *>(plugin->plugin_data);}

static double min_of(clap_id id) {
    switch (id) {
        case PARAM_DELAY: return DELAY_MIN;
        case PARAM_REPEATS: return REPEATS_MIN;
        case PARAM_TRANSPOSE: return -TRANSPOSE_RANGE;
        default: return 0.0;
    }
}

static double max_of(clap_id id) {
    switch (id) {
        case PARAM_PRESET: return PRESET_COUNT - 1;
        case PARAM_MODE: return MODE_COUNT - 1;
        case PARAM_DELAY: return DELAY_MAX;
        case PARAM_REPEATS: return REPEATS_MAX;
        default: return TRANSPOSE_RANGE;
    }
}

static double clamp_value(clap_id id, double value) {
    const double clamped = std::fmin(max_of(id), std::fmax(min_of(id), value));
    return id == PARAM_DELAY ? clamped : std::round(clamped);
}

static double value_of(const Lab *lab, clap_id id) {
    const Slot &slot = lab->slots[lab->preset];
    switch (id) {
        case PARAM_PRESET: return lab->preset;
        case PARAM_MODE: return slot.mode;
        case PARAM_DELAY: return slot.delay;
        case PARAM_REPEATS: return slot.repeats;
        default: return slot.transpose;
    }
}

static void record_activity(Lab *lab, uint8_t flags, int16_t key, double velocity) {
    if (lab->activity_count >= ACTIVITY) {
        lab->snapshot_due = true;
        return;
    }
    const long scaled = std::lround(velocity * 127.0);
    lab->activity[lab->activity_count++] = Activity{flags, static_cast<uint8_t>(key),
                                                    static_cast<uint8_t>(scaled < 0 ? 0 : scaled > 127 ? 127 : scaled)};
}

static void push_note(Lab *lab, const clap_output_events_t *out, uint32_t time, bool on, int16_t channel, int16_t key,
                      double velocity) {
    clap_event_note_t note = {};
    note.header = {sizeof(note), time, CLAP_CORE_EVENT_SPACE_ID, static_cast<uint16_t>(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
    note.note_id = -1;
    note.port_index = 0;
    note.channel = channel;
    note.key = key;
    note.velocity = velocity;
    out->try_push(out, &note.header);
    record_activity(lab, static_cast<uint8_t>(ACTIVITY_OUTPUT | (on ? ACTIVITY_ON : 0)), key, velocity);
}

// a note on for a key that sounds already re-attacks it, a note off ends it once every source released it
static void note_on(Lab *lab, const clap_output_events_t *out, uint64_t time, int16_t channel, int16_t key, double velocity) {
    const uint32_t offset = static_cast<uint32_t>(time - lab->now);
    if (lab->held[channel][key] > 0) {push_note(lab, out, offset, false, channel, key, 0.0);}
    push_note(lab, out, offset, true, channel, key, velocity);
    if (lab->held[channel][key] < 255) {lab->held[channel][key]++;}
    lab->started[channel][key] = time;
}

static void note_off(Lab *lab, const clap_output_events_t *out, uint64_t time, int16_t channel, int16_t key, double velocity) {
    if (lab->held[channel][key] == 0) {return;}
    if (--lab->held[channel][key] == 0) {push_note(lab, out, static_cast<uint32_t>(time - lab->now), false, channel, key, velocity);}
}

static void schedule(Lab *lab, uint64_t time, bool on, int16_t channel, int16_t key, double velocity) {
    std::vector<Scheduled> &list = lab->scheduled;
    size_t index = list.size();
    while (index > 0 && list[index - 1].time > time) {index--;}
    list.insert(list.begin() + static_cast<long>(index), Scheduled{time, on, channel, key, velocity});
}

static void fire_scheduled(Lab *lab, const clap_output_events_t *out, uint64_t until) {
    size_t fired = 0;
    for (const Scheduled &event : lab->scheduled) {
        if (event.time > until) {break;}
        if (event.on) {
            note_on(lab, out, event.time, event.channel, event.key, event.velocity);
        } else {
            note_off(lab, out, event.time, event.channel, event.key, event.velocity);
        }
        fired++;
    }
    if (fired > 0) {lab->scheduled.erase(lab->scheduled.begin(), lab->scheduled.begin() + static_cast<long>(fired));}
}

static void forget_notes(Lab *lab) {
    std::memset(lab->inputs, 0, sizeof(lab->inputs));
    std::memset(lab->held, 0, sizeof(lab->held));
    lab->scheduled.clear();
    lab->snapshot_due = true;
    lab->release_due = false;
}

static void release_all(Lab *lab, const clap_output_events_t *out, uint64_t time) {
    for (int16_t channel = 0; channel < CHANNELS; channel++) {
        for (int16_t key = 0; key < KEYS; key++) {
            if (lab->held[channel][key] == 0) {continue;}
            lab->held[channel][key] = 0;
            push_note(lab, out, static_cast<uint32_t>(time - lab->now), false, channel, key, 0.0);
        }
    }
    forget_notes(lab);
}

static void release_older(Lab *lab, const clap_output_events_t *out, uint64_t time) {
    for (int16_t channel = 0; channel < CHANNELS; channel++) {
        for (int16_t key = 0; key < KEYS; key++) {
            if (lab->held[channel][key] == 0 || lab->started[channel][key] >= time) {continue;}
            lab->held[channel][key] = 0;
            push_note(lab, out, static_cast<uint32_t>(time - lab->now), false, channel, key, 0.0);
        }
    }
}

static void input_off(Lab *lab, const clap_output_events_t *out, uint64_t time, int16_t channel, int16_t key, double velocity) {
    InputNote &input = lab->inputs[channel][key];
    if (!input.active) {return;}
    input.active = false;
    record_activity(lab, 0, key, velocity);
    if (input.mode == MODE_HOLD) {return;}
    for (uint8_t voice = 0; voice < input.count; voice++) {
        note_off(lab, out, time, channel, input.keys[voice], velocity);
        if (input.mode != MODE_ECHO) {continue;}
        for (uint8_t repeat = 1; repeat <= input.repeats; repeat++) {
            schedule(lab, time + static_cast<uint64_t>(repeat) * input.delay, false, channel, input.keys[voice], velocity);
        }
    }
}

static void input_on(Lab *lab, const clap_output_events_t *out, uint64_t time, int16_t channel, int16_t key, double velocity) {
    input_off(lab, out, time, channel, key, 0.0);
    const Slot &slot = lab->slots[lab->preset];
    InputNote &input = lab->inputs[channel][key];
    input.active = true;
    input.mode = slot.mode;
    input.count = 0;
    input.repeats = slot.repeats;
    input.delay = static_cast<uint32_t>(std::lround(slot.delay * 0.001 * lab->sample_rate));
    if (input.delay == 0) {input.delay = 1;}
    record_activity(lab, ACTIVITY_ON, key, velocity);
    const int intervals[HARMONY_VOICES] = {0, 4, 7};
    const int voices = slot.mode == MODE_HARMONY ? HARMONY_VOICES : 1;
    for (int voice = 0; voice < voices; voice++) {
        const int target = key + slot.transpose + intervals[voice];
        if (target >= 0 && target < KEYS) {input.keys[input.count++] = static_cast<int16_t>(target);}
    }
    if (slot.mode == MODE_HOLD) {release_older(lab, out, time);}
    for (uint8_t voice = 0; voice < input.count; voice++) {
        note_on(lab, out, time, channel, input.keys[voice], velocity);
        if (slot.mode != MODE_ECHO) {continue;}
        for (uint8_t repeat = 1; repeat <= input.repeats; repeat++) {
            const double decay = 1.0 - static_cast<double>(repeat) / (input.repeats + 1);
            schedule(lab, time + static_cast<uint64_t>(repeat) * input.delay, true, channel, input.keys[voice], velocity * decay);
        }
    }
}

static void push_param(const clap_output_events_t *out, uint32_t time, clap_id id, double value) {
    clap_event_param_value_t event = {};
    event.header = {sizeof(event), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    event.param_id = id;
    event.note_id = -1;
    event.port_index = -1;
    event.channel = -1;
    event.key = -1;
    event.value = value;
    out->try_push(out, &event.header);
}

static void queue_out_event(Lab *lab, uint16_t type, clap_id id, double value) {
    if (lab->out_write - lab->out_read >= OUT_EVENTS) {return;}
    lab->out_events[lab->out_write % OUT_EVENTS] = OutEvent{type, id, value};
    lab->out_write++;
}

static void drain_out_events(Lab *lab, const clap_output_events_t *out) {
    while (lab->out_read != lab->out_write) {
        const OutEvent &event = lab->out_events[lab->out_read % OUT_EVENTS];
        if (event.type == CLAP_EVENT_PARAM_VALUE) {
            push_param(out, 0, event.id, event.value);
        } else {
            clap_event_param_gesture_t gesture = {};
            gesture.header = {sizeof(gesture), 0, CLAP_CORE_EVENT_SPACE_ID, event.type, 0};
            gesture.param_id = event.id;
            out->try_push(out, &gesture.header);
        }
        lab->out_read++;
    }
}

// a preset pick changes the other values too, the host hears about them at once (out) or with the next flush (queued)
static void set_value(Lab *lab, clap_id id, double value, const clap_output_events_t *out, uint64_t time) {
    if (id >= PARAM_COUNT) {return;}
    const double clamped = clamp_value(id, value);
    if (clamped == value_of(lab, id)) {return;}
    const uint8_t mode = lab->slots[lab->preset].mode;
    Slot &slot = lab->slots[lab->preset];
    switch (id) {
        case PARAM_PRESET: lab->preset = static_cast<uint8_t>(clamped); break;
        case PARAM_MODE: slot.mode = static_cast<uint8_t>(clamped); break;
        case PARAM_DELAY: slot.delay = static_cast<float>(clamped); break;
        case PARAM_REPEATS: slot.repeats = static_cast<uint8_t>(clamped); break;
        default: slot.transpose = static_cast<int8_t>(clamped); break;
    }
    lab->changed |= 1u << id;
    if (id == PARAM_PRESET) {
        for (clap_id other = PARAM_MODE; other < PARAM_COUNT; other++) {
            lab->changed |= 1u << other;
            if (out != nullptr) {
                push_param(out, static_cast<uint32_t>(time - lab->now), other, value_of(lab, other));
            } else {
                queue_out_event(lab, CLAP_EVENT_PARAM_VALUE, other, value_of(lab, other));
            }
        }
    }
    if (lab->slots[lab->preset].mode == mode) {return;}
    if (out != nullptr) {
        release_all(lab, out, time);
    } else {
        lab->release_due = true;
    }
}

static void handle_event(Lab *lab, const clap_event_header_t *header, const clap_output_events_t *out) {
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) {return;}
    const uint64_t time = lab->now + header->time;
    switch (header->type) {
        case CLAP_EVENT_NOTE_ON:
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE: {
            const clap_event_note_t *note = reinterpret_cast<const clap_event_note_t *>(header);
            if (note->key < 0 || note->key >= KEYS) {return;}
            const int16_t channel = note->channel < 0 || note->channel >= CHANNELS ? 0 : note->channel;
            if (header->type == CLAP_EVENT_NOTE_ON && note->velocity > 0.0) {
                input_on(lab, out, time, channel, note->key, note->velocity);
            } else {
                input_off(lab, out, time, channel, note->key, note->velocity);
            }
            return;
        }
        case CLAP_EVENT_MIDI: {
            const clap_event_midi_t *midi = reinterpret_cast<const clap_event_midi_t *>(header);
            const uint8_t status = midi->data[0] & 0xf0;
            const int16_t channel = midi->data[0] & 0x0f;
            const int16_t key = midi->data[1] & 0x7f;
            if (status == 0x90 && midi->data[2] > 0) {
                input_on(lab, out, time, channel, key, midi->data[2] / 127.0);
            } else if (status == 0x80 || status == 0x90) {
                input_off(lab, out, time, channel, key, midi->data[2] / 127.0);
            } else if (status == 0xb0 && midi->data[1] == 123) {
                release_all(lab, out, time);
            } else {
                clap_event_midi_t copy = *midi;
                copy.header.time = header->time;
                copy.port_index = 0;
                out->try_push(out, &copy.header);
            }
            return;
        }
        case CLAP_EVENT_PARAM_VALUE: {
            const clap_event_param_value_t *param = reinterpret_cast<const clap_event_param_value_t *>(header);
            set_value(lab, param->param_id, param->value, out, time);
            return;
        }
        default:
            return;
    }
}

static bool lab_init(const clap_plugin_t *plugin) {
    Lab *lab = lab_of(plugin);
    const clap_host_t *host = lab->host;
    lab->host_params = static_cast<const clap_host_params_t *>(host->get_extension(host, CLAP_EXT_PARAMS));
    lab->host_state = static_cast<const clap_host_state_t *>(host->get_extension(host, CLAP_EXT_STATE));
    lab->host_webview = static_cast<const clap_host_webview_t *>(host->get_extension(host, CLAP_EXT_WEBVIEW));
    lab->host_hovered = static_cast<const clap_host_param_hovered_t *>(host->get_extension(host, CLAP_EXT_PARAM_HOVERED));
    lab->host_gui = static_cast<const clap_host_gui_t *>(host->get_extension(host, CLAP_EXT_GUI));
    return true;
}

static void lab_destroy(const clap_plugin_t *plugin) {
    Lab *lab = lab_of(plugin);
    lab->~Lab();
    std::free(lab);
}

static bool lab_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames) {
    (void) min_frames; (void) max_frames;
    Lab *lab = lab_of(plugin);
    lab->sample_rate = sample_rate;
    lab->now = 0;
    lab->scheduled.reserve(1024);
    forget_notes(lab);
    return true;
}

// the host releases what it got from us when it resets, deactivates or stops us, we only forget
static void lab_deactivate(const clap_plugin_t *plugin) {forget_notes(lab_of(plugin));}

static bool lab_start_processing(const clap_plugin_t *plugin) {(void) plugin; return true;}

static void lab_stop_processing(const clap_plugin_t *plugin) {forget_notes(lab_of(plugin));}

static void lab_reset(const clap_plugin_t *plugin) {forget_notes(lab_of(plugin));}

static clap_process_status lab_process(const clap_plugin_t *plugin, const clap_process_t *process) {
    Lab *lab = lab_of(plugin);
    const uint32_t frames = process->frames_count;
    const clap_output_events_t *out = process->out_events;
    drain_out_events(lab, out);
    if (lab->release_due) {
        lab->release_due = false;
        release_all(lab, out, lab->now);
    }
    const uint32_t count = process->in_events->size(process->in_events);
    for (uint32_t index = 0; index < count; index++) {
        const clap_event_header_t *header = process->in_events->get(process->in_events, index);
        if (header->time >= frames) {break;}
        fire_scheduled(lab, out, lab->now + header->time);
        handle_event(lab, header, out);
    }
    if (frames > 0) {fire_scheduled(lab, out, lab->now + frames - 1);}
    lab->now += frames;
    return CLAP_PROCESS_CONTINUE;
}

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input) {(void) plugin; (void) is_input; return 0;}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
    (void) plugin; (void) index; (void) is_input; (void) info;
    return false;
}

static const clap_plugin_audio_ports_t audio_ports = {audio_ports_count, audio_ports_get};

static uint32_t note_ports_count(const clap_plugin_t *plugin, bool is_input) {(void) plugin; (void) is_input; return 1;}

static bool note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info) {
    (void) plugin;
    if (index > 0) {return false;}
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "%s", is_input ? "Notes In" : "Notes Out");
    return true;
}

static const clap_plugin_note_ports_t note_ports = {note_ports_count, note_ports_get};

static uint32_t params_count(const clap_plugin_t *plugin) {(void) plugin; return PARAM_COUNT;}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info) {
    (void) plugin;
    if (index >= PARAM_COUNT) {return false;}
    static const char *names[PARAM_COUNT] = {"Preset", "Mode", "Delay", "Repeats", "Transpose"};
    std::memset(info, 0, sizeof(*info));
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (index != PARAM_DELAY) {info->flags |= CLAP_PARAM_IS_STEPPED;}
    if (index == PARAM_PRESET || index == PARAM_MODE) {info->flags |= CLAP_PARAM_IS_ENUM;}
    std::snprintf(info->name, sizeof(info->name), "%s", names[index]);
    info->min_value = min_of(index);
    info->max_value = max_of(index);
    const Slot &slot = default_slots[0];
    info->default_value = index == PARAM_PRESET ? 0.0 : index == PARAM_MODE ? slot.mode : index == PARAM_DELAY
        ? slot.delay : index == PARAM_REPEATS ? slot.repeats : slot.transpose;
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value) {
    if (id >= PARAM_COUNT) {return false;}
    *value = value_of(lab_of(plugin), id);
    return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *text, uint32_t size) {
    (void) plugin;
    if (id >= PARAM_COUNT || size == 0) {return false;}
    const double clamped = clamp_value(id, value);
    switch (id) {
        case PARAM_PRESET: std::snprintf(text, size, "%c", 'A' + static_cast<int>(clamped)); break;
        case PARAM_MODE: std::snprintf(text, size, "%s", mode_names[static_cast<int>(clamped)]); break;
        case PARAM_DELAY: std::snprintf(text, size, "%.0f ms", clamped); break;
        case PARAM_REPEATS: std::snprintf(text, size, "%d", static_cast<int>(clamped)); break;
        default: std::snprintf(text, size, "%+d st", static_cast<int>(clamped)); break;
    }
    return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value) {
    (void) plugin;
    if (id >= PARAM_COUNT || text == nullptr) {return false;}
    if (id == PARAM_PRESET && text[0] != '\0') {
        const char letter = text[0] >= 'a' && text[0] <= 'z' ? static_cast<char>(text[0] - 32) : text[0];
        if (letter >= 'A' && letter < 'A' + PRESET_COUNT) {*value = letter - 'A'; return true;}
    }
    if (id == PARAM_MODE) {
        for (int mode = 0; mode < MODE_COUNT; mode++) {
            if (std::strncmp(text, mode_names[mode], std::strlen(mode_names[mode])) == 0) {*value = mode; return true;}
        }
    }
    *value = clamp_value(id, std::atof(text));
    return true;
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out) {
    Lab *lab = lab_of(plugin);
    drain_out_events(lab, out);
    const uint32_t count = in->size(in);
    for (uint32_t index = 0; index < count; index++) {
        const clap_event_header_t *header = in->get(in, index);
        if (header->space_id != CLAP_CORE_EVENT_SPACE_ID || header->type != CLAP_EVENT_PARAM_VALUE) {continue;}
        const clap_event_param_value_t *param = reinterpret_cast<const clap_event_param_value_t *>(header);
        set_value(lab, param->param_id, param->value, out, lab->now);
    }
}

static const clap_plugin_params_t params = {
    params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush
};

// "NLAB", version, preset, two reserved bytes, then the eight slots: a value edit changes at most four bytes
static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    const Lab *lab = lab_of(plugin);
    uint8_t bytes[STATE_BYTES] = {'N', 'L', 'A', 'B', STATE_VERSION, lab->preset, 0, 0};
    std::memcpy(bytes + 8, lab->slots, sizeof(lab->slots));
    int64_t written = 0;
    while (written < STATE_BYTES) {
        const int64_t result = stream->write(stream, bytes + written, STATE_BYTES - written);
        if (result <= 0) {return false;}
        written += result;
    }
    return true;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    Lab *lab = lab_of(plugin);
    uint8_t bytes[STATE_BYTES];
    int64_t read = 0;
    while (read < STATE_BYTES) {
        const int64_t result = stream->read(stream, bytes + read, STATE_BYTES - read);
        if (result <= 0) {break;}
        read += result;
    }
    if (read < STATE_BYTES || std::memcmp(bytes, "NLAB", 4) != 0 || bytes[4] != STATE_VERSION) {return false;}
    const uint8_t mode = lab->slots[lab->preset].mode;
    std::memcpy(lab->slots, bytes + 8, sizeof(lab->slots));
    for (Slot &slot : lab->slots) {
        slot.mode = static_cast<uint8_t>(clamp_value(PARAM_MODE, slot.mode));
        slot.repeats = static_cast<uint8_t>(clamp_value(PARAM_REPEATS, slot.repeats));
        slot.transpose = static_cast<int8_t>(clamp_value(PARAM_TRANSPOSE, slot.transpose));
        slot.delay = static_cast<float>(clamp_value(PARAM_DELAY, std::isfinite(slot.delay) ? slot.delay : 250.0));
        slot.reserved = 0;
    }
    lab->preset = static_cast<uint8_t>(clamp_value(PARAM_PRESET, bytes[5]));
    if (lab->slots[lab->preset].mode != mode) {lab->release_due = true;}
    lab->changed = (1u << PARAM_COUNT) - 1;
    return true;
}

static const clap_plugin_state_t state = {state_save, state_load};

static int32_t write_uri(const char *page, uint32_t length, char *uri, uint32_t capacity) {
    if (capacity > 0 && uri != nullptr) {std::snprintf(uri, capacity, "%s", page);}
    return static_cast<int32_t>(length);
}

static int32_t webview_get_uri(const clap_plugin_t *plugin, char *uri, uint32_t capacity) {
    (void) plugin;
    static const char page[] = "/index.html";
    return write_uri(page, sizeof(page), uri, capacity);
}

static bool webview_get_resource(const clap_plugin_t *plugin, const char *path, char *mime, uint32_t capacity,
                                 const clap_ostream_t *stream) {
    (void) plugin; (void) path; (void) mime; (void) capacity; (void) stream;
    return false;
}

static void put_held(const Lab *lab, uint8_t *bits, bool output) {
    for (int channel = 0; channel < CHANNELS; channel++) {
        for (int key = 0; key < KEYS; key++) {
            const bool on = output ? lab->held[channel][key] > 0 : lab->inputs[channel][key].active;
            if (on) {bits[key >> 3] |= static_cast<uint8_t>(1 << (key & 7));}
        }
    }
}

// everything: the values and which keys are down on either side
static void send_snapshot(Lab *lab) {
    uint8_t message[8 + PARAM_COUNT * 8 + 32];
    std::memset(message, 0, sizeof(message));
    message[0] = PLUGIN_SNAPSHOT;
    for (clap_id id = 0; id < PARAM_COUNT; id++) {
        const double value = value_of(lab, id);
        std::memcpy(message + 8 + id * 8, &value, 8);
    }
    put_held(lab, message + 8 + PARAM_COUNT * 8, false);
    put_held(lab, message + 8 + PARAM_COUNT * 8 + 16, true);
    lab->host_webview->send(lab->host, message, sizeof(message));
    lab->changed = 0;
    lab->activity_count = 0;
    lab->snapshot_due = false;
}

// what changed since the last answer: a mask, the changed values, the note events, nothing at all if nothing did
static void send_delta(Lab *lab) {
    if (lab->snapshot_due) {
        send_snapshot(lab);
        return;
    }
    if (lab->changed == 0 && lab->activity_count == 0) {return;}
    uint8_t message[4 + PARAM_COUNT * 8 + ACTIVITY * 4];
    message[0] = PLUGIN_DELTA;
    message[1] = static_cast<uint8_t>(lab->changed);
    const uint16_t count = static_cast<uint16_t>(lab->activity_count);
    std::memcpy(message + 2, &count, 2);
    uint32_t size = 4;
    for (clap_id id = 0; id < PARAM_COUNT; id++) {
        if ((lab->changed & (1u << id)) == 0) {continue;}
        const double value = value_of(lab, id);
        std::memcpy(message + size, &value, 8);
        size += 8;
    }
    for (uint32_t index = 0; index < lab->activity_count; index++) {
        const Activity &activity = lab->activity[index];
        message[size++] = activity.flags;
        message[size++] = activity.key;
        message[size++] = activity.velocity;
        message[size++] = 0;
    }
    lab->host_webview->send(lab->host, message, size);
    lab->changed = 0;
    lab->activity_count = 0;
}

static void page_param(Lab *lab, uint16_t type, const uint8_t *bytes, uint32_t size) {
    clap_id id;
    if (size < 8) {return;}
    std::memcpy(&id, bytes + 4, 4);
    if (id >= PARAM_COUNT) {return;}
    if (type == CLAP_EVENT_PARAM_VALUE) {
        double value;
        if (size < 16) {return;}
        std::memcpy(&value, bytes + 8, 8);
        set_value(lab, id, value, nullptr, lab->now);
        if (lab->host_state != nullptr) {lab->host_state->mark_dirty(lab->host);}
    }
    queue_out_event(lab, type, id, value_of(lab, id));
    if (lab->host_params != nullptr) {lab->host_params->request_flush(lab->host);}
}

static bool webview_receive(const clap_plugin_t *plugin, const void *buffer, uint32_t size) {
    Lab *lab = lab_of(plugin);
    const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
    if (size < 1 || lab->host_webview == nullptr) {return false;}
    switch (bytes[0]) {
        case PAGE_HELLO:
            send_snapshot(lab);
            return true;
        case PAGE_POLL:
            send_delta(lab);
            return true;
        case PAGE_PARAM_BEGIN:
            page_param(lab, CLAP_EVENT_PARAM_GESTURE_BEGIN, bytes, size);
            return true;
        case PAGE_PARAM_SET:
            page_param(lab, CLAP_EVENT_PARAM_VALUE, bytes, size);
            return true;
        case PAGE_PARAM_END:
            page_param(lab, CLAP_EVENT_PARAM_GESTURE_END, bytes, size);
            return true;
        case PAGE_HOVER: {
            clap_id id;
            if (size < 8 || lab->host_hovered == nullptr) {return false;}
            std::memcpy(&id, bytes + 4, 4);
            lab->host_hovered->update(lab->host, id < PARAM_COUNT ? id : CLAP_INVALID_ID);
            return true;
        }
        case PAGE_SHOW_WINDOW:
            return lab->host_gui != nullptr && lab->host_gui->request_show(lab->host);
        default:
            return false;
    }
}

static const clap_plugin_webview_t webview = {webview_get_uri, webview_get_resource, webview_receive};

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
    (void) plugin;
    *width = GUI_WIDTH;
    *height = GUI_HEIGHT;
    return true;
}

static bool gui_can_resize(const clap_plugin_t *plugin) {(void) plugin; return false;}

static bool gui_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints) {(void) plugin; (void) hints; return false;}

static bool gui_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    (void) plugin;
    *width = GUI_WIDTH;
    *height = GUI_HEIGHT;
    return true;
}

static bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height) {
    (void) plugin;
    return width == GUI_WIDTH && height == GUI_HEIGHT;
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

static int32_t panel_get_uri(const clap_plugin_t *plugin, char *uri, uint32_t capacity) {
    (void) plugin;
    static const char page[] = "/index.html?panel";
    return write_uri(page, sizeof(page), uri, capacity);
}

static bool panel_get_width(const clap_plugin_t *plugin, uint32_t height, uint32_t *width) {
    (void) plugin;
    *width = height * 2;
    return true;
}

static const opendaw_plugin_device_panel_t device_panel = {panel_get_uri, panel_get_width};

static const void *lab_get_extension(const clap_plugin_t *plugin, const char *id) {
    (void) plugin;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {return &audio_ports;}
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) {return &note_ports;}
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {return &params;}
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {return &state;}
    if (std::strcmp(id, CLAP_EXT_WEBVIEW) == 0) {return &webview;}
    if (std::strcmp(id, CLAP_EXT_GUI) == 0) {return &gui;}
    if (std::strcmp(id, OPENDAW_EXT_DEVICE_PANEL) == 0) {return &device_panel;}
    return nullptr;
}

static void lab_on_main_thread(const clap_plugin_t *plugin) {(void) plugin;}

static const clap_plugin_t *create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id) {
    (void) factory;
    if (std::strcmp(plugin_id, descriptor.id) != 0) {return nullptr;}
    void *memory = std::calloc(1, sizeof(Lab));
    if (memory == nullptr) {return nullptr;}
    Lab *lab = new(memory) Lab();
    lab->host = host;
    std::memcpy(lab->slots, default_slots, sizeof(default_slots));
    lab->sample_rate = 48000.0;
    lab->plugin.desc = &descriptor;
    lab->plugin.plugin_data = lab;
    lab->plugin.init = lab_init;
    lab->plugin.destroy = lab_destroy;
    lab->plugin.activate = lab_activate;
    lab->plugin.deactivate = lab_deactivate;
    lab->plugin.start_processing = lab_start_processing;
    lab->plugin.stop_processing = lab_stop_processing;
    lab->plugin.reset = lab_reset;
    lab->plugin.process = lab_process;
    lab->plugin.get_extension = lab_get_extension;
    lab->plugin.on_main_thread = lab_on_main_thread;
    return &lab->plugin;
}

static uint32_t plugin_count(const clap_plugin_factory_t *factory) {(void) factory; return 1;}

static const clap_plugin_descriptor_t *plugin_descriptor(const clap_plugin_factory_t *factory, uint32_t index) {
    (void) factory;
    return index == 0 ? &descriptor : nullptr;
}

static const clap_plugin_factory_t factory = {plugin_count, plugin_descriptor, create};

static bool entry_init(const char *path) {(void) path; return true;}

static void entry_deinit(void) {}

static const void *entry_get_factory(const char *id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory};
