// What a plain CLAP plugin (no JUCE) needs for openDAW's device panel and its page, header only: the device panel
// extension and its features, the page's messages (modules/opendaw-page/panel.js: values, gestures, the hovered
// parameter, polls), the gestures queued for the host, and the webview window: the panel zoomed, its aspect kept.
// See docs/building-for-opendaw.md. The JUCE ports have the same in modules/juce_webclap.
#pragma once

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>
#include <clap/ext/draft/param-hovered.h>
#include <cmath>
#include <cstdint>
#include <cstring>

// --- the device panel ---------------------------------------------------------------------------------------------

// openDAW's device panel is 176 px high: the device body's 192 px minus its padding
#define OPENDAW_PANEL_HEIGHT 176
#define OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL "opendaw:device-panel"
#define OPENDAW_EXT_DEVICE_PANEL "opendaw.device-panel/1"
#define OPENDAW_STRING_OF(value) #value
#define OPENDAW_STRING(value) OPENDAW_STRING_OF(value)
// the panel's width at 176 px as a feature, a number or a macro holding one: openDAW reads it from its cloud index to
// open the panel at its size before the plugin loads; it has to match get_width(176)
#define OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL_WIDTH(width) "opendaw:device-panel-width:" OPENDAW_STRING(width)

typedef struct opendaw_plugin_device_panel {
    // the page for the panel, like clap.webview's get_uri
    int32_t (*get_uri)(const clap_plugin_t *plugin, char *uri, uint32_t capacity);
    // the panel's width for this height
    bool (*get_width)(const clap_plugin_t *plugin, uint32_t height, uint32_t *width);
} opendaw_plugin_device_panel_t;

namespace opendaw {

// a panel of this width at 176 px, at another height
inline uint32_t panel_width_at(double width, uint32_t height) {
    return static_cast<uint32_t>(std::lround(static_cast<double>(height) * width / OPENDAW_PANEL_HEIGHT));
}

// --- the page's messages ------------------------------------------------------------------------------------------
// From the page (panel.js), little endian: the type at byte 0, the parameter id at 4, a value (f64) at 8. The plugin
// answers a poll with its own status message, its type PLUGIN_STATUS at byte 0.

enum PageMessage : uint8_t {PAGE_POLL = 1, PAGE_PARAM_BEGIN = 2, PAGE_PARAM_SET = 3, PAGE_PARAM_END = 4, PAGE_HOVER = 7};
constexpr uint8_t PLUGIN_STATUS = 1;

// The host's extensions the page needs, queried in the plugin's init.
struct PageHost {
    const clap_host_t *host = nullptr;
    const clap_host_params_t *params = nullptr;
    const clap_host_state_t *state = nullptr;
    const clap_host_webview_t *webview = nullptr;
    const clap_host_param_hovered_t *hovered = nullptr;

    void init(const clap_host_t *clap_host) {
        host = clap_host;
        params = static_cast<const clap_host_params_t *>(host->get_extension(host, CLAP_EXT_PARAMS));
        state = static_cast<const clap_host_state_t *>(host->get_extension(host, CLAP_EXT_STATE));
        webview = static_cast<const clap_host_webview_t *>(host->get_extension(host, CLAP_EXT_WEBVIEW));
        hovered = static_cast<const clap_host_param_hovered_t *>(host->get_extension(host, CLAP_EXT_PARAM_HOVERED));
    }

    bool send(const void *bytes, uint32_t size) const {return webview != nullptr && webview->send(host, bytes, size);}
};

// The page's gestures and values for the host, pushed from the page's messages, sent from process or params.flush.
template<uint32_t Capacity = 128>
struct OutEvents {
    struct Event {
        uint16_t type; // CLAP_EVENT_PARAM_VALUE, CLAP_EVENT_PARAM_GESTURE_BEGIN or _END
        clap_id id;
        double value;
    };
    Event events[Capacity];
    uint32_t read = 0, write = 0;

    void push(uint16_t type, clap_id id, double value) {
        if (write - read >= Capacity) {return;}
        events[write % Capacity] = Event{type, id, value};
        write++;
    }

    void drain(const clap_output_events_t *out) {
        while (read != write) {
            const Event &event = events[read % Capacity];
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
            read++;
        }
    }
};

// A message from the page. The plugin answers:
//   bool has(clap_id id)              the id is one of its parameters
//   void set(clap_id id, double value) the page set it
//   double value(clap_id id)          its value now (the host's)
//   void status()                     answer a poll (PageHost::send)
// A drag is one gesture, so one undo step in openDAW: begin, values, end. Its end marks the state dirty. The hovered
// parameter goes to the host (clap.param-hovered), so openDAW's menu and value entry work on the page's controls.
template<class Plugin, uint32_t Capacity>
bool receive(Plugin &plugin, const PageHost &host, OutEvents<Capacity> &out, const void *buffer, uint32_t size) {
    const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
    if (size < 1 || host.webview == nullptr) {return false;}
    if (bytes[0] == PAGE_POLL) {
        plugin.status();
        return true;
    }
    clap_id id;
    if (size < 8) {return false;}
    std::memcpy(&id, bytes + 4, 4);
    if (bytes[0] == PAGE_HOVER) {
        if (host.hovered == nullptr) {return false;}
        host.hovered->update(host.host, plugin.has(id) ? id : CLAP_INVALID_ID);
        return true;
    }
    uint16_t type;
    switch (bytes[0]) {
        case PAGE_PARAM_BEGIN: type = CLAP_EVENT_PARAM_GESTURE_BEGIN; break;
        case PAGE_PARAM_SET: type = CLAP_EVENT_PARAM_VALUE; break;
        case PAGE_PARAM_END: type = CLAP_EVENT_PARAM_GESTURE_END; break;
        default: return false;
    }
    if (!plugin.has(id)) {return true;}
    if (type == CLAP_EVENT_PARAM_VALUE) {
        double value;
        if (size < 16) {return false;}
        std::memcpy(&value, bytes + 8, 8);
        plugin.set(id, value);
    }
    if (type == CLAP_EVENT_PARAM_GESTURE_END && host.state != nullptr) {host.state->mark_dirty(host.host);}
    out.push(type, id, plugin.value(id));
    if (host.params != nullptr) {host.params->request_flush(host.host);}
    return true;
}

// --- the window -----------------------------------------------------------------------------------------------------
// The webview shows the panel's page zoomed, its aspect kept. A plugin keeps the window's size and answers
// get_size; these are the parts every plugin answers alike.

inline bool gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    (void) plugin; (void) is_floating;
    return std::strcmp(api, CLAP_WINDOW_API_WEBVIEW) == 0;
}

inline bool gui_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *is_floating) {
    (void) plugin;
    *api = CLAP_WINDOW_API_WEBVIEW;
    *is_floating = false;
    return true;
}

inline bool gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    return gui_is_api_supported(plugin, api, is_floating);
}

inline void gui_destroy(const clap_plugin_t *plugin) {(void) plugin;}
inline bool gui_set_scale(const clap_plugin_t *plugin, double scale) {(void) plugin; (void) scale; return false;}
inline bool gui_can_resize(const clap_plugin_t *plugin) {(void) plugin; return true;}
inline bool gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window) {(void) plugin; (void) window; return true;}
inline bool gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window) {(void) plugin; (void) window; return false;}
inline void gui_suggest_title(const clap_plugin_t *plugin, const char *title) {(void) plugin; (void) title;}
inline bool gui_show(const clap_plugin_t *plugin) {(void) plugin; return true;}
inline bool gui_hide(const clap_plugin_t *plugin) {(void) plugin; return true;}

// resizable at the panel's aspect ratio
inline void window_hints(double panel_width, clap_gui_resize_hints_t *hints) {
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = true;
    hints->aspect_ratio_width = static_cast<uint32_t>(std::lround(panel_width));
    hints->aspect_ratio_height = OPENDAW_PANEL_HEIGHT;
}

// the largest panel zoom between min and max that fits the size
inline void window_fit(double panel_width, double min_zoom, double max_zoom, uint32_t *width, uint32_t *height) {
    const double wanted = std::fmin(*width / panel_width, static_cast<double>(*height) / OPENDAW_PANEL_HEIGHT);
    const double zoom = std::fmin(max_zoom, std::fmax(min_zoom, wanted));
    *width = static_cast<uint32_t>(std::lround(panel_width * zoom));
    *height = static_cast<uint32_t>(std::lround(OPENDAW_PANEL_HEIGHT * zoom));
}

} // namespace opendaw
