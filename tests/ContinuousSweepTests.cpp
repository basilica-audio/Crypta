#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "params/ParameterIds.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <vector>

// Continuous full-range parameter sweeps (issue #34, the v1.0.0 measurement
// gate): *sweep every parameter across its full range at a fast rate and assert
// no sample-to-sample discontinuity above a derived threshold. Smoothing that
// is missing will show up here.*
//
// WHY THIS IS NOT tests/ParameterSweepTests.cpp
// ---------------------------------------------
// That file snaps a parameter from its default straight to an endpoint, once,
// in one block. That is the right test for a *jump* - a preset recall, or a host
// writing a single automation point - and it is what caught the unsmoothed
// bypass in #87.
//
// It is the wrong test for a *gesture*. A user turning a knob, or a host
// replaying a drawn automation curve, produces a new value on every single
// block. If a parameter is unsmoothed, one snap produces one step, which the
// endpoint test sees; a 200-block gesture produces 200 steps, one per block
// boundary, at a 750 Hz repetition rate - and that is not a click, it is a tone.
// Zipper noise is precisely this artefact, and a test that only ever moves a
// parameter once cannot produce it. This file moves every parameter the way a
// hand does.
//
// THE THRESHOLD, AND WHERE IT COMES FROM
// --------------------------------------
// Comparing against an absolute slew limit is useless here for the reason
// ParameterSweepTests.cpp already documents: a hard-clipping drive stage's own
// steady-state waveform contains near-vertical edges, so any absolute number is
// either unreachable or toothless.
//
// The construction used instead is: **the sweep may not produce anything the
// settings it passes through do not already produce.** For each parameter, the
// same signal is rendered at nine STATIC points across its normalised range
// (0, 0.125, ... 1.0), and the largest sample-to-sample step of any of those
// nine renders is the reference. The sweep - the same range traversed
// continuously, one new value per 64-sample block, the whole range in 267 ms -
// must not exceed 1.5x that reference.
//
// The 1.5 comes from two identified sources of legitimate excess, and nothing
// else:
//
//   GRID COARSENESS   the steepest waveform in the range may sit between two of
//                     the nine grid points. Nine points over a normalised range
//                     bound the interpolation error of any smooth
//                     slew-vs-parameter curve to well inside a factor of 1.5;
//                     the grid is deliberately uniform in NORMALISED space, so
//                     it is uniform in whatever perceptual mapping the
//                     parameter's own skew declares.
//   BLOCK INCREMENT   the parameter genuinely moves by 1/200 of its range at
//                     each block boundary. For a smoothed parameter that
//                     increment is ramped across the block and contributes a
//                     slope, not a step; the factor leaves room for it.
//
// What the bound will NOT absorb is missing smoothing. An unsmoothed parameter
// applied as a step at each block boundary puts a discontinuity into the SIGNAL
// - its size is the whole per-block increment times the parameter's sensitivity,
// not a fraction of the waveform's own slope - and it exceeds this bound by
// orders of magnitude, not by 50 %. That asymmetry is what makes 1.5 a bound
// rather than a fitted number.
namespace
{
    constexpr double gestureSampleRate = 48000.0;
    constexpr int gestureBlockSize = 64;     // 1.33 ms - a small, realistic host block
    constexpr int gestureWarmUpBlocks = 50;  // 67 ms of settling, discarded
    constexpr int gestureBlocks = 200;       // full range in 267 ms: a fast knob throw

    // 110 Hz, the open A. Same choice, and the same reasoning, as
    // tests/ParameterSweepTests.cpp: low enough that the input's own
    // sample-to-sample slew is negligible next to any artefact, and squarely
    // inside the low band so the crossover, compressor and growl branch are all
    // live.
    constexpr double gestureFrequencyHz = 110.0;
    constexpr float gestureAmplitude = 0.5f;

    // Same derivation as tests/ParameterSweepTests.cpp: the safety clipper's
    // ceiling plus transient headroom for a non-gain parameter, and the
    // parameter's own advertised maximum gain for a dB-labelled one.
    constexpr float gestureBoundedCeiling = 4.0f;

    float ceilingFor (const juce::RangedAudioParameter& parameter)
    {
        if (parameter.getLabel() != "dB")
            return gestureBoundedCeiling;

        const auto maximumDb = parameter.getNormalisableRange().end;

        if (maximumDb <= 0.0f)
            return gestureBoundedCeiling;

        return gestureBoundedCeiling * juce::Decibels::decibelsToGain (maximumDb);
    }

    struct GestureRender
    {
        float peak = 0.0f;
        float maxStep = 0.0f;
        bool finite = true;
    };

    // Renders `gestureWarmUpBlocks` + `gestureBlocks` blocks of a steady sine,
    // calling `setNormalised(t)` before every block of the measured region with
    // t running 0 -> 1 (or holding a fixed value for a static reference render).
    // Only the measured region is analysed, so filter settling after the initial
    // parameter placement never counts as a step.
    GestureRender render (const std::function<void (CryptaAudioProcessor&, float)>& setNormalised,
                          float startValue,
                          bool sweeping)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, gestureSampleRate, gestureBlockSize);
        processor.prepareToPlay (gestureSampleRate, gestureBlockSize);

        setNormalised (processor, startValue);
        processor.reset();

        juce::AudioBuffer<float> block (2, gestureBlockSize);
        juce::MidiBuffer midi;

        GestureRender result;
        float previous [2] = { 0.0f, 0.0f };
        bool havePrevious = false;

        const auto totalBlocks = gestureWarmUpBlocks + gestureBlocks;

        for (int index = 0; index < totalBlocks; ++index)
        {
            const auto measuring = index >= gestureWarmUpBlocks;

            if (measuring && sweeping)
            {
                const auto position = static_cast<float> (index - gestureWarmUpBlocks)
                                        / static_cast<float> (gestureBlocks - 1);
                setNormalised (processor, position);
            }

            TestHelpers::fillWithSine (block, gestureSampleRate, gestureFrequencyHz, gestureAmplitude,
                                        static_cast<juce::int64> (index) * gestureBlockSize);
            processor.processBlock (block, midi);

            if (! measuring)
                continue;

            for (int channel = 0; channel < block.getNumChannels() && channel < 2; ++channel)
            {
                const auto* data = block.getReadPointer (channel);

                for (int sample = 0; sample < block.getNumSamples(); ++sample)
                {
                    const auto value = data[sample];

                    if (! std::isfinite (value))
                        result.finite = false;

                    result.peak = juce::jmax (result.peak, std::abs (value));

                    // The step across a BLOCK boundary matters as much as the
                    // step inside one - that is exactly where an unsmoothed
                    // parameter update lands - so the previous sample is
                    // carried between blocks rather than reset per block.
                    if (havePrevious || sample > 0)
                    {
                        const auto reference = sample > 0 ? data[sample - 1] : previous[channel];
                        result.maxStep = juce::jmax (result.maxStep, std::abs (value - reference));
                    }
                }

                previous[channel] = data[block.getNumSamples() - 1];
            }

            havePrevious = true;
        }

        return result;
    }

    struct Entry
    {
        juce::String id;
        juce::RangedAudioParameter* parameter = nullptr;
        bool discrete = false;
    };

    std::vector<Entry> everyParameter (CryptaAudioProcessor& processor)
    {
        std::vector<Entry> entries;

        for (auto* parameter : processor.getParameters())
        {
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
            {
                const auto isChoice = dynamic_cast<juce::AudioParameterChoice*> (parameter) != nullptr;
                const auto isBool = dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr;

                entries.push_back ({ ranged->paramID, ranged, isChoice || isBool });
            }
        }

        return entries;
    }
}

//==============================================================================
TEST_CASE ("Continuous sweep: a full-range gesture on any continuous parameter produces nothing its own settings do not",
           "[robustness][automation][continuous-sweep]")
{
    CryptaAudioProcessor probe;
    const auto entries = everyParameter (probe);

    // Mirrors the guard in tests/ParameterSweepTests.cpp: if this drops, a
    // parameter silently stopped being swept.
    REQUIRE (entries.size() >= 50);

    // See the file header for the derivation. This is a bound on legitimate
    // excess, not a fitted tolerance.
    constexpr float allowedGestureFactor = 1.5f;

    auto swept = 0;

    for (const auto& entry : entries)
    {
        // A choice or a bool has no "full range" to traverse: every value
        // between its steps quantises to one of them, so a sweep is a sequence
        // of mode switches and its step size is a property of the modes, not of
        // smoothing. Those are covered at their endpoints, with measured
        // allowances, by tests/ParameterSweepTests.cpp; sweeping them here
        // would measure the same mode switch 200 times.
        if (entry.discrete)
            continue;

        ++swept;
        INFO ("parameter: " << entry.id);

        const auto set = [&entry] (CryptaAudioProcessor& processor, float normalised)
        {
            auto* parameter = processor.apvts.getParameter (entry.id);
            jassert (parameter != nullptr);
            parameter->setValueNotifyingHost (normalised);
        };

        // Nine static reference points across the normalised range.
        float staticWorstStep = 0.0f;
        float staticWorstPeak = 0.0f;

        for (int point = 0; point < 9; ++point)
        {
            const auto normalised = static_cast<float> (point) / 8.0f;
            const auto measured = render (set, normalised, false);

            CHECK (measured.finite);
            staticWorstStep = juce::jmax (staticWorstStep, measured.maxStep);
            staticWorstPeak = juce::jmax (staticWorstPeak, measured.peak);
        }

        const auto sweep = render (set, 0.0f, true);

        INFO ("sweep max step " << sweep.maxStep
              << ", static worst " << staticWorstStep
              << ", allowed " << (staticWorstStep * allowedGestureFactor)
              << ", sweep peak " << sweep.peak
              << ", ceiling " << ceilingFor (*entry.parameter));

        CHECK (sweep.finite);
        CHECK (sweep.peak < ceilingFor (*entry.parameter));
        CHECK (sweep.maxStep <= staticWorstStep * allowedGestureFactor);
    }

    // 54 declared parameters, of which the choices and bools are excluded
    // above. If this number collapses, the exclusion rule broke rather than the
    // parameter set shrinking.
    INFO ("continuously swept " << swept << " parameters");
    CHECK (swept >= 40);
}

TEST_CASE ("Continuous sweep: simultaneous gestures on the whole set stay finite and bounded",
           "[robustness][automation][continuous-sweep]")
{
    // The other half of "at a fast rate": every continuous parameter moving at
    // once, at a different rate each, which is what a busy automation pass
    // actually looks like and what the one-at-a-time sweep above deliberately
    // does not cover. No step assertion is possible here - there is no static
    // reference for "all 40 parameters somewhere in the middle" - so this
    // asserts the two things that remain decidable: finite, and bounded by the
    // loudest gain the parameter set can legitimately ask for.
    CryptaAudioProcessor processor;
    processor.setPlayConfigDetails (2, 2, gestureSampleRate, gestureBlockSize);
    processor.prepareToPlay (gestureSampleRate, gestureBlockSize);

    const auto entries = everyParameter (processor);
    REQUIRE (entries.size() >= 50);

    juce::AudioBuffer<float> block (2, gestureBlockSize);
    juce::MidiBuffer midi;

    // This is a DIVERGENCE bound, not a headroom claim, and the difference
    // matters. With every continuous parameter somewhere in its range at once,
    // the gain stages cascade: Input Gain (+24 dB) feeds Low Comp Makeup
    // (+24 dB) feeds the band level (+12 dB) feeds Output Gain (+24 dB), so
    // +84 dB is a level the parameter set can legitimately be asked for and
    // legitimately deliver. Against a 0.5 peak input that is
    // 0.5 * 10^(84/20) = 7924, and anything below it is the user getting what
    // they asked for rather than the plugin running away.
    //
    // A first version of this case used +24 dB - the largest gain any SINGLE
    // parameter advertises - and failed at a measured peak of 93.4 against a
    // ceiling of 63.4. That was the bound being wrong, not the plugin: four
    // gain stages in series are not one gain stage. The number below is the
    // cascade, computed rather than fitted.
    //
    // What it still catches is the thing this case exists for: an unbounded
    // output. Self-oscillation, a coefficient update that destabilises a
    // filter, or a feedback path that latches all exceed a fixed ceiling by
    // orders of magnitude within two seconds; the per-block finiteness REQUIRE
    // below catches the rest.
    const auto ceiling = 0.5f * juce::Decibels::decibelsToGain (24.0f + 24.0f + 12.0f + 24.0f);

    constexpr int busyBlocks = 1500; // 2 s of continuous automation

    float peak = 0.0f;

    for (int index = 0; index < busyBlocks; ++index)
    {
        auto rateIndex = 0;

        for (const auto& entry : entries)
        {
            if (entry.discrete)
                continue;

            // Each parameter gets its own incommensurate rate, so they never
            // line up into one synchronised gesture.
            const auto rate = 0.25 + 0.05 * static_cast<double> (rateIndex % 17);
            const auto phase = static_cast<double> (index) * rate / 40.0;
            const auto normalised = 0.5 - 0.5 * std::cos (phase);

            entry.parameter->setValueNotifyingHost (static_cast<float> (normalised));
            ++rateIndex;
        }

        TestHelpers::fillWithSine (block, gestureSampleRate, gestureFrequencyHz, gestureAmplitude,
                                    static_cast<juce::int64> (index) * gestureBlockSize);
        processor.processBlock (block, midi);

        REQUIRE (TestHelpers::allSamplesFinite (block));
        peak = juce::jmax (peak, block.getMagnitude (0, block.getNumSamples()));
    }

    INFO ("peak under simultaneous automation " << peak << ", ceiling " << ceiling);
    CHECK (peak < ceiling);
}
