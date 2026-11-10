// From one rate to another, stereo, with a windowed sinc (8 zero crossings, cut below the lower Nyquist). Output
// sample k sits at input time k * step, it comes out as soon as the samples after it are in. Equal rates pass through.
// The resampler of ports/pearls/plugin/pearls.cpp.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace openrb {

constexpr int SINC_ZEROS = 8;
constexpr int SINC_RESOLUTION = 256;
constexpr int RING = 512;

inline float *sinc_table() {
    static float table[SINC_ZEROS * SINC_RESOLUTION + 2];
    static bool built = false;
    if (!built) {
        for (int index = 0; index < SINC_ZEROS * SINC_RESOLUTION + 2; index++) {
            const double t = static_cast<double>(index) / SINC_RESOLUTION;
            const double x = M_PI * t;
            const double sinc = index == 0 ? 1.0 : std::sin(x) / x;
            // Blackman-Harris over the zero crossings
            const double w = t >= SINC_ZEROS ? 0.0 : 0.5 + 0.5 * t / SINC_ZEROS;
            const double a = 2.0 * M_PI * w;
            const double window = 0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a);
            table[index] = static_cast<float>(t >= SINC_ZEROS ? 0.0 : sinc * window);
        }
        built = true;
    }
    return table;
}

inline float kernel(const float *table, double t) {
    t = std::fabs(t) * SINC_RESOLUTION;
    const int index = static_cast<int>(t);
    if (index >= SINC_ZEROS * SINC_RESOLUTION) {return 0.0f;}
    const float fraction = static_cast<float>(t - index);
    return table[index] + (table[index + 1] - table[index]) * fraction;
}

struct Resampler {
    bool identity = true;
    double step = 1.0, cutoff = 1.0, position = 0.0;
    int half = 0; // input samples either side of an output
    int64_t written = 0;
    const float *table = nullptr;
    float ring[2][RING];

    void init(double in_rate, double out_rate) {
        table = sinc_table();
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
                const float w = kernel(table, (position - static_cast<double>(index)) * cutoff);
                left += ring[0][index & (RING - 1)] * w;
                right += ring[1][index & (RING - 1)] * w;
            }
            emit(left * static_cast<float>(cutoff), right * static_cast<float>(cutoff));
            position += step;
        }
    }
};

} // namespace openrb
