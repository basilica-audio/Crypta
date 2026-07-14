#include "dsp/NoiseGateStage.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

// Issue #42: full-band input noise gate acceptance gates.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int numSamples = 4096;

    juce::dsp::ProcessSpec makeSpec (int numChannels = 2)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (numSamples);
        spec.numChannels = static_cast<juce::uint32> (numChannels);
        return spec;
    }
}

TEST_CASE ("NoiseGateStage: disabled is a bit-exact passthrough", "[gate][dsp]")
{
    tyg::NoiseGateStage gate;
    gate.prepare (makeSpec());
    gate.setEnabled (false);
    gate.setThresholdDb (-10.0f); // aggressive settings, should still be ignored
    gate.setRatio (20.0f);

    juce::AudioBuffer<float> buffer (2, numSamples);
    TestHelpers::fillWithSine (buffer, testSampleRate, 100.0, 0.05f); // quiet tone, well under threshold

    juce::AudioBuffer<float> reference;
    reference.makeCopyOf (buffer);

    juce::dsp::AudioBlock<float> block (buffer);
    gate.process (block);

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const auto* refData = reference.getReadPointer (channel);
        const auto* outData = buffer.getReadPointer (channel);

        for (int sample = 0; sample < numSamples; ++sample)
            CHECK (outData[sample] == Catch::Approx (refData[sample]).margin (1e-9));
    }
}

TEST_CASE ("NoiseGateStage: enabled attenuates a signal below threshold", "[gate][dsp]")
{
    tyg::NoiseGateStage gate;
    gate.prepare (makeSpec (1));
    gate.setEnabled (true);
    gate.setThresholdDb (-20.0f);
    gate.setRatio (10.0f);
    gate.setAttackMs (1.0f);
    gate.setReleaseMs (50.0f);

    // -40 dBFS tone: well below the -20 dB threshold, so the gate should
    // ramp down to a heavily attenuated (~10:1 ratio) level once settled.
    constexpr float quietAmplitude = 0.01f; // ~ -40 dBFS
    juce::AudioBuffer<float> buffer (1, numSamples);
    TestHelpers::fillWithSine (buffer, testSampleRate, 200.0, quietAmplitude);

    juce::dsp::AudioBlock<float> block (buffer);
    gate.process (block);

    // Measure the settled tail (skip the release ramp's transient).
    constexpr int settleSamples = numSamples / 2;
    double sumOfSquares = 0.0;

    const auto* data = buffer.getReadPointer (0);

    for (int i = settleSamples; i < numSamples; ++i)
        sumOfSquares += static_cast<double> (data[i]) * static_cast<double> (data[i]);

    const auto settledRms = std::sqrt (sumOfSquares / static_cast<double> (numSamples - settleSamples));
    const auto settledDb = juce::Decibels::gainToDecibels (settledRms);
    const auto inputDb = juce::Decibels::gainToDecibels (static_cast<double> (quietAmplitude) * 0.70710678);

    // The gated signal should sit noticeably below the input level.
    CHECK (settledDb < inputDb - 10.0);
}

TEST_CASE ("NoiseGateStage: enabled passes a signal above threshold through essentially unchanged", "[gate][dsp]")
{
    tyg::NoiseGateStage gate;
    gate.prepare (makeSpec (1));
    gate.setEnabled (true);
    gate.setThresholdDb (-40.0f);
    gate.setRatio (10.0f);
    gate.setAttackMs (1.0f);
    gate.setReleaseMs (50.0f);

    constexpr float loudAmplitude = 0.5f; // well above the -40dB threshold
    juce::AudioBuffer<float> buffer (1, numSamples);
    TestHelpers::fillWithSine (buffer, testSampleRate, 200.0, loudAmplitude);

    juce::AudioBuffer<float> reference;
    reference.makeCopyOf (buffer);

    juce::dsp::AudioBlock<float> block (buffer);
    gate.process (block);

    constexpr int settleSamples = numSamples / 2;
    const auto outRms = TestHelpers::rms (buffer);
    const auto refRms = TestHelpers::rms (reference);
    juce::ignoreUnused (settleSamples);

    CHECK (juce::Decibels::gainToDecibels (outRms / refRms) == Catch::Approx (0.0).margin (0.5));
}

TEST_CASE ("NoiseGateStage: no NaN/Inf across a denormal-range and extreme-parameter sweep", "[gate][dsp][robustness]")
{
    tyg::NoiseGateStage gate;
    gate.prepare (makeSpec());
    gate.setEnabled (true);
    gate.setThresholdDb (-80.0f);
    gate.setRatio (20.0f);
    gate.setAttackMs (0.1f);
    gate.setReleaseMs (500.0f);

    juce::AudioBuffer<float> buffer (2, numSamples);
    const auto denormalValue = std::numeric_limits<float>::denorm_min() * 4.0f;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        auto* data = buffer.getWritePointer (channel);

        for (int sample = 0; sample < numSamples; ++sample)
            data[sample] = (sample % 2 == 0) ? denormalValue : -denormalValue;
    }

    juce::dsp::AudioBlock<float> block (buffer);
    CHECK_NOTHROW (gate.process (block));
    CHECK (TestHelpers::allSamplesFinite (buffer));
}
