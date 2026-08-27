#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "params/ParameterIds.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <limits>
#include <random>
#include <vector>

// Adversarial input (issue #34, the v1.0.0 measurement gate): *no NaN/Inf ever
// reaches the output under full-scale, DC, impulse, silence, denormal-range
// input, and sample-rate / block-size changes mid-stream.*
//
// The suite already has per-stage denormal and extreme-parameter sweeps, and a
// whole-processor fuzz that randomises every parameter every block. What it did
// not have is the other axis: a hostile SIGNAL against a fixed, ordinary
// configuration. Those find different bugs. A parameter fuzz cannot produce a
// DC-locked input, and a per-stage sweep cannot produce the interaction between
// a full-scale Nyquist square and an oversampler's polyphase history.
//
// THE BOUND
// ---------
// `adversarialCeiling` = 4.0 (+12 dBFS), the same figure and the same
// derivation tests/ParameterSweepTests.cpp uses: the safety clipper is the last
// stage in the chain, every band level is at its 0 dB default here, and no
// input in this file exceeds full scale - so an output above +12 dBFS is a
// runaway rather than a user asking for gain. It is deliberately a bound on
// *divergence*, not a transparency claim: a hard-clipping drive stage fed a
// full-scale square is supposed to be loud.
//
// The NaN case is stated honestly rather than aspirationally. A host that
// delivers NaN has already broken its contract, and no plugin can produce a
// meaningful sample from one. What IS a fair requirement, and what is asserted
// here, is that the plugin does not crash on it and that it RECOVERS: after the
// poisoned block and a reset(), the next clean signal must render finite and at
// the level it would have had if the NaN had never happened.
namespace
{
    constexpr double adversarialSampleRate = 48000.0;
    constexpr int adversarialBlockSize = 256;
    constexpr float adversarialCeiling = 4.0f;

    enum class Signal
    {
        silence,
        fullScaleSine,
        positiveDc,
        negativeDc,
        impulse,
        nyquistSquare,
        denormalNoise,
        fullScaleChirp
    };

    const char* nameOf (Signal signal)
    {
        switch (signal)
        {
            case Signal::silence:        return "digital silence";
            case Signal::fullScaleSine:  return "full-scale 110 Hz sine";
            case Signal::positiveDc:     return "+1.0 DC";
            case Signal::negativeDc:     return "-1.0 DC";
            case Signal::impulse:        return "single full-scale impulse";
            case Signal::nyquistSquare:  return "full-scale alternating +-1 (Nyquist)";
            case Signal::denormalNoise:  return "denormal-range noise";
            case Signal::fullScaleChirp: return "full-scale 20 Hz - 20 kHz chirp";
        }

        return "?";
    }

    void fill (juce::AudioBuffer<float>& buffer, Signal signal, juce::int64 startSample, double sampleRate)
    {
        const auto numSamples = buffer.getNumSamples();

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* data = buffer.getWritePointer (channel);

            for (int sample = 0; sample < numSamples; ++sample)
            {
                const auto absolute = startSample + sample;
                float value = 0.0f;

                switch (signal)
                {
                    case Signal::silence:
                        value = 0.0f;
                        break;

                    case Signal::fullScaleSine:
                        value = static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * 110.0
                                                               * static_cast<double> (absolute) / sampleRate));
                        break;

                    case Signal::positiveDc:
                        value = 1.0f;
                        break;

                    case Signal::negativeDc:
                        value = -1.0f;
                        break;

                    case Signal::impulse:
                        value = absolute == 0 ? 1.0f : 0.0f;
                        break;

                    case Signal::nyquistSquare:
                        value = (absolute % 2) == 0 ? 1.0f : -1.0f;
                        break;

                    case Signal::denormalNoise:
                    {
                        // 1e-40 is inside the denormal range for float
                        // (smallest normal 1.18e-38), so every sample here is a
                        // number the FPU has to flush rather than compute with.
                        const auto pseudo = std::sin (0.37 * static_cast<double> (absolute));
                        value = static_cast<float> (pseudo * 1.0e-40);
                        break;
                    }

                    case Signal::fullScaleChirp:
                    {
                        // Linear sweep 20 Hz - 20 kHz over 2 s, restarting; the
                        // phase is computed from the absolute sample index so
                        // the sweep is continuous across block boundaries.
                        const auto seconds = static_cast<double> (absolute) / sampleRate;
                        const auto period = std::fmod (seconds, 2.0);
                        const auto phase = juce::MathConstants<double>::twoPi
                                            * (20.0 * period + 0.5 * (19980.0 / 2.0) * period * period);
                        value = static_cast<float> (std::sin (phase));
                        break;
                    }
                }

                data[sample] = value;
            }
        }
    }

    void configureHostile (CryptaAudioProcessor& processor, int engineIndex)
    {
        TestHelpers::setParameter (processor, ParamIDs::driveEngine, static_cast<float> (engineIndex));
        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 1.0f);
        // CUTS, not boosts. The EQ is engaged here to put four more biquads
        // with real state into the path, not to ask for level: a first version
        // of this case boosted the low shelf and a Q=5 peak by 12 dB each and
        // then failed its own +12 dBFS ceiling at 8.2 on a full-scale chirp -
        // which was the configuration asking for 24 dB of gain and getting it,
        // not the plugin diverging. Cuts keep the filters (and their ringing on
        // an impulse, and their behaviour on DC) in the signal path while
        // leaving the ceiling below a statement about divergence.
        TestHelpers::setParameter (processor, ParamIDs::eqLowShelfGain, -12.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqPeak1Gain, -12.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqPeak1Q, 5.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqHighShelfGain, -12.0f);
        TestHelpers::setParameter (processor, ParamIDs::midDrive, 100.0f);
        TestHelpers::setParameter (processor, ParamIDs::highDrive, 100.0f);
        TestHelpers::setParameter (processor, ParamIDs::highBias, 100.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowGrowl, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowGrowlAmount, 100.0f);
        // Levels and gains deliberately left at their 0 dB defaults, so the
        // ceiling below stays a statement about divergence rather than about
        // requested gain.
    }
}

//==============================================================================
TEST_CASE ("Adversarial input: every degenerate signal renders finite and bounded on both engines",
           "[robustness][adversarial]")
{
    const std::vector<Signal> signals {
        Signal::silence, Signal::fullScaleSine, Signal::positiveDc, Signal::negativeDc,
        Signal::impulse, Signal::nyquistSquare, Signal::denormalNoise, Signal::fullScaleChirp
    };

    for (const auto engineIndex : { 0, 1 })
    {
        for (const auto signal : signals)
        {
            INFO ("engine " << engineIndex << ", signal " << nameOf (signal));

            CryptaAudioProcessor processor;
            processor.setPlayConfigDetails (2, 2, adversarialSampleRate, adversarialBlockSize);
            processor.prepareToPlay (adversarialSampleRate, adversarialBlockSize);
            configureHostile (processor, engineIndex);
            processor.reset();

            juce::AudioBuffer<float> block (2, adversarialBlockSize);
            juce::MidiBuffer midi;

            float peak = 0.0f;
            auto allFinite = true;

            // 2 s: long enough for the gate to have opened and closed, the
            // compressor's release to have run, and the convolution and
            // oversampling histories to be entirely made of this signal.
            const auto blocks = static_cast<int> (2.0 * adversarialSampleRate / adversarialBlockSize);

            for (int index = 0; index < blocks; ++index)
            {
                fill (block, signal, static_cast<juce::int64> (index) * adversarialBlockSize, adversarialSampleRate);
                processor.processBlock (block, midi);

                allFinite = allFinite && TestHelpers::allSamplesFinite (block);
                peak = juce::jmax (peak, block.getMagnitude (0, block.getNumSamples()));
            }

            INFO ("peak " << peak << ", ceiling " << adversarialCeiling);
            CHECK (allFinite);
            CHECK (peak < adversarialCeiling);
        }
    }
}

TEST_CASE ("Adversarial input: the block size changing every block, including zero and oversized",
           "[robustness][adversarial][chunking]")
{
    // A host is allowed to hand a different number of samples every callback,
    // including zero (JUCE's own offline render does), and - although it should
    // not - more than prepareToPlay() was promised. processChunk() exists for
    // that last case; this is the assertion that the whole cycle survives being
    // driven that way continuously rather than once.
    const std::vector<int> sizes { 0, 1, 7, 64, 33, 512, 1024, 2048, 3, 128, 0, 4096 };

    for (const auto engineIndex : { 0, 1 })
    {
        INFO ("engine " << engineIndex);

        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, adversarialSampleRate, 512);
        processor.prepareToPlay (adversarialSampleRate, 512);
        configureHostile (processor, engineIndex);
        processor.reset();

        juce::AudioBuffer<float> block (2, 4096);
        juce::MidiBuffer midi;

        juce::int64 position = 0;
        float peak = 0.0f;
        auto allFinite = true;

        for (int pass = 0; pass < 40; ++pass)
        {
            for (const auto size : sizes)
            {
                block.setSize (2, size, false, false, true);

                if (size > 0)
                    fill (block, Signal::fullScaleChirp, position, adversarialSampleRate);

                processor.processBlock (block, midi);

                if (size > 0)
                {
                    allFinite = allFinite && TestHelpers::allSamplesFinite (block);
                    peak = juce::jmax (peak, block.getMagnitude (0, size));
                }

                position += size;
            }
        }

        INFO ("peak " << peak);
        CHECK (allFinite);
        CHECK (peak < adversarialCeiling);
    }
}

TEST_CASE ("Adversarial input: the sample rate changing mid-stream, repeatedly, under signal",
           "[robustness][adversarial][sample-rate]")
{
    // A host changing the session rate calls prepareToPlay() again on a plugin
    // that is already full of state from the previous rate. This drives that
    // transition thirty times without ever letting the signal stop, and
    // additionally checks the plugin re-reports a latency after each one -
    // a stale latency figure is a silent PDC bug rather than a crash.
    const std::vector<double> rates { 44100.0, 96000.0, 48000.0, 192000.0, 88200.0, 48000.0 };

    for (const auto engineIndex : { 0, 1 })
    {
        INFO ("engine " << engineIndex);

        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, 48000.0, 512);
        processor.prepareToPlay (48000.0, 512);
        configureHostile (processor, engineIndex);

        juce::AudioBuffer<float> block (2, 512);
        juce::MidiBuffer midi;

        juce::int64 position = 0;
        float peak = 0.0f;
        auto allFinite = true;

        for (int pass = 0; pass < 5; ++pass)
        {
            for (const auto rate : rates)
            {
                processor.setPlayConfigDetails (2, 2, rate, 512);
                processor.prepareToPlay (rate, 512);

                // Deliberately NO reset() here: a host is not required to call
                // one, and state carried across a rate change is exactly what
                // could go non-finite.
                INFO ("rate " << rate << ", reported latency " << processor.getLatencySamples());
                CHECK (processor.getLatencySamples() > 0);

                for (int index = 0; index < 20; ++index)
                {
                    fill (block, Signal::fullScaleChirp, position, rate);
                    processor.processBlock (block, midi);

                    allFinite = allFinite && TestHelpers::allSamplesFinite (block);
                    peak = juce::jmax (peak, block.getMagnitude (0, block.getNumSamples()));
                    position += 512;
                }
            }
        }

        INFO ("peak " << peak);
        CHECK (allFinite);
        CHECK (peak < adversarialCeiling);
    }
}

TEST_CASE ("Adversarial input: a NaN/Inf-poisoned block does not crash, and the plugin recovers",
           "[robustness][adversarial]")
{
    // What can honestly be asked of a plugin handed NaN: survive it, and come
    // back. The level assertion after the reset is what makes "recovers"
    // testable - a chain left holding a NaN in a filter state would emit NaN
    // or silence forever, and both are distinguishable from a correct render.
    for (const auto engineIndex : { 0, 1 })
    {
        INFO ("engine " << engineIndex);

        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, adversarialSampleRate, adversarialBlockSize);
        processor.prepareToPlay (adversarialSampleRate, adversarialBlockSize);
        configureHostile (processor, engineIndex);
        processor.reset();

        juce::AudioBuffer<float> block (2, adversarialBlockSize);
        juce::MidiBuffer midi;

        // Reference: what a clean signal renders to on a clean instance.
        const auto renderCleanRms = [&] (CryptaAudioProcessor& target)
        {
            juce::AudioBuffer<float> local (2, adversarialBlockSize);
            juce::MidiBuffer localMidi;
            double sumOfSquares = 0.0;
            juce::int64 counted = 0;

            for (int index = 0; index < 200; ++index)
            {
                TestHelpers::fillWithSine (local, adversarialSampleRate, 110.0, 0.5f,
                                            static_cast<juce::int64> (index) * adversarialBlockSize);
                target.processBlock (local, localMidi);

                if (index >= 100)
                {
                    for (int channel = 0; channel < 2; ++channel)
                    {
                        const auto* data = local.getReadPointer (channel);

                        for (int sample = 0; sample < adversarialBlockSize; ++sample)
                        {
                            sumOfSquares += static_cast<double> (data[sample]) * static_cast<double> (data[sample]);
                            ++counted;
                        }
                    }
                }
            }

            return std::sqrt (sumOfSquares / static_cast<double> (juce::jmax<juce::int64> (1, counted)));
        };

        const auto before = renderCleanRms (processor);
        REQUIRE (before > 1.0e-3);

        // Poison it.
        for (int index = 0; index < 4; ++index)
        {
            for (int channel = 0; channel < 2; ++channel)
            {
                auto* data = block.getWritePointer (channel);

                for (int sample = 0; sample < adversarialBlockSize; ++sample)
                    data[sample] = (sample % 3 == 0) ? std::numeric_limits<float>::quiet_NaN()
                                  : (sample % 3 == 1) ? std::numeric_limits<float>::infinity()
                                                      : -std::numeric_limits<float>::infinity();
            }

            processor.processBlock (block, midi); // must not crash
        }

        // The documented recovery path: the host stops, which calls reset().
        processor.reset();

        const auto after = renderCleanRms (processor);

        INFO ("clean RMS before " << before << ", after " << after
              << ", ratio " << (after / juce::jmax (1.0e-12, before)));

        CHECK (std::isfinite (after));
        // Same signal, same settings, same settled state: the two renders must
        // agree to within 0.1 dB. A wider tolerance would let a chain that came
        // back at half level read as "recovered".
        CHECK (std::abs (juce::Decibels::gainToDecibels (after / juce::jmax (1.0e-12, before))) < 0.1);
    }
}
