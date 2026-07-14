#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// Issue #10: gain-staging acceptance gates for the low/high band level
// trims and the optional safety clip. lowLevel/highLevel are applied after
// the LR4 split (issue #8) and before the bands are summed back together,
// so a probe tone safely inside one band should be attenuated by that
// band's level control and left alone by the other band's.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int testBlockSize = 512;

    // Well below/above the 250 Hz crossoverFreq default, with margin so
    // filter roll-off near the crossover point doesn't leak the "wrong"
    // band's attenuation into the measurement.
    constexpr double lowProbeFrequencyHz = 80.0;
    constexpr double highProbeFrequencyHz = 4000.0;

    // Runs 12 blocks of a steady, phase-continuous sine at `probeFrequencyHz`
    // through the processor (already prepared and parameterised by the
    // caller) and returns the RMS level, in dB relative to full scale, of
    // the last block once smoothing/filter transients have settled. Phase
    // must stay continuous across the block boundaries: the LR4 crossover
    // is a stateful IIR filter, so restarting the sine at phase zero every
    // block would inject a small broadband transient at each boundary and
    // pollute the level measurement.
    // Since issue #42 wired the low-band parallel compressor and high-band
    // voicing permanently into the signal path, both bands are no longer
    // level-transparent at their *default* parameters (lowCompMix defaults
    // to 100% wet with a -18dB threshold that a 0.5-amplitude probe tone
    // sits well above; highBlend defaults to 100% wet Gnaw hard-clip
    // distortion) - that's by design, not a regression. These tests are
    // about the lowLevel/highLevel *trim* controls, not compressor/voicing
    // character (which get their own dedicated tests), so both stages'
    // blend controls are pulled to 0% (fully dry) up front to isolate the
    // level-trim behaviour being tested.
    void neutralizeDynamicsAndVoicing (TwistYourGutsAudioProcessor& processor)
    {
        auto* lowCompMixParam = processor.apvts.getParameter (ParamIDs::lowCompMix);
        auto* highBlendParam = processor.apvts.getParameter (ParamIDs::highBlend);
        REQUIRE (lowCompMixParam != nullptr);
        REQUIRE (highBlendParam != nullptr);

        lowCompMixParam->setValueNotifyingHost (lowCompMixParam->convertTo0to1 (0.0f));
        highBlendParam->setValueNotifyingHost (highBlendParam->convertTo0to1 (0.0f));
    }

    double measureSettledLevelDb (TwistYourGutsAudioProcessor& processor, double probeFrequencyHz)
    {
        juce::AudioBuffer<float> buffer (2, testBlockSize);
        juce::MidiBuffer midi;

        for (int i = 0; i < 12; ++i)
        {
            TestHelpers::fillWithSine (buffer, testSampleRate, probeFrequencyHz, 0.5f,
                                        static_cast<juce::int64> (i) * testBlockSize);
            processor.processBlock (buffer, midi);
        }

        return juce::Decibels::gainToDecibels (TestHelpers::rms (buffer));
    }
}

TEST_CASE ("Gain staging: lowLevel attenuates the low band only", "[gain-staging][dsp]")
{
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, testBlockSize);
    neutralizeDynamicsAndVoicing (processor);

    auto* lowLevelParam = processor.apvts.getParameter (ParamIDs::lowLevel);
    REQUIRE (lowLevelParam != nullptr);

    const auto referenceLowLevelDb = measureSettledLevelDb (processor, lowProbeFrequencyHz);

    TwistYourGutsAudioProcessor attenuatedProcessor;
    attenuatedProcessor.prepareToPlay (testSampleRate, testBlockSize);
    neutralizeDynamicsAndVoicing (attenuatedProcessor);
    auto* attenuatedLowLevelParam = attenuatedProcessor.apvts.getParameter (ParamIDs::lowLevel);
    REQUIRE (attenuatedLowLevelParam != nullptr);
    attenuatedLowLevelParam->setValueNotifyingHost (attenuatedLowLevelParam->convertTo0to1 (-12.0f));

    const auto attenuatedLowLevelDb = measureSettledLevelDb (attenuatedProcessor, lowProbeFrequencyHz);
    const auto attenuatedHighLevelDb = measureSettledLevelDb (attenuatedProcessor, highProbeFrequencyHz);

    TwistYourGutsAudioProcessor referenceProcessor;
    referenceProcessor.prepareToPlay (testSampleRate, testBlockSize);
    neutralizeDynamicsAndVoicing (referenceProcessor);
    const auto referenceHighLevelDb = measureSettledLevelDb (referenceProcessor, highProbeFrequencyHz);

    // A low-frequency probe should drop by ~12 dB when lowLevel is -12 dB...
    CHECK ((attenuatedLowLevelDb - referenceLowLevelDb) == Catch::Approx (-12.0).margin (0.5));

    // ...but a high-frequency probe should be unaffected by lowLevel.
    CHECK ((attenuatedHighLevelDb - referenceHighLevelDb) == Catch::Approx (0.0).margin (0.5));
}

TEST_CASE ("Gain staging: highLevel attenuates the high band only", "[gain-staging][dsp]")
{
    TwistYourGutsAudioProcessor referenceProcessor;
    referenceProcessor.prepareToPlay (testSampleRate, testBlockSize);
    neutralizeDynamicsAndVoicing (referenceProcessor);
    const auto referenceLowLevelDb = measureSettledLevelDb (referenceProcessor, lowProbeFrequencyHz);

    TwistYourGutsAudioProcessor referenceProcessor2;
    referenceProcessor2.prepareToPlay (testSampleRate, testBlockSize);
    neutralizeDynamicsAndVoicing (referenceProcessor2);
    const auto referenceHighLevelDb = measureSettledLevelDb (referenceProcessor2, highProbeFrequencyHz);

    TwistYourGutsAudioProcessor attenuatedProcessor;
    attenuatedProcessor.prepareToPlay (testSampleRate, testBlockSize);
    neutralizeDynamicsAndVoicing (attenuatedProcessor);
    auto* attenuatedHighLevelParam = attenuatedProcessor.apvts.getParameter (ParamIDs::highLevel);
    REQUIRE (attenuatedHighLevelParam != nullptr);
    attenuatedHighLevelParam->setValueNotifyingHost (attenuatedHighLevelParam->convertTo0to1 (-12.0f));

    const auto attenuatedHighLevelDb = measureSettledLevelDb (attenuatedProcessor, highProbeFrequencyHz);
    const auto attenuatedLowLevelDb = measureSettledLevelDb (attenuatedProcessor, lowProbeFrequencyHz);

    // A high-frequency probe should drop by ~12 dB when highLevel is -12 dB...
    CHECK ((attenuatedHighLevelDb - referenceHighLevelDb) == Catch::Approx (-12.0).margin (0.5));

    // ...but a low-frequency probe should be unaffected by highLevel.
    CHECK ((attenuatedLowLevelDb - referenceLowLevelDb) == Catch::Approx (0.0).margin (0.5));
}

TEST_CASE ("Gain staging: outputClip only engages when explicitly enabled, and clamps peaks", "[gain-staging][dsp]")
{
    // Push well past 0 dBFS with input gain so the safety clip actually has
    // something to do.
    constexpr float loudInputGainDb = 24.0f;

    TwistYourGutsAudioProcessor unclippedProcessor;
    unclippedProcessor.prepareToPlay (testSampleRate, testBlockSize);
    auto* unclippedInputGainParam = unclippedProcessor.apvts.getParameter (ParamIDs::inputGain);
    REQUIRE (unclippedInputGainParam != nullptr);
    unclippedInputGainParam->setValueNotifyingHost (unclippedInputGainParam->convertTo0to1 (loudInputGainDb));

    juce::AudioBuffer<float> unclippedBuffer (2, testBlockSize);
    juce::MidiBuffer midi;

    for (int i = 0; i < 12; ++i)
    {
        TestHelpers::fillWithSine (unclippedBuffer, testSampleRate, 1000.0);
        unclippedProcessor.processBlock (unclippedBuffer, midi);
    }

    // Confirms the loud input gain actually produces an over, so the clip
    // assertion below is testing something real.
    REQUIRE (unclippedBuffer.getMagnitude (0, unclippedBuffer.getNumSamples()) > 1.0f);

    TwistYourGutsAudioProcessor clippedProcessor;
    clippedProcessor.prepareToPlay (testSampleRate, testBlockSize);
    auto* clippedInputGainParam = clippedProcessor.apvts.getParameter (ParamIDs::inputGain);
    auto* outputClipParam = clippedProcessor.apvts.getParameter (ParamIDs::outputClip);
    REQUIRE (clippedInputGainParam != nullptr);
    REQUIRE (outputClipParam != nullptr);
    clippedInputGainParam->setValueNotifyingHost (clippedInputGainParam->convertTo0to1 (loudInputGainDb));
    outputClipParam->setValueNotifyingHost (1.0f);

    juce::AudioBuffer<float> clippedBuffer (2, testBlockSize);

    for (int i = 0; i < 12; ++i)
    {
        TestHelpers::fillWithSine (clippedBuffer, testSampleRate, 1000.0);
        clippedProcessor.processBlock (clippedBuffer, midi);
    }

    CHECK (clippedBuffer.getMagnitude (0, clippedBuffer.getNumSamples()) <= 1.0f + 1e-3f);
    CHECK (TestHelpers::allSamplesFinite (clippedBuffer));
}

TEST_CASE ("Gain staging: 0 dB band level defaults are transparent", "[gain-staging][dsp]")
{
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, testBlockSize);
    neutralizeDynamicsAndVoicing (processor);

    // Defaults: lowLevel/highLevel/inputGain/outputGain all 0 dB,
    // outputClip off. A full-band-ish probe (sum of a low and a high tone)
    // should come out within a tight margin of its input level.
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    juce::MidiBuffer midi;

    for (int i = 0; i < 12; ++i)
    {
        TestHelpers::fillWithSine (buffer, testSampleRate, lowProbeFrequencyHz, 0.25f);

        juce::AudioBuffer<float> highComponent (2, testBlockSize);
        TestHelpers::fillWithSine (highComponent, testSampleRate, highProbeFrequencyHz, 0.25f);

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            buffer.addFrom (channel, 0, highComponent, channel, 0, testBlockSize);

        processor.processBlock (buffer, midi);
    }

    juce::AudioBuffer<float> reference (2, testBlockSize);
    TestHelpers::fillWithSine (reference, testSampleRate, lowProbeFrequencyHz, 0.25f);
    juce::AudioBuffer<float> highReference (2, testBlockSize);
    TestHelpers::fillWithSine (highReference, testSampleRate, highProbeFrequencyHz, 0.25f);

    for (int channel = 0; channel < reference.getNumChannels(); ++channel)
        reference.addFrom (channel, 0, highReference, channel, 0, testBlockSize);

    const auto inputRms = TestHelpers::rms (reference);
    const auto outputRms = TestHelpers::rms (buffer);

    REQUIRE (inputRms > 0.0);
    CHECK (juce::Decibels::gainToDecibels (outputRms / inputRms) == Catch::Approx (0.0).margin (0.1));
}
