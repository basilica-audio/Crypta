#include "PluginProcessor.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

// End-to-end parameter-extreme sweep (issue #34, "Automation gaps worth closing
// before v1.0.0": *A parameter-sweep null test. Render at each parameter's
// extremes and assert the output is finite, bounded and free of discontinuities
// - currently covered per-stage, not end-to-end across the whole set.*)
//
// What was already covered, and why this is not it: the suite has per-stage
// denormal and extreme sweeps (CrossoverTests, MidBandTests, VoicingTests,
// ParallelCompressorTests, ...) and one whole-processor case that randomises
// every parameter every block and checks for NaN/Inf
// (SampleRateAndRobustnessTests.cpp). Randomising every parameter simultaneously
// is a good fuzz, but it is not a sweep: a bad interaction at one parameter's
// endpoint is diluted by fifty other parameters also being somewhere random, and
// "not NaN" is a much weaker claim than "bounded, and it did not click".
//
// This walks the whole parameter set, one parameter at a time, and asserts three
// separate things at each endpoint:
//
//   FINITE    no NaN, no Inf.
//   BOUNDED   the output stays under a documented ceiling. Crypta's safety clip
//             is the last stage in the chain, so an endpoint that produces a
//             runaway is a real defect rather than a taste question.
//   NO CLICK  moving the parameter to its endpoint mid-render does not produce a
//             sample-to-sample step bigger than what either endpoint's own
//             steady state produces. This is the honest form of "free of
//             discontinuities": the steady-state waveform of a hard-clipping
//             drive stage legitimately contains very fast edges, so an absolute
//             slew limit would either fail on correct behaviour or be too loose
//             to catch anything. Comparing the transition against both of its
//             own endpoints is scale-free and does not care how distorted the
//             signal is.
namespace
{
    constexpr double sweepSampleRate = 48000.0;
    constexpr int sweepBlockSize = 256;
    constexpr int sweepBlocks = 40;

    // 110 Hz: an open A on a bass, low enough that the natural sample-to-sample
    // slew of the input is tiny compared with any click, and squarely inside
    // the low band so the crossover, the compressor and the growl branch are
    // all doing something.
    constexpr double sweepFrequencyHz = 110.0;
    constexpr float sweepAmplitude = 0.5f;

    // "Bounded" for a parameter that is not a gain control: the safety
    // clipper's ceiling plus headroom for the transient it is allowed to pass.
    //
    // A gain control is a different question. Output Gain and Low Comp Makeup
    // both range to +24 dB, and a user asking for +24 dB and getting it is the
    // parameter working, not a runaway - measured peaks of 11.4 and 5.0
    // respectively at 0.5 input. The ceiling for those is therefore derived
    // from the parameter's own declared range (see ceilingFor()) rather than
    // being a flat number, so the assertion keeps its teeth without calling
    // correct behaviour a defect.
    constexpr float boundedCeiling = 4.0f;

    struct Render
    {
        juce::AudioBuffer<float> audio;
        float peak = 0.0f;
        float maxStep = 0.0f;
        bool finite = true;
    };

    float maxSampleStep (const juce::AudioBuffer<float>& buffer)
    {
        float maximum = 0.0f;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int sample = 1; sample < buffer.getNumSamples(); ++sample)
                maximum = juce::jmax (maximum, std::abs (data[sample] - data[sample - 1]));
        }

        return maximum;
    }

    // Renders `sweepBlocks` blocks of a steady sine. `applyAtBlock` (if given)
    // is invoked before that block so a parameter change can be dropped into
    // the middle of a running render; the returned Render covers the whole
    // concatenated output, not just the last block, so the transition itself is
    // inside the measured region.
    Render renderSweep (const std::function<void (CryptaAudioProcessor&)>& setUp,
                        const std::function<void (CryptaAudioProcessor&)>& applyMidway = {},
                        int applyAtBlock = -1)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, sweepSampleRate, sweepBlockSize);
        processor.prepareToPlay (sweepSampleRate, sweepBlockSize);

        if (setUp)
            setUp (processor);

        Render render;
        render.audio.setSize (2, sweepBlocks * sweepBlockSize);
        render.audio.clear();

        juce::AudioBuffer<float> block (2, sweepBlockSize);
        juce::MidiBuffer midi;

        for (int i = 0; i < sweepBlocks; ++i)
        {
            if (applyMidway && i == applyAtBlock)
                applyMidway (processor);

            TestHelpers::fillWithSine (block, sweepSampleRate, sweepFrequencyHz, sweepAmplitude,
                                        static_cast<juce::int64> (i) * sweepBlockSize);
            processor.processBlock (block, midi);

            for (int channel = 0; channel < 2; ++channel)
                render.audio.copyFrom (channel, i * sweepBlockSize, block, channel, 0, sweepBlockSize);
        }

        render.finite = TestHelpers::allSamplesFinite (render.audio);
        render.peak = render.audio.getMagnitude (0, render.audio.getNumSamples());
        render.maxStep = maxSampleStep (render.audio);

        return render;
    }

    struct ParameterUnderTest
    {
        juce::String id;
        juce::RangedAudioParameter* parameter = nullptr;
    };

    // A dB-labelled parameter is allowed to reach the gain it advertises;
    // everything else is held to the safety clipper's ceiling.
    float ceilingFor (const ParameterUnderTest& entry)
    {
        if (entry.parameter == nullptr || entry.parameter->getLabel() != "dB")
            return boundedCeiling;

        const auto maximumDb = entry.parameter->getNormalisableRange().end;

        if (maximumDb <= 0.0f)
            return boundedCeiling;

        return boundedCeiling * juce::Decibels::decibelsToGain (maximumDb);
    }

    // Parameters whose endpoint transition is a KNOWN, measured step rather
    // than a smooth ramp. These are not exemptions - each carries the value
    // measured on the build that introduced this test, and the assertion is
    // that it does not get worse. Removing an entry is how one of these gets
    // fixed; raising a number needs a reason.
    //
    //   gateEnabled   engaging the gate mid-signal is a mode switch, not a
    //                 continuous control; the gate's own attack starts from
    //                 closed.
    //   splitLowHz    juce::dsp::LinkwitzRileyFilter recomputes coefficients
    //                 immediately, so snapping a crossover across its entire
    //                 range in a single block steps the filter state. Standard
    //                 for un-smoothed coefficient updates.
    //
    // `bypass` had an entry here (0.85, against a 0.772 measured step) until
    // issue #87 replaced its unsmoothed early-return with a click-free,
    // latency-compensated crossfade (see PluginProcessor.h's bypassWetMix/
    // bypassDryDelay docs and tests/BypassTests.cpp) - it now transitions
    // within the same 4x-steady-state-slew bound as every un-exempted
    // parameter (measured ~0.011, against the general bound's ~0.06), so the
    // exemption is gone rather than merely tightened.
    float knownStepAllowance (const juce::String& id)
    {
        if (id == "gateEnabled")  return 0.25f;
        if (id == "splitLowHz")   return 0.10f;

        return 0.0f;
    }

    std::vector<ParameterUnderTest> everyParameter (CryptaAudioProcessor& processor)
    {
        std::vector<ParameterUnderTest> parameters;

        for (auto* parameter : processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                parameters.push_back ({ ranged->paramID, ranged });

        return parameters;
    }
}

//==============================================================================
TEST_CASE ("Parameter sweep: every parameter at both extremes renders finite and bounded",
           "[robustness][automation][sweep]")
{
    CryptaAudioProcessor probe;
    const auto parameters = everyParameter (probe);

    // If this ever drops, a parameter stopped being a RangedAudioParameter and
    // silently fell out of the sweep - which is exactly the regression that
    // would make this test quietly stop testing anything.
    REQUIRE (parameters.size() >= 50);

    for (const auto& entry : parameters)
    {
        for (const auto normalised : { 0.0f, 1.0f })
        {
            INFO ("parameter: " << entry.id << " at normalised " << normalised);

            const auto render = renderSweep ([&] (CryptaAudioProcessor& processor)
            {
                auto* parameter = processor.apvts.getParameter (entry.id);
                REQUIRE (parameter != nullptr);
                parameter->setValueNotifyingHost (normalised);
            });

            CHECK (render.finite);
            INFO ("peak: " << render.peak << ", ceiling: " << ceilingFor (entry));
            CHECK (render.peak < ceilingFor (entry));
        }
    }
}

TEST_CASE ("Parameter sweep: moving any parameter to an extreme mid-render does not click",
           "[robustness][automation][sweep]")
{
    // The discontinuity half. Each parameter is snapped from its default
    // straight to an endpoint, in one block, in the middle of a running render -
    // the worst case a host's automation can produce - and the resulting
    // sample-to-sample step is compared against what the two endpoints produce
    // on their own.
    CryptaAudioProcessor probe;
    const auto parameters = everyParameter (probe);
    REQUIRE (parameters.size() >= 50);

    constexpr int transitionBlock = sweepBlocks / 2;

    // Generous, because it has to hold for a hard-clipping drive stage whose
    // own steady-state edges are already near-vertical: the transition may not
    // be more than four times as steep as the steeper of its two endpoints.
    // A real zipper or a discontinuous coefficient swap is orders of magnitude
    // worse than that, not four times.
    constexpr float allowedStepFactor = 4.0f;

    for (const auto& entry : parameters)
    {
        for (const auto normalised : { 0.0f, 1.0f })
        {
            INFO ("parameter: " << entry.id << " snapped to normalised " << normalised);

            const auto setEndpoint = [&] (CryptaAudioProcessor& processor)
            {
                auto* parameter = processor.apvts.getParameter (entry.id);
                REQUIRE (parameter != nullptr);
                parameter->setValueNotifyingHost (normalised);
            };

            const auto atDefault = renderSweep ({});
            const auto atEndpoint = renderSweep (setEndpoint);
            const auto transitioning = renderSweep ({}, setEndpoint, transitionBlock);

            CHECK (transitioning.finite);
            CHECK (transitioning.peak < ceilingFor (entry));

            const auto steadyStateStep = juce::jmax (atDefault.maxStep, atEndpoint.maxStep);
            const auto allowed = juce::jmax (allowedStepFactor * steadyStateStep + 1.0e-4f,
                                              knownStepAllowance (entry.id));

            INFO ("max step while transitioning: " << transitioning.maxStep
                  << ", steady-state endpoints: " << atDefault.maxStep << " / " << atEndpoint.maxStep
                  << ", allowed: " << allowed);

            CHECK (transitioning.maxStep <= allowed);
        }
    }
}
