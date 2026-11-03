/*
    Helm as module.wasm: Matt Tytel's polyphonic synthesizer (https://github.com/mtytel/helm, GPL-3.0) as a CLAP
    instrument. Plain CLAP, no JUCE: upstream's engine (mopo and src/synthesis, which never used JUCE) behind this
    wrapper, changed only by patches/helm-webclap.patch (float wavetables, clearMemory, killAllVoices).

    Parameters. Every Helm control is a CLAP parameter, 139 of them, in Helm's own units and ranges (the editor
    shows them skewed as Helm does, value_to_text too). Their ids are the controls' positions in name order, fixed
    by paramNames below, so a later build reads older states and automation. The modulation routings are not
    parameters: up to 32 slots, each a source, a destination (a parameter id) and an amount in the destination's
    units.

    Processing. Helm reads its controls once per block, so parameter events apply at the start of the block, the
    host's block is not split at them. Notes start and end at their sample: mopo's envelopes run at control rate (a
    value per block), so the block is split at note events (upstream starts every note at the start of a
    256-sample block). Blocks over 256 samples (mopo's buffers) are split too. The engine sleeps after
    half a second without a voice and below -120 dB at the output: silence without processing until the next note.
    reset() kills every voice (rendered 20 ms out of sight) and clears the delay lines, the reverb and the stutter
    buffers, allocating nothing.

    State: a fixed layout of 884 bytes (State below), so openDAW, which keeps the state as bytes and records an edit
    as the bytes that changed, stores a knob edit as 4 bytes and a modulation edit as at most 8. A patch load from
    the editor goes to the host as one gesture around all the values that changed: one undo step.

    Editor: ui/index.html through clap.gui (the webview API) and clap.webview. Messages are binary, little endian,
    see the Page and Plugin enums: the page says hello and gets the parameter table (JSON, once) and a snapshot,
    then polls at most every 33 ms and gets only what changed, nothing while nothing does.
*/

#include <clap/clap.h>
#include <clap/ext/draft/param-hovered.h>
#include <clap/ext/draft/webview.h>

#include "helm_common.h"
#include "helm_engine.h"
#include "helm_lfo.h"
#include "smooth_value.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace
{
constexpr const char* pluginId = "org.tytel.helm";

// The controls in name order (the engine's control_map), the parameter ids. Never reorder: states store values in
// this order and hosts automate by id.
constexpr const char* paramNames[] = {
    "amp_attack", "amp_decay", "amp_release", "amp_sustain", "arp_frequency", "arp_gate", "arp_octaves", "arp_on",
    "arp_pattern", "arp_sync", "arp_tempo", "beats_per_minute", "cross_modulation", "cutoff", "delay_dry_wet",
    "delay_feedback", "delay_frequency", "delay_on", "delay_sync", "delay_tempo", "distortion_drive",
    "distortion_mix", "distortion_on", "distortion_type", "fil_attack", "fil_decay", "fil_env_depth", "fil_release",
    "fil_sustain", "filter_blend", "filter_drive", "filter_on", "filter_shelf", "filter_style", "formant_on",
    "formant_x", "formant_y", "keytrack", "legato", "mod_attack", "mod_decay", "mod_release", "mod_sustain",
    "mono_lfo_1_amplitude", "mono_lfo_1_frequency", "mono_lfo_1_retrigger", "mono_lfo_1_sync", "mono_lfo_1_tempo",
    "mono_lfo_1_waveform", "mono_lfo_2_amplitude", "mono_lfo_2_frequency", "mono_lfo_2_retrigger",
    "mono_lfo_2_sync", "mono_lfo_2_tempo", "mono_lfo_2_waveform", "noise_volume", "num_steps", "osc_1_transpose",
    "osc_1_tune", "osc_1_unison_detune", "osc_1_unison_voices", "osc_1_volume", "osc_1_waveform", "osc_2_transpose",
    "osc_2_tune", "osc_2_unison_detune", "osc_2_unison_voices", "osc_2_volume", "osc_2_waveform",
    "osc_feedback_amount", "osc_feedback_transpose", "osc_feedback_tune", "pitch_bend_range", "poly_lfo_amplitude",
    "poly_lfo_frequency", "poly_lfo_sync", "poly_lfo_tempo", "poly_lfo_waveform", "polyphony", "portamento",
    "portamento_type", "resonance", "reverb_damping", "reverb_dry_wet", "reverb_feedback", "reverb_on",
    "step_frequency", "step_seq_00", "step_seq_01", "step_seq_02", "step_seq_03", "step_seq_04", "step_seq_05",
    "step_seq_06", "step_seq_07", "step_seq_08", "step_seq_09", "step_seq_10", "step_seq_11", "step_seq_12",
    "step_seq_13", "step_seq_14", "step_seq_15", "step_seq_16", "step_seq_17", "step_seq_18", "step_seq_19",
    "step_seq_20", "step_seq_21", "step_seq_22", "step_seq_23", "step_seq_24", "step_seq_25", "step_seq_26",
    "step_seq_27", "step_seq_28", "step_seq_29", "step_seq_30", "step_seq_31", "step_sequencer_retrigger",
    "step_sequencer_sync", "step_sequencer_tempo", "step_smoothing", "stutter_frequency", "stutter_on",
    "stutter_resample_frequency", "stutter_resample_sync", "stutter_resample_tempo", "stutter_softness",
    "stutter_sync", "stutter_tempo", "sub_octave", "sub_shuffle", "sub_volume", "sub_waveform", "unison_1_harmonize",
    "unison_2_harmonize", "velocity_track", "volume"
};
constexpr uint32_t paramCount = sizeof paramNames / sizeof paramNames[0];
static_assert (paramCount == 139);

// The modulation sources the editor offers, their index is what a slot stores. readout: the engine output that shows
// the source's current value (the last voice's for the polyphonic ones), none for per-note values.
struct Source
{
    const char* name;
    const char* readout;
};

constexpr Source sources[] = {
    { "amp_envelope", "amp_envelope_amp" }, { "fil_envelope", "fil_envelope_amp" },
    { "mod_envelope", "mod_envelope_amp" }, { "mono_lfo_1", "mono_lfo_1" }, { "mono_lfo_2", "mono_lfo_2" },
    { "poly_lfo", "poly_lfo_amp" }, { "step_sequencer", "step_sequencer" }, { "velocity", nullptr },
    { "note", nullptr }, { "aftertouch", nullptr }, { "mod_wheel", "mod_wheel" }, { "pitch_wheel", "pitch_wheel" },
    { "random", nullptr }
};
constexpr uint8_t sourceCount = sizeof sources / sizeof sources[0];

// The editor's size at 100 %; the page scales to its window, hosts resize it at this aspect ratio (openDAW's zoom
// levels), from half to twice the size
constexpr uint32_t editorWidth = 1128, editorHeight = 700;
constexpr double minEditorScale = 0.5, maxEditorScale = 2.0;

constexpr size_t maxSlots = 32;
constexpr uint8_t emptySlot = 0xff;
constexpr int maxChunk = mopo::MAX_BUFFER_SIZE;
constexpr size_t textSize = 32;

struct Slot
{
    uint8_t source = emptySlot, destination = 0;
    uint16_t reserved = 0;
    float amount = 0.0f;
};
static_assert (sizeof (Slot) == 8);

// The saved state, and the page's snapshot and patch messages after their 8-byte headers
struct State
{
    uint32_t magic, version;
    float values[paramCount];
    Slot slots[maxSlots];
    char name[textSize], author[textSize];
};
static_assert (sizeof (State) == 884);

constexpr uint32_t stateMagic = 0x4d4c4548; // "HELM"
constexpr uint32_t stateVersion = 1;

// Message types, the first byte of every message
enum Page : uint8_t
{
    pageHello = 1,
    pagePoll = 2,
    pageBegin = 3,    // u16 id at 2
    pageSet = 4,      // u16 id at 2, f32 value at 4
    pageEnd = 5,      // u16 id at 2
    pageHover = 6,    // u16 id at 2, 0xffff for none
    pageModulate = 7, // u8 source at 1, u8 destination at 2, u8 live at 3, f32 amount at 4 (0 removes)
    pagePatch = 8,    // a State from 8 on (values, slots, name, author): a patch the page loaded
};

enum Plugin : uint8_t
{
    pluginDescribe = 0x81, // JSON from 4 on: the parameter table, the sources
    pluginSnapshot = 0x82, // a State from 8 on
    pluginValues = 0x83,   // u16 count at 2, then count x (u16 id, u16 0, f32 value)
    pluginActivity = 0x84, // i8 per source at 4, then u8 step, u8 voices, u8 left peak, the stages of
                           // the amp, filter and mod envelopes (Envelope::State), the phases of the mono LFOs 1, 2
                           // and the poly LFO (0..255 for 0..1), the mono LFOs' random cycles (i8 from, to each,
                           // before the amplitude: what sample and hold and sample and glide do in this cycle), the
                           // expressive sources the host sends (bits: 1 aftertouch, 2 mod wheel, 4 pitch wheel), u8
                           // right peak. The peaks are the highest since the last answer, 0 for -72 dB and below,
                           // half dB steps above
};

double clampValue (const mopo::ValueDetails& details, double value)
{
    if (! std::isfinite (value))
        return details.default_value;
    value = std::clamp (value, details.min, details.max);
    return details.steps > 0 ? std::round (value) : value;
}

// Helm's SynthSlider::getTextFromValue: the display skew, offset, inversion and multiplier, then the units
void formatValue (const mopo::ValueDetails& details, double value, char* text, uint32_t size)
{
    double display = value;
    switch (details.display_skew)
    {
        case mopo::ValueDetails::kQuadratic: display = display * display; break;
        case mopo::ValueDetails::kExponential: display = std::pow (2.0, display); break;
        case mopo::ValueDetails::kSquareRoot: display = std::sqrt (display); break;
        default: break;
    }
    display += details.post_offset;
    if (details.display_invert)
        display = 1.0 / display;
    display *= details.display_multiply;
    if (details.steps > 0)
        std::snprintf (text, size, "%d%s%s", (int) std::lround (display), details.display_units.empty() ? "" : " ",
                       details.display_units.c_str());
    else
        std::snprintf (text, size, "%.3g%s%s", display, details.display_units.empty() ? "" : " ",
                       details.display_units.c_str());
}

// The names of a stepped control's choices, as Helm's editor shows them, indexed by the value
const std::string* choices (const std::string& name)
{
    using namespace mopo::strings;
    if (name.find ("waveform") != std::string::npos) return waveforms;
    if (name.size() > 5 && name.compare (name.size() - 5, 5, "_sync") == 0) return freq_sync_styles;
    if (name.size() > 6 && name.compare (name.size() - 6, 6, "_tempo") == 0) return synced_frequencies;
    if (name.find ("retrigger") != std::string::npos) return freq_retrigger_styles;
    if (name == "arp_pattern") return arp_patterns;
    if (name == "distortion_type") return distortion_types_long;
    if (name == "filter_shelf") return filter_shelves;
    if (name == "filter_style") return filter_style;
    if (name == "portamento_type") return off_auto_on;
    if (name.size() > 3 && (name.compare (name.size() - 3, 3, "_on") == 0 || name == "legato" || name == "sub_octave"
                            || name.find ("harmonize") != std::string::npos))
        return off_on;
    return nullptr;
}

struct Helm
{
    clap_plugin plugin {};
    const clap_host* host = nullptr;
    const clap_host_params* hostParams = nullptr;
    const clap_host_state* hostState = nullptr;
    const clap_host_webview* hostWebview = nullptr;
    const clap_host_param_hovered* hostHovered = nullptr;

    std::unique_ptr<mopo::HelmEngine> engine;
    const mopo::ValueDetails* details[paramCount] {};
    mopo::Value* controls[paramCount] {};
    mopo::cr::SmoothValue* smoothed[paramCount] {}; // the controls that glide to a new value (the cutoff)
    bool modulatable[paramCount] {};
    const mopo::Output* readouts[sourceCount] {};
    const mopo::Output* stepReadout = nullptr;
    const mopo::Output* stageReadouts[3] {};
    const mopo::Output* phaseReadouts[3] {};

    State state {};
    mopo::ModulationConnection connections[maxSlots];
    bool connected[maxSlots] {};

    // The sources only MIDI (or note expressions) move: the host says it sends MIDI (clap.note-ports' dialects), or
    // one of their events arrived. openDAW sends neither yet, the editor disables them until it does.
    enum Expressive : uint8_t { aftertouchBit = 1, modWheelBit = 2, pitchWheelBit = 4 };
    uint8_t expressive = 0;

    double sampleRate = 44100.0;
    bool active = false;
    size_t quietSamples = 0;
    bool sleeping = false;

    // editor: what the page did, for the host (output events), and what the host did, for the page
    struct OutEvent
    {
        uint16_t type;
        clap_id id;
        double value;
    };
    std::vector<OutEvent> outEvents;
    bool guiCreated = false;
    uint32_t guiWidth = editorWidth, guiHeight = editorHeight;
    bool pageValues[paramCount] {};
    bool pageSnapshot = false;
    uint8_t lastActivity[sourceCount + 15] {};
    double meterPeaks[2] {}; // per channel, the highest since the last activity
    std::vector<uint8_t> message;

    // --- setup ----------------------------------------------------------------------------------------------

    bool init()
    {
        try
        {
            engine = std::make_unique<mopo::HelmEngine>();
            mopo::control_map map = engine->getControls();
            const auto& mono = engine->getMonoModulations();
            for (uint32_t id = 0; id < paramCount; ++id)
            {
                const auto control = map.find (paramNames[id]);
                if (control == map.end() || ! mopo::Parameters::isParameter (paramNames[id]))
                {
                    std::fprintf (stderr, "helm: no control %s\n", paramNames[id]);
                    return false;
                }
                controls[id] = control->second;
                smoothed[id] = dynamic_cast<mopo::cr::SmoothValue*> (control->second);
                details[id] = &mopo::Parameters::getDetails (paramNames[id]);
                modulatable[id] = mono.count (paramNames[id]) > 0;
                state.values[id] = (float) details[id]->default_value;
            }
            for (uint8_t s = 0; s < sourceCount; ++s)
                readouts[s] = sources[s].readout != nullptr ? engine->getModulationSource (sources[s].readout) : nullptr;
            stepReadout = engine->getModulationSource ("step_sequencer_step");
            const char* stages[] = { "amp_envelope_phase", "fil_envelope_phase", "mod_envelope_phase" };
            const char* phases[] = { "mono_lfo_1_phase", "mono_lfo_2_phase", "poly_lfo_phase" };
            for (int i = 0; i < 3; ++i)
            {
                stageReadouts[i] = engine->getModulationSource (stages[i]);
                phaseReadouts[i] = engine->getModulationSource (phases[i]);
            }
            for (auto& connection : connections)
            {
                // room for the longest names, so a routing never allocates its strings
                connection.source.reserve (32);
                connection.destination.reserve (32);
            }
            state.magic = stateMagic;
            state.version = stateVersion;
            setText (state.name, "Init");
            outEvents.reserve (3 * paramCount + 64);
            message.reserve (8 + sizeof (State));
            hostParams = static_cast<const clap_host_params*> (host->get_extension (host, CLAP_EXT_PARAMS));
            hostState = static_cast<const clap_host_state*> (host->get_extension (host, CLAP_EXT_STATE));
            hostWebview = static_cast<const clap_host_webview*> (host->get_extension (host, CLAP_EXT_WEBVIEW));
            hostHovered = static_cast<const clap_host_param_hovered*> (host->get_extension (host, CLAP_EXT_PARAM_HOVERED));
            const auto* hostNotePorts = static_cast<const clap_host_note_ports*> (host->get_extension (host, CLAP_EXT_NOTE_PORTS));
            if (hostNotePorts != nullptr && hostNotePorts->supported_dialects != nullptr
                && (hostNotePorts->supported_dialects (host) & CLAP_NOTE_DIALECT_MIDI) != 0)
                expressive = aftertouchBit | modWheelBit | pitchWheelBit;
            return true;
        }
        catch (const std::exception& e)
        {
            std::fprintf (stderr, "helm: %s\n", e.what());
            return false;
        }
    }

    ~Helm()
    {
        for (size_t i = 0; i < maxSlots; ++i)
            disconnect (i);
    }

    static void setText (char* dest, const char* text)
    {
        std::memset (dest, 0, textSize);
        std::strncpy (dest, text, textSize - 1);
    }

    bool activate (double rate, uint32_t, uint32_t maxFrames)
    {
        sampleRate = rate;
        engine->setSampleRate ((int) rate);
        engine->setBufferSize (std::clamp<int> ((int) maxFrames, 1, maxChunk));
        // the engine starts with every voice active, they free themselves once silent
        for (int i = 0; i < 8 && engine->getNumActiveVoices() > 0; ++i)
            engine->process();
        quietSamples = 0;
        sleeping = false;
        active = true;
        return true;
    }

    void deactivate() { active = false; }

    void reset()
    {
        engine->killAllVoices();
        // the kill fades take VOICE_KILL_TIME (20 ms), rendered here and dropped
        const int blocks = (int) std::ceil (0.05 * sampleRate / engine->getBufferSize());
        for (int i = 0; i < blocks && engine->getNumActiveVoices() > 0; ++i)
            engine->process();
        engine->clearMemory();
        sleeping = true;
        quietSamples = 0;
    }

    // --- values and routings --------------------------------------------------------------------------------

    // hard: a smoothed control jumps (a patch or a state loads) instead of gliding there
    void setParam (clap_id id, double value, bool hard = false)
    {
        const double clamped = clampValue (*details[id], value);
        state.values[id] = (float) clamped;
        if (hard && smoothed[id] != nullptr)
            smoothed[id]->setHard (state.values[id]);
        else
            controls[id]->set (state.values[id]);
    }

    // A value from the host (events, state): the page follows if it changes. A change wakes the engine, so it
    // settles before the next note rather than with it (the arpeggiator switching drops the held notes).
    void hostParam (clap_id id, double value, bool hard = false)
    {
        if (id >= paramCount)
            return;
        const float before = state.values[id];
        setParam (id, value, hard);
        if (state.values[id] != before)
        {
            pageValues[id] = true;
            wake();
        }
    }

    void disconnect (size_t slot)
    {
        if (connected[slot])
        {
            engine->disconnectModulation (&connections[slot]);
            connected[slot] = false;
        }
    }

    // A slot as the state has it: connected (or reconnected) with its amount, or disconnected when empty
    void applySlot (size_t index)
    {
        Slot& slot = state.slots[index];
        if (slot.source >= sourceCount || slot.destination >= paramCount || ! modulatable[slot.destination]
            || ! std::isfinite (slot.amount) || slot.amount == 0.0f)
            slot = Slot {};
        auto& connection = connections[index];
        if (slot.source == emptySlot)
        {
            disconnect (index);
            return;
        }
        if (connected[index] && (connection.source != sources[slot.source].name
                                 || connection.destination != paramNames[slot.destination]))
            disconnect (index);
        connection.amount.set (slot.amount);
        if (! connected[index])
        {
            connection.resetConnection (sources[slot.source].name, paramNames[slot.destination]);
            connection.amount.set (slot.amount);
            engine->connectModulation (&connection);
            connected[index] = true;
        }
    }

    // The page's routing edit: the slot of that source and destination, else the first free one
    bool modulate (uint8_t source, uint8_t destination, float amount)
    {
        if (source >= sourceCount || destination >= paramCount || ! modulatable[destination] || ! std::isfinite (amount))
            return false;
        size_t index = maxSlots;
        for (size_t i = 0; i < maxSlots && index == maxSlots; ++i)
            if (state.slots[i].source == source && state.slots[i].destination == destination)
                index = i;
        for (size_t i = 0; i < maxSlots && index == maxSlots && amount != 0.0f; ++i)
            if (state.slots[i].source == emptySlot)
                index = i;
        if (index == maxSlots)
            return amount == 0.0f; // nothing to remove, or all slots taken
        state.slots[index] = amount == 0.0f ? Slot {} : Slot { source, destination, 0, amount };
        applySlot (index);
        return true;
    }

    // A whole state (load, a patch from the page): values, routings and names. Slots that did not change stay
    // connected, so an undo step that touched one routing re-routes only that one.
    void applyState (const State& next, bool fromHost)
    {
        for (clap_id id = 0; id < paramCount; ++id)
        {
            if (fromHost)
                hostParam (id, next.values[id], true);
            else
                setParam (id, next.values[id], true);
        }
        for (size_t i = 0; i < maxSlots; ++i)
        {
            if (std::memcmp (&state.slots[i], &next.slots[i], sizeof (Slot)) == 0 && (connected[i] || state.slots[i].source == emptySlot))
                continue;
            state.slots[i] = next.slots[i];
            state.slots[i].reserved = 0;
            applySlot (i);
        }
        std::memcpy (state.name, next.name, textSize);
        std::memcpy (state.author, next.author, textSize);
        state.name[textSize - 1] = state.author[textSize - 1] = 0;
    }

    void markDirty()
    {
        if (hostState != nullptr && hostState->mark_dirty != nullptr)
            hostState->mark_dirty (host);
    }

    void requestFlush()
    {
        if (hostParams != nullptr && hostParams->request_flush != nullptr)
            hostParams->request_flush (host);
    }

    // --- events ---------------------------------------------------------------------------------------------

    // A note event: what comes before it is rendered first, the envelopes start with the next chunk
    void noteTime (size_t& start, size_t time, float* left, float* right)
    {
        if (time > start)
        {
            render (left, right, start, time);
            start = time;
        }
        if (sleeping)
            wake();
    }

    void wake()
    {
        sleeping = false;
        quietSamples = 0;
    }

    void midi (const clap_event_midi& event, size_t& start, size_t time, float* left, float* right)
    {
        const int status = event.data[0] & 0xf0, channel = event.data[0] & 0x0f;
        const int data1 = event.data[1] & 0x7f, data2 = event.data[2] & 0x7f;
        if (status == 0x90 && data2 > 0)
        {
            noteTime (start, time, left, right);
            engine->noteOn (data1, data2 / 127.0, 0, channel);
        }
        else if (status == 0x80 || status == 0x90)
        {
            noteTime (start, time, left, right);
            engine->noteOff (data1, 0);
        }
        else if (status == 0xa0)
        {
            expressive |= aftertouchBit;
            engine->setAftertouch (data1, data2 / 127.0);
        }
        else if (status == 0xd0)
        {
            expressive |= aftertouchBit;
            engine->setChannelAftertouch (channel, data1 / 127.0);
        }
        else if (status == 0xe0)
        {
            expressive |= pitchWheelBit;
            engine->setPitchWheel (((data2 << 7) | data1) / 8191.5 - 1.0, channel);
        }
        else if (status == 0xb0 && data1 == 1)
        {
            expressive |= modWheelBit;
            engine->setModWheel (data2 / 127.0, channel);
        }
        else if (status == 0xb0 && data1 == 64)
        {
            if (data2 >= 64)
                engine->sustainOn();
            else
                engine->sustainOff();
        }
        else if (status == 0xb0 && (data1 == 120 || data1 == 123))
        {
            noteTime (start, time, left, right);
            engine->allNotesOff();
        }
    }

    // --- process --------------------------------------------------------------------------------------------

    // Renders frames [from, to) of this call, in chunks of at most mopo's buffer size
    void render (float* left, float* right, size_t from, size_t to)
    {
        while (from < to)
        {
            const int count = (int) std::min<size_t> (to - from, maxChunk);
            if (sleeping)
            {
                std::fill (left + from, left + from + count, 0.0f);
                std::fill (right + from, right + from + count, 0.0f);
            }
            else
            {
                if (engine->getBufferSize() != count)
                    engine->setBufferSize (count);
                engine->process();
                const mopo::mopo_float* l = engine->output (0)->buffer;
                const mopo::mopo_float* r = engine->output (1)->buffer;
                double peakLeft = 0.0, peakRight = 0.0;
                for (int i = 0; i < count; ++i)
                {
                    left[from + i] = (float) l[i];
                    right[from + i] = (float) r[i];
                    peakLeft = std::max (peakLeft, std::abs (l[i]));
                    peakRight = std::max (peakRight, std::abs (r[i]));
                }
                const double peak = std::max (peakLeft, peakRight);
                meterPeaks[0] = std::max (meterPeaks[0], peakLeft);
                meterPeaks[1] = std::max (meterPeaks[1], peakRight);
                // asleep after half a second without a voice and below -120 dB
                if (engine->getNumActiveVoices() == 0 && peak < 1.0e-6 && engine->getPressedNotes().size() == 0)
                    sleeping = (quietSamples += (size_t) count) > (size_t) (sampleRate / 2);
                else
                    quietSamples = 0;
            }
            from += (size_t) count;
        }
    }

    clap_process_status process (const clap_process& process)
    {
        const size_t frames = process.frames_count;
        if (process.audio_outputs_count < 1 || process.audio_outputs[0].channel_count < 2 || frames == 0)
            return CLAP_PROCESS_CONTINUE;
        float* left = process.audio_outputs[0].data32[0];
        float* right = process.audio_outputs[0].data32[1];

        if (const auto* transport = process.transport; transport != nullptr)
        {
            if ((transport->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0 && transport->tempo > 0.0)
                engine->setBpm (transport->tempo);
            // tempo-synced LFOs and the step sequencer follow the playhead, as upstream's correctToTime
            if ((transport->flags & CLAP_TRANSPORT_IS_PLAYING) != 0 && (transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0
                && transport->tempo > 0.0)
                engine->correctToTime ((double) transport->song_pos_beats / CLAP_BEATTIME_FACTOR * 60.0 / transport->tempo
                                       * sampleRate);
        }

        size_t start = 0;
        const uint32_t count = process.in_events->size (process.in_events);
        for (uint32_t i = 0; i < count; ++i)
        {
            const clap_event_header* header = process.in_events->get (process.in_events, i);
            if (header->space_id != CLAP_CORE_EVENT_SPACE_ID)
                continue;
            // a note at the end of the block starts on its last sample
            const size_t time = std::clamp<size_t> (header->time, start, frames - 1);
            switch (header->type)
            {
                case CLAP_EVENT_NOTE_ON:
                {
                    const auto* e = reinterpret_cast<const clap_event_note*> (header);
                    if (e->key < 0 || e->key > 127)
                        break;
                    noteTime (start, time, left, right);
                    engine->noteOn (e->key, std::clamp (e->velocity, 0.0, 1.0), 0, std::clamp<int> (e->channel, 0, 15));
                    break;
                }
                case CLAP_EVENT_NOTE_OFF:
                case CLAP_EVENT_NOTE_CHOKE:
                {
                    const auto* e = reinterpret_cast<const clap_event_note*> (header);
                    if (e->key < 0 || e->key > 127)
                    {
                        if (e->key == -1 && header->type == CLAP_EVENT_NOTE_CHOKE)
                        {
                            noteTime (start, time, left, right);
                            engine->allNotesOff();
                        }
                        break;
                    }
                    noteTime (start, time, left, right);
                    engine->noteOff (e->key, 0);
                    break;
                }
                case CLAP_EVENT_NOTE_EXPRESSION:
                {
                    const auto* e = reinterpret_cast<const clap_event_note_expression*> (header);
                    if (e->expression_id == CLAP_NOTE_EXPRESSION_PRESSURE)
                        expressive |= aftertouchBit;
                    if (e->expression_id == CLAP_NOTE_EXPRESSION_PRESSURE && e->key >= 0 && e->key <= 127)
                        engine->setAftertouch (e->key, std::clamp (e->value, 0.0, 1.0));
                    break;
                }
                case CLAP_EVENT_MIDI:
                    midi (*reinterpret_cast<const clap_event_midi*> (header), start, time, left, right);
                    break;
                case CLAP_EVENT_PARAM_VALUE:
                {
                    const auto* e = reinterpret_cast<const clap_event_param_value*> (header);
                    hostParam (e->param_id, e->value);
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

    // --- editor ---------------------------------------------------------------------------------------------

    bool receive (const uint8_t* data, uint32_t size)
    {
        if (size < 1)
            return false;
        const auto u16 = [data] (size_t at) { return (uint32_t) (data[at] | data[at + 1] << 8); };
        const auto f32 = [data] (size_t at)
        {
            float value;
            std::memcpy (&value, data + at, sizeof value);
            return value;
        };
        switch (data[0])
        {
            case pageHello:
                sendDescription();
                pageSnapshot = true;
                std::fill (std::begin (lastActivity), std::end (lastActivity), 0xff);
                sendPageUpdates();
                return true;
            case pagePoll:
                sendPageUpdates();
                return true;
            case pageBegin:
            case pageEnd:
                if (size < 4 || u16 (2) >= paramCount)
                    return false;
                outEvents.push_back ({ (uint16_t) (data[0] == pageBegin ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                                        : CLAP_EVENT_PARAM_GESTURE_END),
                                       u16 (2), 0.0 });
                requestFlush();
                return true;
            case pageSet:
            {
                if (size < 8 || u16 (2) >= paramCount)
                    return false;
                const clap_id id = u16 (2);
                setParam (id, f32 (4));
                outEvents.push_back ({ CLAP_EVENT_PARAM_VALUE, id, state.values[id] });
                requestFlush();
                return true;
            }
            case pageHover:
                if (size < 4 || hostHovered == nullptr || hostHovered->update == nullptr)
                    return false;
                hostHovered->update (host, u16 (2) < paramCount ? u16 (2) : CLAP_INVALID_ID);
                return true;
            case pageModulate:
            {
                if (size < 8 || ! modulate (data[1], data[2], f32 (4)))
                    return false;
                // a drag's live amounts play, the host hears of the routing at the drag's end
                if (data[3] == 0)
                    markDirty();
                return true;
            }
            case pagePatch:
            {
                if (size < 8 + sizeof (State) - 8)
                    return false;
                State next = state;
                std::memcpy (reinterpret_cast<uint8_t*> (&next) + 8, data + 8, sizeof (State) - 8);
                loadPatch (next);
                return true;
            }
            default:
                return false;
        }
    }

    // A patch from the page: every value that changes goes to the host inside one gesture, so the patch is one undo
    // step; the routings and names reach the host with the state save that follows
    void loadPatch (const State& next)
    {
        bool changed[paramCount] {};
        for (clap_id id = 0; id < paramCount; ++id)
            changed[id] = (float) clampValue (*details[id], next.values[id]) != state.values[id];
        applyState (next, false);
        wake();
        for (clap_id id = 0; id < paramCount; ++id)
            if (changed[id])
                outEvents.push_back ({ CLAP_EVENT_PARAM_GESTURE_BEGIN, id, 0.0 });
        for (clap_id id = 0; id < paramCount; ++id)
            if (changed[id])
                outEvents.push_back ({ CLAP_EVENT_PARAM_VALUE, id, state.values[id] });
        for (clap_id id = 0; id < paramCount; ++id)
            if (changed[id])
                outEvents.push_back ({ CLAP_EVENT_PARAM_GESTURE_END, id, 0.0 });
        markDirty();
        requestFlush();
        pageSnapshot = true; // the page shows the values the plugin took (clamped, rounded)
    }

    void writeEvents (const clap_output_events& out)
    {
        for (const auto& e : outEvents)
        {
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
        }
        outEvents.clear();
    }

    void send (const uint8_t* bytes, size_t size)
    {
        if (guiCreated && hostWebview != nullptr && hostWebview->send != nullptr)
            hostWebview->send (host, bytes, (uint32_t) size);
    }

    // The parameter table and the sources, once per hello: the page builds its controls and texts from it
    void sendDescription()
    {
        std::string json = "    {\"params\":[";
        json[0] = (char) pluginDescribe;
        json[1] = json[2] = json[3] = 0;
        char buffer[512];
        for (clap_id id = 0; id < paramCount; ++id)
        {
            const auto& d = *details[id];
            std::snprintf (buffer, sizeof buffer,
                           "%s{\"name\":\"%s\",\"label\":\"%s\",\"min\":%.9g,\"max\":%.9g,\"steps\":%d,\"default\":%.9g,"
                           "\"skew\":%d,\"offset\":%.9g,\"multiply\":%.9g,\"invert\":%s,\"units\":\"%s\",\"mod\":%s",
                           id == 0 ? "" : ",", paramNames[id], d.display_name.c_str(), d.min, d.max, d.steps,
                           d.default_value, (int) d.display_skew, d.post_offset, d.display_multiply,
                           d.display_invert ? "true" : "false", d.display_units.c_str(),
                           modulatable[id] ? "true" : "false");
            json += buffer;
            if (const std::string* names = choices (paramNames[id]); names != nullptr && d.steps > 0)
            {
                json += ",\"choices\":[";
                for (int c = 0; c < d.steps; ++c)
                    json += (c == 0 ? "\"" : ",\"") + names[(int) d.min + c] + "\"";
                json += "]";
            }
            json += "}";
        }
        json += "],\"sources\":[";
        for (uint8_t s = 0; s < sourceCount; ++s)
            json += std::string (s == 0 ? "\"" : ",\"") + sources[s].name + "\"";
        json += "]}";
        send (reinterpret_cast<const uint8_t*> (json.data()), json.size());
    }

    // What changed since the last answer: a snapshot after a hello, a state load or a patch, else the values the
    // host changed, and the activity readouts when they moved. Nothing when nothing did.
    void sendPageUpdates()
    {
        if (pageSnapshot)
        {
            pageSnapshot = false;
            std::fill (std::begin (pageValues), std::end (pageValues), false);
            message.assign (8, 0);
            message[0] = pluginSnapshot;
            const auto* bytes = reinterpret_cast<const uint8_t*> (&state);
            message.insert (message.end(), bytes + 8, bytes + sizeof (State));
            send (message.data(), message.size());
        }
        uint16_t changed = 0;
        message.assign (4, 0);
        for (clap_id id = 0; id < paramCount; ++id)
        {
            if (! pageValues[id])
                continue;
            pageValues[id] = false;
            uint8_t entry[8] { (uint8_t) id, (uint8_t) (id >> 8), 0, 0 };
            std::memcpy (entry + 4, &state.values[id], sizeof (float));
            message.insert (message.end(), entry, entry + 8);
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
        uint8_t activity[sourceCount + 15] {};
        const auto decibels = [] (double peak)
        { return peak <= 0.0 ? 0 : (uint8_t) std::clamp<long> (std::lround ((20.0 * std::log10 (peak) + 72.0) * 2.0), 0, 255); };
        const auto read = [] (const mopo::Output* output) { return output != nullptr ? output->buffer[0] : 0.0; };
        // asleep, the free-running LFOs are not shown: nothing to send while nothing sounds
        for (uint8_t s = 0; s < sourceCount && ! sleeping; ++s)
            activity[s] = (uint8_t) (int8_t) std::lround (std::clamp (read (readouts[s]), -1.0, 1.0) * 127.0);
        const bool silent = sleeping || engine->getNumActiveVoices() == 0;
        activity[sourceCount] = (uint8_t) std::clamp<long> (std::lround (read (stepReadout)), 0, 31);
        activity[sourceCount + 1] = silent ? 0 : (uint8_t) std::min (engine->getNumActiveVoices(), 255);
        activity[sourceCount + 2] = decibels (meterPeaks[0]);
        for (int i = 0; i < 3 && ! sleeping; ++i)
        {
            activity[sourceCount + 3 + i] = (uint8_t) std::clamp<long> (std::lround (read (stageReadouts[i])), 0, 3);
            activity[sourceCount + 6 + i] = (uint8_t) std::lround (std::clamp (read (phaseReadouts[i]), 0.0, 1.0) * 255.0);
        }
        const auto signed8 = [] (double value) { return (uint8_t) (int8_t) std::lround (std::clamp (value, -1.0, 1.0) * 127.0); };
        for (int i = 0; i < 2 && ! sleeping; ++i)
        {
            const mopo::HelmLfo* lfo = engine->getMonoLfo (i);
            activity[sourceCount + 9 + 2 * i] = signed8 (lfo->randomFrom());
            activity[sourceCount + 10 + 2 * i] = signed8 (lfo->randomTo());
        }
        activity[sourceCount + 13] = expressive;
        activity[sourceCount + 14] = decibels (meterPeaks[1]);
        meterPeaks[0] = meterPeaks[1] = 0.0;
        if (std::memcmp (activity, lastActivity, sizeof activity) == 0)
            return;
        std::memcpy (lastActivity, activity, sizeof activity);
        message.assign (4, 0);
        message[0] = pluginActivity;
        message.insert (message.end(), activity, activity + sizeof activity);
        send (message.data(), message.size());
    }

    // --- state ----------------------------------------------------------------------------------------------

    bool save (const clap_ostream& stream) const
    {
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
        State next {};
        auto* bytes = reinterpret_cast<uint8_t*> (&next);
        size_t read = 0;
        while (read < sizeof (State))
        {
            const int64_t n = stream.read (&stream, bytes + read, sizeof (State) - read);
            if (n <= 0)
                break;
            read += (size_t) n;
        }
        if (read != sizeof (State) || next.magic != stateMagic || next.version != stateVersion)
            return false;
        applyState (next, true);
        pageSnapshot = true;
        wake();
        return true;
    }
};

Helm& self (const clap_plugin* plugin) { return *static_cast<Helm*> (plugin->plugin_data); }

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

// Module paths group the parameters in a host's lists
const char* paramModule (const std::string& name)
{
    struct Prefix
    {
        const char* prefix;
        const char* module;
    };
    static constexpr Prefix prefixes[] = {
        { "osc_1", "Oscillator 1" }, { "unison_1", "Oscillator 1" }, { "osc_2", "Oscillator 2" },
        { "unison_2", "Oscillator 2" }, { "osc_feedback", "Feedback" }, { "cross_mod", "Oscillators" },
        { "sub_", "Sub" }, { "noise", "Noise" }, { "amp_", "Amplitude Envelope" }, { "fil_", "Filter Envelope" },
        { "mod_", "Mod Envelope" }, { "filter", "Filter" }, { "cutoff", "Filter" }, { "resonance", "Filter" },
        { "keytrack", "Filter" }, { "formant", "Formant" }, { "mono_lfo_1", "Mono LFO 1" },
        { "mono_lfo_2", "Mono LFO 2" }, { "poly_lfo", "Poly LFO" }, { "step", "Step Sequencer" }, { "num_steps", "Step Sequencer" },
        { "arp", "Arpeggiator" }, { "stutter", "Stutter" }, { "distortion", "Distortion" }, { "delay", "Delay" },
        { "reverb", "Reverb" }
    };
    for (const auto& p : prefixes)
        if (name.compare (0, std::strlen (p.prefix), p.prefix) == 0)
            return p.module;
    return "Voice";
}

bool paramsInfo (const clap_plugin* plugin, uint32_t index, clap_param_info* info)
{
    if (index >= paramCount)
        return false;
    const auto& d = *self (plugin).details[index];
    std::memset (info, 0, sizeof *info);
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (d.steps > 0)
    {
        info->flags |= CLAP_PARAM_IS_STEPPED;
        if (choices (paramNames[index]) != nullptr)
            info->flags |= CLAP_PARAM_IS_ENUM;
    }
    std::snprintf (info->name, sizeof info->name, "%s", d.display_name.c_str());
    std::snprintf (info->module, sizeof info->module, "%s", paramModule (paramNames[index]));
    info->min_value = d.min;
    info->max_value = d.max;
    info->default_value = d.default_value;
    return true;
}

bool paramsValue (const clap_plugin* plugin, clap_id id, double* value)
{
    if (id >= paramCount)
        return false;
    *value = self (plugin).state.values[id];
    return true;
}

bool paramsValueToText (const clap_plugin* plugin, clap_id id, double value, char* text, uint32_t size)
{
    if (id >= paramCount || size == 0)
        return false;
    const auto& d = *self (plugin).details[id];
    if (const std::string* names = choices (paramNames[id]); names != nullptr && d.steps > 0)
    {
        // Helm's string lookups are indexed by the value (the stutter tempo starts at 6)
        const int index = std::clamp ((int) std::lround (value - d.min), 0, d.steps - 1);
        std::snprintf (text, size, "%s", names[(int) d.min + index].c_str());
        return true;
    }
    if (std::strcmp (paramNames[id], "volume") == 0)
    {
        // a quadratic gain, as the editor shows it
        if (value <= 0.0)
            std::snprintf (text, size, "-inf dB");
        else
            std::snprintf (text, size, "%.1f dB", 40.0 * std::log10 (value));
        return true;
    }
    formatValue (d, value, text, size);
    return true;
}

bool paramsTextToValue (const clap_plugin* plugin, clap_id id, const char* text, double* value)
{
    if (id >= paramCount)
        return false;
    const auto& d = *self (plugin).details[id];
    if (const std::string* names = choices (paramNames[id]); names != nullptr && d.steps > 0)
        for (int c = 0; c < d.steps; ++c)
            if (names[(int) d.min + c] == text)
            {
                *value = d.min + c;
                return true;
            }
    char* end = nullptr;
    const double number = std::strtod (text, &end);
    if (end == text)
        return false;
    if (std::strcmp (paramNames[id], "volume") == 0)
    {
        *value = clampValue (d, std::pow (10.0, number / 40.0));
        return true;
    }
    // the display value back to the control's: undo the multiplier, the inversion, the offset and the skew
    double display = number / d.display_multiply;
    if (d.display_invert)
        display = 1.0 / display;
    display -= d.post_offset;
    switch (d.display_skew)
    {
        case mopo::ValueDetails::kQuadratic: display = std::sqrt (std::max (display, 0.0)); break;
        case mopo::ValueDetails::kExponential: display = std::log2 (std::max (display, 1.0e-9)); break;
        case mopo::ValueDetails::kSquareRoot: display = display * display; break;
        default: break;
    }
    *value = clampValue (d, display);
    return std::isfinite (*value);
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
            p.hostParam (e->param_id, e->value);
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

// The editor: ui/index.html in the bundle, resizable at its aspect ratio

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

// The size of the editor's aspect ratio closest to the request (the smaller of its two scales), within the limits
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
    "Helm",
    "Matt Tytel",
    "https://tytel.org/helm",
    "",
    "",
    HELM_VERSION,
    "Polyphonic subtractive synth: two unison oscillators, sub, feedback, blendable filter, step sequencer, "
    "arpeggiator and effects, deep modulation",
    features
};

const clap_plugin* createPlugin (const clap_host* host)
{
    auto* p = new (std::nothrow) Helm();
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
