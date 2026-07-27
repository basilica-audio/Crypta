#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

// Small shared helpers used across the Tests target. Kept dependency-free
// (just juce_audio_basics) so it can be included from any test file.
namespace TestHelpers
{
    // Fills every channel of the buffer with a sine wave of the given
    // frequency. `startSampleIndex` offsets the phase as if this buffer were
    // a continuation of a longer signal starting at absolute sample 0 - pass
    // 0 (the default) for phase-at-zero-on-each-call behaviour (fine for
    // memoryless gain-only passthrough checks), or the running sample count
    // when feeding a filter/IIR-stateful processor block-by-block, where a
    // phase discontinuity at every block boundary would inject spurious
    // broadband energy into the filter and pollute level measurements.
    inline void fillWithSine (juce::AudioBuffer<float>& buffer,
                              double sampleRate,
                              double frequencyHz,
                              float amplitude = 0.5f,
                              juce::int64 startSampleIndex = 0)
    {
        const auto numChannels = buffer.getNumChannels();
        const auto numSamples = buffer.getNumSamples();

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto* data = buffer.getWritePointer (channel);

            for (int sample = 0; sample < numSamples; ++sample)
            {
                const auto phase = juce::MathConstants<double>::twoPi * frequencyHz
                                    * static_cast<double> (startSampleIndex + sample) / sampleRate;
                data[sample] = amplitude * static_cast<float> (std::sin (phase));
            }
        }
    }

    // Root-mean-square level across all channels/samples in the buffer.
    inline double rms (const juce::AudioBuffer<float>& buffer)
    {
        double sumOfSquares = 0.0;
        juce::int64 numValues = 0;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto value = static_cast<double> (data[sample]);
                sumOfSquares += value * value;
                ++numValues;
            }
        }

        return numValues > 0 ? std::sqrt (sumOfSquares / static_cast<double> (numValues)) : 0.0;
    }

    // Returns true if every sample in the buffer is finite (no NaN/Inf).
    inline bool allSamplesFinite (const juce::AudioBuffer<float>& buffer)
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                if (! std::isfinite (data[sample]))
                    return false;
        }

        return true;
    }

    //==========================================================================
    // Spectral analysis harness for the v0.3.0 DSP assertions (alias floors,
    // THD profiles, filter corners, transparency contracts). Methodology
    // follows research-oversampling-architecture.md §5: large FFT, 4-term
    // Blackman-Harris window, warm-up discarded.

    // Sets a parameter by its plain (unnormalised) value.
    inline void setParameter (juce::AudioProcessor& processor, const juce::String& id, float plainValue)
    {
        auto* apvtsParameter = processor.getParameters().getFirst();
        juce::ignoreUnused (apvtsParameter);

        for (auto* parameter : processor.getParameters())
        {
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
            {
                if (ranged->paramID == id)
                {
                    ranged->setValueNotifyingHost (ranged->convertTo0to1 (plainValue));
                    return;
                }
            }
        }

        jassertfalse; // unknown parameter ID
    }

    // Renders `buffer` through `processor` in place, in fixed-size blocks.
    // The processor must already have been prepared.
    inline void renderThrough (juce::AudioProcessor& processor, juce::AudioBuffer<float>& buffer, int blockSize = 512)
    {
        const auto numChannels = buffer.getNumChannels();
        const auto numSamples = buffer.getNumSamples();

        juce::AudioBuffer<float> block (numChannels, blockSize);
        juce::MidiBuffer midi;

        for (int offset = 0; offset < numSamples; offset += blockSize)
        {
            const auto length = juce::jmin (blockSize, numSamples - offset);
            block.setSize (numChannels, length, false, false, true);

            for (int channel = 0; channel < numChannels; ++channel)
                block.copyFrom (channel, 0, buffer, channel, offset, length);

            processor.processBlock (block, midi);

            for (int channel = 0; channel < numChannels; ++channel)
                buffer.copyFrom (channel, offset, block, channel, 0, length);
        }
    }

    // Snaps `frequencyHz` to the nearest exact FFT bin centre. Analysing a
    // bin-centred tone is what keeps spectral leakage from masquerading as
    // distortion when the assertion floor is -80 dB.
    inline double snapToBin (double frequencyHz, double sampleRate, int fftSize)
    {
        const auto binWidth = sampleRate / static_cast<double> (fftSize);
        return std::round (frequencyHz / binWidth) * binWidth;
    }

    // Power spectrum of one channel, windowed with a 4-term Blackman-Harris
    // (-92 dB sidelobes). Returns fftSize/2 + 1 bins.
    inline std::vector<double> powerSpectrum (const juce::AudioBuffer<float>& buffer,
                                               int channel,
                                               int fftOrder,
                                               int startSample = 0)
    {
        const auto fftSize = 1 << fftOrder;
        jassert (buffer.getNumSamples() >= startSample + fftSize);

        juce::dsp::FFT fft (fftOrder);
        std::vector<float> data (static_cast<size_t> (fftSize) * 2, 0.0f);

        const auto* source = buffer.getReadPointer (channel);

        // 4-term Blackman-Harris.
        constexpr double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;

        for (int index = 0; index < fftSize; ++index)
        {
            const auto phase = juce::MathConstants<double>::twoPi * static_cast<double> (index)
                                / static_cast<double> (fftSize - 1);
            const auto window = a0 - a1 * std::cos (phase) + a2 * std::cos (2.0 * phase) - a3 * std::cos (3.0 * phase);
            data[static_cast<size_t> (index)] = static_cast<float> (source[startSample + index] * window);
        }

        fft.performFrequencyOnlyForwardTransform (data.data());

        std::vector<double> power (static_cast<size_t> (fftSize / 2 + 1));

        for (size_t bin = 0; bin < power.size(); ++bin)
        {
            const auto magnitude = static_cast<double> (data[bin]);
            power[bin] = magnitude * magnitude;
        }

        return power;
    }

    // Ratio of non-harmonic in-band energy to the fundamental's energy, in dB
    // - i.e. the alias-to-signal ratio a Plugin-Doctor style sweep reports.
    //
    // Every bin within `excludeBinRadius` of an integer multiple of
    // `fundamentalHz` is treated as harmonic (wanted) content and excluded;
    // the radius must cover the window's mainlobe, which for 4-term
    // Blackman-Harris is 8 bins wide.
    inline double aliasToSignalRatioDb (const std::vector<double>& power,
                                         double sampleRate,
                                         int fftSize,
                                         double fundamentalHz,
                                         double analysisLowHz = 20.0,
                                         double analysisHighHz = 20000.0,
                                         int excludeBinRadius = 12)
    {
        const auto binWidth = sampleRate / static_cast<double> (fftSize);
        const auto numBins = static_cast<int> (power.size());

        const auto fundamentalBin = static_cast<int> (std::round (fundamentalHz / binWidth));

        double fundamentalPower = 0.0;

        for (int bin = fundamentalBin - excludeBinRadius; bin <= fundamentalBin + excludeBinRadius; ++bin)
            if (bin >= 0 && bin < numBins)
                fundamentalPower += power[static_cast<size_t> (bin)];

        const auto lowBin = juce::jmax (1, static_cast<int> (std::floor (analysisLowHz / binWidth)));
        const auto highBin = juce::jmin (numBins - 1, static_cast<int> (std::ceil (analysisHighHz / binWidth)));

        double aliasPower = 0.0;

        for (int bin = lowBin; bin <= highBin; ++bin)
        {
            // Distance to the nearest harmonic of the fundamental, in bins.
            const auto harmonicIndex = std::round (static_cast<double> (bin) / static_cast<double> (fundamentalBin));
            const auto nearestHarmonicBin = harmonicIndex * static_cast<double> (fundamentalBin);

            if (std::abs (static_cast<double> (bin) - nearestHarmonicBin) <= static_cast<double> (excludeBinRadius))
                continue;

            aliasPower += power[static_cast<size_t> (bin)];
        }

        if (fundamentalPower <= 0.0)
            return 0.0;

        return 10.0 * std::log10 (juce::jmax (1.0e-30, aliasPower / fundamentalPower));
    }

    // Magnitude, in dB, of a single spectral peak near `frequencyHz` (summed
    // over the window mainlobe so the result does not depend on exact bin
    // alignment).
    inline double peakMagnitudeDb (const std::vector<double>& power,
                                    double sampleRate,
                                    int fftSize,
                                    double frequencyHz,
                                    int binRadius = 12)
    {
        const auto binWidth = sampleRate / static_cast<double> (fftSize);
        const auto centreBin = static_cast<int> (std::round (frequencyHz / binWidth));
        const auto numBins = static_cast<int> (power.size());

        double total = 0.0;

        for (int bin = centreBin - binRadius; bin <= centreBin + binRadius; ++bin)
            if (bin >= 0 && bin < numBins)
                total += power[static_cast<size_t> (bin)];

        return 10.0 * std::log10 (juce::jmax (1.0e-30, total));
    }
}
