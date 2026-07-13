#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "params/ParameterIds.h"

namespace
{
    // ~20ms smoothing ramp for gain changes: fast enough to feel responsive,
    // slow enough to avoid zipper noise on parameter automation.
    constexpr double gainRampDurationSeconds = 0.02;
}

//==============================================================================
TwistYourGutsAudioProcessor::TwistYourGutsAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    inputGainDb = apvts.getRawParameterValue (ParamIDs::inputGain);
    outputGainDb = apvts.getRawParameterValue (ParamIDs::outputGain);
    bypassFlag = apvts.getRawParameterValue (ParamIDs::bypass);
    bypassParameter = apvts.getParameter (ParamIDs::bypass);

    jassert (inputGainDb != nullptr);
    jassert (outputGainDb != nullptr);
    jassert (bypassFlag != nullptr);
    jassert (bypassParameter != nullptr);
}

TwistYourGutsAudioProcessor::~TwistYourGutsAudioProcessor() = default;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout TwistYourGutsAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::inputGain, 1 },
        "Input Gain",
        juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::outputGain, 1 },
        "Output Gain",
        juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ParamIDs::bypass, 1 },
        "Bypass",
        false));

    return layout;
}

//==============================================================================
const juce::String TwistYourGutsAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool TwistYourGutsAudioProcessor::acceptsMidi() const
{
    return false;
}

bool TwistYourGutsAudioProcessor::producesMidi() const
{
    return false;
}

bool TwistYourGutsAudioProcessor::isMidiEffect() const
{
    return false;
}

double TwistYourGutsAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int TwistYourGutsAudioProcessor::getNumPrograms()
{
    return 1;
}

int TwistYourGutsAudioProcessor::getCurrentProgram()
{
    return 0;
}

void TwistYourGutsAudioProcessor::setCurrentProgram (int)
{
}

const juce::String TwistYourGutsAudioProcessor::getProgramName (int)
{
    return {};
}

void TwistYourGutsAudioProcessor::changeProgramName (int, const juce::String&)
{
}

//==============================================================================
void TwistYourGutsAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32> (getTotalNumOutputChannels());

    inputGainProcessor.setRampDurationSeconds (gainRampDurationSeconds);
    inputGainProcessor.prepare (spec);
    inputGainProcessor.setGainDecibels (inputGainDb->load (std::memory_order_relaxed));

    outputGainProcessor.setRampDurationSeconds (gainRampDurationSeconds);
    outputGainProcessor.prepare (spec);
    outputGainProcessor.setGainDecibels (outputGainDb->load (std::memory_order_relaxed));

    // This M0 skeleton reports zero latency. Milestone M3 introduces
    // band-split oversampling, which will require setLatencySamples() to
    // report the oversampling filter's actual latency.
    setLatencySamples (0);
}

void TwistYourGutsAudioProcessor::releaseResources()
{
}

bool TwistYourGutsAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mono = juce::AudioChannelSet::mono();
    const auto stereo = juce::AudioChannelSet::stereo();

    const auto mainOut = layouts.getMainOutputChannelSet();
    const auto mainIn = layouts.getMainInputChannelSet();

    if (mainOut != mono && mainOut != stereo)
        return false;

    if (mainOut != mainIn)
        return false;

    return true;
}

void TwistYourGutsAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto totalNumInputChannels = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Buses are constrained to in == out (mono or stereo), so this is
    // normally a no-op, but it's cheap insurance against stray channels.
    for (auto channel = totalNumInputChannels; channel < totalNumOutputChannels; ++channel)
        buffer.clear (channel, 0, buffer.getNumSamples());

    if (bypassFlag->load (std::memory_order_relaxed) >= 0.5f)
        return;

    inputGainProcessor.setGainDecibels (inputGainDb->load (std::memory_order_relaxed));
    outputGainProcessor.setGainDecibels (outputGainDb->load (std::memory_order_relaxed));

    juce::dsp::AudioBlock<float> block (buffer);
    juce::dsp::ProcessContextReplacing<float> context (block);

    inputGainProcessor.process (context);
    outputGainProcessor.process (context);
}

//==============================================================================
bool TwistYourGutsAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* TwistYourGutsAudioProcessor::createEditor()
{
    return new TwistYourGutsAudioProcessorEditor (*this);
}

//==============================================================================
juce::AudioProcessorParameter* TwistYourGutsAudioProcessor::getBypassParameter() const
{
    return bypassParameter;
}

//==============================================================================
void TwistYourGutsAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState();
    const std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void TwistYourGutsAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState != nullptr && xmlState->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TwistYourGutsAudioProcessor();
}
