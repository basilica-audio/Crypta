#include "ParallelCompressor.h"

namespace
{
    // Matches the ~20ms ramp used for the plugin's other gain stages
    // (PluginProcessor.cpp's gainRampDurationSeconds).
    constexpr double makeupGainRampDurationSeconds = 0.02;
}

namespace cryp
{
    void ParallelCompressor::prepare (const juce::dsp::ProcessSpec& spec,
                                       float initialWetMixProportion01,
                                       float initialMakeupGainDb)
    {
        compressor.prepare (spec);
        detector.prepare (spec);

        // Issue #98: the makeup value goes in BEFORE prepare(), which is what
        // makes prepare()'s internal reset() snap the smoother to it instead
        // of leaving it to ramp up from a default-constructed silence. Same
        // ordering requirement as the mixer below, same JUCE 8.0.14 cause.
        makeupGain.setRampDurationSeconds (makeupGainRampDurationSeconds);
        makeupGain.setGainDecibels (initialMakeupGainDb);
        makeupGain.prepare (spec);

        // Prime the mix *before* prepare() so the mixer's internal reset()
        // (called at the end of DryWetMixer::prepare()) snaps its smoothed
        // dry/wet volumes to the correct starting point instead of a stale
        // default.
        mixer.setMixingRule (juce::dsp::DryWetMixingRule::linear);
        mixer.setWetMixProportion (initialWetMixProportion01);
        mixer.prepare (spec);

        // The compressor and makeup gain add no sample latency, so the
        // dry path never needs compensating delay.
        mixer.setWetLatency (0.0f);
    }

    void ParallelCompressor::reset()
    {
        compressor.reset();
        detector.reset();
        makeupGain.reset();
        mixer.reset();

        // A meter must not keep displaying reduction that belongs to material
        // the transport has already left behind.
        classicGainReductionDb = 0.0f;
    }
}
