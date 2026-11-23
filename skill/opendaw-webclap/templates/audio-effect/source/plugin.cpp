// {{NAME}}: {{DESCRIPTION}}
// An audio effect on the openDAW WebCLAP kit (kit/opendaw_kit.h): soft clipping drive, a tone filter, mix and output.
#include "opendaw_kit.h"

using namespace opendaw;

// parameter ids: 0, 1, 2, ... in table order, never reused for another meaning once published
enum : clap_id {DRIVE, TONE, MIX, OUTPUT, PARAM_COUNT};

static constexpr Param PARAMS[PARAM_COUNT] = {
    Param::knob(DRIVE, "Drive", 0.0, 36.0, 12.0, Unit::Decibel),
    Param::knob(TONE, "Tone", 500.0, 20000.0, 8000.0, Unit::Hertz, Curve::Exponential),
    Param::knob(MIX, "Mix", 0.0, 100.0, 100.0, Unit::Percent),
    Param::knob(OUTPUT, "Output", -24.0, 12.0, 0.0, Unit::Decibel)
};

static constexpr Info INFO = {
    "{{CLAP_ID}}", "{{NAME}}", "{{VENDOR}}", "1.0.0", "{{DESCRIPTION}}",
    Kind::AudioEffect, "distortion", {{PANEL_WIDTH}}
};

struct Dsp : DspBase {
    double sampleRate = 48000.0;
    Smoothed drive, mix, output, tone;
    float state[2] = {0.0f, 0.0f};
    Peak inputPeak, outputPeak;

    void activate(double rate, uint32_t maxFrames, const double *params) {
        (void) maxFrames;
        sampleRate = rate;
        drive.prepare(rate, 20.0, dbToGain(params[DRIVE]));
        tone.prepare(rate, 20.0, coefficient(params[TONE]));
        mix.prepare(rate, 20.0, float(params[MIX] / 100.0));
        output.prepare(rate, 20.0, dbToGain(params[OUTPUT]));
        reset();
    }

    void reset() {state[0] = state[1] = 0.0f;}

    float coefficient(double hz) const {return float(1.0 - std::exp(-6.283185307179586 * hz / sampleRate));}

    void changed(clap_id id, double value) {
        switch (id) {
            case DRIVE: drive.set(dbToGain(value)); break;
            case TONE: tone.set(coefficient(value)); break;
            case MIX: mix.set(float(value / 100.0)); break;
            case OUTPUT: output.set(dbToGain(value)); break;
            default: break;
        }
    }

    void process(Block &block) {
        for (uint32_t frame = 0; frame < block.frames; frame++) {
            const float gain = drive.next(), wet = mix.next(), level = output.next(), lowpass = tone.next();
            // full scale stays full scale, whatever the drive
            const float makeup = 1.0f / std::tanh(gain);
            for (int channel = 0; channel < 2; channel++) {
                const float in = block.input[channel][frame];
                inputPeak.feed(in);
                const float clipped = std::tanh(in * gain) * makeup;
                state[channel] = flush(state[channel] + lowpass * (clipped - state[channel]));
                const float out = (in + wet * (state[channel] - in)) * level;
                block.output[channel][frame] = out;
                outputPeak.feed(out);
            }
        }
    }

    // the page's display: input and output peak since the last poll
    uint32_t meters(float *out, uint32_t capacity) {
        if (capacity < 2) {return 0;}
        out[0] = inputPeak.take();
        out[1] = outputPeak.take();
        return 2;
    }
};

OPENDAW_PLUGIN(Dsp, INFO, PARAMS)
