#include "TestHelpers.h"
#include "dsp/GateEngine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

// The Modern gate (brief §6 T11, T12).
namespace
{
    constexpr double gateSampleRate = 48000.0;

    cryp::GateEngine makeGate (float thresholdDb,
                                float hysteresisDb,
                                float holdMs,
                                float attackMs,
                                float releaseMs,
                                float rangeDb,
                                float sidechainHz = 20.0f)
    {
        cryp::GateEngine gate;

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = gateSampleRate;
        spec.maximumBlockSize = 512;
        spec.numChannels = 1;
        gate.prepare (spec);

        gate.setThresholdDb (thresholdDb);
        gate.setHysteresisDb (hysteresisDb);
        gate.setHoldMs (holdMs);
        gate.setAttackMs (attackMs);
        gate.setReleaseMs (releaseMs);
        gate.setRangeDb (rangeDb);
        gate.setSidechainHighPassHz (sidechainHz);
        gate.reset();

        return gate;
    }

    // Renders a tone whose amplitude is given per sample by `envelope`, and
    // returns the per-sample gain the gate applied.
    template <typename EnvelopeFunction>
    std::vector<double> renderGateGain (cryp::GateEngine& gate,
                                         int numSamples,
                                         double frequencyHz,
                                         EnvelopeFunction&& envelope)
    {
        juce::AudioBuffer<float> buffer (1, numSamples);
        std::vector<double> input (static_cast<size_t> (numSamples));

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto phase = juce::MathConstants<double>::twoPi * frequencyHz
                                * static_cast<double> (sample) / gateSampleRate;
            const auto value = envelope (sample) * std::sin (phase);
            input[static_cast<size_t> (sample)] = value;
            buffer.setSample (0, sample, static_cast<float> (value));
        }

        juce::dsp::AudioBlock<float> block (buffer);

        // Small blocks, so the per-sample control path is exercised the way a
        // real host would drive it.
        constexpr int blockSize = 32;

        for (int offset = 0; offset + blockSize <= numSamples; offset += blockSize)
        {
            auto subBlock = block.getSubBlock (static_cast<size_t> (offset), blockSize);
            gate.process (subBlock);
        }

        // Recover the applied gain by dividing output by input - but only
        // where the input is far enough from a zero crossing for the quotient
        // to mean anything, carrying the last good value forward elsewhere.
        //
        // Defaulting the in-between samples to unity instead (the obvious
        // shortcut) makes every zero crossing look like a wide-open gate,
        // which silently breaks any test that looks for state transitions.
        //
        // The guard tracks the LOCAL envelope rather than the render's peak,
        // so the gain stays measurable through deliberately quiet passages -
        // which is exactly where a gate test needs to see it.
        std::vector<double> gains (static_cast<size_t> (numSamples), 0.0);
        double lastGoodGain = 0.0;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto in = input[static_cast<size_t> (sample)];
            const auto guard = juce::jmax (1.0e-12, std::abs (envelope (sample)) * 0.25);

            if (std::abs (in) > guard)
                lastGoodGain = static_cast<double> (buffer.getSample (0, sample)) / in;

            gains[static_cast<size_t> (sample)] = lastGoodGain;
        }

        return gains;
    }
}

TEST_CASE ("T11: hysteresis stops the gate chattering on a signal sitting at the threshold", "[gate]")
{
    // The defining problem a single-threshold gate has: material hovering
    // around the threshold makes it open and close continuously. With
    // hysteresis the gate opens at T and closes only at T - H, so a signal
    // dithering by less than H crosses once and stays.
    constexpr float thresholdDb = -30.0f;
    constexpr float hysteresisDb = 4.0f;

    auto gate = makeGate (thresholdDb, hysteresisDb, 0.0f, 1.0f, 50.0f, 60.0f);

    constexpr int numSamples = static_cast<int> (gateSampleRate * 2.0);

    // Amplitude dithering +/-1.5 dB around the threshold at 3 Hz - inside the
    // 4 dB hysteresis window, so after the first opening the gate must never
    // move again.
    const auto thresholdAmplitude = std::pow (10.0, thresholdDb / 20.0) * std::sqrt (2.0);

    const auto gains = renderGateGain (gate, numSamples, 500.0, [thresholdAmplitude] (int sample)
    {
        const auto ditherDb = 1.5 * std::sin (juce::MathConstants<double>::twoPi * 3.0
                                               * static_cast<double> (sample) / gateSampleRate);
        return thresholdAmplitude * std::pow (10.0, ditherDb / 20.0);
    });

    // Count transitions after the gate has first opened, in the second half of
    // the render (by which point it is settled).
    int transitions = 0;
    bool wasOpen = false;
    bool seenOpen = false;

    for (size_t sample = static_cast<size_t> (numSamples / 2); sample < gains.size(); ++sample)
    {
        const auto isOpen = gains[sample] > 0.5;

        if (! seenOpen)
        {
            seenOpen = isOpen;
            wasOpen = isOpen;
            continue;
        }

        if (isOpen != wasOpen)
            ++transitions;

        wasOpen = isOpen;
    }

    INFO ("transitions after settling: " << transitions);
    CHECK (seenOpen);
    CHECK (transitions == 0);
}

TEST_CASE ("T11: the open and close thresholds differ by the hysteresis amount", "[gate]")
{
    // Measured by sweeping the level up until the gate opens, then down until
    // it closes.
    constexpr float thresholdDb = -30.0f;

    const auto measureThresholds = [] (float hysteresisDb)
    {
        auto gate = makeGate (thresholdDb, hysteresisDb, 0.0f, 1.0f, 20.0f, 60.0f);

        constexpr int rampSamples = static_cast<int> (gateSampleRate * 3.0);

        struct Thresholds
        {
            double openDb;
            double closeDb;
        };

        // Up-ramp from -50 to -10 dB, then back down.
        const auto levelDbAt = [rampSamples] (int sample)
        {
            const auto position = static_cast<double> (sample) / static_cast<double> (rampSamples);
            return position < 0.5 ? -50.0 + 80.0 * position : -10.0 - 80.0 * (position - 0.5);
        };

        const auto gains = renderGateGain (gate, rampSamples, 500.0, [&levelDbAt] (int sample)
        {
            return std::pow (10.0, levelDbAt (sample) / 20.0) * std::sqrt (2.0);
        });

        Thresholds result { 0.0, 0.0 };
        bool wasOpen = false;

        for (size_t sample = 0; sample < gains.size(); ++sample)
        {
            const auto isOpen = gains[sample] > 0.5;

            if (isOpen && ! wasOpen)
                result.openDb = levelDbAt (static_cast<int> (sample));
            else if (! isOpen && wasOpen)
                result.closeDb = levelDbAt (static_cast<int> (sample));

            wasOpen = isOpen;
        }

        return result;
    };

    const auto wide = measureThresholds (8.0f);
    const auto narrow = measureThresholds (2.0f);

    INFO ("hysteresis 8 dB: open " << wide.openDb << " dB, close " << wide.closeDb << " dB");
    INFO ("hysteresis 2 dB: open " << narrow.openDb << " dB, close " << narrow.closeDb << " dB");

    // The gate always closes below where it opened.
    CHECK (wide.closeDb < wide.openDb);
    CHECK (narrow.closeDb < narrow.openDb);

    // And a larger hysteresis setting produces a wider window. The absolute
    // figures carry the detector's own attack/release lag, so the difference
    // between the two settings is the honest measurement.
    const auto wideWindow = wide.openDb - wide.closeDb;
    const auto narrowWindow = narrow.openDb - narrow.closeDb;

    INFO ("window: 8 dB setting -> " << wideWindow << " dB, 2 dB setting -> " << narrowWindow << " dB");
    CHECK (wideWindow > narrowWindow + 3.0);
}

TEST_CASE ("T11: hold keeps the gate open between fast notes", "[gate]")
{
    // 16th notes at 120 BPM are 125 ms apart. With a 50 ms hold the gate must
    // not slam shut in the gaps.
    constexpr double noteIntervalSeconds = 0.125;
    constexpr double noteLengthSeconds = 0.04;

    const auto worstGainBetweenNotes = [] (float holdMs)
    {
        auto gate = makeGate (-40.0f, 3.0f, holdMs, 1.0f, 200.0f, 60.0f);

        constexpr int numSamples = static_cast<int> (gateSampleRate * 1.5);

        const auto gains = renderGateGain (gate, numSamples, 500.0, [] (int sample)
        {
            const auto t = static_cast<double> (sample) / gateSampleRate;
            const auto phaseInNote = std::fmod (t, noteIntervalSeconds);
            return phaseInNote < noteLengthSeconds ? 0.5 : 0.0005;
        });

        // Look at the gaps only, after the first few notes.
        double worst = 1.0;

        for (int sample = static_cast<int> (gateSampleRate * 0.5); sample < numSamples; ++sample)
        {
            const auto t = static_cast<double> (sample) / gateSampleRate;
            const auto phaseInNote = std::fmod (t, noteIntervalSeconds);

            if (phaseInNote > noteLengthSeconds + 0.005)
                worst = juce::jmin (worst, gains[static_cast<size_t> (sample)]);
        }

        return juce::Decibels::gainToDecibels (worst, -200.0);
    };

    const auto withHold = worstGainBetweenNotes (50.0f);
    const auto withoutHold = worstGainBetweenNotes (0.0f);

    INFO ("worst gain between notes: 50 ms hold " << withHold << " dB, no hold " << withoutHold << " dB");

    // With hold the gate stays essentially open through the gaps.
    CHECK (withHold > -3.0);

    // Without it, it starts closing - which is what hold is for.
    CHECK (withoutHold < withHold - 3.0);
}

TEST_CASE ("T11: the release is linear in dB", "[gate]")
{
    // An exponential release approaches the floor asymptotically and never
    // audibly arrives; a straight line in dB closes in exactly the time the
    // control says. Fitted here against the ideal slope, range / release.
    constexpr float rangeDb = 60.0f;
    constexpr float releaseMs = 200.0f;

    auto gate = makeGate (-40.0f, 3.0f, 0.0f, 1.0f, releaseMs, rangeDb);

    constexpr int burstSamples = static_cast<int> (gateSampleRate * 0.3);
    constexpr int numSamples = static_cast<int> (gateSampleRate * 1.0);

    const auto gains = renderGateGain (gate, numSamples, 500.0, [] (int sample)
    {
        return sample < burstSamples ? 0.5 : 0.0;
    });

    // The input is silent during the release, so the measured "gain" is
    // meaningless there. Re-derive the trajectory from the gate's own output
    // on a quiet-but-nonzero tail instead.
    auto tailGate = makeGate (-40.0f, 3.0f, 0.0f, 1.0f, releaseMs, rangeDb);

    const auto tailGains = renderGateGain (tailGate, numSamples, 500.0, [] (int sample)
    {
        // Drops to -80 dB, far below the threshold, but not to digital
        // silence - so the applied gain stays observable.
        return sample < burstSamples ? 0.5 : 0.0001;
    });

    // Collect the descending portion and fit a line in dB.
    std::vector<double> times;
    std::vector<double> levelsDb;

    for (int sample = burstSamples; sample < numSamples; ++sample)
    {
        const auto gainDb = juce::Decibels::gainToDecibels (tailGains[static_cast<size_t> (sample)], -200.0);

        // Fit over the clearly-descending middle of the ramp, away from the
        // hold/attack rounding at the top and the floor at the bottom.
        if (gainDb < -6.0 && gainDb > -(rangeDb - 6.0))
        {
            times.push_back (static_cast<double> (sample - burstSamples) / gateSampleRate);
            levelsDb.push_back (gainDb);
        }
    }

    REQUIRE (times.size() > 100);

    // Least squares.
    const auto count = static_cast<double> (times.size());
    double sumT = 0.0, sumL = 0.0, sumTT = 0.0, sumTL = 0.0;

    for (size_t index = 0; index < times.size(); ++index)
    {
        sumT += times[index];
        sumL += levelsDb[index];
        sumTT += times[index] * times[index];
        sumTL += times[index] * levelsDb[index];
    }

    const auto slope = (count * sumTL - sumT * sumL) / (count * sumTT - sumT * sumT);
    const auto intercept = (sumL - slope * sumT) / count;

    // Coefficient of determination.
    const auto meanL = sumL / count;
    double residualSquares = 0.0, totalSquares = 0.0;

    for (size_t index = 0; index < times.size(); ++index)
    {
        const auto predicted = slope * times[index] + intercept;
        residualSquares += (levelsDb[index] - predicted) * (levelsDb[index] - predicted);
        totalSquares += (levelsDb[index] - meanL) * (levelsDb[index] - meanL);
    }

    const auto rSquared = 1.0 - residualSquares / totalSquares;
    const auto expectedSlope = -static_cast<double> (rangeDb) / (static_cast<double> (releaseMs) / 1000.0);

    INFO ("fitted slope " << slope << " dB/s, expected " << expectedSlope << " dB/s, R^2 " << rSquared);

    CHECK (rSquared > 0.99);
    CHECK (slope == Catch::Approx (expectedSlope).epsilon (0.10));
}

TEST_CASE ("T12: the sidechain highpass keeps the fundamental from holding the gate open", "[gate]")
{
    // The reason a bass gate needs a detector highpass at all: without it, a
    // ringing low string holds the gate open indefinitely.
    const auto opensFor = [] (double frequencyHz, double levelDbRelativeToThreshold)
    {
        constexpr float thresholdDb = -40.0f;

        auto gate = makeGate (thresholdDb, 3.0f, 0.0f, 1.0f, 50.0f, 60.0f, 100.0f);

        const auto amplitude = std::pow (10.0, (thresholdDb + levelDbRelativeToThreshold) / 20.0) * std::sqrt (2.0);

        constexpr int numSamples = static_cast<int> (gateSampleRate * 1.0);
        const auto gains = renderGateGain (gate, numSamples, frequencyHz, [amplitude] (int) { return amplitude; });

        // Did it end up open?
        return gains[gains.size() - 100] > 0.5;
    };

    // 50 Hz, a full 10 dB over the threshold, is attenuated ~12 dB by the
    // 100 Hz second-order sidechain filter and must not open the gate.
    CHECK_FALSE (opensFor (50.0, 10.0));

    // 2 kHz only 3 dB over passes the filter untouched and must open it.
    CHECK (opensFor (2000.0, 3.0));
}

TEST_CASE ("The Modern gate closes to its Range setting and reports gain reduction", "[gate]")
{
    SECTION ("a closed gate attenuates by exactly Range")
    {
        for (const auto rangeDb : { 12.0f, 40.0f, 80.0f })
        {
            auto gate = makeGate (-30.0f, 3.0f, 0.0f, 1.0f, 20.0f, rangeDb);

            constexpr int numSamples = static_cast<int> (gateSampleRate * 1.0);

            // Well below the threshold throughout, so the gate stays shut.
            const auto gains = renderGateGain (gate, numSamples, 500.0, [] (int) { return 0.0001; });

            const auto finalDb = juce::Decibels::gainToDecibels (gains[gains.size() - 100], -200.0);

            INFO ("range " << rangeDb << " dB: settled at " << finalDb << " dB");
            CHECK (finalDb == Catch::Approx (-rangeDb).margin (0.5));
            CHECK (gate.getGainReductionDb() == Catch::Approx (rangeDb).margin (0.5f));
        }
    }

    SECTION ("an open gate reports no reduction")
    {
        auto gate = makeGate (-40.0f, 3.0f, 0.0f, 1.0f, 20.0f, 60.0f);

        constexpr int numSamples = static_cast<int> (gateSampleRate * 0.5);
        const auto gains = renderGateGain (gate, numSamples, 500.0, [] (int) { return 0.5; });

        CHECK (gains[gains.size() - 100] == Catch::Approx (1.0).margin (0.01));
        CHECK (gate.getGainReductionDb() < 0.5f);
    }
}
