// Prism's eight analyzers as WebCLAP audio effects in one module: the audio passes through unchanged, Prism's native
// DSP (external/prism/native/src, unmodified) runs in the scope engines (engines.h, after upstream's
// plugin/Source/*Engine.h), and the page is Prism's own plugin UI (src/plugin-ui) on a WebCLAP stand-in for JUCE's
// webview backend (page/backend.ts). Each plugin is its device panel, the scope with its settings beside it; there is
// no window (no clap.gui).
//
// Upstream's editor owns its engine and feeds it from the processor's FIFO on each display frame. Here every page
// (a host may show the panel's page more than once) gets an engine of its own, a slot,
// fed from one ring of the recent audio, block by block while the page polls or on each poll: a closed page costs a
// copy into the ring and nothing else.

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>
#include "opendaw_page.h"
#include "engines.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>

using namespace prism;

#define VERSION "1.0.0"
#define STATE_VERSION 1
#define RING_SIZE (1 << 16) // samples per channel, upstream's FIFO
#define SLOTS 4
#define SLOT_TIMEOUT_SECONDS 5.0

// page -> plugin, little endian: the type at byte 0, the page's id (u32) at 4
enum : uint8_t {PAGE_POLL = 1, PAGE_READY = 2, PAGE_CONFIG = 3, PAGE_BYE = 5};
// PAGE_CONFIG: the kind at 8, persist at 9, the JSON from 12
enum : uint8_t {CONFIG_SETTINGS = 0, CONFIG_NATIVE = 1, CONFIG_MEASUREMENT = 2};
// plugin -> page: a frame (the scope at 1, the page at 4, the engine's fields from 8), or the settings to restore
// (the page at 4, 0 for all, the page they came from at 8, the JSON from 12)
enum : uint8_t {PLUGIN_FRAME = 1, PLUGIN_RESTORE = 2};
#define FRAME_HEADER 8

struct ScopeInfo {
    const char *id;
    const char *name;
    const char *description;
    const char *panelWidthFeature;
    uint32_t panelWidth; // at 176 px: the scope and its settings' columns (page/webclap.css)
};

#define SCOPE_INFO(key, name, description, panel) \
    {key, name, description, OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL_WIDTH(panel), panel}

static const ScopeInfo scopes[SCOPE_COUNT] = {
    SCOPE_INFO("spectrum", "Prism Spectrum", "FFT spectrum analyzer with calibrated dBFS levels", 830),
    SCOPE_INFO("oscilloscope", "Prism Oscilloscope", "Waveform display locked to the fundamental's pitch", 570),
    SCOPE_INFO("vectorscope", "Prism Vectorscope", "Stereo phase scope: XY, polar and M/S views, optionally in three bands", 446),
    SCOPE_INFO("spectrogram", "Prism Spectrogram", "Scrolling frequency over time display with frequency reassignment", 830),
    SCOPE_INFO("vumeter", "Prism VU Meter", "300 ms VU meter with correlation, needles or bars", 460),
    SCOPE_INFO("lufsmeter", "Prism Loudness Meter", "ITU-R BS.1770 momentary, short-term and integrated loudness, true peak", 316),
    SCOPE_INFO("waveform", "Prism Waveform", "Scrolling waveform, mono or stereo, optionally colored by band", 570),
    SCOPE_INFO("waterfall", "Prism Waterfall", "Spectrum history as layered ridges", 700),
};

static const char *features[SCOPE_COUNT][6];
static clap_plugin_descriptor_t descriptors[SCOPE_COUNT];
static char descriptorIds[SCOPE_COUNT][64];

struct Slot {
    uint32_t page = 0; // 0: free
    std::unique_ptr<Engine> engine;
    uint64_t read = 0; // in the ring
    uint64_t lastSeen = 0; // the ring's write position when the page last spoke
    bool fed = false; // audio since the last frame
};

struct Prism {
    clap_plugin_t plugin;
    const clap_host_t *host = nullptr;
    const clap_host_state_t *hostState = nullptr;
    const clap_host_webview_t *hostWebview = nullptr;
    Scope scope = SCOPE_SPECTRUM;
    double sampleRate = 48000.0;
    std::vector<float> ringLeft, ringRight;
    uint64_t written = 0;
    Slot slots[SLOTS];
    std::string settingsJson; // the page's settings, the state
    Frame frame;
};

static Prism *prism_of(const clap_plugin_t *plugin) {return static_cast<Prism *>(plugin->plugin_data);}

static bool send(Prism *prism, const void *bytes, size_t size) {
    return prism->hostWebview != nullptr && prism->hostWebview->send(prism->host, bytes, static_cast<uint32_t>(size));
}

// the settings for a page (0: every page), and the page they came from (0: the host)
static void send_restore(Prism *prism, uint32_t page, uint32_t origin) {
    std::vector<uint8_t> message(12 + prism->settingsJson.size(), 0);
    message[0] = PLUGIN_RESTORE;
    std::memcpy(message.data() + 4, &page, 4);
    std::memcpy(message.data() + 8, &origin, 4);
    std::memcpy(message.data() + 12, prism->settingsJson.data(), prism->settingsJson.size());
    send(prism, message.data(), message.size());
}

static void reset_slot(Slot &slot) {
    slot.page = 0;
    slot.engine.reset();
}

// the page's slot, made when it first speaks: a free one, one whose page went quiet, or the one quiet longest
static Slot &slot_of(Prism *prism, uint32_t page) {
    for (Slot &slot : prism->slots) {if (slot.page == page && slot.engine) {slot.lastSeen = prism->written; return slot;}}
    const uint64_t timeout = static_cast<uint64_t>(SLOT_TIMEOUT_SECONDS * prism->sampleRate);
    Slot *pick = nullptr;
    for (Slot &slot : prism->slots) {
        if (slot.page == 0 || prism->written - slot.lastSeen > timeout) {pick = &slot; break;}
        if (pick == nullptr || slot.lastSeen < pick->lastSeen) {pick = &slot;}
    }
    reset_slot(*pick);
    pick->page = page;
    pick->engine.reset(createEngine(prism->scope));
    pick->engine->setSampleRate(prism->sampleRate);
    pick->read = prism->written;
    pick->lastSeen = prism->written;
    return *pick;
}

// the audio since the slot's last feed into its engine: upstream's editor drains its FIFO on each display frame, here
// the open pages' engines that can take it are fed in process, a block at a time, so the analysis spreads over the
// blocks (Engine::takesBlocks)
static void feed(Prism *prism, Slot &slot) {
    Engine &engine = *slot.engine;
    if (prism->written - slot.read > RING_SIZE) {
        slot.read = prism->written - RING_SIZE;
        engine.resetAudioHistory();
    }
    while (slot.read < prism->written) {
        const size_t start = static_cast<size_t>(slot.read % RING_SIZE);
        const size_t count = static_cast<size_t>(std::min<uint64_t>(prism->written - slot.read, RING_SIZE - start));
        engine.process(prism->ringLeft.data() + start, prism->ringRight.data() + start, static_cast<int>(count));
        slot.read += count;
        slot.fed = true;
    }
}

// a poll: the engine's frame to the page (upstream's display frame, which recomputes or decays without new audio)
static void render_frame(Prism *prism, Slot &slot) {
    Engine &engine = *slot.engine;
    feed(prism, slot);
    if (!slot.fed) {engine.process(prism->ringLeft.data(), prism->ringRight.data(), 0);}
    slot.fed = false;
    prism->frame.clear(FRAME_HEADER);
    engine.buildFrame(prism->sampleRate, prism->frame);
    uint8_t *bytes = prism->frame.bytes.data();
    bytes[0] = PLUGIN_FRAME;
    bytes[1] = prism->scope;
    std::memcpy(bytes + 4, &slot.page, 4);
    send(prism, bytes, prism->frame.bytes.size());
}

static bool prism_init(const clap_plugin_t *plugin) {
    Prism *prism = prism_of(plugin);
    const clap_host_t *host = prism->host;
    prism->hostState = static_cast<const clap_host_state_t *>(host->get_extension(host, CLAP_EXT_STATE));
    prism->hostWebview = static_cast<const clap_host_webview_t *>(host->get_extension(host, CLAP_EXT_WEBVIEW));
    return true;
}

static void prism_destroy(const clap_plugin_t *plugin) {
    Prism *prism = prism_of(plugin);
    prism->~Prism();
    std::free(prism);
}

static bool prism_activate(const clap_plugin_t *plugin, double sampleRate, uint32_t, uint32_t) {
    Prism *prism = prism_of(plugin);
    prism->sampleRate = sampleRate;
    for (Slot &slot : prism->slots) {
        if (!slot.engine) {continue;}
        slot.engine->setSampleRate(sampleRate);
        slot.engine->resetAudioHistory();
        slot.read = prism->written;
    }
    return true;
}

static void prism_deactivate(const clap_plugin_t *) {}
static bool prism_start_processing(const clap_plugin_t *) {return true;}
static void prism_stop_processing(const clap_plugin_t *) {}
static void prism_reset(const clap_plugin_t *) {}

static clap_process_status prism_process(const clap_plugin_t *plugin, const clap_process_t *process) {
    Prism *prism = prism_of(plugin);
    if (process->audio_inputs_count < 1 || process->audio_outputs_count < 1) {return CLAP_PROCESS_ERROR;}
    const clap_audio_buffer_t &input = process->audio_inputs[0];
    const clap_audio_buffer_t &output = process->audio_outputs[0];
    if (input.channel_count < 1 || output.channel_count < 1) {return CLAP_PROCESS_ERROR;}
    const uint32_t frames = process->frames_count;
    const float *left = input.data32[0];
    const float *right = input.channel_count > 1 ? input.data32[1] : input.data32[0];
    for (uint32_t channel = 0; channel < output.channel_count; channel++) {
        const float *from = input.data32[channel < input.channel_count ? channel : input.channel_count - 1];
        if (output.data32[channel] != from) {std::memcpy(output.data32[channel], from, frames * sizeof(float));}
    }
    for (uint32_t index = 0; index < frames;) {
        const size_t start = static_cast<size_t>(prism->written % RING_SIZE);
        const uint32_t count = std::min<uint32_t>(frames - index, static_cast<uint32_t>(RING_SIZE - start));
        std::memcpy(prism->ringLeft.data() + start, left + index, count * sizeof(float));
        std::memcpy(prism->ringRight.data() + start, right + index, count * sizeof(float));
        prism->written += count;
        index += count;
    }
    // the engines of the pages still polling (a closed page's stops after the timeout, its slot is reused)
    const uint64_t timeout = static_cast<uint64_t>(SLOT_TIMEOUT_SECONDS * prism->sampleRate);
    for (Slot &slot : prism->slots) {
        if (slot.engine && slot.engine->takesBlocks() && prism->written - slot.lastSeen <= timeout) {feed(prism, slot);}
    }
    return CLAP_PROCESS_CONTINUE;
}

static uint32_t audio_ports_count(const clap_plugin_t *, bool) {return 1;}

static bool audio_ports_get(const clap_plugin_t *, uint32_t index, bool isInput, clap_audio_port_info_t *info) {
    if (index > 0) {return false;}
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Input" : "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = 0;
    return true;
}

static const clap_plugin_audio_ports_t audio_ports = {audio_ports_count, audio_ports_get};

// no parameters: Prism's settings are the page's, in the state
static uint32_t params_count(const clap_plugin_t *) {return 0;}
static bool params_get_info(const clap_plugin_t *, uint32_t, clap_param_info_t *) {return false;}
static bool params_get_value(const clap_plugin_t *, clap_id, double *) {return false;}
static bool params_value_to_text(const clap_plugin_t *, clap_id, double, char *, uint32_t) {return false;}
static bool params_text_to_value(const clap_plugin_t *, clap_id, const char *, double *) {return false;}
static void params_flush(const clap_plugin_t *, const clap_input_events_t *, const clap_output_events_t *) {}

static const clap_plugin_params_t params = {
    params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush
};

static uint32_t latency_get(const clap_plugin_t *) {return 0;}
static const clap_plugin_latency_t latency = {latency_get};

static uint32_t tail_get(const clap_plugin_t *) {return 0;}
static const clap_plugin_tail_t tail = {tail_get};

// the state: the version (u32), the settings' length (u32), the settings as the page's JSON (upstream's settingsJson)
static bool write_all(const clap_ostream_t *stream, const void *data, size_t size) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    size_t written = 0;
    while (written < size) {
        const int64_t result = stream->write(stream, bytes + written, size - written);
        if (result <= 0) {return false;}
        written += static_cast<size_t>(result);
    }
    return true;
}

static size_t read_all(const clap_istream_t *stream, void *data, size_t size) {
    uint8_t *bytes = static_cast<uint8_t *>(data);
    size_t read = 0;
    while (read < size) {
        const int64_t result = stream->read(stream, bytes + read, size - read);
        if (result <= 0) {break;}
        read += static_cast<size_t>(result);
    }
    return read;
}

static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    const Prism *prism = prism_of(plugin);
    const uint32_t header[2] = {STATE_VERSION, static_cast<uint32_t>(prism->settingsJson.size())};
    return write_all(stream, header, sizeof(header)) && write_all(stream, prism->settingsJson.data(), prism->settingsJson.size());
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    Prism *prism = prism_of(plugin);
    uint32_t header[2] = {0, 0};
    if (read_all(stream, header, sizeof(header)) < sizeof(header) || header[0] < 1 || header[1] > (1u << 24)) {return false;}
    std::string json(header[1], '\0');
    if (read_all(stream, json.data(), json.size()) < json.size()) {return false;}
    prism->settingsJson = std::move(json);
    send_restore(prism, 0, 0);
    return true;
}

static const clap_plugin_state_t state = {state_save, state_load};

// the device panel's page, also clap.webview's: there is no window
static int32_t write_uri(const Prism *prism, char *uri, uint32_t capacity) {
    char page[64];
    const int length = std::snprintf(page, sizeof(page), "/index.html?panel#%s", scopes[prism->scope].id);
    if (capacity > 0 && uri != nullptr) {std::snprintf(uri, capacity, "%s", page);}
    return length + 1;
}

static int32_t webview_get_uri(const clap_plugin_t *plugin, char *uri, uint32_t capacity) {
    return write_uri(prism_of(plugin), uri, capacity);
}

static bool webview_get_resource(const clap_plugin_t *, const char *, char *, uint32_t, const clap_ostream_t *) {return false;}

static bool webview_receive(const clap_plugin_t *plugin, const void *buffer, uint32_t size) {
    Prism *prism = prism_of(plugin);
    const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
    if (size < 8) {return false;}
    uint32_t page;
    std::memcpy(&page, bytes + 4, 4);
    if (page == 0) {return false;}
    switch (bytes[0]) {
        case PAGE_POLL:
            render_frame(prism, slot_of(prism, page));
            return true;
        case PAGE_READY: {
            // a page starts (its scope's effects may have configured the engine already): nothing of the audio
            // before it, upstream restarts the history
            Slot &slot = slot_of(prism, page);
            slot.read = prism->written;
            slot.engine->resetAudioHistory();
            send_restore(prism, page, 0);
            return true;
        }
        case PAGE_CONFIG: {
            if (size < 12) {return false;}
            const char *json = reinterpret_cast<const char *>(bytes + 12);
            const size_t length = size - 12;
            const Var payload = Var::parse(json, length);
            Slot &slot = slot_of(prism, page);
            if (bytes[8] == CONFIG_SETTINGS) {
                slot.engine->configure(payload);
                // a change of the user's (persist), not a restore: the state, and the other pages follow
                if (bytes[9] != 0 && payload.isObject()) {
                    std::string next(json, length);
                    if (next != prism->settingsJson) {
                        prism->settingsJson = std::move(next);
                        if (prism->hostState != nullptr) {prism->hostState->mark_dirty(prism->host);}
                        send_restore(prism, 0, page);
                    }
                }
            } else if (bytes[8] == CONFIG_NATIVE) {
                slot.engine->configureNative(payload);
            } else if (bytes[8] == CONFIG_MEASUREMENT) {
                slot.engine->setMeasurementActive(static_cast<bool>(payload.getProperty("active", false)));
            }
            return true;
        }
        case PAGE_BYE:
            for (Slot &slot : prism->slots) {if (slot.page == page) {reset_slot(slot);}}
            return true;
        default:
            return false;
    }
}

static const clap_plugin_webview_t webview = {webview_get_uri, webview_get_resource, webview_receive};

// openDAW's device panel: the scope and its settings, at the scope's width
static int32_t panel_get_uri(const clap_plugin_t *plugin, char *uri, uint32_t capacity) {
    return write_uri(prism_of(plugin), uri, capacity);
}

static bool panel_get_width(const clap_plugin_t *plugin, uint32_t height, uint32_t *width) {
    *width = opendaw::panel_width_at(scopes[prism_of(plugin)->scope].panelWidth, height);
    return true;
}

static const opendaw_plugin_device_panel_t device_panel = {panel_get_uri, panel_get_width};

static const void *prism_get_extension(const clap_plugin_t *, const char *id) {
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {return &audio_ports;}
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {return &params;}
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {return &state;}
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) {return &latency;}
    if (std::strcmp(id, CLAP_EXT_TAIL) == 0) {return &tail;}
    if (std::strcmp(id, CLAP_EXT_WEBVIEW) == 0) {return &webview;}
    if (std::strcmp(id, OPENDAW_EXT_DEVICE_PANEL) == 0) {return &device_panel;}
    return nullptr;
}

static void prism_on_main_thread(const clap_plugin_t *) {}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *, const clap_host_t *host, const char *pluginId) {
    int scope = -1;
    for (int index = 0; index < SCOPE_COUNT; index++) {if (std::strcmp(pluginId, descriptors[index].id) == 0) {scope = index;}}
    if (scope < 0) {return nullptr;}
    void *memory = std::calloc(1, sizeof(Prism));
    if (memory == nullptr) {return nullptr;}
    Prism *prism = new(memory) Prism();
    prism->host = host;
    prism->scope = static_cast<Scope>(scope);
    prism->ringLeft.assign(RING_SIZE, 0.0f);
    prism->ringRight.assign(RING_SIZE, 0.0f);
    prism->plugin.desc = &descriptors[scope];
    prism->plugin.plugin_data = prism;
    prism->plugin.init = prism_init;
    prism->plugin.destroy = prism_destroy;
    prism->plugin.activate = prism_activate;
    prism->plugin.deactivate = prism_deactivate;
    prism->plugin.start_processing = prism_start_processing;
    prism->plugin.stop_processing = prism_stop_processing;
    prism->plugin.reset = prism_reset;
    prism->plugin.process = prism_process;
    prism->plugin.get_extension = prism_get_extension;
    prism->plugin.on_main_thread = prism_on_main_thread;
    return &prism->plugin;
}

static uint32_t factory_count(const clap_plugin_factory_t *) {return SCOPE_COUNT;}

static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *, uint32_t index) {
    return index < SCOPE_COUNT ? &descriptors[index] : nullptr;
}

static const clap_plugin_factory_t factory = {factory_count, factory_descriptor, factory_create};

static bool entry_init(const char *) {
    for (int index = 0; index < SCOPE_COUNT; index++) {
        const ScopeInfo &info = scopes[index];
        std::snprintf(descriptorIds[index], sizeof(descriptorIds[index]), "studio.opendaw.prism.%s", info.id);
        features[index][0] = CLAP_PLUGIN_FEATURE_AUDIO_EFFECT;
        features[index][1] = CLAP_PLUGIN_FEATURE_ANALYZER;
        features[index][2] = CLAP_PLUGIN_FEATURE_STEREO;
        features[index][3] = OPENDAW_PLUGIN_FEATURE_DEVICE_PANEL;
        features[index][4] = info.panelWidthFeature;
        features[index][5] = nullptr;
        descriptors[index] = clap_plugin_descriptor_t{
            CLAP_VERSION_INIT, descriptorIds[index], info.name, "Astra / open-webclap",
            "https://github.com/andremichelle/open-webclap/tree/main/ports/prism", "https://github.com/Boof2015/prism", "",
            VERSION, info.description, features[index]
        };
    }
    return true;
}

static void entry_deinit() {}

static const void *entry_get_factory(const char *id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
}

extern "C" const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory};
