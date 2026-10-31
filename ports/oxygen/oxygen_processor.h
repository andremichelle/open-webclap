/*
    oXygen's mastering chain as one processor for WebCLAP, in both modules (in ui.wasm as the editor's stand-in).

    Upstream runs its modules in a juce::AudioProcessorGraph and keeps each module's parameters in the module, so a
    host sees none of them. This processor owns the same modules, unchanged, in upstream's order (Graphic EQ,
    Dynamic EQ, Multiband Comp, Stereo Imager, Gain, Maximizer) and mirrors every module parameter as a host
    parameter ("<module>.<upstream id>", same range and default), copied into the module before each block. The
    state is these parameters. Left out: the Master Assistant and Reference Match (a worker thread, a minute of
    captured audio, reference files), the analyzer and reordering the modules.
*/

#pragma once

#include <JuceHeader.h>

#include "Modules/DynamicEqModule.h"
#include "Modules/EqualizerModule.h"
#include "Modules/GainModule.h"
#include "Modules/MaximizerModule.h"
#include "Modules/MultibandCompressorModule.h"
#include "Modules/StereoImagerModule.h"

namespace oxygen_webclap
{
/** What the meters show: the most since the page last asked. Cuts and reductions in dB, 0 or more; levels as peak
    gain. */
struct Meters
{
    float dynamicEq[4] {};
    float multiband[4] {};
    float maximizer = 0.0f;
    float input = 0.0f;
    float output = 0.0f;
};

class Processor final : public juce::AudioProcessor
{
    struct Chain
    {
        oxygen::EqualizerModule eq;
        oxygen::DynamicEqModule dynamicEq;
        oxygen::MultibandCompressorModule multiband;
        oxygen::StereoImagerModule imager;
        oxygen::GainModule trim;
        oxygen::MaximizerModule maximizer;

        std::array<juce::AudioProcessor*, 6> modules() { return { &eq, &dynamicEq, &multiband, &imager, &trim, &maximizer }; }
    };

    std::unique_ptr<Chain> chain; // before state: its layout is built from the modules' parameters

public:
    Processor();

    // The host parameters' id prefixes, one per module
    static constexpr const char* eq = "eq";
    static constexpr const char* dynamicEq = "deq";
    static constexpr const char* multiband = "mbc";
    static constexpr const char* imager = "img";
    static constexpr const char* trim = "trim";
    static constexpr const char* maximizer = "max";

    static juce::String id (const char* module, const juce::String& upstreamId) { return juce::String (module) + "." + upstreamId; }

    /** The meters since the last call, then zero. */
    Meters takeMeters();

    /** The input's and the output's spectrum (mono sums) over the last 2048 samples, in spectrumBands bands spaced
        logarithmically from 20 Hz to 20 kHz: per band the strongest bin, 0 for -90 dBFS or less to 255 for 0 dBFS.
        Inputs first, then outputs. */
    static constexpr int spectrumBands = 64;
    void computeSpectrum (std::uint8_t* destination);

    juce::AudioProcessorValueTreeState state;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override { return "oXygen"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout (Chain&);
    bool bypassed (const char* module, const char* bypassId) const;

    std::vector<std::pair<juce::AudioProcessorParameter*, juce::AudioProcessorParameter*>> links; // host, module
    Meters meters;

    static constexpr int fftOrder = 11, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    std::array<std::vector<float>, 2> history; // the input and the output, a ring of fftSize samples each
    std::vector<float> window, transform;
    int historyPosition = 0;
    double rate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Processor)
};
} // namespace oxygen_webclap
