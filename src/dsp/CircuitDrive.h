#pragma once

#include "ADAAShaper.h"
#include "Crossover.h"
#include "Voicing.h"

#include <juce_dsp/juce_dsp.h>

#include <memory>
#include <vector>

// The v0.3.0 "Circuit" drive engine: the Mid and High bands rebuilt as
// circuit-derived, ADAA-antialiased clipper stages sharing ONE oversampling
// region.
//
// Why this class exists at all. v0.2.0 ran two independent 4x oversampling
// instances - one inside cryp::MidBand, one inside cryp::Voicing - and
// MidBand.h's own class comment concedes the duplication, justifying it as a
// risk trade rather than a design goal. Collapsing them is what pays for the
// extra per-voicing filtering below at roughly the same CPU: the remainder
// band is upsampled once, split into Mid/High by a second LR4 crossover
// running AT the oversampled rate, processed, summed, and downsampled once.
//
//   remainder (base rate)
//     -> upsample (shared juce::dsp::Oversampling)
//     -> LR4 split #2, at fs*M
//     -> Mid:  ADAA tanh core, dry-crossfaded by drive
//        High: tight HPF -> per-voicing pre-emphasis -> ADAA clipper core
//              -> de-emphasis -> drive-tracked LPF -> DC blocker
//              -> character filter -> tone LPF -> clean/distorted blend
//     -> per-band level trim -> sum -> downsample (base rate)
//
// The Classic engine (cryp::MidBand + cryp::Voicing, untouched) remains the
// bit-identical fallback and is what every pre-v0.3.0 session and preset is
// migrated onto, so nothing here can change how existing work sounds.
//
// Per-voicing topologies follow research-diode-clipper-dk.md; the specific
// corner frequencies and gain ceilings are engineering-derived starting
// points pending the suite's ear-tuning gate, not final voicing decisions.
namespace cryp
{
    // Second-order section in transposed direct form II, double precision,
    // with coefficients held as plain doubles so the pre/de-emphasis pair can
    // be made *exactly* mutually inverse (see makeInverse()). juce::dsp::IIR
    // would work for most of this, but the inverse-filter trick needs direct
    // coefficient access and the ADAA cores need to interleave per-sample
    // with the filtering anyway.
    struct CircuitBiquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        void reset() noexcept { z1 = z2 = 0.0; }

        double process (double x) noexcept
        {
            const auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        // RBJ cookbook sections, normalised by a0.
        static CircuitBiquad makeHighPass (double sampleRate, double frequencyHz, double q) noexcept;
        static CircuitBiquad makePeak (double sampleRate, double frequencyHz, double q, double gainDb) noexcept;
        static CircuitBiquad makeHighShelf (double sampleRate, double frequencyHz, double q, double gainDb) noexcept;

        // The exact algebraic inverse: 1/H(z), obtained by swapping numerator
        // and denominator and renormalising. This is what makes the Gnaw
        // pre-emphasis / de-emphasis pair cancel to machine precision in the
        // clipper's linear region, so that "drive 0 is transparent" is a
        // structural property rather than an approximation that happens to
        // measure well. Valid because the shelf being inverted is minimum
        // phase, so its inverse is stable.
        CircuitBiquad makeInverse() const noexcept;
    };

    // First-order lowpass, y += g*(x - y). Its complement (x - y) is the
    // matching first-order highpass, which is how the Razor pre-emphasis and
    // the DC blocker are built.
    struct CircuitOnePole
    {
        double g = 1.0;
        double state = 0.0;

        void reset() noexcept { state = 0.0; }

        void setCutoff (double sampleRate, double frequencyHz) noexcept
        {
            const auto clamped = juce::jlimit (1.0, sampleRate * 0.45, frequencyHz);
            g = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * clamped / sampleRate);
        }

        double processLowPass (double x) noexcept
        {
            state += g * (x - state);
            return state;
        }

        double processHighPass (double x) noexcept { return x - processLowPass (x); }
    };

    class CircuitDrive
    {
    public:
        CircuitDrive() = default;

        // `spec` is the BASE-rate spec. The oversampling factor is chosen from
        // spec.sampleRate (see chooseFactorExponent()) and everything
        // downstream is prepared at fs*M.
        void prepare (const juce::dsp::ProcessSpec& spec);
        void reset();

        // Integer sample latency of the shared oversampling region, at base
        // rate. Zero until prepare() has run. The ADAA cores' own half-sample
        // group delay is deliberately NOT included: it is identical across
        // Mid and High so the bands stay aligned, and at base rate it is a
        // fraction of a sample, far below what setLatencySamples() can express.
        int getLatencySamples() const noexcept { return latencySamples; }

        int getOversamplingFactor() const noexcept { return oversamplingFactor; }

        // Post-drive, post-level RMS of each band for the last processed
        // block. The two bands are summed inside the oversampled region, so
        // they cannot be measured from outside this class.
        float getMidBandLevel() const noexcept { return midBandLevel; }
        float getHighBandLevel() const noexcept { return highBandLevel; }

        //======================================================================
        // Control-rate setters. All real-time safe (scalar stores only; the
        // coefficients they imply are recomputed once per block in process()).
        void setSplitHighHz (float newSplitHighHz) noexcept { splitHighHz = newSplitHighHz; }
        void setMidDrive (float drive01) noexcept { midDrive01 = juce::jlimit (0.0f, 1.0f, drive01); }
        void setVoicing (VoicingType newVoicing) noexcept { voicing = newVoicing; }
        void setHighDrive (float drive01) noexcept { highDrive01 = juce::jlimit (0.0f, 1.0f, drive01); }
        void setHighTone (float tone01) noexcept { highTone01 = juce::jlimit (0.0f, 1.0f, tone01); }
        void setHighTightHz (float newTightHz) noexcept { tightHz = juce::jlimit (20.0f, 500.0f, newTightHz); }
        void setHighBlend (float blend01) noexcept { highBlend01 = juce::jlimit (0.0f, 1.0f, blend01); }
        void setHighBias (float bias01) noexcept { highBias01 = juce::jlimit (0.0f, 1.0f, bias01); }
        void setMidLevelDb (float db) noexcept { midLevelDb = db; }
        void setHighLevelDb (float db) noexcept { highLevelDb = db; }

        // In-place. `block` carries the post-split-#1 remainder (Mid+High
        // content) at base rate and receives the summed, level-trimmed
        // Mid+High result. Real-time safe once prepare() has run.
        void process (juce::dsp::AudioBlock<float>& block) noexcept;

    private:
        // 4x at <= 50 kHz, 2x at <= 100 kHz, 1x (ADAA only) above. ADAA-1
        // contributes 20-30 dB of alias suppression on top of the
        // oversampling headroom, which is what makes 2x viable at 96 kHz -
        // research-oversampling-architecture.md §1.2 puts "2x + ADAA1" on par
        // with plain 8x.
        static size_t chooseFactorExponent (double sampleRate) noexcept;

        void updateCoefficients() noexcept;
        void processHighChannel (float* data, const float* dry, size_t numSamples, size_t channel) noexcept;
        void processMidChannel (float* data, size_t numSamples, size_t channel) noexcept;

        // Per-block scalar targets are ramped across the block rather than
        // stepped at its boundary (brief §3.6). A host automating drive sends
        // one value per block; applying it as a constant puts a staircase into
        // the audio, which measures as broadband non-harmonic spurs - about
        // -27 dBc on a fast highDrive sweep before this existed.
        //
        // Each smoothed scalar keeps the value it ENDED the last block at.
        // Every channel then interpolates from that value to the new target
        // across the block, and the stored value is advanced once, after all
        // channels are done - so the channels stay in step with each other.
        struct RampedScalar
        {
            double current = 0.0;
            double target = 0.0;

            void snap (double value) noexcept { current = target = value; }
            void setTarget (double value) noexcept { target = value; }

            double at (size_t sample, double inverseNumSamples) const noexcept
            {
                const auto position = static_cast<double> (sample + 1) * inverseNumSamples;
                return current + (target - current) * position;
            }

            void commit() noexcept { current = target; }
        };

        void commitRamps() noexcept;

        double baseSampleRate = 44100.0;
        double oversampledRate = 176400.0;
        int oversamplingFactor = 4;
        int latencySamples = 0;
        size_t numChannels = 2;

        std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;

        // Split #2, running at the oversampled rate. cryp::Crossover is used
        // exactly as-is; it simply gets prepared with a spec whose sampleRate
        // is fs*M (brief §5 keeps Crossover on the blacklist for this reason).
        Crossover midHighSplitOversampled;

        // Scratch, all sized for maximumBlockSize * maxFactor in prepare().
        juce::AudioBuffer<float> midBuffer;
        juce::AudioBuffer<float> highBuffer;
        juce::AudioBuffer<float> highDryBuffer;

        //======================================================================
        // Control state.
        float splitHighHz = 600.0f;
        float midDrive01 = 0.3f;
        VoicingType voicing = VoicingType::gnaw;
        float highDrive01 = 0.5f;
        float highTone01 = 0.5f;
        float tightHz = 100.0f;
        float highBlend01 = 1.0f;
        float highBias01 = 0.0f;
        float midLevelDb = 0.0f;
        float highLevelDb = 0.0f;

        //======================================================================
        // Derived per-block coefficients and gains.
        CircuitBiquad tightHighPassPrototype;
        CircuitBiquad preEmphasisPrototype;
        CircuitBiquad deEmphasisPrototype;
        CircuitBiquad characterPrototype;
        double razorHighPassG = 1.0;
        double dcBlockerG = 1.0;
        double biasAttackG = 1.0;
        double biasReleaseG = 1.0;

        // Ramped across each block - see RampedScalar.
        RampedScalar trackedLowPassG;
        RampedScalar toneLowPassG;
        RampedScalar highDriveGain;
        RampedScalar midDriveGain;
        RampedScalar midDriveAmount;
        RampedScalar highBlendAmount;
        RampedScalar highBiasOffset;
        RampedScalar midGainLinear;
        RampedScalar highGainLinear;
        bool rampsInitialised = false;

        float midBandLevel = 0.0f;
        float highBandLevel = 0.0f;

        //======================================================================
        // Per-channel state.
        struct HighChannelState
        {
            CircuitBiquad tightHighPass;
            CircuitBiquad preEmphasis;
            CircuitBiquad deEmphasis;
            CircuitBiquad character;
            CircuitOnePole trackedLowPass;
            CircuitOnePole toneLowPass;
            CircuitOnePole razorHighPass;
            CircuitOnePole dcBlocker;
            ADAAState shaper;
            double biasEnvelope = 0.0;

            void reset() noexcept
            {
                tightHighPass.reset();
                preEmphasis.reset();
                deEmphasis.reset();
                character.reset();
                trackedLowPass.reset();
                toneLowPass.reset();
                razorHighPass.reset();
                dcBlocker.reset();
                shaper.reset();
                biasEnvelope = 0.0;
            }
        };

        struct MidChannelState
        {
            ADAAState shaper;

            void reset() noexcept { shaper.reset(); }
        };

        std::vector<HighChannelState> highState;
        std::vector<MidChannelState> midState;

        //======================================================================
        // Tabulated voicing curves, built once in prepare().
        //
        // Wool: the DC solution of the asymmetric shunt diode clipper - the
        // implicit equation (P - y)/Req = iD(y) solved by Newton per table
        // point, with the SD-1-style asymmetric diode law from
        // research-diode-clipper-dk.md §2.1.
        //
        // Razor: Yeh's tanh-fit x/(1+|x|^2.5)^(1/2.5) (eq. 19), which has no
        // elementary antiderivative and so needs the table for F1 as well.
        //
        // Gnaw needs no table - a hard clip's antiderivative is closed form.
        ShaperTable woolCurve;
        ShaperTable razorCurve;
    };
}
