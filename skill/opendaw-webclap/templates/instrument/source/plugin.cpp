// {{NAME}}: {{DESCRIPTION}}
// An instrument on the openDAW WebCLAP kit (kit/opendaw_kit.h): eight voices, saw or square, a resonant lowpass, an
// attack and release envelope.
#include "opendaw_kit.h"

using namespace opendaw;

// parameter ids: 0, 1, 2, ... in table order, never reused for another meaning once published
enum : clap_id {CUTOFF, ATTACK, RELEASE, RESONANCE, VOLUME, WAVE, PARAM_COUNT};

static constexpr const char *WAVES[] = {"Saw", "Square"};

static constexpr Param PARAMS[PARAM_COUNT] = {
    Param::knob(CUTOFF, "Cutoff", 40.0, 18000.0, 2400.0, Unit::Hertz, Curve::Exponential),
    Param::knob(ATTACK, "Attack", 1.0, 2000.0, 5.0, Unit::Milliseconds, Curve::Squared),
    Param::knob(RELEASE, "Release", 5.0, 4000.0, 300.0, Unit::Milliseconds, Curve::Squared),
    Param::knob(RESONANCE, "Res", 0.0, 100.0, 20.0, Unit::Percent),
    Param::knob(VOLUME, "Volume", -48.0, 6.0, -6.0, Unit::Decibel),
    Param::choice(WAVE, "Wave", WAVES, 2, 0)
};

static constexpr Info INFO = {
    "{{CLAP_ID}}", "{{NAME}}", "{{VENDOR}}", "1.0.0", "{{DESCRIPTION}}",
    Kind::Instrument, "synthesizer", {{PANEL_WIDTH}}
};

static constexpr int VOICES = 8;

// a band limited step for the oscillators' edges (polyBLEP)
static float blep(float phase, float increment) {
    if (phase < increment) {
        const float t = phase / increment;
        return t + t - t * t - 1.0f;
    }
    if (phase > 1.0f - increment) {
        const float t = (phase - 1.0f) / increment;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

struct Voice {
    bool active = false, released = false;
    int key = 0, channel = 0;
    uint32_t age = 0;
    float phase = 0.0f, increment = 0.0f, velocity = 0.0f, envelope = 0.0f;
    float low[2] = {0.0f, 0.0f}; // the filter's two integrator states (TPT state variable filter)
};

struct Dsp : DspBase {
    double sampleRate = 48000.0;
    Voice voices[VOICES];
    uint32_t clock = 0;
    Smoothed cutoff, resonance, volume;
    float attackStep = 0.0f, releaseFactor = 0.0f;
    int wave = 0;

    void activate(double rate, uint32_t maxFrames, const double *params) {
        (void) maxFrames;
        sampleRate = rate;
        cutoff.prepare(rate, 10.0, float(params[CUTOFF]));
        resonance.prepare(rate, 10.0, float(params[RESONANCE] / 100.0));
        volume.prepare(rate, 20.0, dbToGain(params[VOLUME]));
        for (clap_id id = 0; id < PARAM_COUNT; id++) {changed(id, params[id]);}
        reset();
    }

    // everything silent at once: openDAW resets on stop and expects no sound after it
    void reset() {
        for (Voice &voice : voices) {voice = Voice{};}
    }

    void changed(clap_id id, double value) {
        switch (id) {
            case CUTOFF: cutoff.set(float(value)); break;
            case RESONANCE: resonance.set(float(value / 100.0)); break;
            case ATTACK: attackStep = float(1000.0 / (value * sampleRate)); break;
            case RELEASE: releaseFactor = float(std::exp(std::log(1e-5) * 1000.0 / (value * sampleRate))); break;
            case VOLUME: volume.set(dbToGain(value)); break;
            case WAVE: wave = int(value); break;
            default: break;
        }
    }

    void noteOn(const Note &note, Notes &) {
        Voice *chosen = nullptr;
        for (Voice &voice : voices) {
            if (voice.active && voice.key == note.key && voice.channel == note.channel) {chosen = &voice; break;}
        }
        for (Voice &voice : voices) {
            if (chosen == nullptr && !voice.active) {chosen = &voice;}
        }
        // all busy: the oldest released voice goes, else the oldest
        if (chosen == nullptr) {
            chosen = &voices[0];
            for (Voice &voice : voices) {
                if ((voice.released && !chosen->released) ||
                    (voice.released == chosen->released && voice.age < chosen->age)) {chosen = &voice;}
            }
        }
        const bool retrigger = chosen->active;
        chosen->active = true;
        chosen->released = false;
        chosen->key = note.key;
        chosen->channel = note.channel;
        chosen->age = clock++;
        chosen->velocity = float(note.velocity);
        chosen->increment = float(keyToHz(note.key) / sampleRate);
        if (!retrigger) {
            chosen->phase = 0.0f;
            chosen->envelope = 0.0f;
            chosen->low[0] = chosen->low[1] = 0.0f;
        }
    }

    void noteOff(const Note &note, Notes &) {
        for (Voice &voice : voices) {
            if (voice.active && !voice.released && voice.key == note.key && voice.channel == note.channel) {voice.released = true;}
        }
    }

    void process(Block &block) {
        bool any = false;
        for (const Voice &voice : voices) {any = any || voice.active;}
        // asleep without voices: the output is already silent
        if (!any) {
            cutoff.value = cutoff.target;
            resonance.value = resonance.target;
            volume.value = volume.target;
            return;
        }
        for (uint32_t frame = 0; frame < block.frames; frame++) {
            const float hz = cutoff.next(), damping = 2.0f - 1.9f * resonance.next(), level = volume.next();
            const float g = std::tan(3.14159265f * std::fmin(hz, float(sampleRate) * 0.45f) / float(sampleRate));
            const float a1 = 1.0f / (1.0f + g * (g + damping));
            float sum = 0.0f;
            for (Voice &voice : voices) {
                if (!voice.active) {continue;}
                if (voice.released) {
                    voice.envelope *= releaseFactor;
                    // a voice under -100 dB is free again
                    if (voice.envelope < 1e-5f) {
                        voice.active = false;
                        continue;
                    }
                } else if (voice.envelope < 1.0f) {
                    voice.envelope = std::fmin(1.0f, voice.envelope + attackStep);
                }
                float sample = 2.0f * voice.phase - 1.0f - blep(voice.phase, voice.increment);
                if (wave == 1) {
                    float shifted = voice.phase + 0.5f;
                    if (shifted >= 1.0f) {shifted -= 1.0f;}
                    sample -= 2.0f * shifted - 1.0f - blep(shifted, voice.increment);
                }
                voice.phase += voice.increment;
                if (voice.phase >= 1.0f) {voice.phase -= 1.0f;}
                const float high = (sample - (damping + g) * voice.low[0] - voice.low[1]) * a1;
                const float band = g * high + voice.low[0];
                const float lowpass = g * band + voice.low[1];
                voice.low[0] = flush(g * high + band);
                voice.low[1] = flush(g * band + lowpass);
                sum += lowpass * voice.envelope * voice.velocity;
            }
            const float out = sum * level * 0.25f;
            block.output[0][frame] += out;
            block.output[1][frame] += out;
        }
    }
};

OPENDAW_PLUGIN(Dsp, INFO, PARAMS)
