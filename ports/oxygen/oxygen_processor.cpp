#include "oxygen_processor.h"

namespace oxygen_webclap
{
namespace
{
struct Module
{
    juce::AudioProcessor* processor;
    const char* prefix;
    const char* label; // the host parameters' names begin with it
};

std::array<Module, 6> modulesOf (std::array<juce::AudioProcessor*, 6> processors)
{
    return { Module { processors[0], Processor::eq, "EQ" },
             Module { processors[1], Processor::dynamicEq, "Dyn EQ" },
             Module { processors[2], Processor::multiband, "Multiband" },
             Module { processors[3], Processor::imager, "Imager" },
             Module { processors[4], Processor::trim, "Trim" },
             Module { processors[5], Processor::maximizer, "Maximizer" } };
}

/** A host parameter like the module's: same type, range, default and choices. */
std::unique_ptr<juce::RangedAudioParameter> mirror (juce::AudioProcessorParameter& parameter, const Module& module)
{
    auto& ranged = dynamic_cast<juce::RangedAudioParameter&> (parameter);
    const juce::ParameterID id { Processor::id (module.prefix, ranged.getParameterID()), 1 };
    const auto name = juce::String (module.label) + " " + parameter.getName (64);
    const auto normalisedDefault = parameter.getDefaultValue(); // private in the subclasses

    if (auto* toggle = dynamic_cast<juce::AudioParameterBool*> (&parameter))
        return std::make_unique<juce::AudioParameterBool> (id, name, normalisedDefault > 0.5f);

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&parameter))
        return std::make_unique<juce::AudioParameterChoice> (
            id, name, choice->choices, juce::roundToInt (choice->convertFrom0to1 (normalisedDefault)));

    auto& value = dynamic_cast<juce::AudioParameterFloat&> (parameter);
    return std::make_unique<juce::AudioParameterFloat> (id, name, value.range,
                                                        value.convertFrom0to1 (normalisedDefault));
}

float peak (const juce::AudioBuffer<float>& buffer)
{
    auto level = 0.0f;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        level = juce::jmax (level, buffer.getMagnitude (channel, 0, buffer.getNumSamples()));

    return level;
}
} // namespace

Processor::Processor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      chain (std::make_unique<Chain>()),
      state (*this, nullptr, "oXygen", createLayout (*chain))
{
    for (const auto& module : modulesOf (chain->modules()))
        for (auto* parameter : module.processor->getParameters())
            links.emplace_back (state.getParameter (id (module.prefix,
                                                       dynamic_cast<juce::RangedAudioParameter&> (*parameter)
                                                           .getParameterID())),
                                parameter);
}

juce::AudioProcessorValueTreeState::ParameterLayout Processor::createLayout (Chain& chain)
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (const auto& module : modulesOf (chain.modules()))
        for (auto* parameter : module.processor->getParameters())
            layout.add (mirror (*parameter, module));

    return layout;
}

void Processor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    for (auto* module : chain->modules())
    {
        module->setRateAndBufferSizeDetails (sampleRate, samplesPerBlock);
        module->prepareToPlay (sampleRate, samplesPerBlock);
    }

    setLatencySamples (chain->maximizer.getLatencySamples());
    meters = {};
}

void Processor::releaseResources()
{
    for (auto* module : chain->modules())
        module->releaseResources();
}

bool Processor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

bool Processor::bypassed (const char* module, const char* bypassId) const
{
    return state.getRawParameterValue (id (module, bypassId))->load() > 0.5f;
}

void Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // Notifying: the modules' APVTS hear of a change only through the listeners (their raw values are the APVTS's)
    for (auto& [host, module] : links)
        if (const auto value = host->getValue(); value != module->getValue())
            module->setValueNotifyingHost (value);

    meters.input = juce::jmax (meters.input, peak (buffer));

    for (auto* module : chain->modules())
        module->processBlock (buffer, midi);

    meters.output = juce::jmax (meters.output, peak (buffer));

    // A bypassed module keeps its last reduction: the meters show none
    const auto dynamicEqOn = ! bypassed (dynamicEq, "Bypass");
    const auto multibandOn = ! bypassed (multiband, "Bypass");

    for (int band = 0; band < 4; ++band)
    {
        if (dynamicEqOn)
            meters.dynamicEq[band] = juce::jmax (meters.dynamicEq[band], chain->dynamicEq.getGainReductionDb (band));

        if (multibandOn)
            meters.multiband[band] = juce::jmax (meters.multiband[band], -chain->multiband.getGainReductionDb (band));
    }

    if (! bypassed (maximizer, "Bypass"))
        meters.maximizer = juce::jmax (meters.maximizer, -chain->maximizer.getGainReductionDb());
}

Meters Processor::takeMeters()
{
    const auto taken = meters;
    meters = {};
    return taken;
}

void Processor::getStateInformation (juce::MemoryBlock& destData)
{
    if (const auto xml = state.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void Processor::setStateInformation (const void* data, int sizeInBytes)
{
    if (const auto xml = getXmlFromBinary (data, sizeInBytes); xml != nullptr && xml->hasTagName (state.state.getType()))
        state.replaceState (juce::ValueTree::fromXml (*xml));
}
} // namespace oxygen_webclap

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new oxygen_webclap::Processor();
}
