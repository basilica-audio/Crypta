#pragma once

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <vector>

// The v0.3.0 "Modern" noise gate (brief §3.5), from
// research-gate-expander.md §2.1 and §2.4 - the DN100/Zuul class of gate,
// minus lookahead (which would add latency and needs UX decisions of its own).
//
// What it adds over the juce::dsp::NoiseGate wrapper the Classic mode keeps:
//
//   - HYSTERESIS. A gate with one threshold chatters on any signal sitting
//     near it. Modern opens at the threshold and closes only once the signal
//     has fallen a further `hysteresis` dB, so a note decaying through the
//     threshold crosses it exactly once.
//   - HOLD, retriggering. Keeps the gate open for a set time after the signal
//     drops, so the gate does not slam shut between fast notes. Retriggering
//     means each new transient restarts the hold rather than being ignored.
//   - A SIDECHAIN HIGHPASS on the detector only. Without it the bass
//     fundamental holds the gate open on an otherwise silent string, which is
//     the single most common complaint about gating a bass DI.
//   - A dB-LINEAR RELEASE. An exponential release approaches the floor
//     asymptotically and never audibly arrives; a straight line in dB closes
//     in a predictable, dialable time, which is what `range / release` means.
//
// The control path runs PER SAMPLE, not per block: research-gate-expander.md
// §3.1 is explicit that block-rate gates chatter, and at a 512-sample block a
// 2 ms attack cannot even be expressed.
//
// Channels are LINKED - the detector runs on the maximum across channels and
// one gain is applied to all of them. A per-channel gate on stereo material
// would open one side before the other and wander the image.
namespace cryp
{
    class GateEngine
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec);
        void reset();

        void setThresholdDb (float newThresholdDb) noexcept { thresholdDb = newThresholdDb; }
        void setHysteresisDb (float newHysteresisDb) noexcept { hysteresisDb = juce::jmax (0.0f, newHysteresisDb); }
        void setRangeDb (float newRangeDb) noexcept { rangeDb = juce::jmax (1.0f, newRangeDb); }

        void setAttackMs (float newAttackMs) noexcept
        {
            attackMs = juce::jmax (0.01f, newAttackMs);
            updateCoefficients();
        }

        void setReleaseMs (float newReleaseMs) noexcept
        {
            releaseMs = juce::jmax (1.0f, newReleaseMs);
        }

        void setHoldMs (float newHoldMs) noexcept { holdMs = juce::jmax (0.0f, newHoldMs); }

        void setSidechainHighPassHz (float newFrequencyHz) noexcept
        {
            sidechainHz = juce::jlimit (20.0f, 400.0f, newFrequencyHz);
            updateSidechainCoefficients();
        }

        // Gain reduction currently applied, as a POSITIVE number of dB.
        float getGainReductionDb() const noexcept { return lastGainReductionDb; }

        void process (juce::dsp::AudioBlock<float>& block) noexcept;

    private:
        enum class State
        {
            closed,
            open,
            holding,
            releasing
        };

        void updateCoefficients() noexcept;
        void updateSidechainCoefficients() noexcept;

        // Second-order Butterworth highpass, transposed direct form II. Only
        // ever sees the detector signal; the audio path is untouched by it.
        struct SidechainFilter
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
        };

        double sampleRate = 44100.0;

        float thresholdDb = -60.0f;
        float hysteresisDb = 4.0f;
        float rangeDb = 60.0f;
        float attackMs = 1.0f;
        float releaseMs = 100.0f;
        float holdMs = 20.0f;
        float sidechainHz = 80.0f;

        double attackCoefficient = 1.0;
        double meanSquareCoefficient = 0.0;
        double fastSmoothingCoefficient = 1.0;
        double slowSmoothingCoefficient = 1.0;

        double sidechainB0 = 1.0, sidechainB1 = 0.0, sidechainB2 = 0.0;
        double sidechainA1 = 0.0, sidechainA2 = 0.0;

        // Detector state (shared across channels - the gate is linked).
        double meanSquare = 0.0;
        double smoothedLevelDb = -120.0;
        State state = State::closed;
        double holdSamplesRemaining = 0.0;

        // Current gain, in dB below unity (0 = fully open, -rangeDb = closed).
        double currentGainDb = 0.0;

        float lastGainReductionDb = 0.0f;

        std::vector<SidechainFilter> sidechainFilters;
    };
}
