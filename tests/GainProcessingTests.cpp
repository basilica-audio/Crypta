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

    // Since issue #42 wired the low-band parallel compressor and high-band
    // voicing permanently into the signal path, the plugin is no longer
    // level-transparent at its *default* parameters (lowCompMix defaults to
    // 100% wet with a -18dB threshold, and highBlend defaults to 100% wet
    // Gnaw hard-clip distortion) - that's by design, not a regression. These
    // pure gain-staging tests are about input/output trim math, not
    // compressor/voicing character (which get their own dedicated tests in
    // ParallelCompressorTests.cpp/VoicingTests.cpp), so they pull both
    // stages' blend controls to 0% (fully dry) to isolate the gain-staging
    // path being tested.
    void neutralizeDynamicsAndVoicing (TwistYourGutsAudioProcessor& processor)
    {
        auto* lowCompMixParam = processor.apvts.getParameter (ParamIDs::lowCompMix);
        auto* highBlendParam = processor.apvts.getParameter (ParamIDs::highBlend);
        REQUIRE (lowCompMixParam != nullptr);
        REQUIRE (highBlendParam != nullptr);

        lowCompMixParam->setValueNotifyingHost (lowCompMixParam->convertTo0to1 (0.0f));
        highBlendParam->setValueNotifyingHost (highBlendParam->convertTo0to1 (0.0f));
    }
}

TEST_CASE ("Gain math: +6dB input gain doubles the RMS level", "[gain][dsp]")
{
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, testBlockSize);

    neutralizeDynamicsAndVoicing (processor);

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

TEST_CASE ("Passthrough level test: default parameters leave the signal level unchanged", "[gain][dsp]")
{
    // Since #8/#10 wired the LR4 crossover permanently into the signal path
    // (input trim -> split -> per-band level -> sum -> optional clip ->
    // output trim), even at all-default/unity parameters the signal is no
    // longer a bit-exact identity mapping: the low/high band sum is
    // magnitude-flat (that is the whole point of using LR4) but carries the
    // allpass-like phase response of the crossover, so individual samples of
    // a steady-state tone are shifted relative to the input. This test
    // therefore checks level transparency (RMS ratio ~= 0 dB) rather than
    // sample-for-sample equality; the flat-sum property itself is asserted
    // rigorously, across many probe frequencies, in CrossoverTests.cpp.
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, testBlockSize);
    neutralizeDynamicsAndVoicing (processor);

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
    CHECK (ratioInDecibels == Catch::Approx (0.0).margin (0.1));
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
