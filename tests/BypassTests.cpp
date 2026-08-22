#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

// Issue #87: bypass must be (1) click-free - a cross-fade between the
// continuously-running wet chain and a dry copy, not a hard switch - and (2)
// latency-compensated - the dry path delayed by exactly getLatencySamples()
// so a bypassed instance nulls against a dry track under a host's plugin
// delay compensation. See PluginProcessor.h's bypassDryDelay/bypassWetMix
// member docs for the mechanism.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int testBlockSize = 256;
    constexpr int testBlocks = 40;

    // Same probe as ParameterSweepTests.cpp's reproduction of this issue:
    // 110 Hz, 0.5 amplitude, low enough that steady-state slew is tiny
    // compared to any click and squarely inside every band.
    constexpr double probeFrequencyHz = 110.0;
    constexpr float probeAmplitude = 0.5f;

    float maxSampleStep (const juce::AudioBuffer<float>& buffer, int fromSample, int toSampleExclusive)
    {
        float maximum = 0.0f;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int sample = juce::jmax (1, fromSample); sample < toSampleExclusive; ++sample)
                maximum = juce::jmax (maximum, std::abs (data[sample] - data[sample - 1]));
        }

        return maximum;
    }

    // Renders `testBlocks` blocks of a continuous-phase sine, toggling bypass
    // to `bypassOn` immediately before block index `toggleAtBlock`. Returns
    // the whole concatenated render so the transition itself is inside the
    // measured region, exactly like ParameterSweepTests.cpp's renderSweep().
    juce::AudioBuffer<float> renderBypassToggle (bool startBypassed, bool bypassOnAtToggle, int toggleAtBlock)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, testSampleRate, testBlockSize);
        processor.prepareToPlay (testSampleRate, testBlockSize);

        TestHelpers::setParameter (processor, ParamIDs::bypass, startBypassed ? 1.0f : 0.0f);

        juce::AudioBuffer<float> render (2, testBlocks * testBlockSize);
        render.clear();

        juce::AudioBuffer<float> block (2, testBlockSize);
        juce::MidiBuffer midi;

        for (int i = 0; i < testBlocks; ++i)
        {
            if (i == toggleAtBlock)
                TestHelpers::setParameter (processor, ParamIDs::bypass, bypassOnAtToggle ? 1.0f : 0.0f);

            TestHelpers::fillWithSine (block, testSampleRate, probeFrequencyHz, probeAmplitude,
                                        static_cast<juce::int64> (i) * testBlockSize);
            processor.processBlock (block, midi);

            for (int channel = 0; channel < 2; ++channel)
                render.copyFrom (channel, i * testBlockSize, block, channel, 0, testBlockSize);
        }

        return render;
    }
}

TEST_CASE ("Bypass: engaging mid-render does not step further than either steady state", "[bypass][dsp]")
{
    constexpr int toggleAtBlock = 20;
    const auto render = renderBypassToggle (false, true, toggleAtBlock);

    // Steady state before the toggle (skip the first few blocks so any
    // startup transient has settled) and after it (skip enough blocks for
    // the crossfade/latency-compensation delay to have fully flushed).
    const auto steadyBeforeStep = maxSampleStep (render, 4 * testBlockSize, toggleAtBlock * testBlockSize);
    const auto steadyAfterStep = maxSampleStep (render, (toggleAtBlock + 4) * testBlockSize, testBlocks * testBlockSize);
    const auto transitionStep = maxSampleStep (render, toggleAtBlock * testBlockSize, (toggleAtBlock + 4) * testBlockSize);

    INFO ("steady-state (wet) max step  = " << steadyBeforeStep);
    INFO ("steady-state (dry) max step  = " << steadyAfterStep);
    INFO ("transition max step          = " << transitionStep);

    CHECK (TestHelpers::allSamplesFinite (render));

    // The honest, scale-free "no click" bound used throughout the suite
    // (see ParameterSweepTests.cpp): the transition must not step further
    // than a small multiple of either endpoint's own steady-state slew.
    const auto ownSlewBound = 4.0f * juce::jmax (steadyBeforeStep, steadyAfterStep);
    CHECK (transitionStep <= ownSlewBound);
}

TEST_CASE ("Bypass: disengaging mid-render does not step further than either steady state", "[bypass][dsp]")
{
    constexpr int toggleAtBlock = 20;
    const auto render = renderBypassToggle (true, false, toggleAtBlock);

    const auto steadyBeforeStep = maxSampleStep (render, 4 * testBlockSize, toggleAtBlock * testBlockSize);
    const auto steadyAfterStep = maxSampleStep (render, (toggleAtBlock + 4) * testBlockSize, testBlocks * testBlockSize);
    const auto transitionStep = maxSampleStep (render, toggleAtBlock * testBlockSize, (toggleAtBlock + 4) * testBlockSize);

    INFO ("steady-state (dry) max step  = " << steadyBeforeStep);
    INFO ("steady-state (wet) max step  = " << steadyAfterStep);
    INFO ("transition max step          = " << transitionStep);

    CHECK (TestHelpers::allSamplesFinite (render));

    const auto ownSlewBound = 4.0f * juce::jmax (steadyBeforeStep, steadyAfterStep);
    CHECK (transitionStep <= ownSlewBound);
}

TEST_CASE ("Bypass: reported latency does not change when bypass is engaged", "[bypass][latency][dsp]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, testBlockSize);

    const auto latencyBeforeBypass = processor.getLatencySamples();
    CHECK (latencyBeforeBypass > 0);

    TestHelpers::setParameter (processor, ParamIDs::bypass, 1.0f);

    // Engaging bypass is a parameter change, not a prepareToPlay() call - it
    // must never re-derive or change the reported figure (that would be a
    // latency change mid-stream, which hosts handle very poorly).
    CHECK (processor.getLatencySamples() == latencyBeforeBypass);
}

TEST_CASE ("Bypass: a dirac arrives exactly at the reported latency while bypassed", "[bypass][latency][dsp]")
{
    // The direct analogue of LatencyTests.cpp's T14 dirac test, but for the
    // bypass dry path: if a bypassed instance does not delay the dry signal
    // by exactly getLatencySamples(), a host's plugin delay compensation
    // pulls its output early relative to every other (non-bypassed) track,
    // and a null test against a dry copy does not null.
    CryptaAudioProcessor processor;
    processor.setPlayConfigDetails (1, 1, testSampleRate, testBlockSize);
    processor.prepareToPlay (testSampleRate, testBlockSize);

    TestHelpers::setParameter (processor, ParamIDs::bypass, 1.0f);

    // Re-priming via prepareToPlay(), same pattern LatencyTests.cpp's T14
    // tests use after changing driveEngine: snaps bypassWetMix straight to
    // its new (bypassed) target rather than leaving a 20ms crossfade ramp
    // in flight, which would otherwise blend a genuinely-bypassed dirac
    // with a genuinely-wet one for the first ~960 samples and smear the
    // peak this test is about to look for.
    processor.prepareToPlay (testSampleRate, testBlockSize);
    processor.reset();

    const auto reportedLatency = processor.getLatencySamples();
    REQUIRE (reportedLatency > 0);

    juce::AudioBuffer<float> buffer (1, 8192);
    buffer.clear();
    buffer.setSample (0, 0, 1.0f);

    TestHelpers::renderThrough (processor, buffer, testBlockSize);

    int peakIndex = 0;
    float peakValue = 0.0f;

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        const auto magnitude = std::abs (buffer.getSample (0, sample));

        if (magnitude > peakValue)
        {
            peakValue = magnitude;
            peakIndex = sample;
        }
    }

    INFO ("reported latency = " << reportedLatency << ", dirac peak at " << peakIndex);
    CHECK (peakIndex == reportedLatency);
    CHECK (peakValue == Catch::Approx (1.0f).margin (1e-3));
}

TEST_CASE ("Bypass: settled bypass nulls against a dry copy delayed by the reported latency", "[bypass][latency][dsp]")
{
    // The QA checklist's Reaper protocol (docs/qa-checklist.md): "a null test
    // against a dry-copy track cancels when the plugin is in a neutral
    // state". Neutral state via the bypass switch must null against a dry
    // track shifted by getLatencySamples() - not against an unshifted one,
    // and not fail to null at all.
    CryptaAudioProcessor processor;
    processor.setPlayConfigDetails (1, 1, testSampleRate, testBlockSize);
    processor.prepareToPlay (testSampleRate, testBlockSize);

    TestHelpers::setParameter (processor, ParamIDs::bypass, 1.0f);
    processor.reset();

    const auto latency = processor.getLatencySamples();
    REQUIRE (latency > 0);

    constexpr int numSamples = 8192;
    juce::AudioBuffer<float> dry (1, numSamples);
    TestHelpers::fillWithSine (dry, testSampleRate, probeFrequencyHz, probeAmplitude);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (dry);

    // Settle: run the same continuous-phase signal through first so the
    // crossfade (already at its bypassed target from construction/reset,
    // per the latency test above) and the delay line's history are both
    // fully in steady state before the samples under test.
    TestHelpers::renderThrough (processor, processed, testBlockSize);

    // Null test: output[n] should equal dry[n - latency] once the delay
    // line has flushed (i.e. for n >= latency), well within the settled
    // region this test only samples.
    double sumSquaredError = 0.0;
    double sumSquaredReference = 0.0;
    const auto checkFrom = numSamples / 2; // deep in the settled region

    for (int n = checkFrom; n < numSamples; ++n)
    {
        const auto dryDelayed = (n - latency >= 0) ? dry.getSample (0, n - latency) : 0.0f;
        const auto error = processed.getSample (0, n) - dryDelayed;

        sumSquaredError += static_cast<double> (error) * static_cast<double> (error);
        sumSquaredReference += static_cast<double> (dryDelayed) * static_cast<double> (dryDelayed);
    }

    REQUIRE (sumSquaredReference > 0.0);

    const auto nullDepthDb = 10.0 * std::log10 (sumSquaredError / sumSquaredReference);
    INFO ("null depth = " << nullDepthDb << " dB (lower is a better null)");

    // A genuine null test on real audio settles somewhere below -80 dB once
    // dither/float rounding is accounted for; this is a synthetic float
    // render with no dither at all, so a much tighter bound is fair.
    CHECK (nullDepthDb < -100.0);
}

TEST_CASE ("Bypass: the wet chain keeps running while bypassed, so its state never goes stale",
           "[bypass][dsp]")
{
    // The previous (early-return) implementation froze every stage's
    // internal state (filter memory, envelope followers, oversampling FIR
    // history) the instant bypass engaged - processChunk() was never called
    // at all while bypassed. That means, fed the SAME continuous input
    // throughout, a run that spends a while bypassed in the middle and a
    // run that never bypasses at all would end up in DIFFERENT internal
    // states once bypass disengages and the crossfade settles back to fully
    // wet: the never-bypassed run's state kept evolving with the live
    // signal the whole time, while the old bypassed run's state sat frozen
    // at whatever it was when bypass engaged. That divergence is exactly
    // what surfaces as a click on the way OUT of bypass.
    //
    // With the wet chain always running (this fix), both runs process
    // identical input at every single sample regardless of what the bypass
    // flag or the output crossfade are doing - the blend only touches the
    // final output stage, never feeds back into anything - so once the
    // crossfade has settled back to fully wet, the two runs' outputs must
    // be identical to numerical precision, not just "close enough not to
    // click".
    constexpr int bypassBlocks = 30;
    constexpr int postBypassSettleBlocks = 8; // several multiples of the 20ms/~960-sample ramp
    constexpr int totalBlocks = 4 /* pre-settle */ + bypassBlocks + postBypassSettleBlocks + 4;

    const auto renderWithMidBypass = [] (bool engageBypass)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, testSampleRate, testBlockSize);
        processor.prepareToPlay (testSampleRate, testBlockSize);

        // A parameter setting that gives every stage real, evolving state to
        // potentially go stale: the low-band compressor's envelope and the
        // noise gate both track a signal, rather than sitting inert.
        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 1.0f);

        juce::AudioBuffer<float> block (2, testBlockSize);
        juce::MidiBuffer midi;
        juce::int64 sampleIndex = 0;

        for (int i = 0; i < totalBlocks; ++i)
        {
            if (engageBypass && i == 4)
                TestHelpers::setParameter (processor, ParamIDs::bypass, 1.0f);

            if (engageBypass && i == 4 + bypassBlocks)
                TestHelpers::setParameter (processor, ParamIDs::bypass, 0.0f);

            // A varying-amplitude probe (rather than a flat sine) so the
            // gate and compressor envelopes actually move during the
            // bypassed section instead of sitting at a fixed level - state
            // that stayed frozen would diverge more visibly against this
            // than against a constant-level signal.
            const auto envelope = 0.5f + 0.5f * std::sin (juce::MathConstants<float>::twoPi
                                                            * static_cast<float> (i) / 17.0f);
            TestHelpers::fillWithSine (block, testSampleRate, probeFrequencyHz,
                                        probeAmplitude * envelope, sampleIndex);
            processor.processBlock (block, midi);
            sampleIndex += testBlockSize;
        }

        return block; // the final block, well after the crossfade has settled back to wet
    };

    const auto neverBypassed = renderWithMidBypass (false);
    const auto bypassedThenRestored = renderWithMidBypass (true);

    double sumSquaredDifference = 0.0;
    double sumSquaredReference = 0.0;

    for (int channel = 0; channel < 2; ++channel)
    {
        for (int sample = 0; sample < testBlockSize; ++sample)
        {
            const auto reference = neverBypassed.getSample (channel, sample);
            const auto restored = bypassedThenRestored.getSample (channel, sample);
            const auto diff = restored - reference;

            sumSquaredDifference += static_cast<double> (diff) * static_cast<double> (diff);
            sumSquaredReference += static_cast<double> (reference) * static_cast<double> (reference);
        }
    }

    REQUIRE (sumSquaredReference > 0.0);

    const auto differenceDb = 10.0 * std::log10 (sumSquaredDifference / sumSquaredReference);
    INFO ("state-continuity difference, settled well after un-bypass = " << differenceDb << " dB");

    CHECK (differenceDb < -100.0);
}
