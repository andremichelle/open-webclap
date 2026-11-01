#include "radar_processor.h"

#include "radar_frames.h"

namespace radar
{
namespace
{
constexpr std::uint32_t stateMagic = 0x31524452; // "RDR1"
constexpr int matchLength = 128, matchStride = 4; // the alignment compares this many samples, every fourth

template <typename T>
T readLE (const std::uint8_t*& p)
{
    T value;
    std::memcpy (&value, p, sizeof (T));
    p += sizeof (T);
    return value;
}
} // namespace

Processor::Processor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, "Radar", createLayout())
{
    patternParameter = state.getRawParameterValue ("pattern");
    barsParameter = state.getRawParameterValue ("bars");
    grainParameter = state.getRawParameterValue ("grain");
    pitchParameter = state.getRawParameterValue ("pitch");
    keysParameter = state.getRawParameterValue ("keys");
    playParameter = state.getRawParameterValue ("play");
    attackParameter = state.getRawParameterValue ("attack");
    releaseParameter = state.getRawParameterValue ("release");
    volumeParameter = state.getRawParameterValue ("volume");
}

juce::AudioProcessorValueTreeState::ParameterLayout Processor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    const auto ms = [] (float v, int) { return (v < 10.0f ? String (v, 1) : String (roundToInt (v))) + " ms"; }; // String (v, 0) keeps decimals

    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "pattern", 1 }, "Pattern", 1, numPatterns, 1));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "bars", 1 }, "Bars", 1, 16, 2));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "grain", 1 }, "Grain", NormalisableRange<float> (5.0f, 250.0f, 0.0f, 0.4f), 30.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (ms)));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "pitch", 1 }, "Pitch", NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 2) + " st"; })));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "keys", 1 }, "Keys", StringArray { "Pattern", "Pitch" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "play", 1 }, "Play", StringArray { "Note", "Free" }, 1));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "attack", 1 }, "Attack", NormalisableRange<float> (0.0f, 1000.0f, 0.0f, 0.3f), 2.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (ms)));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "release", 1 }, "Release", NormalisableRange<float> (1.0f, 2000.0f, 0.0f, 0.3f), 40.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (ms)));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "volume", 1 }, "Volume", NormalisableRange<float> (-48.0f, 6.0f, 0.0f, 2.0f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));
    return layout;
}

void Processor::prepareToPlay (double sampleRate_, int)
{
    rate = sampleRate_;
    restart = true;
    tailLength = juce::jmax (1, juce::roundToInt (0.005 * rate)); // the fast release of a retriggered loop
    tail = {};
}

void Processor::reset()
{
    held.clear();
    envelope = 0.0f;
    tail = {};
    retrigger = false;
    restart = true;
    freePhase = 0.0;
}

bool Processor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

bool Processor::loadSample (const std::uint8_t* payload, size_t size)
{
    if (size < 12)
        return false;

    auto p = payload;
    const auto end = payload + size;
    const auto newRate = readLE<std::uint32_t> (p);
    const auto newFrames = readLE<std::uint32_t> (p);
    const auto channels = readLE<std::uint16_t> (p);
    const auto nameBytes = readLE<std::uint16_t> (p);

    if ((size_t) (end - p) < nameBytes)
        return false;

    const auto name = juce::String::fromUTF8 ((const char*) p, nameBytes);
    p += nameBytes;

    if (newFrames == 0)
    {
        sampleName = {};
        left.clear();
        right.clear();
        frames = 0;
        ++sampleRevision;
        return true;
    }

    if (channels < 1 || channels > 2 || newRate < 8000 || newRate > 384000
        || newFrames > (std::uint32_t) (maxSampleSeconds * newRate) + 1
        || (size_t) (end - p) < (size_t) newFrames * channels * 2)
        return false;

    const auto convert = [&] (std::vector<float>& channel)
    {
        channel.resize (newFrames);

        for (auto& v : channel)
            v = (float) readLE<std::int16_t> (p) / 32768.0f;
    };

    convert (left);

    if (channels == 2)
        convert (right);
    else
        right.clear();

    sampleName = name;
    sampleRate = newRate;
    frames = (int) newFrames;
    sampleEmbedded = true; // kept in the state until the host has the sample (a slot delivered it)
    restart = true;
    ++sampleRevision;
    return true;
}

float* Processor::prepareSample (std::uint32_t newFrames, std::uint32_t channels, double rate)
{
    if (rate < 8000.0 || rate > 384000.0 || newFrames > (std::uint32_t) (maxHostSampleSeconds * rate))
        return nullptr;

    staging.assign ((size_t) newFrames * channels, 0.0f);
    stagingFrames = (int) newFrames;
    stagingChannels = (int) channels;
    stagingRate = rate;
    return staging.data();
}

bool Processor::commitSample()
{
    if (staging.empty())
        return false;

    const auto stateChanges = sampleEmbedded; // the state embedded the sample, it holds only the name now

    const auto count = (size_t) stagingFrames;
    left.assign (staging.begin(), staging.begin() + (std::ptrdiff_t) count);

    if (stagingChannels > 1)
        right.assign (staging.begin() + (std::ptrdiff_t) count, staging.begin() + (std::ptrdiff_t) (2 * count));
    else
        right.clear();

    sampleRate = stagingRate;
    frames = stagingFrames;
    sampleEmbedded = false;
    staging = {};
    restart = true;
    ++sampleRevision;
    return stateChanges;
}

void Processor::clearHostSample()
{
    left.clear();
    right.clear();
    frames = 0;
    sampleEmbedded = false;
    ++sampleRevision;
}

juce::MemoryBlock Processor::describeSample() const
{
    juce::MemoryOutputStream out;
    const auto name = sampleName.toUTF8();
    const auto nameBytes = (int) juce::jmin ((size_t) 1024, std::strlen (name));
    out.writeInt (frames);
    out.writeInt (juce::roundToInt (sampleRate));
    out.writeShort ((short) (right.empty() ? 1 : 2));
    out.writeShort ((short) nameBytes);
    out.write (name, (size_t) nameBytes);
    out.writeShort ((short) (frames > 0 ? Frame::waveformBins : 0));

    for (int bin = 0; frames > 0 && bin < Frame::waveformBins; ++bin)
    {
        const auto from = (int) ((juce::int64) frames * bin / Frame::waveformBins);
        const auto to = juce::jmax (from + 1, (int) ((juce::int64) frames * (bin + 1) / Frame::waveformBins));
        auto low = 0.0f, high = 0.0f;

        for (int i = from; i < to; ++i)
        {
            for (const auto* channel : { &left, &right })
                if (! channel->empty())
                {
                    low = juce::jmin (low, (*channel)[(size_t) i]);
                    high = juce::jmax (high, (*channel)[(size_t) i]);
                }
        }

        out.writeByte ((char) juce::jlimit (-127, 127, juce::roundToInt (low * 127.0f)));
        out.writeByte ((char) juce::jlimit (-127, 127, juce::roundToInt (high * 127.0f)));
    }

    out.writeByte (hostKeepsSamples ? 1 : 0); // flags: 1 the page hands files to the host
    return out.getMemoryBlock();
}

void Processor::noteOn (int noteKey, float noteVelocity)
{
    if (held.empty() && envelope <= 0.0f)
    {
        restart = true;
        freePhase = 0.0; // a loop that starts from a note starts at its beginning
    }

    held.erase (std::remove (held.begin(), held.end(), noteKey), held.end());
    held.push_back (noteKey);
    key = noteKey;
    velocity = noteVelocity;
    retrigger = true;
}

void Processor::noteOff (int noteKey)
{
    held.erase (std::remove (held.begin(), held.end(), noteKey), held.end());

    if (! held.empty())
        key = held.back();
}

float Processor::read (const std::vector<float>& channel, double position) const
{
    // Four point Hermite, the sample looping
    const auto index = (int) std::floor (position);
    const auto t = (float) (position - index);
    const auto at = [&] (int i) { return channel[(size_t) (((i % frames) + frames) % frames)]; };
    const auto y0 = at (index - 1), y1 = at (index), y2 = at (index + 1), y3 = at (index + 2);
    const auto c1 = 0.5f * (y2 - y0);
    const auto c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const auto c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

/** Where the silent head goes: where the ray points, moved (by up to a few milliseconds) to where the sample
    continues most like the audible head does, so the crossfade does not comb. */
double Processor::target (float phase, double audible, double step)
{
    const auto& pattern = patterns[(size_t) playback.pattern];
    const auto traced = trace (pattern, phase);
    playback.escaped = traced.escaped ? 1 : 0;

    if (traced.escaped)
        lastExit = traced.exit;

    const auto position = (double) lastExit * frames;
    const auto range = juce::jmin (256, frames / 8);
    auto best = 0;
    auto bestError = std::numeric_limits<float>::max();

    for (int offset = -range; offset <= range; offset += 2)
    {
        auto error = 0.0f;

        for (int i = 0; i < matchLength && error < bestError; i += matchStride)
        {
            const auto d = read (left, audible + i * step) - read (left, position + offset + i * step);
            error += d * d;
        }

        if (error < bestError)
        {
            bestError = error;
            best = offset;
        }
    }

    return std::fmod (position + best + frames, (double) frames);
}

void Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    const auto samples = buffer.getNumSamples();
    const auto bars = juce::jmax (1, juce::roundToInt (barsParameter->load()));
    const auto keysPickPatterns = keysParameter->load() < 0.5f;
    const auto grainLength = juce::jmax (32, juce::roundToInt (grainParameter->load() * 0.001 * rate));
    const auto free = playParameter->load() > 0.5f;

    // Free fades in and out with the transport, a few milliseconds against clicks: Attack and Release are the notes'
    const auto fadeMs = [&] (std::atomic<float>* parameter) { return free ? 5.0f : parameter->load(); };
    const auto attackStep = 1.0f / juce::jmax (1.0f, fadeMs (attackParameter) * 0.001f * (float) rate);
    const auto releaseStep = 1.0f / juce::jmax (1.0f, fadeMs (releaseParameter) * 0.001f * (float) rate);
    const auto gain = juce::Decibels::decibelsToGain (volumeParameter->load(), -48.0f);

    // One turn: Bars bars of the host's metre, at its tempo
    double bpm = 120.0, ppq = 0.0, quartersPerBar = 4.0;
    auto locked = false;

    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
        {
            bpm = position->getBpm().orFallback (120.0);

            if (const auto signature = position->getTimeSignature())
                quartersPerBar = signature->numerator * 4.0 / juce::jmax (1, signature->denominator);

            if (position->getIsPlaying())
                if (const auto at = position->getPpqPosition())
                {
                    ppq = *at;
                    locked = true;
                }
        }

    const auto quartersPerTurn = bars * quartersPerBar;
    const auto turnsPerSample = bpm / 60.0 / rate / quartersPerTurn;

    auto* outL = buffer.getWritePointer (0);
    auto* outR = buffer.getWritePointer (buffer.getNumChannels() > 1 ? 1 : 0);
    auto events = midi.cbegin();

    for (int i = 0; i < samples; ++i)
    {
        for (; events != midi.cend() && (*events).samplePosition <= i; ++events)
        {
            const auto message = (*events).getMessage();

            if (message.isNoteOn())
                noteOn (message.getNoteNumber(), message.getFloatVelocity());
            else if (message.isNoteOff())
                noteOff (message.getNoteNumber());
            else if (message.isAllNotesOff() || message.isAllSoundOff())
                held.clear();
        }

        // Note: every note starts the loop again, the one playing fades out fast beside it
        if (std::exchange (retrigger, false) && ! free && envelope > 0.0f && frames > 0)
        {
            tail = { headA, headB, lastGainA, lastGainB, lastLevel, lastStep, tailLength, tailLength };
            envelope = 0.0f;
            restart = true;
            freePhase = 0.0;
        }

        if (tail.remaining > 0 && frames > 0)
        {
            const auto fadeOut = (float) tail.remaining-- / (float) tail.length;
            const auto& r = right.empty() ? left : right;
            outL[i] += tail.level * fadeOut * (read (left, tail.headA) * tail.gainA + read (left, tail.headB) * tail.gainB);
            outR[i] += tail.level * fadeOut * (read (r, tail.headA) * tail.gainA + read (r, tail.headB) * tail.gainB);
            tail.headA = std::fmod (tail.headA + tail.step, (double) frames);
            tail.headB = std::fmod (tail.headB + tail.step, (double) frames);
        }

        // Free plays while the transport runs, the notes only pick the pattern or the pitch; Note plays while one is held
        const auto noted = ! held.empty();
        const auto gate = free ? locked : noted;
        envelope = gate ? juce::jmin (1.0f, envelope + attackStep) : juce::jmax (0.0f, envelope - releaseStep);
        // Free keeps the pattern a note picked after it ends, until the Pattern parameter moves; Note only while held
        const auto picked = juce::jlimit (0, numPatterns - 1, juce::roundToInt (patternParameter->load()) - 1);

        if (picked != lastPicked)
        {
            lastPicked = picked;
            latched = -1;
        }

        if (keysPickPatterns && noted)
            latched = key % numPatterns;

        playback.pattern = keysPickPatterns && noted ? key % numPatterns
                                                     : free && keysPickPatterns && latched >= 0 ? latched : picked;

        const auto phase = locked ? std::fmod (ppq / quartersPerTurn + i * turnsPerSample, 1.0) : freePhase;

        if (gate || envelope > 0.0f)
            freePhase = std::fmod (freePhase + turnsPerSample, 1.0);
        playback.phase = (float) phase;

        if (frames == 0 || envelope <= 0.0f)
        {
            if (envelope <= 0.0f)
                restart = true;

            continue;
        }

        const auto semitones = pitchParameter->load() + (keysPickPatterns || ! noted ? 0.0f : (float) (key - 60));
        const auto step = sampleRate / rate * std::pow (2.0, semitones / 12.0);

        if (restart)
        {
            restart = false;
            const auto traced = trace (patterns[(size_t) playback.pattern], (float) phase);
            lastExit = traced.escaped ? traced.exit : (float) phase;
            headA = headB = (double) lastExit * frames;
            fade = 0;
            towardsB = true;
            headB = target ((float) phase, headA, step);
        }

        // The heads take turns: the one fully faded out jumps
        if (towardsB)
        {
            if (++fade >= grainLength)
            {
                fade = grainLength;
                towardsB = false;
                headA = target ((float) phase, headB, step);
            }
        }
        else if (--fade <= 0)
        {
            fade = 0;
            towardsB = true;
            headB = target ((float) phase, headA, step);
        }

        const auto mix = (float) fade / (float) grainLength;
        const auto gainA = std::cos (mix * juce::MathConstants<float>::halfPi);
        const auto gainB = std::sin (mix * juce::MathConstants<float>::halfPi);
        const auto level = gain * (free ? 1.0f : velocity) * envelope;
        const auto& r = right.empty() ? left : right;
        outL[i] += level * (read (left, headA) * gainA + read (left, headB) * gainB);
        outR[i] += level * (read (r, headA) * gainA + read (r, headB) * gainB);

        lastGainA = gainA;
        lastGainB = gainB;
        lastLevel = level;
        lastStep = step;
        headA = std::fmod (headA + step, (double) frames);
        headB = std::fmod (headB + step, (double) frames);
        playback.mix = mix;
        playback.grain = (float) (grainLength * step / frames);
    }

    playback.playing = envelope > 0.0f && frames > 0 ? 1 : 0;
    playback.headA = frames > 0 ? (float) (headA / frames) : 0.0f;
    playback.headB = frames > 0 ? (float) (headB / frames) : 0.0f;
}

void Processor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream out (destData, false);
    out.writeInt ((int) stateMagic);

    juce::MemoryBlock parameters;

    if (const auto xml = state.copyState().createXml())
        copyXmlToBinary (*xml, parameters);

    out.writeInt ((int) parameters.getSize());
    out.write (parameters.getData(), parameters.getSize());
    out.writeByte ((char) numPatterns);

    for (const auto& pattern : patterns)
        pattern.write (out);

    // The sample last: a state without it (the page's) keeps the one there is. 0 none, 1 the sample (16 bit), 2 its
    // name only: the host keeps the audio (opendaw.samples/1) and hands it over after a load.
    if (hostKeepsSamples && ! sampleEmbedded)
    {
        out.writeByte (2);
        out.writeString (sampleName);
        return;
    }

    out.writeByte (frames > 0 ? 1 : 0);

    if (frames > 0)
    {
        out.writeString (sampleName);
        out.writeInt (juce::roundToInt (sampleRate));
        out.writeByte ((char) (right.empty() ? 1 : 2));
        out.writeInt (frames);

        for (const auto* channel : { &left, &right })
            for (const auto v : *channel)
                out.writeShort ((short) juce::jlimit (-32768, 32767, juce::roundToInt (v * 32768.0f)));
    }
}

void Processor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::MemoryInputStream in (data, (size_t) sizeInBytes, false);

    if (sizeInBytes < 8 || (std::uint32_t) in.readInt() != stateMagic)
        return;

    const auto parametersSize = in.readInt();

    if (parametersSize < 0 || parametersSize > in.getNumBytesRemaining())
        return;

    juce::MemoryBlock parameters;
    in.readIntoMemoryBlock (parameters, parametersSize);

    if (const auto xml = getXmlFromBinary (parameters.getData(), (int) parameters.getSize());
        xml != nullptr && xml->hasTagName (state.state.getType()))
        state.replaceState (juce::ValueTree::fromXml (*xml));

    const auto count = (int) (std::uint8_t) in.readByte();

    for (int i = 0; i < count && i < numPatterns; ++i)
        if (! patterns[(size_t) i].read (in))
            patterns[(size_t) i] = {};

    ++stateRevision;

    if (in.isExhausted())
        return;

    const auto kind = in.readByte();

    if (kind == 2) // the host keeps the audio: it arrives through the slot (setSample)
    {
        sampleName = in.readString();
        sampleEmbedded = false;
        ++sampleRevision;
        return;
    }

    if (kind == 0)
    {
        const std::uint8_t none[12] {};
        loadSample (none, sizeof (none));
        return;
    }

    // As a sample frame, so that both ways check the same
    const auto name = in.readString().toUTF8();
    const auto sampleRate_ = (std::uint32_t) in.readInt();
    const auto channels = (std::uint16_t) in.readByte();
    const auto sampleFrames = (std::uint32_t) in.readInt();
    const auto nameBytes = (std::uint16_t) juce::jmin ((size_t) 1024, std::strlen (name));
    const auto pcmBytes = (size_t) sampleFrames * channels * 2;

    if (channels < 1 || channels > 2 || (juce::int64) pcmBytes > in.getNumBytesRemaining())
        return;

    juce::MemoryOutputStream frame;
    frame.writeInt ((int) sampleRate_);
    frame.writeInt ((int) sampleFrames);
    frame.writeShort ((short) channels);
    frame.writeShort ((short) nameBytes);
    frame.write (name, nameBytes);
    frame.writeFromInputStream (in, (juce::int64) pcmBytes);
    loadSample ((const std::uint8_t*) frame.getData(), frame.getDataSize());
}
} // namespace radar

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new radar::Processor();
}
