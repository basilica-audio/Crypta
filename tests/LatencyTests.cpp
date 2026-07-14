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
            TwistYourGutsAudioProcessor processor;
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
    TwistYourGutsAudioProcessor processor;
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
    // Exercises the same flat-sum property as CrossoverTests.cpp, but end-
    // to-end through TwistYourGutsAudioProcessor::processBlock() - i.e.
    // including the low-band compensation delay line and the high-band
    // voicing's own internal (DryWetMixer) dry-path delay - to confirm the
    // #9/#42 latency-compensation seam doesn't perturb the #8 flat-sum
    // guarantee. highBlend and lowCompMix are pulled to 0% (fully dry) so
    // neither the high band's *voicing* character (deliberately non-
    // transparent at its Gnaw/50% drive defaults) nor the low band's
    // parallel compressor (deliberately non-transparent at its -18dB/4:1
    // defaults, which a 0.5-amplitude probe sits well above) pollute a test
    // that is specifically about the delay-compensation plumbing, not
    // either stage's character - both dry paths still run through their
    // respective latency-compensated DryWetMixers, so the plumbing is still
    // fully exercised.
    constexpr double testSampleRate = 48000.0;

    const double probeFrequenciesHz[] = { 60.0, 150.0, 250.0, 600.0, 2000.0, 8000.0 };

    for (const auto probeFrequencyHz : probeFrequenciesHz)
    {
        TwistYourGutsAudioProcessor processor;
        processor.prepareToPlay (testSampleRate, testBlockSize);

        auto* highBlendParam = processor.apvts.getParameter (ParamIDs::highBlend);
        auto* lowCompMixParam = processor.apvts.getParameter (ParamIDs::lowCompMix);
        REQUIRE (highBlendParam != nullptr);
        REQUIRE (lowCompMixParam != nullptr);
        highBlendParam->setValueNotifyingHost (highBlendParam->convertTo0to1 (0.0f));
        lowCompMixParam->setValueNotifyingHost (lowCompMixParam->convertTo0to1 (0.0f));

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
