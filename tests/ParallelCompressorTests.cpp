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
    cryp::ParallelCompressor compressor;
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
    cryp::ParallelCompressor compressor;
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
    cryp::ParallelCompressor unityCompressor;
    unityCompressor.prepare (makeSpec(), 1.0f);
    unityCompressor.setThresholdDb (-60.0f);
    unityCompressor.setRatio (4.0f);
    unityCompressor.setAttackMs (1.0f);
    unityCompressor.setReleaseMs (50.0f);
    unityCompressor.setMakeupGainDb (0.0f);

    cryp::ParallelCompressor makeupCompressor;
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
    cryp::ParallelCompressor compressor;
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

//==============================================================================
// Issue #12's remaining acceptance criteria, stated as measurements: the gain
// reduction the stage reports has to be the gain reduction it applied (both
// detector engines), and the parallel mix has to be a genuine equal-latency
// blend rather than a crossfade between two paths that have drifted apart.
namespace
{
    // Steady-state gain reduction: the value the stage reports once the
    // ballistics have settled on a continuous tone.
    float settledGainReductionDb (cryp::ParallelCompressor& compressor, double toneHz, float amplitude)
    {
        juce::AudioBuffer<float> buffer (1, numSamples);

        for (int block = 0; block < 8; ++block)
        {
            TestHelpers::fillWithSine (buffer, testSampleRate, toneHz, amplitude,
                                        static_cast<juce::int64> (block) * numSamples);
            juce::dsp::AudioBlock<float> audioBlock (buffer);
            compressor.process (audioBlock);
        }

        return compressor.getGainReductionDb();
    }

    // The static compressor curve, in dB of reduction, for a hard knee.
    double staticCurveReductionDb (double levelDb, double thresholdDb, double ratio)
    {
        const auto overshoot = levelDb - thresholdDb;
        return overshoot > 0.0 ? overshoot * (1.0 - 1.0 / ratio) : 0.0;
    }
}

TEST_CASE ("ParallelCompressor: the Smooth RMS engine reports the gain reduction its static curve prescribes", "[compressor][dsp][metering]")
{
    // The detector is an RMS one, so the level it acts on is the tone's RMS
    // level (3.01 dB below its peak), not its peak - which is exactly why the
    // expected value is computed from the RMS level here. Knee is set to 0 so
    // the comparison is against the plain ratio line rather than the quadratic
    // interpolation of the soft knee.
    constexpr float amplitude = 0.5f;      // -6.02 dBFS peak
    constexpr float thresholdDb = -21.0f;
    constexpr float ratio = 4.0f;

    cryp::ParallelCompressor compressor;
    compressor.prepare (makeSpec(), 1.0f);
    compressor.setUseSmoothRmsDetector (true);
    compressor.setKneeDb (0.0f);
    compressor.setAutoRelease (false);
    compressor.setThresholdDb (thresholdDb);
    compressor.setRatio (ratio);
    compressor.setAttackMs (1.0f);
    compressor.setReleaseMs (50.0f);
    compressor.setMakeupGainDb (0.0f);

    const auto measured = settledGainReductionDb (compressor, 100.0, amplitude);

    const auto rmsLevelDb = juce::Decibels::gainToDecibels (amplitude) - 3.01;
    const auto expected = staticCurveReductionDb (rmsLevelDb, thresholdDb, ratio);

    INFO ("RMS level " << rmsLevelDb << " dBFS, expected " << expected << " dB, measured " << measured << " dB");
    CHECK (measured == Catch::Approx (expected).margin (0.75));
}

TEST_CASE ("ParallelCompressor: the Classic Peak engine reports a gain reduction that tracks its static curve", "[compressor][dsp][metering]")
{
    // juce::dsp::Compressor exposes no internal gain, so the figure is
    // estimated from the block's peak either side of the compressor (see
    // ParallelCompressor::getGainReductionDb()). This is the test that keeps
    // that estimate honest: on the steady tone a meter is read against, it has
    // to land on the peak detector's own static curve.
    constexpr float amplitude = 0.5f;      // -6.02 dBFS peak
    constexpr float thresholdDb = -21.0f;
    constexpr float ratio = 4.0f;

    cryp::ParallelCompressor compressor;
    compressor.prepare (makeSpec(), 1.0f);
    compressor.setUseSmoothRmsDetector (false);
    compressor.setThresholdDb (thresholdDb);
    compressor.setRatio (ratio);
    compressor.setAttackMs (1.0f);
    compressor.setReleaseMs (50.0f);
    compressor.setMakeupGainDb (0.0f);

    const auto measured = settledGainReductionDb (compressor, 100.0, amplitude);

    const auto peakLevelDb = juce::Decibels::gainToDecibels (amplitude);
    const auto expected = staticCurveReductionDb (peakLevelDb, thresholdDb, ratio);

    INFO ("peak level " << peakLevelDb << " dBFS, expected " << expected << " dB, measured " << measured << " dB");
    CHECK (measured == Catch::Approx (expected).margin (1.5));
}

TEST_CASE ("ParallelCompressor: below the threshold, both engines report no gain reduction", "[compressor][dsp][metering]")
{
    for (const bool useSmoothRms : { false, true })
    {
        cryp::ParallelCompressor compressor;
        compressor.prepare (makeSpec(), 1.0f);
        compressor.setUseSmoothRmsDetector (useSmoothRms);
        compressor.setKneeDb (0.0f);
        compressor.setThresholdDb (-12.0f);
        compressor.setRatio (8.0f);
        compressor.setAttackMs (1.0f);
        compressor.setReleaseMs (50.0f);
        compressor.setMakeupGainDb (0.0f);

        // -40 dBFS, far below the threshold.
        const auto measured = settledGainReductionDb (compressor, 100.0, 0.01f);

        INFO ("smooth RMS = " << (useSmoothRms ? 1 : 0) << ", measured " << measured << " dB");
        CHECK (measured == Catch::Approx (0.0).margin (0.1));
    }
}

TEST_CASE ("ParallelCompressor: the parallel mix is a linear blend of the same instant of dry and wet", "[compressor][dsp][mix]")
{
    // "Mix param blends dry/compressed at equal latency" (issue #12). If the
    // dry capture and the compressed path were even one sample apart, a 50 %
    // blend would not equal the average of the 0 % and 100 % renders - it
    // would comb-filter. This asserts the identity sample by sample.
    const auto render = [] (float mix)
    {
        cryp::ParallelCompressor compressor;
        compressor.prepare (makeSpec(), mix);
        compressor.setUseSmoothRmsDetector (true);
        compressor.setThresholdDb (-24.0f);
        compressor.setRatio (6.0f);
        compressor.setAttackMs (2.0f);
        compressor.setReleaseMs (80.0f);
        compressor.setMakeupGainDb (3.0f);

        juce::AudioBuffer<float> buffer (1, numSamples);
        TestHelpers::fillWithSine (buffer, testSampleRate, 90.0, 0.6f);

        juce::dsp::AudioBlock<float> block (buffer);
        compressor.process (block);

        return buffer;
    };

    const auto dry = render (0.0f);
    const auto wet = render (1.0f);
    const auto blended = render (0.5f);

    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto expected = 0.5f * (dry.getSample (0, sample) + wet.getSample (0, sample));
        REQUIRE (blended.getSample (0, sample) == Catch::Approx (expected).margin (1.0e-5));
    }
}
