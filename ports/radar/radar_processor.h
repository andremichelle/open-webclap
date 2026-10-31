/*
    Radar as one processor for WebCLAP, in both modules (in ui.wasm as the editor's stand-in, never processing).

    An instrument that plays one sample as a loop, the position in the sample found by a ray for every moment of the
    loop (radar_model.h). Play Free plays it while the host's transport runs (notes only pick the pattern or the
    pitch, Attack and Release do not apply), Play Note while a note is held; one
    turn of the ray lasts Bars bars, locked to the host's position while the transport runs, from the note on
    otherwise. Two read heads take turns: every grain the
    silent one jumps to where the ray points (aligned to the playing one by the best match nearby) and fades in
    while the other fades out, so the loop keeps the host's tempo at the sample's pitch.

    Twelve patterns; the Pattern parameter picks one, or the notes do (Keys: C plays pattern 1, C# pattern 2 and so
    on). With Keys on Pitch, notes transpose from C3 instead.

    The state is the parameters, the patterns and the sample (16 bit, at most maxSampleSeconds).
*/

#pragma once

#include <JuceHeader.h>

#include "radar_model.h"

namespace radar
{
/** What the editors show of the playing loop. Positions in turns of the sample. */
struct Playback
{
    float phase = 0.0f;   // the ray's angle
    float headA = 0.0f;   // the read heads
    float headB = 0.0f;
    float mix = 0.0f;     // 0 hears head A, 1 head B
    float grain = 0.0f;   // a grain's length
    std::int32_t pattern = 0;
    std::int32_t playing = 0;
    std::int32_t escaped = 1; // the ray left the circle (else it is trapped and the heads stay)
};

class Processor final : public juce::AudioProcessor
{
public:
    Processor();

    static constexpr double maxSampleSeconds = 30.0;

    juce::AudioProcessorValueTreeState state;

    Patterns patterns;

    /** A sample from the page (Frame::sample's payload). False if it is malformed. */
    bool loadSample (const std::uint8_t* payload, size_t size);

    /** The sample's overview for the editors (Frame::waveform's payload). */
    juce::MemoryBlock describeSample() const;

    /** Counts what the pages must hear of again: a new sample, a state load (patterns and sample). */
    int sampleRevision = 0, stateRevision = 0;

    Playback playback;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override { return "Radar"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    void noteOn (int key, float velocity);
    void noteOff (int key);
    float read (const std::vector<float>& channel, double position) const;
    double target (float phase, double audible, double step);

    // The sample
    juce::String sampleName;
    double sampleRate = 48000.0;
    std::vector<float> left, right; // right empty for mono
    int frames = 0;

    // Notes
    std::vector<int> held;
    float velocity = 1.0f, envelope = 0.0f;
    int key = 60;

    // Playing
    double rate = 48000.0;
    double freePhase = 0.0;
    double headA = 0.0, headB = 0.0;
    int fade = 0;
    bool towardsB = true, restart = true;
    float lastExit = 0.0f;

    std::atomic<float>* patternParameter = nullptr;
    std::atomic<float>* barsParameter = nullptr;
    std::atomic<float>* grainParameter = nullptr;
    std::atomic<float>* pitchParameter = nullptr;
    std::atomic<float>* keysParameter = nullptr;
    std::atomic<float>* playParameter = nullptr;
    std::atomic<float>* attackParameter = nullptr;
    std::atomic<float>* releaseParameter = nullptr;
    std::atomic<float>* volumeParameter = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Processor)
};
} // namespace radar
