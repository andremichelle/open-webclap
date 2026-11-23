// {{NAME}}: {{DESCRIPTION}}
// A MIDI effect (CLAP note effect) on the openDAW WebCLAP kit (kit/opendaw_kit.h): transposes every note and adds
// a second voice at an interval, with its own velocity.
#include "opendaw_kit.h"

using namespace opendaw;

// parameter ids: 0, 1, 2, ... in table order, never reused for another meaning once published
enum : clap_id {TRANSPOSE, INTERVAL, VELOCITY, PARAM_COUNT};

static constexpr const char *INTERVALS[] = {"None", "Third", "Fifth", "Octave"};
static constexpr int SEMITONES[] = {0, 4, 7, 12};

static constexpr Param PARAMS[PARAM_COUNT] = {
    Param::integer(TRANSPOSE, "Transpose", -24, 24, 0, Unit::Semitones),
    Param::choice(INTERVAL, "Interval", INTERVALS, 4, 2),
    Param::knob(VELOCITY, "Velocity", 0.0, 100.0, 70.0, Unit::Percent)
};

static constexpr Info INFO = {
    "{{CLAP_ID}}", "{{NAME}}", "{{VENDOR}}", "1.0.0", "{{DESCRIPTION}}",
    Kind::MidiEffect, "pitch", {{PANEL_WIDTH}}
};

struct Dsp : DspBase {
    // what each incoming key started, so its note off ends exactly that, whatever the parameters say by then
    struct Started {
        int8_t main = -1, added = -1;
    };
    Started started[16][128];
    const double *params = nullptr;

    void activate(double rate, uint32_t maxFrames, const double *values) {
        (void) rate; (void) maxFrames;
        params = values;
        reset();
    }

    // the host ends what it got from us: only forget
    void reset() {
        for (auto &channel : started) {
            for (Started &entry : channel) {entry = Started{};}
        }
    }

    void noteOn(const Note &note, Notes &notes) {
        Started &entry = started[note.channel][note.key];
        // a key struck again before its release: end what it started first
        if (entry.main >= 0) {notes.off(entry.main, note.channel);}
        if (entry.added >= 0) {notes.off(entry.added, note.channel);}
        entry = Started{};
        const int key = note.key + int(params[TRANSPOSE]);
        if (key >= 0 && key <= 127) {
            notes.on(key, note.velocity, note.channel);
            entry.main = int8_t(key);
        }
        const int interval = SEMITONES[int(params[INTERVAL])];
        if (interval > 0 && key + interval >= 0 && key + interval <= 127) {
            notes.on(key + interval, note.velocity * params[VELOCITY] / 100.0, note.channel);
            entry.added = int8_t(key + interval);
        }
    }

    void noteOff(const Note &note, Notes &notes) {
        Started &entry = started[note.channel][note.key];
        if (entry.main >= 0) {notes.off(entry.main, note.channel);}
        if (entry.added >= 0) {notes.off(entry.added, note.channel);}
        entry = Started{};
    }
};

OPENDAW_PLUGIN(Dsp, INFO, PARAMS)
