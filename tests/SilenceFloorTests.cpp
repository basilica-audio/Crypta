#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "params/ParameterIds.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

// Silence in -> silence out (issue #34, the v1.0.0 measurement gate).
//
// The suite already proves that reset() flushes the chain (tests/ResetTests.cpp)
// and that no denormal survives a decay to silence (tests/RobustnessTests.cpp,
// "[denormals]"). Neither of those answers the question a user actually asks of
// a bass plugin sitting on an idle track: *does it put anything into my mix
// when I am not playing?* Two things can, and neither shows up as a NaN or as
// a denormal:
//
//   DC OFFSET     any stage that adds a constant ahead of a nonlinearity leaves
//                 a constant behind it. Crypta has exactly such a stage - the
//                 Circuit high band's `highBias`, documented in
//                 src/params/ParameterLayout.cpp as "adds a DC offset ahead of
//                 the shaper (removed again by a 10 Hz blocker)". Whether the
//                 blocker actually removes it in the steady state is a
//                 measurement, not a comment.
//   NOISE FLOOR   anything self-oscillating, or any envelope/allpass state that
//                 settles on a non-zero fixed point rather than on zero.
//
// THRESHOLD, AND WHERE IT COMES FROM
// ----------------------------------
// The bound below is NOT "quiet enough to be inaudible" - that would be a taste
// judgement dressed up as a number. It is the point below which the artefact
// cannot survive being written to a delivery format at all:
//
//     24-bit PCM quantisation step = 2^-23 = 1.1921e-07 full-scale units
//     symmetric rounding -> the largest value that always quantises to zero
//     is half a step = 2^-24 = 5.9605e-08  ==  -144.49 dBFS
//
// Anything at or below 2^-24 is *provably* absent from a 24-bit render: it can
// never set a bit. That is a stronger and more defensible statement than any
// audibility argument, and it does not move with the listener, the monitoring
// chain or the room. `silenceFloor` below is that number, used unmodified.
//
// It is also a bound this plugin can genuinely be held to, because a chain fed
// exact zeros through juce::ScopedNoDenormals (FTZ|DAZ on Intel, FZ on ARM)
// has no mechanism to produce anything except an added constant: multiplying
// and accumulating zeros gives zero, and any decaying state is flushed to
// exactly zero once it passes the smallest normal. The measured results are in
// the INFO lines - most configurations come back at exactly 0.0f.
namespace
{
    constexpr double silenceSampleRate = 48000.0;
    constexpr int silenceBlockSize = 128;

    // 2^-24: half of a 24-bit LSB. See the derivation above.
    constexpr float silenceFloor = 5.9604645e-08f;

    // The bound for the OTHER bias regime: `highBias` arriving at an already
    // prepared instance, where cryp::CircuitDrive ramps the offset across one
    // block instead of snapping it (a knob move must not click). During that
    // ramp, ADAA-1 returns the average of the voicing curve f over each
    // one-sample input segment - f at the segment midpoint, to second order -
    // while the static-image subtraction at the call sites (issue #34 item 4)
    // removes f at the segment ENDPOINT. Per sample that mismatch is at most
    //
    //     Lmax * delta / 2
    //
    // with Lmax <= 1 the largest slope of any voicing curve (hard clip:
    // exactly 1; the Yeh tanh-fit: 1 at the origin; the normalised Wool diode
    // solve: < 1) and delta the per-sample offset step: maxBiasOffset (0.15,
    // src/dsp/CircuitDrive.cpp) over one block at the oversampled rate
    // (128 * 4 = 512 samples at 48 kHz) -> 0.15 / (2 * 512) = 1.465e-4. The
    // largest small-signal gain downstream is the Razor character peak's
    // +5 dB (1.778x); everything else in the path - de-emphasis, the tracked
    // and tone lowpasses, the DC blocker, the blend, a 0 dB level trim - has
    // gain <= 1. A factor of 4 covers filter transient overshoot and the
    // downsampler's ripple: margin on a derivation, not a widening to fit a
    // measurement.
    //
    //     0.15 / (2 * 512) * 1.778 * 4 = 1.042e-3   (~ -59.6 dBFS)
    //
    // (Measured on this build: 1.46e-4 on Gnaw, i.e. Lmax * delta / 2 almost
    // exactly, with the 0 dB Gnaw character filter contributing nothing.)
    constexpr float biasRampResidualBound = 0.15f / (2.0f * 512.0f) * 1.778f * 4.0f;

    struct SilenceMeasurement
    {
        float peak = 0.0f;         // largest |sample| in the measured window
        double dc = 0.0;           // mean sample value in the measured window
        double rms = 0.0;          // RMS in the measured window
        float transientPeak = 0.0f; // largest |sample| across the WHOLE run
        int denormals = 0;         // samples in the denormal range, whole run
    };

    // Feeds `seconds` of digital silence to an already-configured processor and
    // measures the last `measureSeconds` of it. Splitting the run that way is
    // deliberate: a stage with a settling transient (the bias blocker's 10 Hz
    // one-pole has a 15.9 ms time constant, so ~0.1 s to settle to -40 dB and
    // ~0.4 s to reach the 24-bit floor) is allowed to settle, and the transient
    // it produced on the way is reported separately rather than being averaged
    // away.
    SilenceMeasurement measureSilence (CryptaAudioProcessor& processor,
                                        double seconds = 5.0,
                                        double measureSeconds = 1.0)
    {
        juce::AudioBuffer<float> buffer (2, silenceBlockSize);
        juce::MidiBuffer midi;

        const auto totalBlocks = static_cast<int> (seconds * silenceSampleRate / silenceBlockSize);
        const auto measureFromBlock = static_cast<int> ((seconds - measureSeconds) * silenceSampleRate / silenceBlockSize);

        constexpr float smallestNormal = std::numeric_limits<float>::min();

        SilenceMeasurement result;
        double sum = 0.0;
        double sumOfSquares = 0.0;
        juce::int64 counted = 0;

        for (int block = 0; block < totalBlocks; ++block)
        {
            buffer.clear();
            processor.processBlock (buffer, midi);

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            {
                const auto* data = buffer.getReadPointer (channel);

                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                {
                    const auto value = data[sample];
                    const auto magnitude = std::abs (value);

                    result.transientPeak = juce::jmax (result.transientPeak, magnitude);

                    if (magnitude > 0.0f && magnitude < smallestNormal)
                        ++result.denormals;

                    if (block >= measureFromBlock)
                    {
                        result.peak = juce::jmax (result.peak, magnitude);
                        sum += static_cast<double> (value);
                        sumOfSquares += static_cast<double> (value) * static_cast<double> (value);
                        ++counted;
                    }
                }
            }
        }

        if (counted > 0)
        {
            result.dc = sum / static_cast<double> (counted);
            result.rms = std::sqrt (sumOfSquares / static_cast<double> (counted));
        }

        return result;
    }

    // A fresh, prepared processor with no history at all.
    void prepareFresh (CryptaAudioProcessor& processor)
    {
        processor.setPlayConfigDetails (2, 2, silenceSampleRate, silenceBlockSize);
        processor.prepareToPlay (silenceSampleRate, silenceBlockSize);
    }

    // Drives real energy through every stage so that filters, envelopes,
    // oversampling histories and the convolution tail all carry state before
    // the silence starts. Without this, "silence out" is trivially true.
    void excite (CryptaAudioProcessor& processor, double seconds = 1.0)
    {
        juce::AudioBuffer<float> buffer (2, silenceBlockSize);
        juce::MidiBuffer midi;

        const auto blocks = static_cast<int> (seconds * silenceSampleRate / silenceBlockSize);

        for (int block = 0; block < blocks; ++block)
        {
            TestHelpers::fillWithSine (buffer, silenceSampleRate, 82.41, 0.7f,
                                        static_cast<juce::int64> (block) * silenceBlockSize);
            processor.processBlock (buffer, midi);
        }
    }

    // Everything the plugin can switch on, at settings a user would plausibly
    // pick, with every level control left where it is - the point is to engage
    // stages, not to add gain that would scale the floor being measured.
    void engageEverything (CryptaAudioProcessor& processor)
    {
        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::gateMode, 1.0f);      // Modern
        TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqLowShelfGain, 6.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqPeak1Gain, -6.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqHighShelfGain, 6.0f);
        TestHelpers::setParameter (processor, ParamIDs::outputClip, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowGrowl, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowGrowlAmount, 70.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowCompAutoMakeup, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::midDrive, 60.0f);
        TestHelpers::setParameter (processor, ParamIDs::highDrive, 70.0f);
    }
}

//==============================================================================
TEST_CASE ("Silence: a fresh instance fed digital silence emits nothing at all", "[silence][robustness]")
{
    // The baseline. No history, no input: any output can only have been
    // manufactured out of nothing.
    for (const auto engineIndex : { 0, 1 }) // Classic, Circuit
    {
        INFO ("drive engine " << engineIndex);

        CryptaAudioProcessor processor;
        prepareFresh (processor);
        TestHelpers::setParameter (processor, ParamIDs::driveEngine, static_cast<float> (engineIndex));
        processor.reset();

        const auto measured = measureSilence (processor);

        INFO ("steady peak " << measured.peak
              << ", DC " << measured.dc
              << ", RMS " << measured.rms
              << ", worst transient " << measured.transientPeak
              << ", denormal samples " << measured.denormals);

        CHECK (measured.peak <= silenceFloor);
        CHECK (std::abs (measured.dc) <= silenceFloor);
        CHECK (measured.transientPeak <= silenceFloor);
        CHECK (measured.denormals == 0);
    }
}

TEST_CASE ("Silence: every stage engaged, fed digital silence, still emits nothing", "[silence][robustness]")
{
    for (const auto engineIndex : { 0, 1 })
    {
        INFO ("drive engine " << engineIndex);

        CryptaAudioProcessor processor;
        prepareFresh (processor);
        TestHelpers::setParameter (processor, ParamIDs::driveEngine, static_cast<float> (engineIndex));
        engageEverything (processor);
        processor.reset();

        const auto measured = measureSilence (processor);

        INFO ("steady peak " << measured.peak
              << ", DC " << measured.dc
              << ", RMS " << measured.rms
              << ", worst transient " << measured.transientPeak
              << ", denormal samples " << measured.denormals);

        CHECK (measured.peak <= silenceFloor);
        CHECK (std::abs (measured.dc) <= silenceFloor);
        CHECK (measured.denormals == 0);
    }
}

TEST_CASE ("Silence: High Bias adds DC ahead of the shaper and the blocker takes it back out", "[silence][robustness][circuit]")
{
    // The one stage in the plugin that is documented to introduce DC on
    // purpose. `highBias` at 100 % is the worst case; the 10 Hz blocker behind
    // it is what has to earn its place. If the blocker were missing or
    // mis-specified, THIS is the test that fails - the others would not, because
    // highBias defaults to 0 %.
    //
    // Measured over the last second of a 5 s silent run, i.e. ~62 time constants
    // after the offset was first applied, so nothing here is a settling
    // artefact.
    CryptaAudioProcessor processor;
    prepareFresh (processor);
    TestHelpers::setParameter (processor, ParamIDs::driveEngine, 1.0f); // Circuit
    TestHelpers::setParameter (processor, ParamIDs::highBias, 100.0f);
    TestHelpers::setParameter (processor, ParamIDs::highDrive, 80.0f);
    processor.reset();

    const auto measured = measureSilence (processor);

    INFO ("steady peak " << measured.peak
          << ", DC " << measured.dc
          << ", worst transient " << measured.transientPeak);

    CHECK (measured.peak <= silenceFloor);
    CHECK (std::abs (measured.dc) <= silenceFloor);

    // The settling transient is gated too, since issue #34 item 4 - before
    // the bias image was subtracted at its creation (src/dsp/CircuitDrive.cpp,
    // the Gnaw/Razor call sites), this measured 0.137 peak: a -17 dBFS thump
    // while the 10 Hz blocker settled. The bound here is the RAMP residual,
    // not the silence floor, because this test deliberately turns the bias up
    // AFTER prepareToPlay() - a knob gesture, which the engine answers with a
    // one-block offset ramp rather than a snap. What is left inside that ramp
    // is the ADAA midpoint-vs-endpoint mismatch derived at
    // `biasRampResidualBound`; measured 1.46e-4 against the 0.137 defect.
    CHECK (measured.transientPeak <= biasRampResidualBound);
}

TEST_CASE ("Silence: a state with High Bias at 100 % restores without a DC thump",
           "[silence][robustness][circuit]")
{
    // Issue #34 item 4, the regression test. QA measured a fresh instance
    // whose SAVED STATE already had High Bias at 100 % emitting a 0.137-peak
    // (-17 dBFS) DC transient into silence while the 10 Hz blocker settled -
    // i.e. every session load and preset recall of such a state opened with a
    // thump. The fix is structural rather than a wider bound: the bias
    // offset's own image through the shaper is subtracted at the point of
    // creation (the construction Wool and cryp::LowGrowl already used), and
    // CircuitDrive::reset() primes each shaper's one-sample ADAA history at
    // the bias operating point, so a fresh render starts AT its quiescent
    // point and the blocker has nothing to settle.
    //
    // Every voicing is exercised: Gnaw and Razor are the two whose call sites
    // gained the subtraction, Wool is the one that always had it and must not
    // have been disturbed.
    const auto donorState = [] (float voicingIndex)
    {
        CryptaAudioProcessor donor;
        prepareFresh (donor);
        TestHelpers::setParameter (donor, ParamIDs::driveEngine, 1.0f); // Circuit
        TestHelpers::setParameter (donor, ParamIDs::highVoicing, voicingIndex);
        TestHelpers::setParameter (donor, ParamIDs::highBias, 100.0f);
        TestHelpers::setParameter (donor, ParamIDs::highDrive, 80.0f);

        juce::MemoryBlock state;
        donor.getStateInformation (state);
        return state;
    };

    SECTION ("session load: restore, then prepare, then silence")
    {
        // The host order on session load: setStateInformation() before
        // prepareToPlay(). The bound is the file's own silence floor,
        // INCLUDING the settling transient - which is exactly where the
        // 0.137 peak lived before the fix, six orders of magnitude above
        // this line.
        for (const auto voicingIndex : { 0.0f, 1.0f, 2.0f })
        {
            INFO ("voicing index " << voicingIndex);
            const auto state = donorState (voicingIndex);

            CryptaAudioProcessor processor;
            processor.setPlayConfigDetails (2, 2, silenceSampleRate, silenceBlockSize);
            processor.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
            processor.prepareToPlay (silenceSampleRate, silenceBlockSize);

            const auto measured = measureSilence (processor);

            INFO ("worst transient " << measured.transientPeak
                  << ", steady peak " << measured.peak << ", DC " << measured.dc);

            CHECK (measured.transientPeak <= silenceFloor);
            CHECK (measured.peak <= silenceFloor);
            CHECK (std::abs (measured.dc) <= silenceFloor);
        }
    }

    SECTION ("mid-playback restore during a silent stretch")
    {
        // The preset-recall shape: the instance is already running (High Bias
        // at its 0 % default) when the max-bias state arrives. highBiasOffset
        // then RAMPS across one block rather than being snapped, so the
        // exact-cancellation argument above does not apply sample-for-sample
        // during that block. The bound is the ramp residual derived at
        // `biasRampResidualBound` (~ -59.6 dBFS); the defect this guards
        // against measured 0.137, two orders of magnitude above it.
        for (const auto voicingIndex : { 0.0f, 1.0f, 2.0f })
        {
            INFO ("voicing index " << voicingIndex);
            const auto state = donorState (voicingIndex);

            CryptaAudioProcessor processor;
            prepareFresh (processor);

            juce::AudioBuffer<float> buffer (2, silenceBlockSize);
            juce::MidiBuffer midi;

            const auto blocksPerSecond = static_cast<int> (silenceSampleRate) / silenceBlockSize;
            float worstAfterRestore = 0.0f;

            for (int block = 0; block < 3 * blocksPerSecond; ++block)
            {
                if (block == blocksPerSecond)
                    processor.setStateInformation (state.getData(), static_cast<int> (state.getSize()));

                buffer.clear();
                processor.processBlock (buffer, midi);

                if (block >= blocksPerSecond)
                    worstAfterRestore = juce::jmax (worstAfterRestore,
                                                     buffer.getMagnitude (0, silenceBlockSize));
            }

            INFO ("worst |sample| from the restore onwards: " << worstAfterRestore
                  << " against the derived bound " << biasRampResidualBound);
            CHECK (worstAfterRestore <= biasRampResidualBound);
        }
    }
}

TEST_CASE ("Silence: the tail after a loud passage decays below the 24-bit floor and stops there", "[silence][robustness]")
{
    // The realistic case: the player stops. Nothing calls reset() - a host only
    // does that on a transport event - so every filter, envelope, delay line and
    // convolution tail in the chain rings down on its own. This asserts that the
    // ring-down actually reaches zero rather than settling on a small non-zero
    // fixed point, and that it does so within a musically ordinary gap.
    //
    // 5 s of silence is measured; the last 1 s is the assertion window. The
    // longest state in the chain is the convolution tail (85.3 ms for every
    // bundled IR, resources/irs/LICENSES.md) and the low-band compensation
    // delay (61 samples), so 4 s of settling is two orders of magnitude more
    // than the chain's own longest time constant.
    for (const auto engineIndex : { 0, 1 })
    {
        INFO ("drive engine " << engineIndex);

        CryptaAudioProcessor processor;
        prepareFresh (processor);
        TestHelpers::setParameter (processor, ParamIDs::driveEngine, static_cast<float> (engineIndex));
        engageEverything (processor);
        // The gate off for this one: a gate that closes would mute the tail
        // being measured and turn a real ring-down failure into a pass.
        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
        REQUIRE (processor.loadFactoryImpulseResponse (0));
        TestHelpers::setParameter (processor, ParamIDs::irEnabled, 1.0f);
        processor.reset();

        excite (processor);

        const auto measured = measureSilence (processor);

        INFO ("steady peak " << measured.peak
              << ", DC " << measured.dc
              << ", RMS " << measured.rms
              << ", denormal samples " << measured.denormals);

        CHECK (measured.peak <= silenceFloor);
        CHECK (std::abs (measured.dc) <= silenceFloor);
        CHECK (measured.denormals == 0);
    }
}
