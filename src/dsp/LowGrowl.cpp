#include "LowGrowl.h"

#include <cmath>

namespace cryp
{
    void LowGrowl::prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;

        const auto numChannels = static_cast<int> (juce::jmax (1u, spec.numChannels));

        // The one allocation this class performs, and it happens here rather
        // than anywhere process() can reach.
        branchBuffer.setSize (numChannels, static_cast<int> (juce::jmax (1u, spec.maximumBlockSize)));
        branchBuffer.clear();

        shaperState.assign (static_cast<size_t> (numChannels), ADAAState {});

        rumbleHighPass.prepare (spec);
        formantHighPass1.prepare (spec);
        formantHighPass2.prepare (spec);
        formantPeak.prepare (spec);
        formantLowPass1.prepare (spec);
        formantLowPass2.prepare (spec);

        growlGain.reset (sampleRate, growlGainRampSeconds);

        // Invalidate the coefficient cache so the first updateCoefficients()
        // call at the new sample rate always recomputes.
        coefficientTone = -1.0f;
        updateCoefficients();

        reset();
    }

    void LowGrowl::reset()
    {
        resetBranchState();
        branchArmed = false;

        // Snap rather than ramp: reset() means "there is no signal in flight",
        // so there is nothing for a fade to protect.
        growlGain.setCurrentAndTargetValue (enabled ? amount * maximumGrowlGain : 0.0f);
    }

    void LowGrowl::resetBranchState() noexcept
    {
        rumbleHighPass.reset();
        formantHighPass1.reset();
        formantHighPass2.reset();
        formantPeak.reset();
        formantLowPass1.reset();
        formantLowPass2.reset();

        for (auto& state : shaperState)
            state.reset();

        branchBuffer.clear();
    }

    float LowGrowl::getFormantCentreHz() const noexcept
    {
        return juce::mapToLog10 (tone, minimumFormantHz, maximumFormantHz);
    }

    void LowGrowl::updateCoefficients() noexcept
    {
        if (juce::exactlyEqual (tone, coefficientTone))
            return;

        coefficientTone = tone;

        const auto nyquistLimitHz = static_cast<float> (sampleRate * 0.49);
        const auto centreHz = juce::jlimit (20.0f, nyquistLimitHz, getFormantCentreHz());
        const auto lowerHz = juce::jlimit (20.0f, nyquistLimitHz, centreHz * lowerCornerRatio);
        const auto upperHz = juce::jlimit (20.0f, nyquistLimitHz, centreHz * upperCornerRatio);

        applyBiquadCoefficients (*rumbleHighPass.state,
                                  juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass (sampleRate, rumbleHighPassHz, guardQ));

        applyBiquadCoefficients (*formantHighPass1.state,
                                  juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass (sampleRate, lowerHz, butterworthQ1));
        applyBiquadCoefficients (*formantHighPass2.state,
                                  juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass (sampleRate, lowerHz, butterworthQ2));

        applyBiquadCoefficients (*formantPeak.state,
                                  juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter (
                                      sampleRate, centreHz, formantPeakQ, juce::Decibels::decibelsToGain (formantPeakGainDb)));

        applyBiquadCoefficients (*formantLowPass1.state,
                                  juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass (sampleRate, upperHz, butterworthQ1));
        applyBiquadCoefficients (*formantLowPass2.state,
                                  juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass (sampleRate, upperHz, butterworthQ2));
    }

    void LowGrowl::process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto numSamples = block.getNumSamples();
        const auto numChannels = juce::jmin (block.getNumChannels(),
                                              static_cast<size_t> (juce::jmax (0, branchBuffer.getNumChannels())));

        if (numSamples == 0 || numChannels == 0)
            return;

        // The bit-exact OFF path: disabled and fully faded out. Nothing is
        // read, nothing is written, and the branch is left flushed so the next
        // enable starts from silence rather than from whatever was in the
        // filters when the user last switched it off.
        if (! enabled && ! growlGain.isSmoothing() && growlGain.getCurrentValue() <= 0.0f)
        {
            if (branchArmed)
            {
                resetBranchState();
                branchArmed = false;
            }

            return;
        }

        if (! branchArmed)
        {
            resetBranchState();
            branchArmed = true;
        }

        updateCoefficients();

        auto branch = juce::dsp::AudioBlock<float> (branchBuffer)
                          .getSubBlock (0, numSamples)
                          .getSubsetChannelBlock (0, numChannels);

        branch.copyFrom (juce::dsp::AudioBlock<const float> (block).getSubsetChannelBlock (0, numChannels));

        juce::dsp::ProcessContextReplacing<float> context (branch);

        // 1. Keep DC and subsonic content out of an asymmetric curve, where it
        //    would slowly wander the operating point (and with it the even/odd
        //    balance) instead of contributing anything audible.
        rumbleHighPass.process (context);

        // 2. The asymmetric shaper itself: tanh(g*x + b) - tanh(b), evaluated
        //    through ADAA-1. The bias is folded into the shaper's input, so the
        //    antialiasing sees exactly the curve that is applied; the constant
        //    is subtracted afterwards so the branch carries no steady-state DC.
        {
            const TanhShaper shaper;
            const auto biasOffset = std::tanh (shaperBias);

            for (size_t channel = 0; channel < numChannels; ++channel)
            {
                auto* data = branch.getChannelPointer (channel);
                auto& state = shaperState[channel];

                for (size_t sample = 0; sample < numSamples; ++sample)
                {
                    const auto shaped = state.process (shaperDriveGain * static_cast<double> (data[sample]) + shaperBias, shaper);
                    data[sample] = static_cast<float> (shaped - biasOffset);
                }
            }
        }

        // 3. Band-limit to the growl formant. This - not a pre-filter - is
        //    what guarantees the sub-fundamental never sees the nonlinearity's
        //    output: 24 dB/octave below the lower corner puts the fundamental
        //    region far below anything audible in the branch.
        formantHighPass1.process (context);
        formantHighPass2.process (context);
        formantPeak.process (context);
        formantLowPass1.process (context);
        formantLowPass2.process (context);

        // 4. Blend on top of the untouched dry low band. The gain is ramped
        //    per sample (one value shared across channels, so the image cannot
        //    shift mid-ramp), which is what makes enabling/disabling Graaawl -
        //    or automating its amount - a fade rather than a step.
        for (size_t sample = 0; sample < numSamples; ++sample)
        {
            const auto gain = growlGain.getNextValue();

            for (size_t channel = 0; channel < numChannels; ++channel)
                block.getChannelPointer (channel)[sample] += gain * branch.getChannelPointer (channel)[sample];
        }
    }
}
