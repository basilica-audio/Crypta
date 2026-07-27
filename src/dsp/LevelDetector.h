#pragma once

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <vector>

// Log-domain RMS compressor detector (brief §3.2), the "Smooth RMS" engine
// behind lowCompDetector.
//
// The problem it solves is specific to a bass processor. A peak detector with
// a 6 ms release - the sourced glue-compressor ballistics this plugin ships -
// follows the individual half-cycles of a 60 Hz fundamental, so the gain
// reduction ripples at 120 Hz and the low band tremolos. That is the single
// most audible weakness of the v0.2.0 low band.
//
// The fix is the standard one (Giannoulis, Massberg & Reiss, JAES 60(6),
// "Digital Dynamic Range Compressor Design - A Tutorial and Analysis"): detect
// mean-square over a window longer than one period of the lowest fundamental,
// convert to dB, run the gain computer, and smooth the result IN THE LOG
// DOMAIN, after the gain computer rather than before it. Smoothing the
// detector instead would make the attack/release times level-dependent.
//
//   ms[n] = a*ms[n-1] + (1-a)*x^2       a = exp(-1/(tau_rms * fs)), tau_rms 15 ms
//   L[n]  = 10*log10(ms[n] + 1e-30)
//   G(L)  = soft-knee gain computer, quadratic across the knee
//   g[n]  = smooth-branching one-pole on G, in dB
//
// 15 ms is one full period of 66 Hz, so the low B of a 5-string (31 Hz) still
// ripples somewhat while a low E (41 Hz) and everything above it does not -
// the trade against how sluggish the detector feels.
namespace cryp
{
    class LevelDetector
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            sampleRate = spec.sampleRate;
            channels.assign (static_cast<size_t> (spec.numChannels), ChannelState {});
            updateCoefficients();
            reset();
        }

        void reset() noexcept
        {
            for (auto& channel : channels)
            {
                channel.meanSquare = 0.0;
                channel.smoothedGainDb = 0.0;
                channel.fastEnvelopeDb = -120.0;
                channel.slowEnvelopeDb = -120.0;
            }

            lastGainReductionDb = 0.0f;
        }

        //======================================================================
        void setThresholdDb (float newThresholdDb) noexcept { thresholdDb = newThresholdDb; }
        void setRatio (float newRatio) noexcept { ratio = juce::jmax (1.0f, newRatio); }
        void setKneeDb (float newKneeDb) noexcept { kneeDb = juce::jmax (0.0f, newKneeDb); }

        void setAttackMs (float newAttackMs) noexcept
        {
            attackMs = juce::jmax (0.01f, newAttackMs);
            updateCoefficients();
        }

        void setReleaseMs (float newReleaseMs) noexcept
        {
            releaseMs = juce::jmax (0.1f, newReleaseMs);
            updateCoefficients();
        }

        void setAutoRelease (bool shouldAutoRelease) noexcept { autoRelease = shouldAutoRelease; }

        // Static makeup that compensates roughly half the gain the compressor
        // takes away at the threshold: -0.5 * T * (1 - 1/R) dB.
        //
        // Note the sign. Thresholds are negative, so (1 - 1/R) positive and
        // T negative makes -0.5*T*(1 - 1/R) POSITIVE - a boost, which is what
        // makeup means. T = -18 dB at 2:1 gives +4.5 dB.
        float getAutoMakeupDb() const noexcept
        {
            return -0.5f * thresholdDb * (1.0f - 1.0f / ratio);
        }

        // Largest gain reduction applied in the last processed block, as a
        // POSITIVE number of dB.
        float getGainReductionDb() const noexcept { return lastGainReductionDb; }

        //======================================================================
        // In-place. Applies the computed gain to `block` and records the peak
        // gain reduction for the meter tap.
        void process (juce::dsp::AudioBlock<float>& block) noexcept
        {
            const auto numChannels = juce::jmin (block.getNumChannels(), channels.size());
            const auto numSamples = block.getNumSamples();

            double blockMaximumReduction = 0.0;

            for (size_t channel = 0; channel < numChannels; ++channel)
            {
                auto* data = block.getChannelPointer (channel);
                auto& state = channels[channel];

                for (size_t sample = 0; sample < numSamples; ++sample)
                {
                    const auto x = static_cast<double> (data[sample]);

                    // Mean-square detector.
                    state.meanSquare = meanSquareCoefficient * state.meanSquare
                                        + (1.0 - meanSquareCoefficient) * x * x;

                    const auto levelDb = 10.0 * std::log10 (state.meanSquare + 1.0e-30);

                    // Gain computer, then smoothing - in that order, in dB.
                    const auto targetGainDb = computeGainDb (levelDb);
                    const auto coefficient = chooseSmoothingCoefficient (state, levelDb, targetGainDb);

                    state.smoothedGainDb += coefficient * (targetGainDb - state.smoothedGainDb);

                    data[sample] = static_cast<float> (x * std::pow (10.0, state.smoothedGainDb / 20.0));

                    blockMaximumReduction = juce::jmax (blockMaximumReduction, -state.smoothedGainDb);
                }
            }

            lastGainReductionDb = static_cast<float> (blockMaximumReduction);
        }

    private:
        struct ChannelState
        {
            double meanSquare = 0.0;
            double smoothedGainDb = 0.0;

            // Dual-envelope race for the program-dependent release.
            double fastEnvelopeDb = -120.0;
            double slowEnvelopeDb = -120.0;
        };

        static double onePoleCoefficient (double milliseconds, double sampleRate) noexcept
        {
            return 1.0 - std::exp (-1.0 / (juce::jmax (1.0e-4, milliseconds) * 0.001 * sampleRate));
        }

        void updateCoefficients() noexcept
        {
            meanSquareCoefficient = std::exp (-1.0 / (meanSquareTimeConstantMs * 0.001 * sampleRate));
            attackCoefficient = onePoleCoefficient (attackMs, sampleRate);
            releaseCoefficient = onePoleCoefficient (releaseMs, sampleRate);
            slowReleaseCoefficient = onePoleCoefficient (releaseMs * autoReleaseStretch, sampleRate);
            fastEnvelopeCoefficient = onePoleCoefficient (fastEnvelopeMs, sampleRate);
        }

        // Soft-knee gain computer, Giannoulis et al.'s construction. Returns
        // the gain to APPLY, in dB (so <= 0).
        double computeGainDb (double levelDb) const noexcept
        {
            const auto threshold = static_cast<double> (thresholdDb);
            const auto knee = static_cast<double> (kneeDb);
            const auto inverseRatio = 1.0 / static_cast<double> (ratio);

            const auto overshoot = levelDb - threshold;

            if (knee > 0.0 && 2.0 * std::abs (overshoot) <= knee)
            {
                // Quadratic interpolation across the knee: the curve and its
                // slope are both continuous at each end, which is what stops a
                // hard-knee compressor's audible "grab".
                const auto kneeTerm = overshoot + knee * 0.5;
                return (inverseRatio - 1.0) * kneeTerm * kneeTerm / (2.0 * knee);
            }

            if (overshoot <= 0.0)
                return 0.0;

            return overshoot * (inverseRatio - 1.0);
        }

        // Smooth-branching: attack coefficient when the gain is moving further
        // into reduction, release when it is recovering.
        //
        // With auto-release on, the release side races two envelopes of the
        // input level. When the fast one has dropped well below the slow one
        // the material is transient and the set release is used; when they are
        // still close the material is sustained and the release is stretched,
        // so a held low note is not pumped.
        double chooseSmoothingCoefficient (ChannelState& state, double levelDb, double targetGainDb) noexcept
        {
            state.fastEnvelopeDb += fastEnvelopeCoefficient * (levelDb - state.fastEnvelopeDb);

            // Self-releasing slow envelope: instant rise, fixed dB-per-second
            // decay, which is what makes the comparison below scale-free.
            if (levelDb > state.slowEnvelopeDb)
                state.slowEnvelopeDb = levelDb;
            else
                state.slowEnvelopeDb -= slowEnvelopeDecayDbPerSecond / sampleRate;

            if (targetGainDb < state.smoothedGainDb)
                return attackCoefficient;

            if (! autoRelease)
                return releaseCoefficient;

            const auto isTransient = (state.slowEnvelopeDb - state.fastEnvelopeDb) > transientWindowDb;
            return isTransient ? releaseCoefficient : slowReleaseCoefficient;
        }

        //======================================================================
        // One period of 66 Hz. Long enough to stop a bass fundamental from
        // rippling the gain reduction, short enough that the detector still
        // feels connected to the playing.
        static constexpr double meanSquareTimeConstantMs = 15.0;

        // How much longer the release gets on sustained material.
        static constexpr double autoReleaseStretch = 4.0;

        static constexpr double fastEnvelopeMs = 20.0;
        static constexpr double slowEnvelopeDecayDbPerSecond = 40.0;

        // Wider than the worst-case detector ripple at 60 Hz with a 15 ms
        // window, so ripple alone can never be mistaken for a transient.
        static constexpr double transientWindowDb = 4.0;

        double sampleRate = 44100.0;

        float thresholdDb = -18.0f;
        float ratio = 2.0f;
        float kneeDb = 6.0f;
        float attackMs = 3.0f;
        float releaseMs = 6.0f;
        bool autoRelease = true;

        double meanSquareCoefficient = 0.0;
        double attackCoefficient = 1.0;
        double releaseCoefficient = 1.0;
        double slowReleaseCoefficient = 1.0;
        double fastEnvelopeCoefficient = 1.0;

        float lastGainReductionDb = 0.0f;

        std::vector<ChannelState> channels;
    };
}
