#pragma once

#include "LevelDetector.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>

// Low-band parallel ("New York style") compressor (issue #42): the low band
// is compressed, made up, and blended back with its own uncompressed self
// via lowCompMix, rather than inserted serially - which is what makes it a
// *parallel* compressor rather than a plain dynamics insert. Sits after the
// LR4 split and before lowLevel in the low-band chain.
//
// juce::dsp::Compressor<float> (JUCE 8.0.14,
// juce_dsp/widgets/juce_Compressor.h) is a simple feed-forward VCA-style
// compressor with no lookahead, so it introduces zero sample latency - the
// juce::dsp::DryWetMixer used for the parallel blend is configured
// accordingly (wetLatency stays 0), so this stage never needs to feed into
// the plugin's latency-compensation seam.
namespace cryp
{
    class ParallelCompressor
    {
    public:
        ParallelCompressor() = default;

        // `initialWetMixProportion01` must be the current lowCompMix value
        // (0..1) *before* prepare() runs the mixer's internal reset(): the
        // DryWetMixer primes its smoothed dry/wet volumes from whatever
        // `mix` was set to at the time reset() executes (JUCE 8.0.14
        // gotcha), so passing the real value here - rather than setting it
        // only after prepare() - avoids an audible fade-in glitch on the
        // very first block.
        // `initialMakeupGainDb` is primed the same way and for the same
        // reason (issue #98): juce::dsp::Gain::prepare() snaps its smoother to
        // whatever target the object currently holds, and a default-constructed
        // one holds silence - so a makeup stage prepared before it is told its
        // value ramps up from nothing across its first 20 ms on a fresh
        // instance while a re-prepared one starts at level. 0 dB is the
        // default because it is the neutral value for a makeup gain: a caller
        // that says nothing gets unity, not silence.
        void prepare (const juce::dsp::ProcessSpec& spec,
                       float initialWetMixProportion01,
                       float initialMakeupGainDb = 0.0f);
        void reset();

        // All real-time safe: Compressor's setters just recompute ballistics
        // coefficients (no allocation), Gain's setter only retargets its
        // SmoothedValue, and DryWetMixer::setWetMixProportion only updates a
        // scalar + recomputes the (already-allocated) dry/wet volume
        // targets.
        void setThresholdDb (float newThresholdDb) noexcept
        {
            compressor.setThreshold (newThresholdDb);
            detector.setThresholdDb (newThresholdDb);
        }

        void setRatio (float newRatio) noexcept
        {
            compressor.setRatio (newRatio);
            detector.setRatio (newRatio);
        }

        void setAttackMs (float newAttackMs) noexcept
        {
            compressor.setAttack (newAttackMs);
            detector.setAttackMs (newAttackMs);
        }

        void setReleaseMs (float newReleaseMs) noexcept
        {
            compressor.setRelease (newReleaseMs);
            detector.setReleaseMs (newReleaseMs);
        }

        void setMakeupGainDb (float newMakeupDb) noexcept { makeupGain.setGainDecibels (newMakeupDb); }
        void setWetMixProportion (float newWetMixProportion01) noexcept { mixer.setWetMixProportion (newWetMixProportion01); }

        //======================================================================
        // v0.3.0 detector engine selection. `Classic Peak` is the stock
        // juce::dsp::Compressor path v0.2.0 shipped, preserved bit-identical;
        // `Smooth RMS` is cryp::LevelDetector (see its header for why a bass
        // low band needs it).
        void setUseSmoothRmsDetector (bool shouldUseSmoothRms) noexcept { useSmoothRms = shouldUseSmoothRms; }
        void setKneeDb (float newKneeDb) noexcept { detector.setKneeDb (newKneeDb); }
        void setAutoRelease (bool shouldAutoRelease) noexcept { detector.setAutoRelease (shouldAutoRelease); }
        void setAutoMakeup (bool shouldAutoMakeup) noexcept { autoMakeup = shouldAutoMakeup; }

        // Auto-makeup is read by BOTH engines, so it is applied here rather
        // than inside the detector: the total makeup is the manual value plus,
        // when enabled, the half-compensation figure.
        float getEffectiveMakeupDb (float manualMakeupDb) const noexcept
        {
            return manualMakeupDb + (autoMakeup ? detector.getAutoMakeupDb() : 0.0f);
        }

        // Peak gain reduction across the last processed block, in POSITIVE
        // decibels (0 = no reduction) - the direction a GR meter draws, and
        // the unit cryp::MeterTaps::lowCompGainReductionDb publishes to the UI
        // (issue #12: "gain reduction exposed atomically for metering").
        //
        // The two engines measure it differently, because they can:
        //   - Smooth RMS owns its gain computer, so the figure is the exact
        //     smoothed gain the detector applied (see LevelDetector).
        //   - Classic Peak is juce::dsp::Compressor, which does not expose its
        //     internal gain at all. Rather than report a flat 0 - which would
        //     leave the meter dead for anyone on the legacy engine, including
        //     every migrated pre-v0.3.0 session - process() measures the block
        //     peak either side of the compressor and reports the ratio. That
        //     is an ESTIMATE, and an honest one: for the steady-state material
        //     a GR meter is read against it tracks the static curve closely
        //     (asserted in tests/ParallelCompressorTests.cpp), while on a fast
        //     transient it reports the block's worst case rather than an
        //     instantaneous value. It is deliberately measured before the
        //     makeup gain, so it shows compression, not net level change.
        float getGainReductionDb() const noexcept
        {
            return useSmoothRms ? detector.getGainReductionDb() : classicGainReductionDb;
        }

        // In-place parallel compression: mixer.pushDrySamples() captures the
        // pre-compression signal, the compressor + makeup gain run in place,
        // then mixer.mixWetSamples() blends the compressed ("wet") result
        // back with the captured dry signal per lowCompMix.
        void process (juce::dsp::AudioBlock<float>& block) noexcept
        {
            mixer.pushDrySamples (juce::dsp::AudioBlock<const float> (block));

            juce::dsp::ProcessContextReplacing<float> context (block);

            if (useSmoothRms)
            {
                detector.process (block);
            }
            else
            {
                // Block-rate only: two O(n) peak passes, no per-sample cost
                // added to the compressor itself, no allocation. See
                // getGainReductionDb() for why this exists.
                const auto peakBefore = blockPeak (block);
                compressor.process (context);
                const auto peakAfter = blockPeak (block);

                constexpr float silenceFloor = 1.0e-6f;

                classicGainReductionDb = (peakBefore > silenceFloor && peakAfter > silenceFloor)
                                              ? juce::jmax (0.0f, juce::Decibels::gainToDecibels (peakBefore / peakAfter))
                                              : 0.0f;
            }

            makeupGain.process (context);

            mixer.mixWetSamples (block);
        }

    private:
        static float blockPeak (const juce::dsp::AudioBlock<float>& block) noexcept
        {
            float peak = 0.0f;

            for (size_t channel = 0; channel < block.getNumChannels(); ++channel)
            {
                const auto* data = block.getChannelPointer (channel);

                for (size_t sample = 0; sample < block.getNumSamples(); ++sample)
                    peak = juce::jmax (peak, std::abs (data[sample]));
            }

            return peak;
        }

        juce::dsp::Compressor<float> compressor;
        LevelDetector detector;
        bool useSmoothRms = false;
        bool autoMakeup = false;

        // See getGainReductionDb(). Written once per block by process(), read
        // by the processor's metering publish step on the same thread.
        float classicGainReductionDb = 0.0f;

        juce::dsp::Gain<float> makeupGain;
        juce::dsp::DryWetMixer<float> mixer;
    };
}
