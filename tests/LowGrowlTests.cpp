#include "AllocationGuard.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "dsp/LowGrowl.h"
#include "params/ParameterIds.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

// MEASURED evidence for the "Graaawl" low-band growl mode (issue #36).
//
// The acceptance criteria the issue sets are all spectral or structural, so
// they are all asserted as numbers here:
//   - growl OFF leaves the low band bit-identical (structural bypass);
//   - growl ON puts its energy in the 700 Hz - 2.2 kHz formant window;
//   - the sub band stays clean - the fundamental and its first harmonics are
//     not measurably touched;
//   - the shaper does not alias into the audio band;
//   - the stage reports (and costs) no latency, so nothing needs compensating;
//   - switching it on is a ramp, not a step;
//   - no allocation on the audio thread.
//
// Method matches the rest of the suite: bin-centred sine, 2^15 FFT, 4-term
// Blackman-Harris window, warm-up discarded (TestHelpers.h).
namespace
{
    constexpr double growlSampleRate = 48000.0;
    constexpr int growlFftOrder = 15;
    constexpr int growlFftSize = 1 << growlFftOrder;
    constexpr int growlWarmUpSamples = 8192;
    constexpr int growlBlockSize = 2048;

    // Bass-register probe. Snapped to a bin so the analysis is leakage-free;
    // deliberately NOT a rational fraction of the sample rate, so aliased
    // products land off the harmonic grid where aliasToSignalRatioDb() can see
    // them (fs/f0 = 963.9, and gcd(34 bins, 32768 bins) = 2, so folded
    // harmonics do not hide on harmonic bins).
    const double growlProbeHz = TestHelpers::snapToBin (50.0, growlSampleRate, growlFftSize);

    juce::dsp::ProcessSpec makeGrowlSpec (int numChannels = 1)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = growlSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (growlBlockSize);
        spec.numChannels = static_cast<juce::uint32> (numChannels);
        return spec;
    }

    struct GrowlSetup
    {
        bool enabled = true;
        float amount01 = 1.0f;
        float tone01 = 0.5f;
    };

    juce::AudioBuffer<float> renderThroughGrowl (const GrowlSetup& setup, double frequencyHz, float amplitude)
    {
        cryp::LowGrowl growl;
        growl.setEnabled (setup.enabled);
        growl.setAmount (setup.amount01);
        growl.setTone (setup.tone01);
        growl.prepare (makeGrowlSpec());

        const auto totalSamples = growlWarmUpSamples + growlFftSize;
        juce::AudioBuffer<float> buffer (1, totalSamples);

        for (int offset = 0; offset < totalSamples; offset += growlBlockSize)
        {
            const auto length = juce::jmin (growlBlockSize, totalSamples - offset);

            juce::AudioBuffer<float> block (1, length);
            TestHelpers::fillWithSine (block, growlSampleRate, frequencyHz, amplitude, offset);

            juce::dsp::AudioBlock<float> audioBlock (block);
            growl.process (audioBlock);

            buffer.copyFrom (0, offset, block, 0, 0, length);
        }

        return buffer;
    }

    // Total power in [lowHz, highHz], in dB.
    double bandEnergyDb (const std::vector<double>& power, double lowHz, double highHz)
    {
        const auto binWidth = growlSampleRate / static_cast<double> (growlFftSize);
        const auto firstBin = juce::jmax (1, static_cast<int> (std::floor (lowHz / binWidth)));
        const auto lastBin = juce::jmin (static_cast<int> (power.size()) - 1, static_cast<int> (std::ceil (highHz / binWidth)));

        double total = 0.0;

        for (int bin = firstBin; bin <= lastBin; ++bin)
            total += power[static_cast<size_t> (bin)];

        return 10.0 * std::log10 (juce::jmax (1.0e-30, total));
    }

    std::vector<double> spectrumOf (const GrowlSetup& setup, float amplitude = 0.25f)
    {
        const auto rendered = renderThroughGrowl (setup, growlProbeHz, amplitude);
        return TestHelpers::powerSpectrum (rendered, 0, growlFftOrder, growlWarmUpSamples);
    }

    // Deterministic non-sine material, for the bit-exactness checks - a sine is
    // a weak witness for "nothing happened at all".
    void fillWithProgramMaterial (juce::AudioBuffer<float>& buffer, int startSample)
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* data = buffer.getWritePointer (channel);

            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto t = static_cast<double> (startSample + sample) / growlSampleRate;
                const auto value = 0.42 * std::sin (juce::MathConstants<double>::twoPi * 47.0 * t)
                                    + 0.19 * std::sin (juce::MathConstants<double>::twoPi * 93.0 * t + 0.7)
                                    + 0.07 * std::sin (juce::MathConstants<double>::twoPi * 131.0 * t + 2.1);
                data[sample] = static_cast<float> (value);
            }
        }
    }
}

//==============================================================================
TEST_CASE ("Graaawl: switched off, the stage is a bit-exact bypass", "[growl][dsp]")
{
    // Not "transparent within a margin" - bit-identical. This is the property
    // that lets the feature ship without a state migration: every pre-v0.4.0
    // session and preset lands on lowGrowl = off and must render exactly what
    // v0.3.0 rendered.
    cryp::LowGrowl growl;
    growl.setEnabled (false);
    growl.setAmount (1.0f); // amount is deliberately non-zero: OFF must win
    growl.prepare (makeGrowlSpec (2));

    juce::AudioBuffer<float> buffer (2, growlBlockSize);

    for (int block = 0; block < 8; ++block)
    {
        fillWithProgramMaterial (buffer, block * growlBlockSize);

        juce::AudioBuffer<float> reference;
        reference.makeCopyOf (buffer);

        juce::dsp::AudioBlock<float> audioBlock (buffer);
        growl.process (audioBlock);

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                REQUIRE (buffer.getSample (channel, sample) == reference.getSample (channel, sample));
    }
}

TEST_CASE ("Graaawl: after being switched off it returns to bit-exact bypass", "[growl][dsp]")
{
    // The ramp has to finish before the bypass can be structural again, so the
    // question is whether it ever does - a stage that keeps a residual smoothed
    // gain forever would keep processing forever.
    cryp::LowGrowl growl;
    growl.setEnabled (true);
    growl.setAmount (1.0f);
    growl.prepare (makeGrowlSpec (2));

    juce::AudioBuffer<float> buffer (2, growlBlockSize);

    for (int block = 0; block < 4; ++block)
    {
        fillWithProgramMaterial (buffer, block * growlBlockSize);
        juce::dsp::AudioBlock<float> audioBlock (buffer);
        growl.process (audioBlock);
    }

    growl.setEnabled (false);

    // One block covers the 20 ms ramp at this block size (2048 samples =
    // 42.7 ms), so from the second block on the bypass must be exact.
    for (int block = 0; block < 3; ++block)
    {
        fillWithProgramMaterial (buffer, block * growlBlockSize);

        juce::AudioBuffer<float> reference;
        reference.makeCopyOf (buffer);

        juce::dsp::AudioBlock<float> audioBlock (buffer);
        growl.process (audioBlock);

        if (block == 0)
            continue; // the ramp-down block itself is expected to differ

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                REQUIRE (buffer.getSample (channel, sample) == reference.getSample (channel, sample));
    }
}

TEST_CASE ("Graaawl: the growl lands in the 700 Hz - 2.2 kHz formant window", "[growl][dsp][spectral]")
{
    const auto withGrowl = spectrumOf ({ true, 1.0f, 0.5f });
    const auto without = spectrumOf ({ false, 1.0f, 0.5f });

    const auto formantOn = bandEnergyDb (withGrowl, 700.0, 2200.0);
    const auto formantOff = bandEnergyDb (without, 700.0, 2200.0);

    INFO ("formant band: " << formantOn << " dB with growl, " << formantOff << " dB without");

    // The band goes from "nothing but the analysis floor of a pure sine" to
    // real signal.
    CHECK (formantOn > formantOff + 40.0);

    // And the energy is concentrated there rather than smeared: the octave
    // above the window is well below the window itself.
    const auto aboveWindow = bandEnergyDb (withGrowl, 4400.0, 8800.0);
    INFO ("4.4-8.8 kHz: " << aboveWindow << " dB");
    CHECK (aboveWindow < formantOn - 24.0);
}

TEST_CASE ("Graaawl: the sub band is not touched - fundamental and low harmonics are unchanged", "[growl][dsp][spectral]")
{
    // The defining constraint of the whole design (issue #36: "sub band below
    // ~150 Hz THD stays negligible"). The growl branch is band-limited AFTER
    // the shaper, so what reaches the sum has nothing below the formant window
    // - and that has to be true to a fraction of a dB, not approximately.
    const auto withGrowl = spectrumOf ({ true, 1.0f, 0.5f });
    const auto without = spectrumOf ({ false, 1.0f, 0.5f });

    const auto fundamentalOn = TestHelpers::peakMagnitudeDb (withGrowl, growlSampleRate, growlFftSize, growlProbeHz);
    const auto fundamentalOff = TestHelpers::peakMagnitudeDb (without, growlSampleRate, growlFftSize, growlProbeHz);

    INFO ("fundamental: " << fundamentalOn << " dB with growl, " << fundamentalOff << " dB without");
    CHECK (fundamentalOn == Catch::Approx (fundamentalOff).margin (0.05));

    // The harmonics that would betray a shaper running on the sub itself: 2f
    // and 3f both sit below 150 Hz. What reaches them is the branch's own
    // harmonic content leaking through the stopband of the formant highpass -
    // 24 dB/octave, so 3f at 149 Hz is about 54 dB down on top of whatever
    // level the harmonic had inside the branch. Measured at -51 dBc, i.e.
    // 0.3 % on a pure-sine probe and some 30 dB below the third harmonic real
    // bass material carries anyway.
    for (const auto harmonic : { 2.0, 3.0 })
    {
        const auto frequency = growlProbeHz * harmonic;
        const auto onDb = TestHelpers::peakMagnitudeDb (withGrowl, growlSampleRate, growlFftSize, frequency);

        INFO ("harmonic " << harmonic << " at " << frequency << " Hz: " << onDb << " dB, fundamental " << fundamentalOn << " dB");
        CHECK (onDb < fundamentalOn - 45.0);
    }

    // Whole-band statement of the same thing: everything below 150 Hz is
    // within a hair of what it was without the growl.
    const auto subOn = bandEnergyDb (withGrowl, 20.0, 150.0);
    const auto subOff = bandEnergyDb (without, 20.0, 150.0);
    INFO ("sub band 20-150 Hz: " << subOn << " dB vs " << subOff << " dB");
    CHECK (subOn == Catch::Approx (subOff).margin (0.1));
}

TEST_CASE ("Graaawl: Amount scales the growl and 0 % is silent", "[growl][dsp][spectral]")
{
    const auto silent = bandEnergyDb (spectrumOf ({ true, 0.0f, 0.5f }), 700.0, 2200.0);
    const auto half = bandEnergyDb (spectrumOf ({ true, 0.5f, 0.5f }), 700.0, 2200.0);
    const auto full = bandEnergyDb (spectrumOf ({ true, 1.0f, 0.5f }), 700.0, 2200.0);
    const auto disabled = bandEnergyDb (spectrumOf ({ false, 1.0f, 0.5f }), 700.0, 2200.0);

    INFO ("formant band: 0 % " << silent << " dB, 50 % " << half << " dB, 100 % " << full
                                << " dB, mode disabled " << disabled << " dB");

    // Amount is a linear gain on the branch, so half of it is 6 dB down.
    CHECK (full - half == Catch::Approx (6.0).margin (0.5));

    // Amount 0 with the mode ON is indistinguishable from the mode being OFF -
    // both sit on the analysis floor of the probe tone itself, which is what
    // "silent" can mean at all here (the floor is around -83 dB, so a
    // "60 dB below full scale" style assertion would be asserting the FFT's
    // own leakage, not the DSP).
    CHECK (silent == Catch::Approx (disabled).margin (0.5));
    CHECK (silent < full - 50.0);
}

TEST_CASE ("Graaawl: Tone moves the formant", "[growl][dsp][spectral]")
{
    // Tone sweeps the band's centre across 800-1600 Hz. Measured as the
    // balance between the bottom and the top of the sweep range, which moves
    // in the right direction by a large margin rather than by a hair.
    const auto darkSpectrum = spectrumOf ({ true, 1.0f, 0.0f });
    const auto brightSpectrum = spectrumOf ({ true, 1.0f, 1.0f });

    const auto darkBalance = bandEnergyDb (darkSpectrum, 450.0, 700.0) - bandEnergyDb (darkSpectrum, 2200.0, 3400.0);
    const auto brightBalance = bandEnergyDb (brightSpectrum, 450.0, 700.0) - bandEnergyDb (brightSpectrum, 2200.0, 3400.0);

    INFO ("low/high balance: dark " << darkBalance << " dB, bright " << brightBalance << " dB");
    CHECK (darkBalance > brightBalance + 10.0);

    cryp::LowGrowl growl;
    growl.setTone (0.5f);
    CHECK (growl.getFormantCentreHz() == Catch::Approx (1131.0).margin (5.0));
}

TEST_CASE ("Graaawl: the shaper does not alias into the audio band", "[growl][dsp][aliasing]")
{
    // The design choice this measurement exists to justify: the branch runs at
    // BASE rate with ADAA-1 rather than inside an oversampling region, which is
    // what makes the stage zero-latency. That is only defensible if the
    // aliasing it produces is inaudible, so it is measured rather than argued.
    //
    // The probe is deliberately hot (-6 dBFS) and the growl is at 100 %.
    const auto spectrum = spectrumOf ({ true, 1.0f, 0.5f }, 0.5f);
    const auto aliasDb = TestHelpers::aliasToSignalRatioDb (spectrum, growlSampleRate, growlFftSize, growlProbeHz);

    INFO ("alias-to-signal " << aliasDb << " dB");
    CHECK (aliasDb < -80.0);
}

TEST_CASE ("Graaawl: enabling it is a ramp, not a step", "[growl][dsp]")
{
    // A hard switch on a running signal is a click. The stage ramps its blend
    // gain over 20 ms; this measures the largest sample-to-sample jump across
    // the switch and compares it against the material's own largest jump.
    cryp::LowGrowl growl;
    growl.setEnabled (false);
    growl.setAmount (1.0f);
    growl.prepare (makeGrowlSpec());

    juce::AudioBuffer<float> buffer (1, growlBlockSize);

    const auto largestStep = [] (const juce::AudioBuffer<float>& target)
    {
        float largest = 0.0f;
        const auto* data = target.getReadPointer (0);

        for (int sample = 1; sample < target.getNumSamples(); ++sample)
            largest = juce::jmax (largest, std::abs (data[sample] - data[sample - 1]));

        return largest;
    };

    fillWithProgramMaterial (buffer, 0);
    const auto materialStep = largestStep (buffer);

    juce::dsp::AudioBlock<float> firstBlock (buffer);
    growl.process (firstBlock);

    growl.setEnabled (true);

    fillWithProgramMaterial (buffer, growlBlockSize);
    juce::dsp::AudioBlock<float> switchBlock (buffer);
    growl.process (switchBlock);

    const auto switchStep = largestStep (buffer);

    INFO ("largest step across the switch " << switchStep << ", material's own " << materialStep);

    // The growl adds harmonics, so the switched-on block legitimately has
    // faster slew than the dry material; what must not happen is a
    // discontinuity, i.e. a step of the order of the signal's own amplitude.
    CHECK (switchStep < 4.0f * materialStep);
    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("Graaawl: no NaN/Inf on denormal or hot input, at any setting", "[growl][dsp][robustness]")
{
    for (const auto amount : { 0.0f, 0.5f, 1.0f })
    {
        for (const auto tone : { 0.0f, 0.5f, 1.0f })
        {
            cryp::LowGrowl growl;
            growl.setEnabled (true);
            growl.setAmount (amount);
            growl.setTone (tone);
            growl.prepare (makeGrowlSpec (2));

            juce::AudioBuffer<float> buffer (2, growlBlockSize);

            // Denormals.
            const auto denormalValue = std::numeric_limits<float>::denorm_min() * 4.0f;

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            {
                auto* data = buffer.getWritePointer (channel);

                for (int sample = 0; sample < growlBlockSize; ++sample)
                    data[sample] = (sample % 2 == 0) ? denormalValue : -denormalValue;
            }

            juce::dsp::AudioBlock<float> denormalBlock (buffer);
            CHECK_NOTHROW (growl.process (denormalBlock));
            CHECK (TestHelpers::allSamplesFinite (buffer));

            // Far above full scale.
            TestHelpers::fillWithSine (buffer, growlSampleRate, 45.0, 8.0f);
            juce::dsp::AudioBlock<float> hotBlock (buffer);
            CHECK_NOTHROW (growl.process (hotBlock));
            CHECK (TestHelpers::allSamplesFinite (buffer));

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                for (int sample = 0; sample < growlBlockSize; ++sample)
                    CHECK (std::abs (buffer.getSample (channel, sample)) < 40.0f);
        }
    }
}

TEST_CASE ("Graaawl: the stage is latency-free, and the plugin's reported latency does not move with it", "[growl][dsp][latency]")
{
    cryp::LowGrowl growl;
    CHECK (growl.getLatencySamples() == 0);
    growl.prepare (makeGrowlSpec());
    CHECK (growl.getLatencySamples() == 0);

    // The plugin-level consequence: a growl-mode toggle can never desynchronise
    // the low band from the Mid+High branch, because there is nothing to
    // compensate.
    CryptaAudioProcessor processor;
    processor.setPlayConfigDetails (2, 2, growlSampleRate, 512);
    processor.prepareToPlay (growlSampleRate, 512);

    const auto latencyWithGrowlOff = processor.getLatencySamples();

    TestHelpers::setParameter (processor, ParamIDs::lowGrowl, 1.0f);
    TestHelpers::setParameter (processor, ParamIDs::lowGrowlAmount, 100.0f);
    processor.prepareToPlay (growlSampleRate, 512);

    CHECK (processor.getLatencySamples() == latencyWithGrowlOff);
}

TEST_CASE ("Graaawl: the low band is bit-identical after a toggle round-trip through the processor", "[growl][processor]")
{
    // Plugin-level version of the bypass guarantee, taken at the Low band's own
    // isolation tap so that nothing in the Mid/High branch can mask it.
    const auto renderLowBand = [] (bool enableGrowlForTheMiddleSection)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (1, 1, growlSampleRate, growlBlockSize);
        processor.prepareToPlay (growlSampleRate, growlBlockSize);

        juce::AudioBuffer<float> capture (1, growlBlockSize);
        processor.setLowBandIsolationCaptureForTests (&capture);

        juce::AudioBuffer<float> buffer (1, growlBlockSize);
        juce::MidiBuffer midi;

        for (int block = 0; block < 6; ++block)
        {
            if (enableGrowlForTheMiddleSection)
            {
                const auto growlOn = block >= 1 && block <= 3;
                TestHelpers::setParameter (processor, ParamIDs::lowGrowl, growlOn ? 1.0f : 0.0f);
                TestHelpers::setParameter (processor, ParamIDs::lowGrowlAmount, 100.0f);
            }

            fillWithProgramMaterial (buffer, block * growlBlockSize);
            processor.processBlock (buffer, midi);
        }

        processor.setLowBandIsolationCaptureForTests (nullptr);

        juce::AudioBuffer<float> result;
        result.makeCopyOf (capture);
        return result;
    };

    const auto neverEnabled = renderLowBand (false);
    const auto toggledOffAgain = renderLowBand (true);

    for (int sample = 0; sample < neverEnabled.getNumSamples(); ++sample)
        REQUIRE (toggledOffAgain.getSample (0, sample) == neverEnabled.getSample (0, sample));
}

TEST_CASE ("Graaawl: state round-trips", "[growl][state]")
{
    CryptaAudioProcessor source;
    TestHelpers::setParameter (source, ParamIDs::lowGrowl, 1.0f);
    TestHelpers::setParameter (source, ParamIDs::lowGrowlAmount, 42.0f);
    TestHelpers::setParameter (source, ParamIDs::lowGrowlTone, 73.0f);

    juce::MemoryBlock state;
    source.getStateInformation (state);

    CryptaAudioProcessor destination;
    destination.setStateInformation (state.getData(), static_cast<int> (state.getSize()));

    CHECK (destination.apvts.getRawParameterValue (ParamIDs::lowGrowl)->load() >= 0.5f);
    CHECK (destination.apvts.getRawParameterValue (ParamIDs::lowGrowlAmount)->load() == Catch::Approx (42.0f).margin (0.05));
    CHECK (destination.apvts.getRawParameterValue (ParamIDs::lowGrowlTone)->load() == Catch::Approx (73.0f).margin (0.05));
}

TEST_CASE ("Graaawl: processBlock() allocates nothing with the growl running", "[growl][robustness][realtime]")
{
    CryptaAudioProcessor processor;
    processor.setPlayConfigDetails (2, 2, growlSampleRate, 512);
    processor.prepareToPlay (growlSampleRate, 512);

    TestHelpers::setParameter (processor, ParamIDs::lowGrowl, 1.0f);
    TestHelpers::setParameter (processor, ParamIDs::lowGrowlAmount, 80.0f);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;

    // Warm up outside the counted region: the first blocks legitimately touch
    // lazily-initialised JUCE internals.
    for (int block = 0; block < 8; ++block)
    {
        TestHelpers::fillWithSine (buffer, growlSampleRate, 55.0, 0.5f, block * 512);
        processor.processBlock (buffer, midi);
    }

    long long allocations = 0;

    for (int block = 0; block < 200; ++block)
    {
        // The controls are swept WHILE counting - an automated parameter is
        // exactly where a hidden allocation (a coefficient object, a resized
        // buffer) would show up - but only processBlock() is inside the
        // counted region: setValueNotifyingHost() legitimately allocates on the
        // way in, and that is the host's thread, not the audio thread.
        TestHelpers::setParameter (processor, ParamIDs::lowGrowlTone, static_cast<float> (block % 101));
        TestHelpers::fillWithSine (buffer, growlSampleRate, 55.0, 0.5f, (8 + block) * 512);

        allocations += AllocationCounter::countDuring ([&] { processor.processBlock (buffer, midi); });
    }

    INFO (allocations << " allocations across 200 blocks with Graaawl running");
    CHECK (allocations == 0);
}

TEST_CASE ("Graaawl spectral table (measured, for the manual and the listening gate)", "[.growl-table]")
{
    std::cout << "\ntone  formantHz  700-2200Hz(dB)  20-150Hz delta(dB)  alias(dB)\n";

    for (const auto tone : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        cryp::LowGrowl probe;
        probe.setTone (tone);

        const auto withGrowl = spectrumOf ({ true, 1.0f, tone }, 0.5f);
        const auto without = spectrumOf ({ false, 1.0f, tone }, 0.5f);

        std::cout << std::fixed << std::setprecision (2) << tone << "  "
                   << std::setw (9) << std::setprecision (0) << probe.getFormantCentreHz() << "  "
                   << std::setw (14) << std::setprecision (1) << bandEnergyDb (withGrowl, 700.0, 2200.0) << "  "
                   << std::setw (18) << std::setprecision (3)
                   << (bandEnergyDb (withGrowl, 20.0, 150.0) - bandEnergyDb (without, 20.0, 150.0)) << "  "
                   << std::setw (9) << std::setprecision (1)
                   << TestHelpers::aliasToSignalRatioDb (withGrowl, growlSampleRate, growlFftSize, growlProbeHz) << "\n";
    }

    SUCCEED();
}
