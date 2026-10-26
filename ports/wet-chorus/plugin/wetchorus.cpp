// WetChorus as a WebCLAP: the unchanged DSP from upstream/ (Ronald Klarenbeek, MIT) behind a CLAP audio effect with a
// webview. openDAW shows the panel scaled into its device panel (feature "opendaw:device-panel", extension
// "opendaw.device-panel/1"), every host shows it in the window.

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>
#include <clap/ext/draft/param-hovered.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include "../upstream/chorusengine.h"
#include "../upstream/meterlevel.h"

using Yonie::ChorusEngine;
namespace ChorusRange = Yonie::ChorusRange;
namespace Meter = Yonie::Meter;

extern "C" void emscripten_notify_memory_growth(size_t index) {(void) index;}

enum : clap_id {PARAM_SPEED = 0, PARAM_DEPTH = 1, PARAM_MODE = 2, PARAM_COUNT = 3};

// host-facing ranges: the printed 0..10 scale, the button, the phase in degrees, snapped to the knob detents inside
static const double kSpeedMax = 10.0;
static const double kModeMax = 180.0;

#define OUT_EVENTS 64
#define GUI_WIDTH 768
#define GUI_HEIGHT 512
#define GUI_MIN_SCALE 0.5
#define GUI_MAX_SCALE 1.25
#define STATE_VERSION 1
#define METER_DECAY 0.9995f

// message types, see page/index.html
#define PAGE_POLL 1
#define PAGE_PARAM_BEGIN 2
#define PAGE_PARAM_SET 3
#define PAGE_PARAM_END 4
#define PAGE_HOVER 7
#define PLUGIN_STATUS 1
#define STATUS_BYTES 40

#define OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL "opendaw:device-panel"
#define OPENDAW_EXT_DEVICE_PANEL "opendaw.device-panel/1"

typedef struct opendaw_plugin_device_panel {
    int32_t (*get_uri)(const clap_plugin_t *plugin, char *uri, uint32_t capacity);
    bool (*get_width)(const clap_plugin_t *plugin, uint32_t height, uint32_t *width);
} opendaw_plugin_device_panel_t;

struct OutEvent {
    uint16_t type;
    clap_id id;
    double value;
};

struct Chorus {
    clap_plugin_t plugin;
    const clap_host_t *host;
    const clap_host_params_t *host_params;
    const clap_host_state_t *host_state;
    const clap_host_webview_t *host_webview;
    const clap_host_param_hovered_t *host_hovered;
    ChorusEngine engine;
    double values[PARAM_COUNT];
    bool settings_dirty;
    float in_peak, out_peak_left, out_peak_right;
    uint32_t gui_width, gui_height;
    OutEvent out_events[OUT_EVENTS];
    uint32_t out_read, out_write;
};

static const char *features[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_CHORUS, CLAP_PLUGIN_FEATURE_STEREO,
    OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL, nullptr
};

static const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT,
    "studio.opendaw.commons.wet-chorus",
    "WetChorus",
    "WET VST / openDAW Commons",
    "https://github.com/andremichelle/opendaw-commons/tree/main/wclaps/wet-chorus",
    "https://github.com/yonie/WetChorus",
    "",
    "1.0.0",
    "A bucket-brigade stereo chorus modelled on a 1970s guitar combo",
    features
};

static double default_value(clap_id id) {
    switch (id) {
        case PARAM_SPEED: return kSpeedMax * ChorusRange::kDefaultSpeed / (ChorusRange::kSteps - 1);
        case PARAM_DEPTH: return ChorusRange::kDefaultDepth;
        default: return kModeMax * ChorusRange::kDefaultMode / (ChorusRange::kSteps - 1);
    }
}

static int knob_step(double value, double max) {
    const int step = static_cast<int>(std::lround(value / max * (ChorusRange::kSteps - 1)));
    return step < 0 ? 0 : step > ChorusRange::kSteps - 1 ? ChorusRange::kSteps - 1 : step;
}

static double clamp_value(clap_id id, double value) {
    const double max = id == PARAM_SPEED ? kSpeedMax : id == PARAM_DEPTH ? 1.0 : kModeMax;
    return std::fmin(max, std::fmax(0.0, value));
}

static ChorusEngine::Settings settings_of(const Chorus *chorus) {
    ChorusEngine::Settings settings;
    settings.speed = knob_step(chorus->values[PARAM_SPEED], kSpeedMax);
    settings.depth = chorus->values[PARAM_DEPTH] >= 0.5 ? ChorusRange::kDeep : ChorusRange::kShallow;
    settings.mode = knob_step(chorus->values[PARAM_MODE], kModeMax);
    return settings;
}

static void set_value(Chorus *chorus, clap_id id, double value) {
    if (id >= PARAM_COUNT) {return;}
    chorus->values[id] = clamp_value(id, value);
    chorus->settings_dirty = true;
}

static void push_out_event(Chorus *chorus, uint16_t type, clap_id id, double value) {
    if (chorus->out_write - chorus->out_read >= OUT_EVENTS) {return;}
    chorus->out_events[chorus->out_write % OUT_EVENTS] = OutEvent{type, id, value};
    chorus->out_write++;
}

static void drain_out_events(Chorus *chorus, const clap_output_events_t *out) {
    while (chorus->out_read != chorus->out_write) {
        const OutEvent &event = chorus->out_events[chorus->out_read % OUT_EVENTS];
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
        chorus->out_read++;
    }
}

static void handle_event(Chorus *chorus, const clap_event_header_t *header) {
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID || header->type != CLAP_EVENT_PARAM_VALUE) {return;}
    const clap_event_param_value_t *param = reinterpret_cast<const clap_event_param_value_t *>(header);
    set_value(chorus, param->param_id, param->value);
}

static void apply_settings(Chorus *chorus) {
    if (!chorus->settings_dirty) {return;}
    chorus->settings_dirty = false;
    chorus->engine.setSettings(settings_of(chorus));
}

static Chorus *chorus_of(const clap_plugin_t *plugin) {return static_cast<Chorus *>(plugin->plugin_data);}

static bool chorus_init(const clap_plugin_t *plugin) {
    Chorus *chorus = chorus_of(plugin);
    const clap_host_t *host = chorus->host;
    chorus->host_params = static_cast<const clap_host_params_t *>(host->get_extension(host, CLAP_EXT_PARAMS));
    chorus->host_state = static_cast<const clap_host_state_t *>(host->get_extension(host, CLAP_EXT_STATE));
    chorus->host_webview = static_cast<const clap_host_webview_t *>(host->get_extension(host, CLAP_EXT_WEBVIEW));
    chorus->host_hovered = static_cast<const clap_host_param_hovered_t *>(host->get_extension(host, CLAP_EXT_PARAM_HOVERED));
    return true;
}

static void chorus_destroy(const clap_plugin_t *plugin) {
    Chorus *chorus = chorus_of(plugin);
    chorus->~Chorus();
    std::free(chorus);
}

static bool chorus_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames) {
    (void) min_frames;
    Chorus *chorus = chorus_of(plugin);
    chorus->engine.prepare(sample_rate, static_cast<int>(max_frames));
    chorus->engine.setSettings(settings_of(chorus));
    chorus->settings_dirty = false;
    chorus->engine.reset();
    chorus->in_peak = chorus->out_peak_left = chorus->out_peak_right = 0.0f;
    return true;
}

static void chorus_deactivate(const clap_plugin_t *plugin) {(void) plugin;}

static bool chorus_start_processing(const clap_plugin_t *plugin) {(void) plugin; return true;}

static void chorus_stop_processing(const clap_plugin_t *plugin) {(void) plugin;}

static void chorus_reset(const clap_plugin_t *plugin) {chorus_of(plugin)->engine.reset();}

static void update_peak(float sample, float &peak) {
    const float level = std::fabs(sample);
    peak = level > peak ? level : peak * METER_DECAY;
}

static void render(Chorus *chorus, const float *in_left, const float *in_right, float *out_left, float *out_right,
                   uint32_t from, uint32_t to) {
    if (to <= from) {return;}
    const int frames = static_cast<int>(to - from);
    // IN reads the mono sum, what the circuit is fed (upstream wetchorusprocessor.cpp)
    for (uint32_t frame = from; frame < to; frame++) {update_peak(0.5f * (in_left[frame] + in_right[frame]), chorus->in_peak);}
    chorus->engine.processStereo(in_left + from, in_right + from, out_left + from, out_right + from, frames);
    for (uint32_t frame = from; frame < to; frame++) {
        update_peak(out_left[frame], chorus->out_peak_left);
        update_peak(out_right[frame], chorus->out_peak_right);
    }
}

static clap_process_status chorus_process(const clap_plugin_t *plugin, const clap_process_t *process) {
    Chorus *chorus = chorus_of(plugin);
    if (process->audio_inputs_count < 1 || process->audio_outputs_count < 1) {return CLAP_PROCESS_ERROR;}
    const clap_audio_buffer_t &input = process->audio_inputs[0];
    const clap_audio_buffer_t &output = process->audio_outputs[0];
    if (input.channel_count < 1 || output.channel_count < 2) {return CLAP_PROCESS_ERROR;}
    const float *in_left = input.data32[0];
    const float *in_right = input.channel_count > 1 ? input.data32[1] : input.data32[0];
    float *out_left = output.data32[0];
    float *out_right = output.data32[1];
    const uint32_t frames = process->frames_count;
    const uint32_t count = process->in_events->size(process->in_events);
    uint32_t position = 0;
    for (uint32_t index = 0; index < count; index++) {
        const clap_event_header_t *header = process->in_events->get(process->in_events, index);
        const uint32_t time = header->time < frames ? header->time : frames;
        if (time > position) {
            apply_settings(chorus);
            render(chorus, in_left, in_right, out_left, out_right, position, time);
            position = time;
        }
        handle_event(chorus, header);
    }
    apply_settings(chorus);
    render(chorus, in_left, in_right, out_left, out_right, position, frames);
    drain_out_events(chorus, process->out_events);
    return CLAP_PROCESS_CONTINUE;
}

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input) {(void) plugin; (void) is_input; return 1;}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
    (void) plugin;
    if (index > 0) {return false;}
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "%s", is_input ? "Stereo In" : "Stereo Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t audio_ports = {audio_ports_count, audio_ports_get};

static uint32_t params_count(const clap_plugin_t *plugin) {(void) plugin; return PARAM_COUNT;}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info) {
    (void) plugin;
    if (index >= PARAM_COUNT) {return false;}
    std::memset(info, 0, sizeof(*info));
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    info->min_value = 0.0;
    info->default_value = default_value(index);
    switch (index) {
        case PARAM_SPEED:
            std::snprintf(info->name, sizeof(info->name), "Speed");
            info->max_value = kSpeedMax;
            break;
        case PARAM_DEPTH:
            std::snprintf(info->name, sizeof(info->name), "Depth");
            info->flags |= CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM;
            info->max_value = 1.0;
            break;
        default:
            std::snprintf(info->name, sizeof(info->name), "Mode");
            info->max_value = kModeMax;
            break;
    }
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value) {
    if (id >= PARAM_COUNT) {return false;}
    *value = chorus_of(plugin)->values[id];
    return true;
}

// the texts of upstream wetchoruscontroller.cpp
static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *text, uint32_t size) {
    (void) plugin;
    if (id >= PARAM_COUNT || size == 0) {return false;}
    if (id == PARAM_SPEED) {
        const int step = knob_step(value, kSpeedMax);
        std::snprintf(text, size, "%.1f  (%.2f Hz)", ChorusRange::stepScale(step), ChorusRange::speedHz(step));
    } else if (id == PARAM_DEPTH) {
        std::snprintf(text, size, "%s", value >= 0.5 ? "DEEP" : "SHALLOW");
    } else {
        const int step = knob_step(value, kModeMax);
        if (step == 0) {
            std::snprintf(text, size, "VIB");
        } else if (step == ChorusRange::kSteps - 1) {
            std::snprintf(text, size, "CHORUS");
        } else {
            std::snprintf(text, size, "%d deg", static_cast<int>(ChorusRange::modePhase(step) * 360.0 + 0.5));
        }
    }
    return true;
}

static bool starts_with(const char *text, const char *prefix) {
    for (; *prefix != '\0'; text++, prefix++) {
        const char letter = *text >= 'a' && *text <= 'z' ? static_cast<char>(*text - 32) : *text;
        if (letter != *prefix) {return false;}
    }
    return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value) {
    (void) plugin;
    if (id >= PARAM_COUNT || text == nullptr) {return false;}
    if (id == PARAM_DEPTH) {
        if (starts_with(text, "DEEP")) {*value = 1.0; return true;}
        if (starts_with(text, "SHALLOW")) {*value = 0.0; return true;}
        *value = std::atof(text) >= 0.5 ? 1.0 : 0.0;
        return true;
    }
    if (id == PARAM_MODE && starts_with(text, "VIB")) {*value = 0.0; return true;}
    if (id == PARAM_MODE && starts_with(text, "CHORUS")) {*value = kModeMax; return true;}
    const double max = id == PARAM_SPEED ? kSpeedMax : kModeMax;
    *value = max * knob_step(std::atof(text), max) / (ChorusRange::kSteps - 1);
    return true;
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out) {
    Chorus *chorus = chorus_of(plugin);
    const uint32_t count = in->size(in);
    for (uint32_t index = 0; index < count; index++) {handle_event(chorus, in->get(in, index));}
    drain_out_events(chorus, out);
}

static const clap_plugin_params_t params = {
    params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush
};

// state as upstream writes it: version, the step count the indices are counted on, speed, depth, mode (int32 LE)
static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    const ChorusEngine::Settings settings = settings_of(chorus_of(plugin));
    const int32_t words[5] = {STATE_VERSION, ChorusRange::kSteps, settings.speed, settings.depth, settings.mode};
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(words);
    int64_t written = 0;
    while (written < static_cast<int64_t>(sizeof(words))) {
        const int64_t result = stream->write(stream, bytes + written, sizeof(words) - written);
        if (result <= 0) {return false;}
        written += result;
    }
    return true;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    Chorus *chorus = chorus_of(plugin);
    int32_t words[5] = {0, 0, 0, 0, 0};
    uint8_t *bytes = reinterpret_cast<uint8_t *>(words);
    int64_t read = 0;
    while (read < static_cast<int64_t>(sizeof(words))) {
        const int64_t result = stream->read(stream, bytes + read, sizeof(words) - read);
        if (result <= 0) {break;}
        read += result;
    }
    if (read < 4) {return false;}
    const int saved_steps = read >= 8 && words[1] > 1 ? words[1] : ChorusRange::kSteps;
    ChorusEngine::Settings settings;
    if (read >= 12) {settings.speed = ChorusRange::rescaleStep(words[2], saved_steps);}
    if (read >= 16) {settings.depth = words[3] < 0 ? 0 : words[3] > ChorusRange::kDepthCount - 1 ? ChorusRange::kDepthCount - 1 : words[3];}
    if (read >= 20) {settings.mode = ChorusRange::rescaleStep(words[4], saved_steps);}
    chorus->values[PARAM_SPEED] = kSpeedMax * settings.speed / (ChorusRange::kSteps - 1);
    chorus->values[PARAM_DEPTH] = settings.depth;
    chorus->values[PARAM_MODE] = kModeMax * settings.mode / (ChorusRange::kSteps - 1);
    chorus->settings_dirty = true;
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

static void send_status(Chorus *chorus) {
    uint8_t message[STATUS_BYTES];
    std::memset(message, 0, sizeof(message));
    message[0] = PLUGIN_STATUS;
    const float meters[3] = {
        Meter::encode(chorus->in_peak), Meter::encode(chorus->out_peak_left), Meter::encode(chorus->out_peak_right)
    };
    std::memcpy(message + 4, meters, sizeof(meters));
    std::memcpy(message + 16, chorus->values, sizeof(chorus->values));
    chorus->host_webview->send(chorus->host, message, sizeof(message));
}

static void page_param(Chorus *chorus, uint16_t type, const uint8_t *bytes, uint32_t size) {
    clap_id id;
    if (size < 8) {return;}
    std::memcpy(&id, bytes + 4, 4);
    if (id >= PARAM_COUNT) {return;}
    if (type == CLAP_EVENT_PARAM_VALUE) {
        double value;
        if (size < 16) {return;}
        std::memcpy(&value, bytes + 8, 8);
        set_value(chorus, id, value);
        if (chorus->host_state != nullptr) {chorus->host_state->mark_dirty(chorus->host);}
    }
    push_out_event(chorus, type, id, chorus->values[id]);
    if (chorus->host_params != nullptr) {chorus->host_params->request_flush(chorus->host);}
}

static bool webview_receive(const clap_plugin_t *plugin, const void *buffer, uint32_t size) {
    Chorus *chorus = chorus_of(plugin);
    const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
    if (size < 1 || chorus->host_webview == nullptr) {return false;}
    switch (bytes[0]) {
        case PAGE_POLL:
            send_status(chorus);
            return true;
        case PAGE_PARAM_BEGIN:
            page_param(chorus, CLAP_EVENT_PARAM_GESTURE_BEGIN, bytes, size);
            return true;
        case PAGE_PARAM_SET:
            page_param(chorus, CLAP_EVENT_PARAM_VALUE, bytes, size);
            return true;
        case PAGE_PARAM_END:
            page_param(chorus, CLAP_EVENT_PARAM_GESTURE_END, bytes, size);
            return true;
        case PAGE_HOVER: {
            clap_id id;
            if (size < 8 || chorus->host_hovered == nullptr) {return false;}
            std::memcpy(&id, bytes + 4, 4);
            chorus->host_hovered->update(chorus->host, id < PARAM_COUNT ? id : CLAP_INVALID_ID);
            return true;
        }
        default:
            return false;
    }
}

static const clap_plugin_webview_t webview = {webview_get_uri, webview_get_resource, webview_receive};

// gui: only the webview api, the host owns the window, sizes are logical pixels at the panel's 3:2
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
    const Chorus *chorus = chorus_of(plugin);
    *width = chorus->gui_width;
    *height = chorus->gui_height;
    return true;
}

static bool gui_can_resize(const clap_plugin_t *plugin) {(void) plugin; return true;}

static bool gui_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints) {
    (void) plugin;
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = true;
    hints->aspect_ratio_width = GUI_WIDTH;
    hints->aspect_ratio_height = GUI_HEIGHT;
    return true;
}

static bool gui_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    (void) plugin;
    const double wanted = std::fmin(static_cast<double>(*width) / GUI_WIDTH, static_cast<double>(*height) / GUI_HEIGHT);
    const double scale = std::fmin(GUI_MAX_SCALE, std::fmax(GUI_MIN_SCALE, wanted));
    *width = static_cast<uint32_t>(std::lround(GUI_WIDTH * scale));
    *height = static_cast<uint32_t>(std::lround(GUI_HEIGHT * scale));
    return true;
}

static bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height) {
    Chorus *chorus = chorus_of(plugin);
    gui_adjust_size(plugin, &width, &height);
    chorus->gui_width = width;
    chorus->gui_height = height;
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

// device panel: the window's page, the whole plate scaled to the panel height
static bool panel_get_width(const clap_plugin_t *plugin, uint32_t height, uint32_t *width) {
    (void) plugin;
    *width = static_cast<uint32_t>(std::lround(static_cast<double>(height) * GUI_WIDTH / GUI_HEIGHT));
    return true;
}

static const opendaw_plugin_device_panel_t device_panel = {webview_get_uri, panel_get_width};

static const void *chorus_get_extension(const clap_plugin_t *plugin, const char *id) {
    (void) plugin;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {return &audio_ports;}
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {return &params;}
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {return &state;}
    if (std::strcmp(id, CLAP_EXT_WEBVIEW) == 0) {return &webview;}
    if (std::strcmp(id, CLAP_EXT_GUI) == 0) {return &gui;}
    if (std::strcmp(id, OPENDAW_EXT_DEVICE_PANEL) == 0) {return &device_panel;}
    return nullptr;
}

static void chorus_on_main_thread(const clap_plugin_t *plugin) {(void) plugin;}

static const clap_plugin_t *create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id) {
    (void) factory;
    if (std::strcmp(plugin_id, descriptor.id) != 0) {return nullptr;}
    void *memory = std::calloc(1, sizeof(Chorus));
    if (memory == nullptr) {return nullptr;}
    Chorus *chorus = new(memory) Chorus();
    chorus->host = host;
    for (clap_id id = 0; id < PARAM_COUNT; id++) {chorus->values[id] = default_value(id);}
    chorus->settings_dirty = true;
    chorus->gui_width = GUI_WIDTH;
    chorus->gui_height = GUI_HEIGHT;
    chorus->plugin.desc = &descriptor;
    chorus->plugin.plugin_data = chorus;
    chorus->plugin.init = chorus_init;
    chorus->plugin.destroy = chorus_destroy;
    chorus->plugin.activate = chorus_activate;
    chorus->plugin.deactivate = chorus_deactivate;
    chorus->plugin.start_processing = chorus_start_processing;
    chorus->plugin.stop_processing = chorus_stop_processing;
    chorus->plugin.reset = chorus_reset;
    chorus->plugin.process = chorus_process;
    chorus->plugin.get_extension = chorus_get_extension;
    chorus->plugin.on_main_thread = chorus_on_main_thread;
    return &chorus->plugin;
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
