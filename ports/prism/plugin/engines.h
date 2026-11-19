// The eight scope engines, after Prism's plugin/Source/*Engine.h: the same DSP calls on the same settings, without
// JUCE. Each builds its frame as binary (page/bridge.ts reads it) instead of a juce::var of base64 strings: after
// the frame header, its fields in a fixed order, 4 bytes each (f32 or u32), an array as its u32 count and floats.
#pragma once

#include "json.h"
#include "spectrum.h"
#include "oscilloscope.h"
#include "vumeter.h"
#include "lufsmeter.h"
#include "vectorscope.h"
#include "spectrogram.h"
#include "waveform.h"
#include "waterfall.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace prism {

enum Scope : uint8_t {
    SCOPE_SPECTRUM = 0, SCOPE_OSCILLOSCOPE, SCOPE_VECTORSCOPE, SCOPE_SPECTROGRAM, SCOPE_VUMETER, SCOPE_LUFSMETER,
    SCOPE_WAVEFORM, SCOPE_WATERFALL, SCOPE_COUNT
};

// a frame in the making: the header (written by the plugin), then the engine's fields
struct Frame {
    std::vector<uint8_t> bytes;

    void clear(size_t header) {bytes.assign(header, 0);}
    void u32(uint32_t value) {append(&value, 4);}
    void f32(double value) {const float v = static_cast<float>(value); append(&v, 4);}
    void floats(const float *values, size_t count) {
        u32(static_cast<uint32_t>(count));
        append(values, count * sizeof(float));
    }
    void floats(const std::vector<float> &values) {floats(values.data(), values.size());}

private:
    void append(const void *data, size_t size) {
        const uint8_t *from = static_cast<const uint8_t *>(data);
        bytes.insert(bytes.end(), from, from + size);
    }
};

class Engine {
public:
    virtual ~Engine() = default;
    virtual void setSampleRate(double sampleRate) = 0;
    // the scope's settings (prismConfig)
    virtual void configure(const Var &settings) = 0;
    // the canvas-derived settings some scopes need (prismSpectrogramConfig, prismWaterfallConfig, ...)
    virtual void configureNative(const Var &) {}
    virtual void setMeasurementActive(bool) {}
    virtual void resetAudioHistory() {}
    // audio since the last frame, count may be 0
    virtual void process(const float *left, const float *right, int count) = 0;
    // the DSP runs on the audio's clock, so it may take the audio a block at a time (spreading its work over the
    // blocks) instead of a display frame at a time as upstream's editor feeds it. Not the spectrum: its FFT and its
    // smoothing run once per push, at the display's rate.
    virtual bool takesBlocks() const {return true;}
    virtual void buildFrame(double sampleRate, Frame &frame) = 0;
};

class SpectrumEngine : public Engine {
public:
    bool takesBlocks() const override {return false;}
    void setSampleRate(double sampleRate) override {spectrum.setSampleRate(static_cast<float>(sampleRate));}

    void configure(const Var &settings) override {
        const int fftSize = static_cast<int>(settings.getProperty("fftSize", 2048));
        if (fftSize > 0 && static_cast<size_t>(fftSize) != spectrum.getFFTSize()) {spectrum.setFFTSize(static_cast<size_t>(fftSize));}
        configuredSmoothing = std::clamp(static_cast<float>(settings.getProperty("smoothing", 0.9)), 0.0f, 0.99f);
        applySmoothing();
    }

    void configureNative(const Var &config) override {
        spectrum.setReferenceEnabled(static_cast<bool>(config.getProperty("referenceEnabled", false)));
    }

    void resetAudioHistory() override {spectrum.reset();}

    void setMeasurementActive(bool active) override {
        if (measurementActive == active) {return;}
        measurementActive = active;
        applySmoothing();
    }

    void process(const float *left, const float *right, int count) override {
        if (count > 0) {spectrum.pushStereoSamples(left, right, static_cast<size_t>(count));}
        else {spectrum.pushStereoSamples(nullptr, nullptr, 0);} // recompute / decay
    }

    // sampleRate, hasSpectrumData, referenceEnabled, referenceMeanSquare, referenceSeconds, magnitudes, side, channelMax
    void buildFrame(double sampleRate, Frame &frame) override {
        frame.f32(sampleRate);
        frame.u32(spectrum.hasSpectrumData() ? 1 : 0);
        frame.u32(spectrum.isReferenceEnabled() ? 1 : 0);
        frame.f32(spectrum.getReferenceMeanSquare());
        frame.f32(spectrum.getReferenceSeconds());
        frame.floats(spectrum.getMagnitudes());
        frame.floats(spectrum.getSideMagnitudes());
        frame.floats(spectrum.getChannelMaxMagnitudes());
    }

private:
    void applySmoothing() {
        constexpr float measurementSmoothing = 0.97f;
        spectrum.setSmoothing(measurementActive ? std::max(configuredSmoothing, measurementSmoothing) : configuredSmoothing);
    }

    Visualizer::Spectrum spectrum{2048};
    float configuredSmoothing = 0.9f;
    bool measurementActive = false;
};

class OscilloscopeEngine : public Engine {
public:
    bool takesBlocks() const override {return false;} // its trigger search runs per frame anyway
    void setSampleRate(double sampleRate) override {
        osc.setSampleRate(static_cast<float>(sampleRate));
        osc.setDisplaySamples(normalizedDisplaySamples(sampleRate));
    }

    void configure(const Var &settings) override {
        pitchLock = static_cast<bool>(settings.getProperty("pitchLock", true));
        osc.setPitchLock(pitchLock);
    }

    void process(const float *left, const float *right, int count) override {
        if (count <= 0) {return;}
        if (static_cast<int>(mono.size()) < count) {mono.resize(static_cast<size_t>(count));}
        for (int i = 0; i < count; ++i) {mono[static_cast<size_t>(i)] = 0.5f * (left[i] + right[i]);}
        osc.pushSamples(mono.data(), static_cast<size_t>(count));
        samplesSeen += count;
    }

    // sampleRate, pitch, samples (none while warming up)
    void buildFrame(double sampleRate, Frame &frame) override {
        frame.f32(sampleRate);
        if (pitchLock && samplesSeen < kWarmupSamples) {
            frame.f32(0.0);
            frame.floats(nullptr, 0);
            return;
        }
        const auto result = osc.process();
        const int samplesToShow = result.samplesToShow;
        if (samplesToShow <= 1) {
            frame.f32(0.0);
            frame.floats(nullptr, 0);
            return;
        }
        float triggerIndex = result.triggerIndex;
        if (!pitchLock) {
            const size_t writePos = osc.getWritePos();
            triggerIndex = static_cast<float>((writePos + Visualizer::OSCILLOSCOPE_BUFFER_SIZE - static_cast<size_t>(samplesToShow))
                                              % Visualizer::OSCILLOSCOPE_BUFFER_SIZE);
        }
        if (static_cast<int>(window.size()) != samplesToShow) {window.resize(static_cast<size_t>(samplesToShow));}
        osc.getSamplesInterpolated(window.data(), triggerIndex, static_cast<size_t>(samplesToShow));
        frame.f32(result.detectedPitch);
        frame.floats(window);
    }

private:
    static int normalizedDisplaySamples(double sampleRate) {
        const double base = 2048.0, rateMin = 44100.0, rateMax = 48000.0;
        double samples = base;
        if (sampleRate > 0.0) {
            if (sampleRate < rateMin) {samples = std::round(base * (sampleRate / rateMin));}
            else if (sampleRate > rateMax) {samples = std::round(base * (sampleRate / rateMax));}
        }
        return static_cast<int>(std::clamp(samples, 64.0, 32767.0));
    }

    static constexpr long long kWarmupSamples = 4096;
    Visualizer::Oscilloscope osc;
    std::vector<float> mono, window;
    bool pitchLock = true;
    long long samplesSeen = 0;
};

class VUMeterEngine : public Engine {
public:
    void setSampleRate(double sampleRate) override {vu.setSampleRate(static_cast<float>(sampleRate));}
    void configure(const Var &) override {}

    void process(const float *left, const float *right, int count) override {
        if (count > 0) {vu.pushSamples(left, right, static_cast<size_t>(count));}
    }

    // sampleRate, vuL, vuR, barL, barR, peakL, peakR, correlation
    void buildFrame(double sampleRate, Frame &frame) override {
        const auto snap = vu.getSnapshot();
        frame.f32(sampleRate);
        frame.f32(snap.vuLDb);
        frame.f32(snap.vuRDb);
        frame.f32(snap.barLDb);
        frame.f32(snap.barRDb);
        frame.f32(snap.peakLDb);
        frame.f32(snap.peakRDb);
        frame.f32(snap.correlation);
    }

private:
    Visualizer::VUMeterAnalyzer vu;
};

class LUFSMeterEngine : public Engine {
public:
    void setSampleRate(double sampleRate) override {lufs.setSampleRate(static_cast<float>(sampleRate));}
    void configure(const Var &) override {}

    void process(const float *left, const float *right, int count) override {
        if (count > 0) {lufs.pushSamples(left, right, static_cast<size_t>(count));}
    }

    // sampleRate, momentary, shortTerm, integrated, vuL, vuR, barL, barR, truePeakL, truePeakR, maxTruePeak, correlation
    void buildFrame(double sampleRate, Frame &frame) override {
        const auto snap = lufs.getSnapshot();
        frame.f32(sampleRate);
        frame.f32(snap.momentaryLUFS);
        frame.f32(snap.shortTermLUFS);
        frame.f32(snap.integratedLUFS);
        frame.f32(snap.vuLDb);
        frame.f32(snap.vuRDb);
        frame.f32(snap.barLDb);
        frame.f32(snap.barRDb);
        frame.f32(snap.truePeakLDb);
        frame.f32(snap.truePeakRDb);
        frame.f32(snap.maxTruePeakDb);
        frame.f32(snap.correlation);
    }

private:
    Visualizer::LUFSMeterAnalyzer lufs;
};

class VectorscopeEngine : public Engine {
public:
    void setSampleRate(double sampleRate) override {vec.setSampleRate(static_cast<float>(sampleRate));}
    void configure(const Var &settings) override {multiband = static_cast<bool>(settings.getProperty("multiband", false));}

    void process(const float *left, const float *right, int count) override {
        if (count <= 0) {return;}
        vec.pushSamples(left, right, static_cast<size_t>(count));
        vec.pushMultibandSamples(left, right, static_cast<size_t>(count));
    }

    // sampleRate, multiband, count, then data (multiband) or x and y
    void buildFrame(double sampleRate, Frame &frame) override {
        frame.f32(sampleRate);
        frame.u32(multiband ? 1 : 0);
        if (multiband) {
            constexpr size_t stride = Visualizer::MULTIBAND_POINT_STRIDE;
            if (mbData.size() < kDisplayPoints * stride) {mbData.resize(kDisplayPoints * stride);}
            const size_t count = vec.getMultibandPoints(mbData.data(), kDisplayPoints);
            frame.u32(static_cast<uint32_t>(count));
            frame.floats(mbData.data(), count * stride);
        } else {
            if (pointX.size() < kDisplayPoints) {
                pointX.resize(kDisplayPoints);
                pointY.resize(kDisplayPoints);
            }
            const size_t count = vec.getPoints(pointX.data(), pointY.data(), kDisplayPoints);
            frame.u32(static_cast<uint32_t>(count));
            frame.floats(pointX.data(), count);
            frame.floats(pointY.data(), count);
        }
    }

private:
    static constexpr size_t kDisplayPoints = 4096; // matches Vectorscope.ts default
    Visualizer::Vectorscope vec;
    std::vector<float> pointX, pointY, mbData;
    bool multiband = false;
};

class SpectrogramEngine : public Engine {
public:
    void setSampleRate(double sampleRate) override {
        if (sampleRate > 0.0 && static_cast<float>(sampleRate) != config.sampleRate) {
            config.sampleRate = static_cast<float>(sampleRate);
            hostRate = true;
            if (hasConfig) {spectro.configure(config);}
        }
    }

    void configure(const Var &) override {}

    void configureNative(const Var &opts) override {
        if (!opts.isObject()) {return;}
        config.fftSize = static_cast<size_t>(std::max(0, static_cast<int>(opts.getProperty("fftSize", 4096))));
        config.rowCount = static_cast<size_t>(std::max(0, static_cast<int>(opts.getProperty("rowCount", 0))));
        config.minFrequency = static_cast<float>(opts.getProperty("minFrequency", 20.0));
        config.maxFrequency = static_cast<float>(opts.getProperty("maxFrequency", 20000.0));
        config.minDecibels = static_cast<float>(opts.getProperty("minDecibels", -90.0));
        config.maxDecibels = static_cast<float>(opts.getProperty("maxDecibels", -12.0));
        config.scrollSpeed = static_cast<float>(opts.getProperty("scrollSpeed", 2.0));
        config.contrast = static_cast<float>(opts.getProperty("contrast", 1.0));
        config.tiltDbPerOctave = static_cast<float>(opts.getProperty("tiltDbPerOctave", 4.0));
        config.clarityMode = opts.getProperty("clarityMode", "sharper").toString();
        config.scaleMode = opts.getProperty("scaleMode", "log").toString();
        config.orientation = opts.getProperty("orientation", "horizontal").toString();
        if (!hostRate) {config.sampleRate = static_cast<float>(opts.getProperty("sampleRate", 48000.0));}
        hasConfig = config.rowCount > 0 && config.fftSize > 0;
        if (hasConfig) {spectro.configure(config);}
        pendingDisplay.clear();
        pendingHeat.clear();
        pendingColumns = 0;
    }

    void process(const float *left, const float *right, int count) override {
        if (!hasConfig || count <= 0) {return;}
        auto result = spectro.processStereo(left, right, static_cast<size_t>(count));
        if (result.columnCount > 0 && result.rowCount == config.rowCount) {
            pendingDisplay.insert(pendingDisplay.end(), result.display.begin(), result.display.end());
            pendingHeat.insert(pendingHeat.end(), result.heat.begin(), result.heat.end());
            pendingColumns += result.columnCount;
        }
    }

    // sampleRate, rowCount, columnCount, display, heat
    void buildFrame(double sampleRate, Frame &frame) override {
        frame.f32(sampleRate);
        frame.u32(static_cast<uint32_t>(config.rowCount));
        frame.u32(static_cast<uint32_t>(pendingColumns));
        frame.floats(pendingDisplay);
        frame.floats(pendingHeat);
        pendingDisplay.clear();
        pendingHeat.clear();
        pendingColumns = 0;
    }

private:
    Visualizer::SpectrogramAnalyzer spectro;
    Visualizer::SpectrogramConfig config;
    bool hasConfig = false, hostRate = false;
    std::vector<float> pendingDisplay, pendingHeat;
    size_t pendingColumns = 0;
};

class WaveformEngine : public Engine {
public:
    WaveformEngine() {reconfigure();}

    void setSampleRate(double rate) override {
        if (rate > 0.0 && static_cast<float>(rate) != sampleRate) {
            sampleRate = static_cast<float>(rate);
            reconfigure();
        }
    }

    void configure(const Var &settings) override {
        stereo = settings.getProperty("mode", "mono").toString() == "stereo";
        const float speed = static_cast<float>(settings.getProperty("scrollSpeed", static_cast<double>(scrollSpeed)));
        if (speed > 0.0f) {scrollSpeed = speed;}
        reconfigure();
    }

    void process(const float *left, const float *right, int count) override {
        if (count <= 0) {return;}
        if (stereo) {
            const auto &cols = wave.processStereo(left, right, static_cast<size_t>(count));
            pending.insert(pending.end(), cols.begin(), cols.end());
        } else {
            if (static_cast<int>(mono.size()) < count) {mono.resize(static_cast<size_t>(count));}
            for (int i = 0; i < count; ++i) {mono[static_cast<size_t>(i)] = 0.5f * (left[i] + right[i]);}
            const auto &cols = wave.processMono(mono.data(), static_cast<size_t>(count));
            pending.insert(pending.end(), cols.begin(), cols.end());
        }
    }

    // sampleRate, stereo, columnCount, summaries
    void buildFrame(double rate, Frame &frame) override {
        const size_t stride = stereo ? Visualizer::WAVEFORM_STEREO_SUMMARY_STRIDE : Visualizer::WAVEFORM_MONO_SUMMARY_STRIDE;
        frame.f32(rate);
        frame.u32(stereo ? 1 : 0);
        frame.u32(static_cast<uint32_t>(pending.size() / stride));
        frame.floats(pending);
        pending.clear();
    }

private:
    void reconfigure() {
        const float pps = 128.0f * std::max(0.01f, scrollSpeed);
        samplesPerColumn = static_cast<size_t>(std::max(1L, std::lround(sampleRate / pps)));
        wave.configure(sampleRate, samplesPerColumn);
        pending.clear();
    }

    Visualizer::WaveformMultibandAnalyzer wave;
    std::vector<float> mono, pending;
    float sampleRate = 48000.0f;
    float scrollSpeed = 1.0f;
    size_t samplesPerColumn = 1;
    bool stereo = false;
};

class WaterfallEngine : public Engine {
public:
    void setSampleRate(double rate) override {
        if (std::isfinite(rate) && rate > 0 && static_cast<float>(rate) != config.sampleRate) {
            config.sampleRate = static_cast<float>(rate);
            analyzer.configure(config);
        }
    }

    void configure(const Var &settings) override {
        if (!settings.isObject()) {return;}
        applyAnalysisSettings(settings);
        const bool audible = settings.getProperty("frequencyRangeMode", "extended").toString() == "audible";
        config.minFrequency = audible ? 20.0f : 10.0f;
        config.maxFrequency = audible ? 20000.0f : 24000.0f;
        analyzer.configure(config);
    }

    void configureNative(const Var &payload) override {
        if (!payload.isObject()) {return;}
        revision = static_cast<int>(payload.getProperty("revision", revision));
        if (static_cast<bool>(payload.getProperty("reset", false))) {analyzer.reset();}
        const Var options = payload.getProperty("config", Var());
        if (options.isObject()) {
            applyAnalysisSettings(options);
            config.minFrequency = number(options, "minFrequency", config.minFrequency);
            config.maxFrequency = number(options, "maxFrequency", config.maxFrequency);
            analyzer.configure(config);
        }
        ridges = static_cast<size_t>(std::clamp(static_cast<int>(payload.getProperty("ridges", static_cast<int>(ridges))), 2, 64));
        columns = static_cast<size_t>(std::clamp(static_cast<int>(payload.getProperty("columns", static_cast<int>(columns))), 2, 512));
    }

    void resetAudioHistory() override {analyzer.reset();}

    void process(const float *left, const float *right, int count) override {
        if (count > 0) {analyzer.processStereo(left, right, static_cast<size_t>(count));}
    }

    // sampleRate, revision, columns, audioSeconds, levels, ages, frequencies
    void buildFrame(double, Frame &frame) override {
        const auto result = analyzer.getFrame(ridges, columns);
        frame.f32(config.sampleRate);
        frame.u32(static_cast<uint32_t>(revision));
        frame.u32(static_cast<uint32_t>(result.columns));
        frame.f32(result.audioSeconds);
        frame.floats(result.levels);
        frame.floats(result.ages);
        frame.floats(result.frequencies);
    }

private:
    static float number(const Var &object, const char *key, float fallback) {
        const Var value = object.getProperty(key, fallback);
        if (!value.isNumber()) {return fallback;}
        const float result = static_cast<float>(value);
        return std::isfinite(result) ? result : fallback;
    }

    void applyAnalysisSettings(const Var &settings) {
        config.fftSize = static_cast<size_t>(std::clamp(number(settings, "fftSize", static_cast<float>(config.fftSize)), 0.0f, 16384.0f));
        config.historySeconds = number(settings, "historySeconds", config.historySeconds);
        config.smoothing = number(settings, "smoothing", config.smoothing);
        config.tiltDbPerOctave = number(settings, "tiltDbPerOctave", config.tiltDbPerOctave);
        config.scaleMode = settings.getProperty("scaleMode", config.scaleMode).toString();
    }

    Visualizer::WaterfallAnalyzer analyzer;
    Visualizer::WaterfallConfig config;
    size_t ridges = 32, columns = 256;
    int revision = 0;
};

inline Engine *createEngine(Scope scope) {
    switch (scope) {
        case SCOPE_SPECTRUM: return new SpectrumEngine();
        case SCOPE_OSCILLOSCOPE: return new OscilloscopeEngine();
        case SCOPE_VECTORSCOPE: return new VectorscopeEngine();
        case SCOPE_SPECTROGRAM: return new SpectrogramEngine();
        case SCOPE_VUMETER: return new VUMeterEngine();
        case SCOPE_LUFSMETER: return new LUFSMeterEngine();
        case SCOPE_WAVEFORM: return new WaveformEngine();
        default: return new WaterfallEngine();
    }
}

} // namespace prism
