#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int testBlockSize = 512;
    constexpr double testFrequencyHz = 1000.0;

    // Feeds the processor a handful of blocks so the ~20ms gain smoothing
    // ramp has settled to its target value before we measure anything.
    void settleSmoothing (TwistYourGutsAudioProcessor& processor, int numBlocks = 8)
    {
        for (int i = 0; i < numBlocks; ++i)
        {
            juce::AudioBuffer<float> buffer (2, testBlockSize);
            TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz);
            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
        }
    }
}

TEST_CASE ("Gain math: +6dB input gain doubles the RMS level", "[gain][dsp]")
{
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, testBlockSize);

    auto* inputGainParam = processor.apvts.getParameter (ParamIDs::inputGain);
    REQUIRE (inputGainParam != nullptr);
    inputGainParam->setValueNotifyingHost (inputGainParam->convertTo0to1 (6.0f));

    settleSmoothing (processor);

    juce::AudioBuffer<float> reference (2, testBlockSize);
    TestHelpers::fillWithSine (reference, testSampleRate, testFrequencyHz);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::MidiBuffer midi;
    processor.processBlock (processed, midi);

    const auto inputRms = TestHelpers::rms (reference);
    const auto outputRms = TestHelpers::rms (processed);

    REQUIRE (inputRms > 0.0);

    const auto ratioInDecibels = juce::Decibels::gainToDecibels (outputRms / inputRms);

    CHECK (ratioInDecibels == Catch::Approx (6.0).margin (0.1));
}

TEST_CASE ("Passthrough null test: default parameters leave the signal unchanged", "[gain][dsp]")
{
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, testBlockSize);

    settleSmoothing (processor);

    juce::AudioBuffer<float> reference (2, testBlockSize);
    TestHelpers::fillWithSine (reference, testSampleRate, testFrequencyHz);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::MidiBuffer midi;
    processor.processBlock (processed, midi);

    for (int channel = 0; channel < reference.getNumChannels(); ++channel)
    {
        const auto* refData = reference.getReadPointer (channel);
        const auto* outData = processed.getReadPointer (channel);

        for (int sample = 0; sample < reference.getNumSamples(); ++sample)
            CHECK (outData[sample] == Catch::Approx (refData[sample]).margin (1e-6));
    }
}

TEST_CASE ("Bypass parameter forces a bit-exact passthrough", "[gain][bypass]")
{
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, testBlockSize);

    auto* inputGainParam = processor.apvts.getParameter (ParamIDs::inputGain);
    auto* bypassParam = processor.apvts.getParameter (ParamIDs::bypass);
    REQUIRE (inputGainParam != nullptr);
    REQUIRE (bypassParam != nullptr);

    // Even with a non-default gain, bypass should make processBlock a
    // pure passthrough.
    inputGainParam->setValueNotifyingHost (inputGainParam->convertTo0to1 (12.0f));
    bypassParam->setValueNotifyingHost (1.0f);

    settleSmoothing (processor);

    juce::AudioBuffer<float> reference (2, testBlockSize);
    TestHelpers::fillWithSine (reference, testSampleRate, testFrequencyHz);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::MidiBuffer midi;
    processor.processBlock (processed, midi);

    for (int channel = 0; channel < reference.getNumChannels(); ++channel)
    {
        const auto* refData = reference.getReadPointer (channel);
        const auto* outData = processed.getReadPointer (channel);

        for (int sample = 0; sample < reference.getNumSamples(); ++sample)
            CHECK (outData[sample] == Catch::Approx (refData[sample]).margin (1e-6));
    }
}
