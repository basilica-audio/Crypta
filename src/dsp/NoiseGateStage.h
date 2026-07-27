#pragma once

#include "GateEngine.h"

#include <juce_dsp/juce_dsp.h>

// Full-band input noise gate (issue #42), sitting between input trim and the
// LR4 crossover split in the signal chain. Thin wrapper around
// juce::dsp::NoiseGate<float> (JUCE 8.0.14,
// juce_dsp/widgets/juce_NoiseGate.h) so the processor and the test suite
// share one real-time-safe seam, matching the pattern already established by
// cryp::Crossover.
//
// juce::dsp::NoiseGate's ballistics filters carry their own per-channel
// state internally (via BallisticsFilter, indexed by the `channel` argument
// passed to processSample()), so a single instance handles mono/stereo
// without any extra per-channel bookkeeping here.
namespace cryp
{
    class NoiseGateStage
    {
    public:
        NoiseGateStage() = default;

        void prepare (const juce::dsp::ProcessSpec& spec);
        void reset();

        void setEnabled (bool shouldBeEnabled) noexcept { enabled = shouldBeEnabled; }
        bool isEnabled() const noexcept { return enabled; }

        // Real-time safe: NoiseGate::setThreshold/setRatio/setAttack/
        // setRelease just recompute ballistics coefficients, no allocation.
        void setThresholdDb (float newThresholdDb) noexcept
        {
            gate.setThreshold (newThresholdDb);
            modernGate.setThresholdDb (newThresholdDb);
        }

        void setRatio (float newRatio) noexcept { gate.setRatio (newRatio); }

        void setAttackMs (float newAttackMs) noexcept
        {
            gate.setAttack (newAttackMs);
            modernGate.setAttackMs (newAttackMs);
        }

        void setReleaseMs (float newReleaseMs) noexcept
        {
            gate.setRelease (newReleaseMs);
            modernGate.setReleaseMs (newReleaseMs);
        }

        //======================================================================
        // v0.3.0 mode selection. Classic is the juce::dsp::NoiseGate wrapper
        // v0.2.0 shipped, preserved bit-identical (including its bit-exact
        // disabled bypass); Modern is cryp::GateEngine.
        //
        // gateRatio is CLASSIC-ONLY and Modern ignores it: Modern is a gate
        // with a range floor, not a downward expander with a ratio. Documented
        // in docs/manual.md.
        void setModernMode (bool shouldUseModern) noexcept { useModern = shouldUseModern; }

        void setHysteresisDb (float value) noexcept { modernGate.setHysteresisDb (value); }
        void setHoldMs (float value) noexcept { modernGate.setHoldMs (value); }
        void setRangeDb (float value) noexcept { modernGate.setRangeDb (value); }
        void setSidechainHighPassHz (float value) noexcept { modernGate.setSidechainHighPassHz (value); }

        // Gain reduction, positive dB. Zero on the Classic path, which does
        // not expose its internal gain.
        float getGainReductionDb() const noexcept
        {
            return enabled && useModern ? modernGate.getGainReductionDb() : 0.0f;
        }

        // In-place gate. When disabled, this is a deliberate no-op (not just
        // a unity-gain pass through the gate's own math) so the disabled
        // state is bit-exact transparent and costs nothing on the audio
        // thread.
        void process (juce::dsp::AudioBlock<float>& block) noexcept
        {
            if (! enabled)
                return;

            if (useModern)
                modernGate.process (block);
            else
                gate.process (juce::dsp::ProcessContextReplacing<float> (block));
        }

    private:
        juce::dsp::NoiseGate<float> gate;
        GateEngine modernGate;
        bool useModern = false;
        bool enabled = false;
    };
}
