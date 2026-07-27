#include "GateEngine.h"

namespace
{
    // Detector RMS window. Short enough to catch a pick attack, long enough
    // not to follow individual cycles.
    constexpr double detectorTimeConstantMs = 5.0;

    // "Non-linear capacitor" anti-chatter smoothing: when the detected level
    // is barely moving, smooth it heavily so noise cannot rattle the state
    // machine; when it jumps, get out of the way so transients are not
    // rounded off.
    constexpr double slowSmoothingMs = 30.0;
    constexpr double fastSmoothingMs = 2.0;
    constexpr double slewThresholdDb = 1.5;

    double onePoleCoefficient (double milliseconds, double sampleRate) noexcept
    {
        return 1.0 - std::exp (-1.0 / (juce::jmax (1.0e-4, milliseconds) * 0.001 * sampleRate));
    }
}

namespace cryp
{
    void GateEngine::prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        sidechainFilters.assign (static_cast<size_t> (spec.numChannels), SidechainFilter {});

        updateCoefficients();
        updateSidechainCoefficients();
        reset();
    }

    void GateEngine::reset()
    {
        for (auto& filter : sidechainFilters)
            filter.reset();

        meanSquare = 0.0;
        smoothedLevelDb = -120.0;
        state = State::closed;
        holdSamplesRemaining = 0.0;

        // Start closed. A gate that reset to "open" would let through the
        // first block after a transport stop, which is precisely the noise it
        // exists to remove.
        currentGainDb = -static_cast<double> (rangeDb);
        lastGainReductionDb = rangeDb;
    }

    void GateEngine::updateCoefficients() noexcept
    {
        attackCoefficient = onePoleCoefficient (attackMs, sampleRate);
        meanSquareCoefficient = std::exp (-1.0 / (detectorTimeConstantMs * 0.001 * sampleRate));
        fastSmoothingCoefficient = onePoleCoefficient (fastSmoothingMs, sampleRate);
        slowSmoothingCoefficient = onePoleCoefficient (slowSmoothingMs, sampleRate);
    }

    void GateEngine::updateSidechainCoefficients() noexcept
    {
        // RBJ highpass at Q = 1/sqrt(2) (Butterworth).
        constexpr double q = 0.70710678118654752;

        const auto w0 = juce::MathConstants<double>::twoPi
                         * juce::jlimit (1.0, sampleRate * 0.45, static_cast<double> (sidechainHz)) / sampleRate;
        const auto cosW0 = std::cos (w0);
        const auto alpha = std::sin (w0) / (2.0 * q);
        const auto a0 = 1.0 + alpha;

        sidechainB0 = ((1.0 + cosW0) * 0.5) / a0;
        sidechainB1 = (-(1.0 + cosW0)) / a0;
        sidechainB2 = ((1.0 + cosW0) * 0.5) / a0;
        sidechainA1 = (-2.0 * cosW0) / a0;
        sidechainA2 = (1.0 - alpha) / a0;
    }

    void GateEngine::process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto numChannels = juce::jmin (block.getNumChannels(), sidechainFilters.size());
        const auto numSamples = block.getNumSamples();

        if (numChannels == 0)
            return;

        // Push the current coefficients into the filters without touching
        // their state, so a sidechain sweep does not click.
        for (auto& filter : sidechainFilters)
        {
            filter.b0 = sidechainB0;
            filter.b1 = sidechainB1;
            filter.b2 = sidechainB2;
            filter.a1 = sidechainA1;
            filter.a2 = sidechainA2;
        }

        const auto openThresholdDb = static_cast<double> (thresholdDb);
        const auto closeThresholdDb = openThresholdDb - static_cast<double> (hysteresisDb);
        const auto floorDb = -static_cast<double> (rangeDb);

        // dB-linear release: a straight line from fully open to the floor in
        // exactly `release` milliseconds, i.e. range/release dB per second.
        const auto releaseDbPerSample = (static_cast<double> (rangeDb)
                                          / (static_cast<double> (releaseMs) * 0.001))
                                         / sampleRate;

        const auto holdSamples = static_cast<double> (holdMs) * 0.001 * sampleRate;

        double blockMaximumReduction = 0.0;

        for (size_t sample = 0; sample < numSamples; ++sample)
        {
            // Detector: sidechain-highpassed, linked across channels by taking
            // the largest magnitude.
            double detectorInput = 0.0;

            for (size_t channel = 0; channel < numChannels; ++channel)
            {
                const auto filtered = sidechainFilters[channel].process (
                    static_cast<double> (block.getChannelPointer (channel)[sample]));
                detectorInput = juce::jmax (detectorInput, std::abs (filtered));
            }

            meanSquare = meanSquareCoefficient * meanSquare
                          + (1.0 - meanSquareCoefficient) * detectorInput * detectorInput;

            const auto levelDb = 10.0 * std::log10 (meanSquare + 1.0e-30);

            // Slew-dependent smoothing (the "non-linear capacitor").
            const auto slewDb = std::abs (levelDb - smoothedLevelDb);
            const auto smoothing = slewDb < slewThresholdDb ? slowSmoothingCoefficient : fastSmoothingCoefficient;
            smoothedLevelDb += smoothing * (levelDb - smoothedLevelDb);

            // State machine. The two thresholds are what make this a gate
            // rather than an oscillator around one level.
            switch (state)
            {
                case State::closed:
                    if (smoothedLevelDb > openThresholdDb)
                        state = State::open;
                    break;

                case State::open:
                    if (smoothedLevelDb < closeThresholdDb)
                    {
                        state = State::holding;
                        holdSamplesRemaining = holdSamples;
                    }
                    break;

                case State::holding:
                    // Retriggering: a new transient during the hold sends the
                    // gate straight back to open rather than letting the hold
                    // expire.
                    if (smoothedLevelDb > openThresholdDb)
                    {
                        state = State::open;
                    }
                    else
                    {
                        holdSamplesRemaining -= 1.0;

                        if (holdSamplesRemaining <= 0.0)
                            state = State::releasing;
                    }
                    break;

                case State::releasing:
                    if (smoothedLevelDb > openThresholdDb)
                        state = State::open;
                    break;
            }

            // Gain trajectory.
            if (state == State::open || state == State::holding)
            {
                // Exponential attack towards unity.
                currentGainDb += attackCoefficient * (0.0 - currentGainDb);
            }
            else if (state == State::releasing)
            {
                currentGainDb = juce::jmax (floorDb, currentGainDb - releaseDbPerSample);
            }
            else
            {
                currentGainDb = floorDb;
            }

            const auto gain = std::pow (10.0, currentGainDb / 20.0);

            for (size_t channel = 0; channel < numChannels; ++channel)
                block.getChannelPointer (channel)[sample] =
                    static_cast<float> (static_cast<double> (block.getChannelPointer (channel)[sample]) * gain);

            blockMaximumReduction = juce::jmax (blockMaximumReduction, -currentGainDb);
        }

        lastGainReductionDb = static_cast<float> (blockMaximumReduction);
    }
}
