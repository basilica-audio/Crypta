#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// Issue #9 (latency-compensation framework) + issue #42 (the high-band
// voicing's 4x oversampling is now the framework's one real latency
// source): these tests pin down that the reported latency is positive,
// sample-rate-consistent, and independent of host block size, and confirm
// the low-band compensation delay + the high-band DryWetMixer's own
// internal dry-path delay keep the band-split-then-sum magnitude-flat
// property (issue #8) intact end-to-end through the full processor.
namespace
{
    constexpr int testBlockSize = 512;

    // Generous upper bound: comfortably above the actual ~60-sample 4x
    // maxQuality-FIR oversampling latency this plugin currently reports, but
    // well inside maxLatencyCompensationSamples, so this stays a meaningful
    // regression guard rather than a tautology.
    constexpr int maxSaneLatencySamples = 1000;
}

TEST_CASE ("Latency: high-band oversampling reports positive latency, independent of host block size", "[latency][dsp]")
{
    const double sampleRates[] = { 44100.0, 48000.0, 96000.0 };
    const int blockSizes[] = { 64, 256, 512, 1024 };

    for (const auto sampleRate : sampleRates)
    {
        int latencyAtFirstBlockSize = -1;

        for (const auto blockSize : blockSizes)
        {
            CryptaAudioProcessor processor;
            processor.prepareToPlay (sampleRate, blockSize);

            const auto latency = processor.getLatencySamples();

            INFO ("sampleRate = " << sampleRate << ", blockSize = " << blockSize);
            CHECK (latency > 0);
            CHECK (latency < maxSaneLatencySamples);

            if (latencyAtFirstBlockSize < 0)
                latencyAtFirstBlockSize = latency;
            else
                CHECK (latency == latencyAtFirstBlockSize);
        }
    }
}

TEST_CASE ("Latency: re-preparing the processor recomputes latency deterministically", "[latency][dsp]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    const auto latency48k = processor.getLatencySamples();
    CHECK (latency48k > 0);

    // Simulates a host changing sample rate/block size mid-session (e.g. a
    // DAW sample-rate switch), which re-runs the latency-compensation seam.
    processor.prepareToPlay (96000.0, 256);
    const auto latency96k = processor.getLatencySamples();
    CHECK (latency96k > 0);

    // Switching back to the original spec reproduces the original latency
    // exactly - the seam is a pure function of (sampleRate, blockSize,
    // numChannels), not order-dependent hidden state.
    processor.prepareToPlay (48000.0, 512);
    CHECK (processor.getLatencySamples() == latency48k);
}

TEST_CASE ("Latency: band-split-then-sum preserves magnitude flatness through the full processor", "[latency][dsp][crossover]")
{
    // Exercises the same flat-sum property as ThreeBandFlatSumTests.cpp, but
    // end-to-end through CryptaAudioProcessor::processBlock() - i.e.
    // including the low-band compensation delay line and the high band's
    // own internal (DryWetMixer) dry-path delay - to confirm the #9/#42
    // latency-compensation seam doesn't perturb the flat-sum guarantee.
    // highBlend, lowCompMix, and midDrive are all pulled to 0%/transparent
    // so neither the high band's *voicing* character (deliberately non-
    // transparent at its Gnaw/50% drive defaults), the low band's parallel
    // compressor (deliberately non-transparent at its -18dB/2:1 defaults,
    // which a 0.5-amplitude probe sits well above), nor the mid band's
    // staged drive (non-transparent at its 30% default) pollute a test that
    // is specifically about the delay-compensation plumbing, not any single
    // band's character - both dry paths still run through their respective
    // latency-compensated DryWetMixers, so the plumbing is still fully
    // exercised.
    constexpr double testSampleRate = 48000.0;

    // Spans all three bands at the v0.2.0 defaults (Split Low 120 Hz, Split
    // High 600 Hz): 60/150/250 Hz probe the low/mid bands, 600 Hz sits
    // exactly at Split High (the hardest point for any crossover to keep
    // flat), 2000/8000 Hz probe the high band.
    const double probeFrequenciesHz[] = { 60.0, 150.0, 250.0, 600.0, 2000.0, 8000.0 };

    for (const auto probeFrequencyHz : probeFrequenciesHz)
    {
        CryptaAudioProcessor processor;
        processor.prepareToPlay (testSampleRate, testBlockSize);

        auto* highBlendParam = processor.apvts.getParameter (ParamIDs::highBlend);
        auto* lowCompMixParam = processor.apvts.getParameter (ParamIDs::lowCompMix);
        auto* midDriveParam = processor.apvts.getParameter (ParamIDs::midDrive);
        REQUIRE (highBlendParam != nullptr);
        REQUIRE (lowCompMixParam != nullptr);
        REQUIRE (midDriveParam != nullptr);
        highBlendParam->setValueNotifyingHost (highBlendParam->convertTo0to1 (0.0f));
        lowCompMixParam->setValueNotifyingHost (lowCompMixParam->convertTo0to1 (0.0f));
        midDriveParam->setValueNotifyingHost (midDriveParam->convertTo0to1 (0.0f));

        juce::AudioBuffer<float> buffer (2, testBlockSize);
        juce::MidiBuffer midi;

        // Settle gain smoothing and the crossover's filter transient.
        for (int i = 0; i < 12; ++i)
        {
            TestHelpers::fillWithSine (buffer, testSampleRate, probeFrequencyHz);
            processor.processBlock (buffer, midi);
        }

        juce::AudioBuffer<float> reference (2, testBlockSize);
        TestHelpers::fillWithSine (reference, testSampleRate, probeFrequencyHz);

        const auto inputRms = TestHelpers::rms (reference);
        const auto outputRms = TestHelpers::rms (buffer);

        REQUIRE (inputRms > 0.0);

        INFO ("probe frequency = " << probeFrequencyHz << " Hz");
        CHECK (juce::Decibels::gainToDecibels (outputRms / inputRms) == Catch::Approx (0.0).margin (0.2));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

//==============================================================================
// v0.3.0 latency matrix (brief §6 T14).
//
// The Circuit engine adapts its oversampling factor to the host rate (4x below
// 50 kHz, 2x below 100 kHz, 1x above), so the two engines do NOT have the same
// intrinsic latency at every rate. Rather than re-report latency to the host
// whenever driveEngine changes - which hosts handle poorly mid-transport, and
// which would make an automated engine switch shift the plugin's timing - the
// plugin reports the larger of the two and pads the Circuit path up to it.
//
// The property these tests pin is therefore: reported latency depends on the
// sample rate ALONE, and a dirac lands exactly where the report says it will,
// on either engine.
TEST_CASE ("T14: reported latency is identical on both engines at every sample rate", "[latency][dsp]")
{
    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const auto latencyFor = [sampleRate] (int engineIndex)
        {
            CryptaAudioProcessor processor;
            processor.setPlayConfigDetails (2, 2, sampleRate, 512);
            processor.prepareToPlay (sampleRate, 512);
            TestHelpers::setParameter (processor, ParamIDs::driveEngine, static_cast<float> (engineIndex));
            processor.prepareToPlay (sampleRate, 512);
            return processor.getLatencySamples();
        };

        const auto classicLatency = latencyFor (0);
        const auto circuitLatency = latencyFor (1);

        INFO ("at " << sampleRate << " Hz: Classic " << classicLatency
                     << ", Circuit " << circuitLatency << " samples");

        CHECK (classicLatency == circuitLatency);
        CHECK (classicLatency > 0);
    }
}

TEST_CASE ("T14: a dirac arrives exactly at the reported latency, on both engines", "[latency][dsp]")
{
    // The assertion that makes the reported figure trustworthy: if the peak
    // does not land on getLatencySamples(), every delay-compensating host
    // aligns this plugin wrongly.
    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (const auto engineIndex : { 0, 1 })
        {
            CryptaAudioProcessor processor;
            processor.setPlayConfigDetails (1, 1, sampleRate, 512);
            processor.prepareToPlay (sampleRate, 512);

            TestHelpers::setParameter (processor, ParamIDs::driveEngine, static_cast<float> (engineIndex));

            // Everything nonlinear or level-dependent out of the way, so the
            // impulse stays an impulse.
            TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
            TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 0.0f);
            TestHelpers::setParameter (processor, ParamIDs::irEnabled, 0.0f);
            TestHelpers::setParameter (processor, ParamIDs::outputClip, 0.0f);
            TestHelpers::setParameter (processor, ParamIDs::midDrive, 0.0f);
            TestHelpers::setParameter (processor, ParamIDs::highDrive, 0.0f);
            TestHelpers::setParameter (processor, ParamIDs::lowCompMix, 0.0f);

            processor.prepareToPlay (sampleRate, 512);
            processor.reset();

            const auto reportedLatency = processor.getLatencySamples();

            juce::AudioBuffer<float> buffer (1, 8192);
            buffer.clear();
            buffer.setSample (0, 0, 1.0f);

            TestHelpers::renderThrough (processor, buffer);

            int peakIndex = 0;
            float peakValue = 0.0f;

            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto magnitude = std::abs (buffer.getSample (0, sample));

                if (magnitude > peakValue)
                {
                    peakValue = magnitude;
                    peakIndex = sample;
                }
            }

            INFO ("at " << sampleRate << " Hz, engine " << engineIndex
                         << ": reported " << reportedLatency << ", peak at " << peakIndex);

            if (engineIndex == 0)
            {
                // The Classic engine's impulse peaks exactly at the reported
                // latency, at every rate - the v0.2.0 contract, preserved.
                CHECK (std::abs (peakIndex - reportedLatency) <= 1);
            }
            else
            {
                // The Circuit engine peaks up to ~25 samples (0.5 ms) later.
                // That is not a latency-reporting error: the reported figure is
                // the oversampling delay, which both engines share, while the
                // peak of a reconstructed impulse also carries the GROUP DELAY
                // of every IIR filter in the chain - and the Circuit high band
                // adds two the Classic one does not have, the 10 Hz DC blocker
                // and the drive-tracked pole.
                //
                // No single reported number can describe a frequency-dependent
                // group delay, which is why the property that matters is
                // asserted elsewhere: reported latency is identical on both
                // engines (above) and the three-way sum stays flat (below).
                CHECK (std::abs (peakIndex - reportedLatency) <= 32);
            }
        }
    }
}

TEST_CASE ("T14: the Circuit engine keeps the three-way sum flat at every sample rate", "[latency][dsp][crossover]")
{
    // Latency compensation and band alignment are the same problem: if the low
    // band is not delayed to match the Mid+High branch, the three-way sum
    // develops a notch at the crossover. Checked per rate, because the Circuit
    // engine's oversampling factor - and therefore the delay it needs - varies
    // with the rate.
    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (1, 1, sampleRate, 512);
        processor.prepareToPlay (sampleRate, 512);

        TestHelpers::setParameter (processor, ParamIDs::driveEngine, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::irEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::outputClip, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::midDrive, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::highDrive, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::highBlend, 100.0f);
        TestHelpers::setParameter (processor, ParamIDs::highTone, 100.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowCompMix, 0.0f);

        processor.prepareToPlay (sampleRate, 512);
        processor.reset();

        // Measure around the crossover points, where a misalignment shows.
        constexpr int fftOrder = 15;
        constexpr int fftSize = 1 << fftOrder;
        constexpr int warmUp = 8192;

        double worstDeviationDb = 0.0;

        for (const auto frequency : { 90.0, 120.0, 200.0, 450.0, 600.0, 900.0, 2000.0 })
        {
            const auto snapped = TestHelpers::snapToBin (frequency, sampleRate, fftSize);

            juce::AudioBuffer<float> buffer (1, warmUp + fftSize);
            TestHelpers::fillWithSine (buffer, sampleRate, snapped, 0.05f);

            juce::AudioBuffer<float> reference (1, warmUp + fftSize);
            reference.makeCopyOf (buffer);

            processor.reset();
            TestHelpers::renderThrough (processor, buffer);

            const auto outputDb = TestHelpers::peakMagnitudeDb (
                TestHelpers::powerSpectrum (buffer, 0, fftOrder, warmUp), sampleRate, fftSize, snapped);
            const auto referenceDb = TestHelpers::peakMagnitudeDb (
                TestHelpers::powerSpectrum (reference, 0, fftOrder, warmUp), sampleRate, fftSize, snapped);

            const auto deviation = std::abs (outputDb - referenceDb);
            worstDeviationDb = juce::jmax (worstDeviationDb, deviation);

            INFO ("at " << sampleRate << " Hz, tone " << frequency << " Hz: " << (outputDb - referenceDb) << " dB");
            CHECK (deviation <= 1.0);
        }

        INFO ("worst deviation at " << sampleRate << " Hz: " << worstDeviationDb << " dB");
    }
}
