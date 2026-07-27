#include "CircuitDrive.h"

#include <cmath>

namespace
{
    //==========================================================================
    // Voicing constants. Engineering-derived starting points from the circuit
    // research, NOT final voicing decisions - the suite's ear-tuning gate
    // (issues #15/#16/#17, #34) still owns the last word on all of these.

    // Gnaw ("op-amp hard clip"): unchanged 40x ceiling from v0.2.0, now with
    // a pre-emphasis shelf ahead of the clipper and its exact inverse behind
    // it. Emphasising the highs into a clipper and de-emphasising afterwards
    // concentrates the clipping on the upper harmonics - the standard
    // pre/de-emphasis trick - while the exact-inverse pairing guarantees the
    // stage collapses to unity in the clipper's linear region.
    constexpr double gnawMaxDriveGain = 40.0;
    constexpr double gnawEmphasisHz = 1200.0;
    constexpr double gnawEmphasisQ = 0.7071;
    constexpr double gnawEmphasisDb = 6.0;
    constexpr double gnawTrackedLowPassMinHz = 6000.0;

    // Wool ("cascaded fuzz"): the diode clipper's own DC curve replaces
    // v0.2.0's two cascaded tanh stages. 1N914/1N4148 SPICE card
    // (research-diode-clipper-dk.md §2.1).
    constexpr double diodeSaturationCurrent = 2.52e-9;
    constexpr double diodeIdeality = 1.75;
    constexpr double thermalVoltage = 25.85e-3;
    constexpr double diodeSeriesResistance = 10.0e3;
    constexpr double woolMaxDriveGain = 12.0;

    // Dynamic bias side chain (research-triode-adaa.md §4, Tier B item 1):
    // a level-tracking DC offset into the shaper, fast to build and slow to
    // decay, so a loud passage leaves the clipper biased for a while
    // afterwards. This is the touch-dependent bloom/sag a memoryless
    // waveshaper structurally cannot produce.
    constexpr double biasAttackMs = 0.5;
    constexpr double biasReleaseMs = 20.0; // the Cout*Rg blocking constant
    constexpr double biasThreshold = 0.2;

    // How far the envelope pushes the clipper's operating point. Kept
    // deliberately modest: the offset makes the clipping asymmetric, that
    // asymmetry produces real DC, and the 10 Hz blocker downstream then has
    // to restore it - so a large depth buys a bigger gain change at the cost
    // of a longer DC-restoration tail after every loud passage.
    constexpr double biasDepthVolts = 0.3;

    // Razor ("tight overdrive"): the TS-style feedback clipper's Tier B
    // factorization (research-diode-clipper-dk.md §4). 720 Hz is the guitar
    // pedal's own pre-emphasis corner; 330 Hz is that corner moved down for
    // the bass register, which is the whole point of this plugin.
    constexpr double razorPreEmphasisHz = 330.0;
    constexpr double razorMaxDriveGain = 8.0;
    constexpr double razorTrackedLowPassMinHz = 5700.0;

    // Drive-tracked post-LPF, shared by Gnaw and Razor. This is the feedback
    // clipper's Cc pole, fc = 1/(2*pi*R2(D)*Cc), which slides down as the
    // drive pot opens up - the "post-smoothing inside the nonlinearity"
    // behaviour a static waveshaper misses entirely.
    //
    // The open-pot end is 61 kHz, straight from research-diode-clipper-dk.md
    // §2.3, i.e. genuinely above audibility. The brief's §3.1 sketches 24 kHz
    // instead, but a one-pole at 24 kHz is already -1.9 dB at 18 kHz, which
    // cannot satisfy the brief's OWN transparency assertion in T3 (high-band
    // response within +/-0.5 dB of the same measurement with this filter
    // bypassed). 61 kHz is both the faithful figure and the one that meets
    // the stated contract: -0.36 dB at 18 kHz.
    constexpr double trackedLowPassMaxHz = 61000.0;

    // R2(D) uses a square-law taper rather than the datasheet's linear
    // 51k + D*500k. Real drive pots are audio-taper, and the square law is
    // what keeps the pole above 12 kHz at half drive (T3) instead of
    // collapsing to ~9 kHz, which would make the mid-drive range audibly
    // duller than the circuit it is modelling.
    double trackedLowPassHz (double drive01, double minHz, double maxOversampledHz) noexcept
    {
        const auto span = trackedLowPassMaxHz / minHz - 1.0;
        const auto frequency = trackedLowPassMaxHz / (1.0 + span * drive01 * drive01);
        return juce::jmin (frequency, maxOversampledHz);
    }

    // Voicing-specific character filter, carried over unchanged from v0.2.0
    // so the Circuit engine keeps each voicing's recognisable placement.
    constexpr double gnawCharacterHz = 1000.0, gnawCharacterDb = 0.0, gnawCharacterQ = 0.7;
    constexpr double woolCharacterHz = 500.0, woolCharacterDb = -6.0, woolCharacterQ = 0.9;
    constexpr double razorCharacterHz = 900.0, razorCharacterDb = 5.0, razorCharacterQ = 1.0;

    constexpr double tightHighPassQ = 0.7071;
    constexpr double minToneHz = 700.0;
    constexpr double maxToneHz = 15000.0;

    // highBias maps to a DC offset into the High clipper - the Wool
    // asymmetry constant from v0.2.0, generalised into a continuous control.
    constexpr double maxBiasOffset = 0.15;
    constexpr double dcBlockerHz = 10.0;

    // Mid band: one ADAA tanh core replaces v0.2.0's two cascaded tanh
    // stages. The ceiling is the product of the old pair (8 * 4), so the
    // available drive range is comparable.
    constexpr double midMaxDriveGain = 32.0;

    // Table range for the tabulated curves: far enough out that the curve is
    // flat at the edge, which is what ShaperTable's out-of-range handling
    // assumes.
    constexpr double woolTableRange = 24.0;
    constexpr double razorTableRange = 16.0;

    //==========================================================================
    // Asymmetric diode pair current (research-diode-clipper-dk.md §2.1: two
    // series diodes forward, one reverse - the SD-1 arrangement, which is
    // what gives Wool its even-order content).
    double diodeCurrent (double v) noexcept
    {
        const auto forward = diodeSaturationCurrent
                              * (std::exp (juce::jmin (v / (2.0 * diodeIdeality * thermalVoltage), 60.0)) - 1.0);
        const auto reverse = diodeSaturationCurrent
                              * (std::exp (juce::jmin (-v / (diodeIdeality * thermalVoltage), 60.0)) - 1.0);
        return forward - reverse;
    }

    double diodeCurrentDerivative (double v) noexcept
    {
        const auto forwardScale = 1.0 / (2.0 * diodeIdeality * thermalVoltage);
        const auto reverseScale = 1.0 / (diodeIdeality * thermalVoltage);

        const auto forward = diodeSaturationCurrent * forwardScale
                              * std::exp (juce::jmin (v * forwardScale, 60.0));
        const auto reverse = diodeSaturationCurrent * reverseScale
                              * std::exp (juce::jmin (-v * reverseScale, 60.0));
        return forward + reverse;
    }

    // Solves the shunt clipper's DC operating point (P - y)/Req = iD(y) for y
    // by damped Newton. Called only while building the table, never on the
    // audio thread.
    double solveDiodeClipper (double p) noexcept
    {
        auto y = juce::jlimit (-1.0, 1.0, p);

        for (int iteration = 0; iteration < 200; ++iteration)
        {
            const auto residual = (p - y) / diodeSeriesResistance - diodeCurrent (y);
            const auto derivative = -1.0 / diodeSeriesResistance - diodeCurrentDerivative (y);
            const auto step = residual / derivative;

            // Damping keeps the exponential from throwing the iterate into a
            // region where exp() saturates and the derivative vanishes.
            y -= juce::jlimit (-0.1, 0.1, step);

            if (std::abs (step) < 1.0e-12)
                break;
        }

        return y;
    }

    // Yeh eq. 19's tanh-fit: a softer knee than tanh with a slower approach
    // to the rail, which is what the measured TS transfer curve looks like.
    double yehTanhFit (double x) noexcept
    {
        constexpr double exponent = 2.5;
        return x / std::pow (1.0 + std::pow (std::abs (x), exponent), 1.0 / exponent);
    }
}

namespace cryp
{
    //==========================================================================
    CircuitBiquad CircuitBiquad::makeHighPass (double sampleRate, double frequencyHz, double q) noexcept
    {
        const auto w0 = juce::MathConstants<double>::twoPi * juce::jlimit (1.0, sampleRate * 0.45, frequencyHz) / sampleRate;
        const auto cosW0 = std::cos (w0);
        const auto alpha = std::sin (w0) / (2.0 * q);

        const auto a0 = 1.0 + alpha;

        CircuitBiquad filter;
        filter.b0 = ((1.0 + cosW0) * 0.5) / a0;
        filter.b1 = (-(1.0 + cosW0)) / a0;
        filter.b2 = ((1.0 + cosW0) * 0.5) / a0;
        filter.a1 = (-2.0 * cosW0) / a0;
        filter.a2 = (1.0 - alpha) / a0;
        return filter;
    }

    CircuitBiquad CircuitBiquad::makePeak (double sampleRate, double frequencyHz, double q, double gainDb) noexcept
    {
        const auto a = std::pow (10.0, gainDb / 40.0);
        const auto w0 = juce::MathConstants<double>::twoPi * juce::jlimit (1.0, sampleRate * 0.45, frequencyHz) / sampleRate;
        const auto cosW0 = std::cos (w0);
        const auto alpha = std::sin (w0) / (2.0 * q);

        const auto a0 = 1.0 + alpha / a;

        CircuitBiquad filter;
        filter.b0 = (1.0 + alpha * a) / a0;
        filter.b1 = (-2.0 * cosW0) / a0;
        filter.b2 = (1.0 - alpha * a) / a0;
        filter.a1 = (-2.0 * cosW0) / a0;
        filter.a2 = (1.0 - alpha / a) / a0;
        return filter;
    }

    CircuitBiquad CircuitBiquad::makeHighShelf (double sampleRate, double frequencyHz, double q, double gainDb) noexcept
    {
        const auto a = std::pow (10.0, gainDb / 40.0);
        const auto w0 = juce::MathConstants<double>::twoPi * juce::jlimit (1.0, sampleRate * 0.45, frequencyHz) / sampleRate;
        const auto cosW0 = std::cos (w0);
        const auto alpha = std::sin (w0) / (2.0 * q);
        const auto twoSqrtAAlpha = 2.0 * std::sqrt (a) * alpha;

        const auto a0 = (a + 1.0) - (a - 1.0) * cosW0 + twoSqrtAAlpha;

        CircuitBiquad filter;
        filter.b0 = (a * ((a + 1.0) + (a - 1.0) * cosW0 + twoSqrtAAlpha)) / a0;
        filter.b1 = (-2.0 * a * ((a - 1.0) + (a + 1.0) * cosW0)) / a0;
        filter.b2 = (a * ((a + 1.0) + (a - 1.0) * cosW0 - twoSqrtAAlpha)) / a0;
        filter.a1 = (2.0 * ((a - 1.0) - (a + 1.0) * cosW0)) / a0;
        filter.a2 = ((a + 1.0) - (a - 1.0) * cosW0 - twoSqrtAAlpha) / a0;
        return filter;
    }

    CircuitBiquad CircuitBiquad::makeInverse() const noexcept
    {
        CircuitBiquad inverse;

        // 1/H(z) = (1 + a1 z^-1 + a2 z^-2) / (b0 + b1 z^-1 + b2 z^-2),
        // renormalised so the leading denominator coefficient is 1 again.
        const auto scale = 1.0 / b0;

        inverse.b0 = scale;
        inverse.b1 = a1 * scale;
        inverse.b2 = a2 * scale;
        inverse.a1 = b1 * scale;
        inverse.a2 = b2 * scale;
        return inverse;
    }

    //==========================================================================
    size_t CircuitDrive::chooseFactorExponent (double sampleRate) noexcept
    {
        if (sampleRate <= 50000.0)
            return 2; // 4x

        if (sampleRate <= 100000.0)
            return 1; // 2x

        return 0; // 1x - ADAA alone, the host is already running well above
                  // the band where alias products would be audible.
    }

    void CircuitDrive::prepare (const juce::dsp::ProcessSpec& spec)
    {
        baseSampleRate = spec.sampleRate;
        numChannels = static_cast<size_t> (spec.numChannels);

        const auto factorExponent = chooseFactorExponent (spec.sampleRate);
        oversamplingFactor = 1 << factorExponent;
        oversampledRate = spec.sampleRate * static_cast<double> (oversamplingFactor);

        oversampling = std::make_unique<juce::dsp::Oversampling<float>> (
            numChannels,
            factorExponent,
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
            true,
            true);
        oversampling->initProcessing (static_cast<size_t> (spec.maximumBlockSize));
        oversampling->reset();
        latencySamples = static_cast<int> (std::lround (oversampling->getLatencyInSamples()));

        // Split #2 runs at the oversampled rate. cryp::Crossover needs no
        // change for this - it just gets a spec whose sampleRate is fs*M and
        // a block size large enough for the upsampled block.
        juce::dsp::ProcessSpec oversampledSpec;
        oversampledSpec.sampleRate = oversampledRate;
        oversampledSpec.maximumBlockSize = spec.maximumBlockSize * static_cast<juce::uint32> (oversamplingFactor);
        oversampledSpec.numChannels = spec.numChannels;

        midHighSplitOversampled.prepare (oversampledSpec);

        const auto scratchSamples = static_cast<int> (oversampledSpec.maximumBlockSize);
        midBuffer.setSize (static_cast<int> (numChannels), scratchSamples);
        highBuffer.setSize (static_cast<int> (numChannels), scratchSamples);
        highDryBuffer.setSize (static_cast<int> (numChannels), scratchSamples);

        highState.assign (numChannels, HighChannelState {});
        midState.assign (numChannels, MidChannelState {});

        // Tabulated curves. Both are built here, off the audio thread: the
        // Wool table runs a Newton solve per point, which is far too
        // expensive to do anywhere else.
        woolCurve.build ([] (double x) { return solveDiodeClipper (x); }, woolTableRange);
        razorCurve.build ([] (double x) { return yehTanhFit (x); }, razorTableRange);

        // Normalise the Wool curve so its positive rail sits at unity, giving
        // it the same output scale as every other voicing (the raw DC
        // solution is in volts and tops out well below 1).
        {
            const auto positiveRail = woolCurve.evaluate (woolTableRange);
            const auto scale = positiveRail > 1.0e-9 ? 1.0 / positiveRail : 1.0;

            if (std::abs (scale - 1.0) > 1.0e-9)
                woolCurve.build ([scale] (double x) { return solveDiodeClipper (x) * scale; }, woolTableRange);
        }

        updateCoefficients();
        reset();
    }

    void CircuitDrive::reset()
    {
        if (oversampling != nullptr)
            oversampling->reset();

        midHighSplitOversampled.reset();

        for (auto& state : highState)
            state.reset();

        for (auto& state : midState)
            state.reset();
    }

    //==========================================================================
    void CircuitDrive::updateCoefficients() noexcept
    {
        midHighSplitOversampled.setCutoffFrequency (splitHighHz);

        tightHighPassPrototype = CircuitBiquad::makeHighPass (oversampledRate, tightHz, tightHighPassQ);

        // Per-voicing pre-emphasis. Gnaw gets the shelf pair; Razor's
        // pre-emphasis is the first-order 330 Hz highpass applied to the
        // clipped path only (handled per-sample below), and Wool has none -
        // its character comes from the diode curve and the dynamic bias.
        preEmphasisPrototype = CircuitBiquad::makeHighShelf (oversampledRate, gnawEmphasisHz, gnawEmphasisQ, gnawEmphasisDb);
        deEmphasisPrototype = preEmphasisPrototype.makeInverse();

        double characterHz = gnawCharacterHz, characterDb = gnawCharacterDb, characterQ = gnawCharacterQ;
        double trackedMinHz = gnawTrackedLowPassMinHz;

        if (voicing == VoicingType::wool)
        {
            characterHz = woolCharacterHz;
            characterDb = woolCharacterDb;
            characterQ = woolCharacterQ;
            trackedMinHz = gnawTrackedLowPassMinHz;
        }
        else if (voicing == VoicingType::razor)
        {
            characterHz = razorCharacterHz;
            characterDb = razorCharacterDb;
            characterQ = razorCharacterQ;
            trackedMinHz = razorTrackedLowPassMinHz;
        }

        characterPrototype = CircuitBiquad::makePeak (oversampledRate, characterHz, characterQ, characterDb);

        CircuitOnePole scratch;

        scratch.setCutoff (oversampledRate,
                            trackedLowPassHz (highDrive01, trackedMinHz, oversampledRate * 0.45));
        trackedLowPassG.setTarget (scratch.g);

        const auto toneHz = juce::mapToLog10 (static_cast<double> (highTone01), minToneHz, maxToneHz);
        scratch.setCutoff (oversampledRate, toneHz);
        toneLowPassG.setTarget (scratch.g);

        scratch.setCutoff (oversampledRate, razorPreEmphasisHz);
        razorHighPassG = scratch.g;

        scratch.setCutoff (oversampledRate, dcBlockerHz);
        dcBlockerG = scratch.g;

        // Bias ballistics, expressed as one-pole coefficients at the
        // oversampled rate.
        const auto timeConstantG = [this] (double milliseconds)
        {
            return 1.0 - std::exp (-1.0 / (juce::jmax (1.0e-4, milliseconds) * 0.001 * oversampledRate));
        };

        biasAttackG = timeConstantG (biasAttackMs);
        biasReleaseG = timeConstantG (biasReleaseMs);

        switch (voicing)
        {
            case VoicingType::gnaw:
                highDriveGain.setTarget (1.0 + static_cast<double> (highDrive01) * (gnawMaxDriveGain - 1.0));
                break;

            case VoicingType::wool:
                highDriveGain.setTarget (1.0 + static_cast<double> (highDrive01) * (woolMaxDriveGain - 1.0));
                break;

            case VoicingType::razor:
            default:
                highDriveGain.setTarget (1.0 + static_cast<double> (highDrive01) * (razorMaxDriveGain - 1.0));
                break;
        }

        midDriveGain.setTarget (1.0 + static_cast<double> (midDrive01) * (midMaxDriveGain - 1.0));
        midDriveAmount.setTarget (static_cast<double> (midDrive01));
        highBlendAmount.setTarget (static_cast<double> (highBlend01));
        highBiasOffset.setTarget (maxBiasOffset * static_cast<double> (highBias01));

        midGainLinear.setTarget (juce::Decibels::decibelsToGain (static_cast<double> (midLevelDb)));
        highGainLinear.setTarget (juce::Decibels::decibelsToGain (static_cast<double> (highLevelDb)));

        // The very first block after prepare() has no previous value to ramp
        // from, so it starts AT the target rather than sliding up to it from
        // whatever the members happened to be constructed with.
        if (! rampsInitialised)
        {
            commitRamps();
            rampsInitialised = true;
        }
    }

    void CircuitDrive::commitRamps() noexcept
    {
        trackedLowPassG.commit();
        toneLowPassG.commit();
        highDriveGain.commit();
        midDriveGain.commit();
        midDriveAmount.commit();
        highBlendAmount.commit();
        highBiasOffset.commit();
        midGainLinear.commit();
        highGainLinear.commit();
    }

    //==========================================================================
    void CircuitDrive::processMidChannel (float* data, size_t numSamples, size_t channel) noexcept
    {
        auto& state = midState[channel];
        const TanhShaper shaper;

        const auto inverseNumSamples = numSamples > 0 ? 1.0 / static_cast<double> (numSamples) : 0.0;

        for (size_t sample = 0; sample < numSamples; ++sample)
        {
            const auto x = static_cast<double> (data[sample]);

            const auto gain = midDriveGain.at (sample, inverseNumSamples);
            const auto drive = midDriveAmount.at (sample, inverseNumSamples);
            const auto level = midGainLinear.at (sample, inverseNumSamples);

            // One ADAA tanh core in place of v0.2.0's two cascaded plain
            // tanh stages, keeping the same dry-crossfade-by-drive law so
            // "Mid Drive = 0 %" remains an exact passthrough.
            const auto driven = state.shaper.process (gain * x, shaper);

            data[sample] = static_cast<float> ((x + drive * (driven - x)) * level);
        }
    }

    void CircuitDrive::processHighChannel (float* data, const float* dry, size_t numSamples, size_t channel) noexcept
    {
        auto& state = highState[channel];

        const HardClipShaper hardClip;
        const TanhShaper tanhShaper;
        juce::ignoreUnused (tanhShaper);

        const auto inverseNumSamples = numSamples > 0 ? 1.0 / static_cast<double> (numSamples) : 0.0;

        for (size_t sample = 0; sample < numSamples; ++sample)
        {
            const auto blend = highBlendAmount.at (sample, inverseNumSamples);
            const auto biasOffset = highBiasOffset.at (sample, inverseNumSamples);
            const auto highDriveGainNow = highDriveGain.at (sample, inverseNumSamples);

            // The drive-tracked and tone lowpass coefficients are ramped too:
            // both move with automatable controls, and a one-pole whose
            // coefficient jumps once per block is as audible as a gain that
            // does.
            state.trackedLowPass.g = trackedLowPassG.at (sample, inverseNumSamples);
            state.toneLowPass.g = toneLowPassG.at (sample, inverseNumSamples);

            auto x = static_cast<double> (data[sample]);

            // Tight: the pre-drive highpass, voicing-independent since
            // v0.2.0, now running inside the oversampled region.
            x = state.tightHighPass.process (x);

            double shaped = 0.0;

            switch (voicing)
            {
                case VoicingType::gnaw:
                {
                    // Pre-emphasis -> hard clip -> exact inverse de-emphasis.
                    const auto emphasised = state.preEmphasis.process (x);
                    const auto clipped = state.shaper.process (highDriveGainNow * emphasised + biasOffset, hardClip);
                    shaped = state.deEmphasis.process (clipped);
                    break;
                }

                case VoicingType::wool:
                {
                    // Dynamic bias: a fast-attack, slow-release envelope of
                    // how far the input exceeds a small threshold, subtracted
                    // from the shaper input. After a loud passage the clipper
                    // stays offset for ~20 ms, so a quiet note immediately
                    // afterwards sits on a shallower part of the diode curve
                    // and is measurably quieter - sag, from the blocking cap's
                    // time constant.
                    const auto excess = juce::jmax (0.0, std::abs (x) - biasThreshold);
                    const auto coefficient = excess > state.biasEnvelope ? biasAttackG : biasReleaseG;
                    state.biasEnvelope += coefficient * (excess - state.biasEnvelope);

                    const auto offset = biasOffset - biasDepthVolts * state.biasEnvelope;
                    const auto driven = highDriveGainNow * x + offset;

                    // Subtract what the shaper does to the offset ON ITS OWN.
                    // Without this the decaying bias envelope is itself an
                    // audible signal - a 20 ms thump after every loud note -
                    // and the DC blocker cannot remove it, because a 20 ms
                    // decay is nowhere near DC. Removing the offset's own
                    // image leaves only what the bias is actually for: the
                    // change in the curve's local SLOPE, i.e. the gain the
                    // next quiet note is shaped by. Same construction v0.2.0's
                    // Wool used for its static asymmetry bias.
                    shaped = state.shaper.process (driven, woolCurve) - woolCurve.evaluate (offset);
                    break;
                }

                case VoicingType::razor:
                default:
                {
                    // "Unity clean + clipped difference": in a non-inverting
                    // feedback clipper the dry signal passes at unity and only
                    // the feedback voltage saturates, which is exactly why
                    // this topology stays touch-sensitive. The 330 Hz
                    // pre-emphasis keeps the bass fundamental out of the
                    // clipped path.
                    const auto emphasised = state.razorHighPass.processHighPass (x);
                    const auto clipped = state.shaper.process (highDriveGainNow * emphasised + biasOffset, razorCurve);
                    shaped = x + clipped;
                    break;
                }
            }

            // Drive-tracked post-LPF: the Cc pole that slides down as the
            // drive pot opens (transparent at drive 0 by construction).
            shaped = state.trackedLowPass.processLowPass (shaped);

            // Remove the DC the bias controls deliberately introduced, so
            // High Bias buys even-harmonic content without an output offset.
            shaped = state.dcBlocker.processHighPass (shaped);

            // Voicing character filter, then the tone lowpass.
            shaped = state.character.process (shaped);
            shaped = state.toneLowPass.processLowPass (shaped);

            // Clean/distorted blend. Both sides live inside the oversampled
            // region and differ only by the ADAA cores' half-sample delay, so
            // a plain linear crossfade is correctly time-aligned here - no
            // DryWetMixer latency compensation (and none of its priming
            // pitfalls) needed.
            const auto blended = (1.0 - blend) * static_cast<double> (dry[sample]) + blend * shaped;

            data[sample] = static_cast<float> (blended * highGainLinear.at (sample, inverseNumSamples));
        }
    }

    //==========================================================================
    void CircuitDrive::process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        jassert (oversampling != nullptr);

        updateCoefficients();

        // Push the freshly computed coefficients into the per-channel filter
        // state without disturbing the state variables themselves (a
        // coefficient write is continuous; a state reset would click).
        for (auto& state : highState)
        {
            state.tightHighPass.b0 = tightHighPassPrototype.b0;
            state.tightHighPass.b1 = tightHighPassPrototype.b1;
            state.tightHighPass.b2 = tightHighPassPrototype.b2;
            state.tightHighPass.a1 = tightHighPassPrototype.a1;
            state.tightHighPass.a2 = tightHighPassPrototype.a2;

            state.preEmphasis.b0 = preEmphasisPrototype.b0;
            state.preEmphasis.b1 = preEmphasisPrototype.b1;
            state.preEmphasis.b2 = preEmphasisPrototype.b2;
            state.preEmphasis.a1 = preEmphasisPrototype.a1;
            state.preEmphasis.a2 = preEmphasisPrototype.a2;

            state.deEmphasis.b0 = deEmphasisPrototype.b0;
            state.deEmphasis.b1 = deEmphasisPrototype.b1;
            state.deEmphasis.b2 = deEmphasisPrototype.b2;
            state.deEmphasis.a1 = deEmphasisPrototype.a1;
            state.deEmphasis.a2 = deEmphasisPrototype.a2;

            state.character.b0 = characterPrototype.b0;
            state.character.b1 = characterPrototype.b1;
            state.character.b2 = characterPrototype.b2;
            state.character.a1 = characterPrototype.a1;
            state.character.a2 = characterPrototype.a2;

            // trackedLowPass and toneLowPass are NOT set here: both follow
            // automatable controls and are ramped per sample inside
            // processHighChannel(). razorHighPass and dcBlocker are fixed
            // corners, so a block-rate write is right for them.
            state.razorHighPass.g = razorHighPassG;
            state.dcBlocker.g = dcBlockerG;
        }

        // ONE upsample for both bands - the whole point of this engine.
        auto upBlock = oversampling->processSamplesUp (juce::dsp::AudioBlock<const float> (block));

        const auto upChannels = upBlock.getNumChannels();
        const auto upSamples = upBlock.getNumSamples();

        auto midBlock = juce::dsp::AudioBlock<float> (midBuffer).getSubBlock (0, upSamples).getSubsetChannelBlock (0, upChannels);
        auto highBlock = juce::dsp::AudioBlock<float> (highBuffer).getSubBlock (0, upSamples).getSubsetChannelBlock (0, upChannels);
        auto highDryBlock = juce::dsp::AudioBlock<float> (highDryBuffer).getSubBlock (0, upSamples).getSubsetChannelBlock (0, upChannels);

        // Split #2, now at fs*M.
        midHighSplitOversampled.process (juce::dsp::AudioBlock<const float> (upBlock), midBlock, highBlock);

        // The blend's dry tap is the high band as split, before any voicing
        // processing - matching what the Classic engine's DryWetMixer captures.
        highDryBlock.copyFrom (juce::dsp::AudioBlock<const float> (highBlock));

        for (size_t channel = 0; channel < upChannels; ++channel)
        {
            processMidChannel (midBlock.getChannelPointer (channel), upSamples, channel);
            processHighChannel (highBlock.getChannelPointer (channel),
                                 highDryBlock.getChannelPointer (channel),
                                 upSamples,
                                 channel);
        }

        // Band levels for the meter taps, measured here because the two bands
        // stop existing separately on the next line.
        {
            const auto rmsOf = [upSamples] (const juce::dsp::AudioBlock<float>& band)
            {
                const auto* data = band.getChannelPointer (0);
                double sumOfSquares = 0.0;

                for (size_t sample = 0; sample < upSamples; ++sample)
                    sumOfSquares += static_cast<double> (data[sample]) * static_cast<double> (data[sample]);

                return upSamples > 0
                           ? static_cast<float> (std::sqrt (sumOfSquares / static_cast<double> (upSamples)))
                           : 0.0f;
            };

            midBandLevel = rmsOf (midBlock);
            highBandLevel = rmsOf (highBlock);
        }

        // Every channel has now walked the same ramps, so advance them once
        // for the whole block.
        commitRamps();

        // Sum the two bands back together at the oversampled rate, then take
        // the single downsample.
        upBlock.replaceWithSumOf (juce::dsp::AudioBlock<const float> (midBlock),
                                   juce::dsp::AudioBlock<const float> (highBlock));

        oversampling->processSamplesDown (block);
    }
}
