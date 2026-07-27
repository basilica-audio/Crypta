#pragma once

#include "ADAAShaper.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <vector>

// The v0.3.0 safety clip (brief §3.4): an ADAA-antialiased ceiling clip at
// base rate, with no oversampling and no added latency.
//
// v0.2.0 applied a raw per-sample std::tanh to the whole mix, which aliases
// freely. The obvious fix - wrap that tanh in ADAA-1 - is a trap, and the
// brief is explicit about why: ADAA-1 of a function that is nearly LINEAR over
// the segment degenerates to the two-tap average (x[n] + x[n-1])/2. That is a
// lowpass with |H(f)| = cos(pi*f/fs), about -8.3 dB at 18 kHz at 48 kHz, plus
// half a sample of delay - applied to the entire mix whenever the safety clip
// is armed, even while nothing is anywhere near the ceiling.
//
// So the ADAA is applied to the RESIDUAL instead:
//
//   c    = ceiling as linear gain
//   r(x) = x - c*tanh(x/c)          the part clipping removes; ~x^3/(3c^2) for small x
//   y[n] = x[n] - ADAA1_r(x[n])
//
// F1_r(x) = x^2/2 - c^2 * ln cosh(x/c), which is closed form, so this costs one
// log1p and one exp per sample and no table.
//
// Because ADAA is linear in the shaped function, this is algebraically
// ADAA1(clip(x)) + (x[n] - x[n-1])/2 - i.e. the antialiased clipper PLUS an
// exact first-order compensator for the droop and delay the naive form would
// have introduced. The consequences are the point:
//
//   - below the ceiling the residual is ~0, so y ~ x: transparent by
//     construction, no droop, no half-sample delay;
//   - the antialiasing applies to the residual, which is where the aliasing
//     actually lives;
//   - the compensator is linear, so it creates no aliasing of its own.
namespace cryp
{
    class OutputClipper
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            states.assign (static_cast<size_t> (spec.numChannels), ADAAState {});
            reset();
        }

        void reset() noexcept
        {
            for (auto& state : states)
                state.reset();
        }

        // Ceiling in dBFS. Clamped to the parameter's own range; a ceiling of
        // 0 dBFS reproduces v0.2.0's implicit unity ceiling.
        void setCeilingDb (float newCeilingDb) noexcept
        {
            ceiling = juce::jlimit (0.0625, 1.0, // dbToGain(-24) .. unity
                                     std::pow (10.0, juce::jlimit (-12.0, 0.0, static_cast<double> (newCeilingDb)) / 20.0));
        }

        double getCeilingLinear() const noexcept { return ceiling; }

        void process (juce::dsp::AudioBlock<float>& block) noexcept
        {
            const auto numChannels = juce::jmin (block.getNumChannels(), states.size());
            const auto numSamples = block.getNumSamples();

            const ResidualCurve curve { ceiling };

            for (size_t channel = 0; channel < numChannels; ++channel)
            {
                auto* data = block.getChannelPointer (channel);
                auto& state = states[channel];

                for (size_t sample = 0; sample < numSamples; ++sample)
                {
                    // Guard the shaper's input. A NaN or a wild value from an
                    // upstream bug must not become a NaN in the ADAA state,
                    // where it would persist for every subsequent sample.
                    auto x = static_cast<double> (data[sample]);

                    if (! std::isfinite (x))
                        x = 0.0;

                    x = juce::jlimit (-16.0, 16.0, x);

                    const auto shaped = x - state.process (x, curve);

                    // Final hard bound at the ceiling.
                    //
                    // The delta form is ADAA1(clip(x)) + (x[n] - x[n-1])/2, and
                    // that second term is a first-order difference: on
                    // fast-moving material it can push a sample back OVER the
                    // ceiling the clipper just enforced. Measured at 1.15
                    // against a ceiling of 1.0 on a loud 1 kHz sine, which
                    // would make this a tone shaper rather than a safety clip.
                    //
                    // The clamp costs almost nothing in the terms that made
                    // the delta form worth having: below the ceiling the
                    // residual is ~0 and the clamp never engages, so
                    // transparency is untouched; above it the ADAA has already
                    // done the antialiasing and the clamp is only trimming the
                    // small overshoot the compensator added.
                    data[sample] = static_cast<float> (juce::jlimit (-ceiling, ceiling, shaped));
                }
            }
        }

    private:
        // The residual r(x) = x - c*tanh(x/c) and its antiderivative
        // F1_r(x) = x^2/2 - c^2*ln cosh(x/c).
        struct ResidualCurve
        {
            double ceiling = 1.0;

            double f (double x) const noexcept { return x - ceiling * std::tanh (x / ceiling); }

            double antiderivative (double x) const noexcept
            {
                return 0.5 * x * x - ceiling * ceiling * TanhCurve::antiderivative (x / ceiling);
            }
        };

        double ceiling = 1.0;
        std::vector<ADAAState> states;
    };
}
