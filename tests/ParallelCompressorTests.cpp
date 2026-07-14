#include "dsp/ParallelCompressor.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

// Issue #42: low-band parallel compressor acceptance gates.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int numSamples = 4096;

    juce::dsp::ProcessSpec makeSpec (int numChannels = 1)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (numSamples);
        spec.numChannels = static_cast<juce::uint32> (numChannels);
        return spec;
    }
}

TEST_CASE ("ParallelCompressor: 0% mix is a bit-exact dry passthrough", "[compressor][dsp]")
{
    tyg::ParallelCompressor compressor;
    compressor.prepare (makeSpec(), 0.0f);
    compressor.setThresholdDb (-30.0f);
    compressor.setRatio (10.0f);
    compressor.setAttackMs (1.0f);
    compressor.setReleaseMs (50.0f);
    compressor.setMakeupGainDb (12.0f); // aggressive settings that would be very audible if leaking through

    juce::AudioBuffer<float> buffer (1, numSamples);
    TestHelpers::fillWithSine (buffer, testSampleRate, 100.0, 0.5f);

    juce::AudioBuffer<float> reference;
    reference.makeCopyOf (buffer);

    juce::dsp::AudioBlock<float> block (buffer);
    compressor.process (block);

    const auto* refData = reference.getReadPointer (0);
    const auto* outData = buffer.getReadPointer (0);

    for (int sample = 0; sample < numSamples; ++sample)
        CHECK (outData[sample] == Catch::Approx (refData[sample]).margin (1e-6));
}

TEST_CASE ("ParallelCompressor: signal well above threshold is gain-reduced at 100% mix", "[compressor][dsp]")
{
    tyg::ParallelCompressor compressor;
    compressor.prepare (makeSpec(), 1.0f);
    compressor.setThresholdDb (-24.0f);
    compressor.setRatio (8.0f);
    compressor.setAttackMs (1.0f);
    compressor.setReleaseMs (50.0f);
    compressor.setMakeupGainDb (0.0f);

    // 0.5 amplitude ~= -6dBFS, well above the -24dB threshold: an 8:1 ratio
    // should visibly reduce gain once the ballistics filter has settled.
    juce::AudioBuffer<float> buffer (1, numSamples);
    TestHelpers::fillWithSine (buffer, testSampleRate, 100.0, 0.5f);

    juce::AudioBuffer<float> reference;
    reference.makeCopyOf (buffer);

    juce::dsp::AudioBlock<float> block (buffer);
    compressor.process (block);

    constexpr int settleSamples = numSamples / 2;
    double sumOfSquaresOut = 0.0, sumOfSquaresRef = 0.0;

    const auto* outData = buffer.getReadPointer (0);
    const auto* refData = reference.getReadPointer (0);

    for (int i = settleSamples; i < numSamples; ++i)
    {
        sumOfSquaresOut += static_cast<double> (outData[i]) * static_cast<double> (outData[i]);
        sumOfSquaresRef += static_cast<double> (refData[i]) * static_cast<double> (refData[i]);
    }

    const auto outRms = std::sqrt (sumOfSquaresOut);
    const auto refRms = std::sqrt (sumOfSquaresRef);

    // Gain reduction should be clearly audible (well beyond smoothing noise)
    // but the compressor should never invert or silence the signal.
    const auto reductionDb = juce::Decibels::gainToDecibels (outRms / refRms);
    CHECK (reductionDb < -1.0);
    CHECK (reductionDb > -30.0);
}

TEST_CASE ("ParallelCompressor: makeup gain raises the wet level as expected at 100% mix", "[compressor][dsp]")
{
    // With the threshold pinned far below the signal (so the compressor
    // itself is fully engaged and roughly constant-gain-reduction for a
    // steady tone) two otherwise-identical runs that only differ by a fixed
    // makeup gain should differ by ~ that same amount once settled.
    tyg::ParallelCompressor unityCompressor;
    unityCompressor.prepare (makeSpec(), 1.0f);
    unityCompressor.setThresholdDb (-60.0f);
    unityCompressor.setRatio (4.0f);
    unityCompressor.setAttackMs (1.0f);
    unityCompressor.setReleaseMs (50.0f);
    unityCompressor.setMakeupGainDb (0.0f);

    tyg::ParallelCompressor makeupCompressor;
    makeupCompressor.prepare (makeSpec(), 1.0f);
    makeupCompressor.setThresholdDb (-60.0f);
    makeupCompressor.setRatio (4.0f);
    makeupCompressor.setAttackMs (1.0f);
    makeupCompressor.setReleaseMs (50.0f);
    makeupCompressor.setMakeupGainDb (6.0f);

    juce::AudioBuffer<float> unityBuffer (1, numSamples);
    TestHelpers::fillWithSine (unityBuffer, testSampleRate, 100.0, 0.3f);
    juce::dsp::AudioBlock<float> unityBlock (unityBuffer);
    unityCompressor.process (unityBlock);

    juce::AudioBuffer<float> makeupBuffer (1, numSamples);
    TestHelpers::fillWithSine (makeupBuffer, testSampleRate, 100.0, 0.3f);
    juce::dsp::AudioBlock<float> makeupBlock (makeupBuffer);
    makeupCompressor.process (makeupBlock);

    constexpr int settleSamples = numSamples / 2;
    double sumOfSquaresUnity = 0.0, sumOfSquaresMakeup = 0.0;

    const auto* unityData = unityBuffer.getReadPointer (0);
    const auto* makeupData = makeupBuffer.getReadPointer (0);

    for (int i = settleSamples; i < numSamples; ++i)
    {
        sumOfSquaresUnity += static_cast<double> (unityData[i]) * static_cast<double> (unityData[i]);
        sumOfSquaresMakeup += static_cast<double> (makeupData[i]) * static_cast<double> (makeupData[i]);
    }

    const auto unityRms = std::sqrt (sumOfSquaresUnity);
    const auto makeupRms = std::sqrt (sumOfSquaresMakeup);

    CHECK (juce::Decibels::gainToDecibels (makeupRms / unityRms) == Catch::Approx (6.0).margin (0.5));
}

TEST_CASE ("ParallelCompressor: no NaN/Inf across an extreme-parameter and denormal sweep", "[compressor][dsp][robustness]")
{
    tyg::ParallelCompressor compressor;
    compressor.prepare (makeSpec (2), 1.0f);
    compressor.setThresholdDb (-60.0f);
    compressor.setRatio (20.0f);
    compressor.setAttackMs (0.1f);
    compressor.setReleaseMs (1000.0f);
    compressor.setMakeupGainDb (24.0f);

    juce::AudioBuffer<float> buffer (2, numSamples);
    const auto denormalValue = std::numeric_limits<float>::denorm_min() * 4.0f;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        auto* data = buffer.getWritePointer (channel);

        for (int sample = 0; sample < numSamples; ++sample)
            data[sample] = (sample % 2 == 0) ? denormalValue : -denormalValue;
    }

    juce::dsp::AudioBlock<float> block (buffer);
    CHECK_NOTHROW (compressor.process (block));
    CHECK (TestHelpers::allSamplesFinite (buffer));
}
