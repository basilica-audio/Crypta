#include "TestHelpers.h"
#include "dsp/LevelDetector.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

// The Smooth RMS low-band detector (brief §6 T7, T8).
namespace
{
    constexpr double detectorSampleRate = 48000.0;

    cryp::LevelDetector makeDetector (float thresholdDb,
                                       float ratio,
                                       float kneeDb,
                                       float attackMs,
                                       float releaseMs,
                                       bool autoRelease = false)
    {
        cryp::LevelDetector detector;

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = detectorSampleRate;
        spec.maximumBlockSize = 512;
        spec.numChannels = 1;
        detector.prepare (spec);

        detector.setThresholdDb (thresholdDb);
        detector.setRatio (ratio);
        detector.setKneeDb (kneeDb);
        detector.setAttackMs (attackMs);
        detector.setReleaseMs (releaseMs);
        detector.setAutoRelease (autoRelease);
        detector.reset();

        return detector;
    }

    // Runs a steady tone through the detector and returns the gain reduction
    // it settles at, in positive dB.
    double settledGainReductionDb (cryp::LevelDetector& detector, double frequencyHz, double amplitude)
    {
        constexpr int numSamples = static_cast<int> (detectorSampleRate * 1.0);

        juce::AudioBuffer<float> buffer (1, numSamples);
        juce::AudioBuffer<float> reference (1, numSamples);

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto phase = juce::MathConstants<double>::twoPi * frequencyHz
                                * static_cast<double> (sample) / detectorSampleRate;
            const auto value = static_cast<float> (amplitude * std::sin (phase));
            buffer.setSample (0, sample, value);
            reference.setSample (0, sample, value);
        }

        juce::dsp::AudioBlock<float> block (buffer);
        detector.process (block);

        // Measure over the last 200 ms, well past any ballistics settling.
        const auto start = numSamples - static_cast<int> (detectorSampleRate * 0.2);

        double processedSquares = 0.0;
        double referenceSquares = 0.0;

        for (int sample = start; sample < numSamples; ++sample)
        {
            processedSquares += static_cast<double> (buffer.getSample (0, sample))
                                 * static_cast<double> (buffer.getSample (0, sample));
            referenceSquares += static_cast<double> (reference.getSample (0, sample))
                                 * static_cast<double> (reference.getSample (0, sample));
        }

        return -10.0 * std::log10 (processedSquares / referenceSquares);
    }
}

TEST_CASE ("T7: the RMS detector does not ripple on a bass fundamental", "[detector]")
{
    // The defect this engine exists to fix. A peak detector with a 6 ms
    // release - the sourced glue ballistics this plugin ships - follows the
    // individual half-cycles of an 80 Hz tone, so the gain reduction ripples
    // at 160 Hz and the low band tremolos.
    constexpr double toneHz = 80.0;
    constexpr double amplitude = 0.5; // ~6 dB over the threshold below

    auto detector = makeDetector (-12.0f, 4.0f, 0.0f, 3.0f, 6.0f);

    constexpr int numSamples = static_cast<int> (detectorSampleRate * 0.6);
    juce::AudioBuffer<float> buffer (1, numSamples);

    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto phase = juce::MathConstants<double>::twoPi * toneHz
                            * static_cast<double> (sample) / detectorSampleRate;
        buffer.setSample (0, sample, static_cast<float> (amplitude * std::sin (phase)));
    }

    juce::dsp::AudioBlock<float> block (buffer);

    // Process in small blocks and record the per-block gain reduction, which
    // is what ripples if the detector is following the waveform.
    constexpr int blockSize = 64;
    std::vector<double> gainReductions;

    for (int offset = 0; offset + blockSize <= numSamples; offset += blockSize)
    {
        auto subBlock = block.getSubBlock (static_cast<size_t> (offset), blockSize);
        detector.process (subBlock);

        // Discard the first 300 ms of settling.
        if (offset > static_cast<int> (detectorSampleRate * 0.3))
            gainReductions.push_back (detector.getGainReductionDb());
    }

    REQUIRE (gainReductions.size() > 10);

    auto minimum = gainReductions.front();
    auto maximum = gainReductions.front();

    for (const auto value : gainReductions)
    {
        minimum = juce::jmin (minimum, value);
        maximum = juce::jmax (maximum, value);
    }

    INFO ("gain reduction ripple: " << (maximum - minimum) << " dB peak-to-peak (min " << minimum
                                     << ", max " << maximum << ")");

    // The detector must be genuinely compressing, or a flat reading would
    // pass trivially.
    CHECK (maximum > 1.0);
    CHECK ((maximum - minimum) <= 0.5);
}

TEST_CASE ("T8: the static curve matches the soft-knee equations", "[detector]")
{
    // Giannoulis et al.'s gain computer, checked against its own algebra at
    // the points that define it: below the knee, at each knee edge, at the
    // threshold, and well above.
    constexpr float thresholdDb = -20.0f;
    constexpr float ratio = 4.0f;
    constexpr float kneeDb = 12.0f;

    // The curve, written out independently of the implementation.
    const auto expectedGainReductionDb = [] (double levelDb)
    {
        constexpr double threshold = thresholdDb;
        constexpr double knee = kneeDb;
        constexpr double inverseRatio = 1.0 / ratio;

        const auto overshoot = levelDb - threshold;

        if (2.0 * std::abs (overshoot) <= knee)
        {
            const auto kneeTerm = overshoot + knee * 0.5;
            return -((inverseRatio - 1.0) * kneeTerm * kneeTerm / (2.0 * knee));
        }

        if (overshoot <= 0.0)
            return 0.0;

        return -(overshoot * (inverseRatio - 1.0));
    };

    for (const auto offsetDb : { -12.0, -6.0, 0.0, 6.0, 12.0, 20.0 })
    {
        // A sine's RMS is its amplitude / sqrt(2), and the detector measures
        // mean square - so the level it sees is the RMS level, not the peak.
        const auto targetRmsDb = thresholdDb + offsetDb;
        const auto amplitude = std::pow (10.0, targetRmsDb / 20.0) * std::sqrt (2.0);

        // Long window and a hard knee-independent settle: a slow release
        // would otherwise leave the measurement short of the static curve.
        auto detector = makeDetector (thresholdDb, ratio, kneeDb, 1.0f, 50.0f);

        const auto measured = settledGainReductionDb (detector, 200.0, amplitude);
        const auto expected = expectedGainReductionDb (targetRmsDb);

        INFO ("at threshold " << offsetDb << " dB: measured " << measured << " dB, expected " << expected << " dB");
        CHECK (measured == Catch::Approx (expected).margin (0.25));
    }
}

TEST_CASE ("T8: auto-makeup compensates half the gain taken at the threshold", "[detector]")
{
    // The sign here is the whole point. -0.5*T*(1 - 1/R) with a NEGATIVE
    // threshold gives a POSITIVE gain - makeup means boost. Writing it as
    // -0.5*T*(1/R - 1) instead (an easy slip) produces attenuation, and every
    // preset would quietly get quieter.
    struct Anchor
    {
        float thresholdDb;
        float ratio;
        float expectedDb;
    };

    // The canonical sanity anchor from the brief, plus two more.
    const std::vector<Anchor> anchors {
        { -18.0f, 2.0f, 4.5f },
        { -24.0f, 4.0f, 9.0f },
        { -12.0f, 3.0f, 4.0f },
    };

    for (const auto& anchor : anchors)
    {
        auto detector = makeDetector (anchor.thresholdDb, anchor.ratio, 6.0f, 3.0f, 6.0f);

        INFO ("threshold " << anchor.thresholdDb << " dB at " << anchor.ratio << ":1");
        CHECK (detector.getAutoMakeupDb() == Catch::Approx (anchor.expectedDb).margin (0.1f));

        // And it is a boost, not a cut.
        CHECK (detector.getAutoMakeupDb() > 0.0f);
    }
}

TEST_CASE ("T9: auto-release stretches on sustained material but not on transients", "[detector]")
{
    // Program-dependent release: a staccato burst should recover at the set
    // release time, while a sustained decaying note should not be pumped.
    const auto recoveryTimeMs = [] (bool sustained, bool autoRelease)
    {
        auto detector = makeDetector (-24.0f, 4.0f, 0.0f, 3.0f, 30.0f, autoRelease);

        constexpr int numSamples = static_cast<int> (detectorSampleRate * 1.2);
        juce::AudioBuffer<float> buffer (1, numSamples);

        const auto burstSamples = static_cast<int> (detectorSampleRate * 0.25);

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto phase = juce::MathConstants<double>::twoPi * 100.0
                                * static_cast<double> (sample) / detectorSampleRate;

            double envelope = 0.0;

            if (sample < burstSamples)
            {
                envelope = 0.7;
            }
            else if (sustained)
            {
                // 30 dB/s decay - a note ringing out, not stopping.
                const auto secondsAfter = static_cast<double> (sample - burstSamples) / detectorSampleRate;
                envelope = 0.7 * std::pow (10.0, -30.0 * secondsAfter / 20.0);
            }

            buffer.setSample (0, sample, static_cast<float> (envelope * std::sin (phase)));
        }

        // Walk block by block after the burst and find when gain reduction
        // falls below 1 dB.
        juce::dsp::AudioBlock<float> block (buffer);
        constexpr int blockSize = 32;

        for (int offset = 0; offset + blockSize <= numSamples; offset += blockSize)
        {
            auto subBlock = block.getSubBlock (static_cast<size_t> (offset), blockSize);
            detector.process (subBlock);

            if (offset > burstSamples && detector.getGainReductionDb() < 1.0f)
                return 1000.0 * static_cast<double> (offset - burstSamples) / detectorSampleRate;
        }

        return 1000.0 * static_cast<double> (numSamples - burstSamples) / detectorSampleRate;
    };

    const auto staccatoMs = recoveryTimeMs (false, true);
    const auto sustainedMs = recoveryTimeMs (true, true);

    INFO ("recovery: staccato " << staccatoMs << " ms, sustained " << sustainedMs << " ms");

    // A hard stop recovers promptly.
    CHECK (staccatoMs < 200.0);

    // A note ringing out holds the compressor down for longer, which is what
    // stops it pumping.
    CHECK (sustainedMs > staccatoMs);
}

TEST_CASE ("The detector is well-behaved at the edges of its ranges", "[detector][robustness]")
{
    SECTION ("silence produces no gain reduction and no NaN")
    {
        auto detector = makeDetector (-40.0f, 8.0f, 6.0f, 1.0f, 10.0f);

        juce::AudioBuffer<float> buffer (1, 4096);
        buffer.clear();

        juce::dsp::AudioBlock<float> block (buffer);
        detector.process (block);

        CHECK (TestHelpers::allSamplesFinite (buffer));
        CHECK (detector.getGainReductionDb() == Catch::Approx (0.0f).margin (0.01f));
    }

    SECTION ("a 1:1 ratio is transparent whatever the level")
    {
        auto detector = makeDetector (-40.0f, 1.0f, 0.0f, 1.0f, 10.0f);
        const auto reduction = settledGainReductionDb (detector, 100.0, 0.9);

        INFO ("gain reduction at 1:1: " << reduction << " dB");
        CHECK (std::abs (reduction) < 0.05);
    }

    SECTION ("a hard knee is continuous at the threshold")
    {
        // Knee 0 must not produce a discontinuity: the two branches have to
        // meet exactly where they cross.
        auto below = makeDetector (-20.0f, 4.0f, 0.0f, 1.0f, 50.0f);
        auto above = makeDetector (-20.0f, 4.0f, 0.0f, 1.0f, 50.0f);

        const auto justBelow = settledGainReductionDb (below, 200.0, std::pow (10.0, -20.5 / 20.0) * std::sqrt (2.0));
        const auto justAbove = settledGainReductionDb (above, 200.0, std::pow (10.0, -19.5 / 20.0) * std::sqrt (2.0));

        INFO ("gain reduction just below threshold " << justBelow << " dB, just above " << justAbove << " dB");
        CHECK (justBelow == Catch::Approx (0.0).margin (0.2));
        CHECK (justAbove < 0.6);
        CHECK (justAbove > justBelow);
    }
}
