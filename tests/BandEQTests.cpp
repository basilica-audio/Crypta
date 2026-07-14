#include "dsp/BandEQ.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

// Issue #42: post-sum 4-band EQ acceptance gates.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int numSamples = 8192;
    constexpr int settleSamples = 2048;

    juce::dsp::ProcessSpec makeSpec (int numChannels = 1)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (numSamples);
        spec.numChannels = static_cast<juce::uint32> (numChannels);
        return spec;
    }

    double measureLevelRatioDb (tyg::BandEQ& eq, double probeFrequencyHz)
    {
        juce::AudioBuffer<float> buffer (1, numSamples);
        TestHelpers::fillWithSine (buffer, testSampleRate, probeFrequencyHz, 0.5f);

        juce::AudioBuffer<float> reference;
        reference.makeCopyOf (buffer);

        juce::dsp::AudioBlock<float> block (buffer);
        eq.process (block);

        double sumOfSquaresIn = 0.0, sumOfSquaresOut = 0.0;
        const auto* refData = reference.getReadPointer (0);
        const auto* outData = buffer.getReadPointer (0);

        for (int i = settleSamples; i < numSamples; ++i)
        {
            sumOfSquaresIn += static_cast<double> (refData[i]) * static_cast<double> (refData[i]);
            sumOfSquaresOut += static_cast<double> (outData[i]) * static_cast<double> (outData[i]);
        }

        const auto inRms = std::sqrt (sumOfSquaresIn);
        const auto outRms = std::sqrt (sumOfSquaresOut);

        return juce::Decibels::gainToDecibels (outRms / inRms);
    }
}

TEST_CASE ("BandEQ: all bands at 0dB gain is transparent across the band", "[eq][dsp]")
{
    tyg::BandEQ eq;
    eq.prepare (makeSpec());
    eq.setLowShelf (100.0f, 0.0f);
    eq.setPeak1 (500.0f, 0.0f, 0.7f);
    eq.setPeak2 (2500.0f, 0.0f, 0.7f);
    eq.setHighShelf (8000.0f, 0.0f);

    const double probeFrequenciesHz[] = { 40.0, 100.0, 500.0, 1000.0, 2500.0, 8000.0, 15000.0 };

    for (const auto probeFrequencyHz : probeFrequenciesHz)
    {
        INFO ("probe frequency = " << probeFrequencyHz << " Hz");
        CHECK (measureLevelRatioDb (eq, probeFrequencyHz) == Catch::Approx (0.0).margin (0.2));
    }
}

TEST_CASE ("BandEQ: low shelf boost raises level at low frequencies but not at high ones", "[eq][dsp]")
{
    tyg::BandEQ eq;
    eq.prepare (makeSpec());
    eq.setLowShelf (200.0f, 12.0f);
    eq.setPeak1 (500.0f, 0.0f, 0.7f);
    eq.setPeak2 (2500.0f, 0.0f, 0.7f);
    eq.setHighShelf (8000.0f, 0.0f);

    CHECK (measureLevelRatioDb (eq, 60.0) == Catch::Approx (12.0).margin (1.0));
    CHECK (measureLevelRatioDb (eq, 12000.0) == Catch::Approx (0.0).margin (0.5));
}

TEST_CASE ("BandEQ: high shelf cut lowers level at high frequencies but not at low ones", "[eq][dsp]")
{
    tyg::BandEQ eq;
    eq.prepare (makeSpec());
    eq.setLowShelf (100.0f, 0.0f);
    eq.setPeak1 (500.0f, 0.0f, 0.7f);
    eq.setPeak2 (2500.0f, 0.0f, 0.7f);
    eq.setHighShelf (6000.0f, -12.0f);

    CHECK (measureLevelRatioDb (eq, 15000.0) == Catch::Approx (-12.0).margin (1.0));
    CHECK (measureLevelRatioDb (eq, 60.0) == Catch::Approx (0.0).margin (0.5));
}

TEST_CASE ("BandEQ: a peak boost raises level at its centre frequency", "[eq][dsp]")
{
    tyg::BandEQ eq;
    eq.prepare (makeSpec());
    eq.setLowShelf (100.0f, 0.0f);
    eq.setPeak1 (1000.0f, 9.0f, 1.0f);
    eq.setPeak2 (2500.0f, 0.0f, 0.7f);
    eq.setHighShelf (8000.0f, 0.0f);

    CHECK (measureLevelRatioDb (eq, 1000.0) == Catch::Approx (9.0).margin (1.0));
    CHECK (measureLevelRatioDb (eq, 60.0) == Catch::Approx (0.0).margin (0.5));
}

TEST_CASE ("BandEQ: coefficient updates across a wide parameter sweep never produce NaN/Inf", "[eq][dsp][robustness]")
{
    tyg::BandEQ eq;
    eq.prepare (makeSpec (2));

    juce::AudioBuffer<float> buffer (2, 512);
    juce::Random random (1234);

    for (int iteration = 0; iteration < 40; ++iteration)
    {
        const auto lowFreq = random.nextFloat() * 360.0f + 40.0f;
        const auto lowGain = random.nextFloat() * 36.0f - 18.0f;
        const auto peak1Freq = random.nextFloat() * 1900.0f + 100.0f;
        const auto peak1Gain = random.nextFloat() * 36.0f - 18.0f;
        const auto peak1Q = random.nextFloat() * 4.8f + 0.2f;
        const auto peak2Freq = random.nextFloat() * 7500.0f + 500.0f;
        const auto peak2Gain = random.nextFloat() * 36.0f - 18.0f;
        const auto peak2Q = random.nextFloat() * 4.8f + 0.2f;
        const auto highFreq = random.nextFloat() * 14000.0f + 2000.0f;
        const auto highGain = random.nextFloat() * 36.0f - 18.0f;

        eq.setLowShelf (lowFreq, lowGain);
        eq.setPeak1 (peak1Freq, peak1Gain, peak1Q);
        eq.setPeak2 (peak2Freq, peak2Gain, peak2Q);
        eq.setHighShelf (highFreq, highGain);

        TestHelpers::fillWithSine (buffer, testSampleRate, 300.0 + iteration * 17.0, 0.5f);
        juce::dsp::AudioBlock<float> block (buffer);
        CHECK_NOTHROW (eq.process (block));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}
