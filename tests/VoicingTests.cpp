#include "dsp/Voicing.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>

// Issue #42: high-band distortion voicing (Gnaw/Wool/Razor) acceptance
// gates.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int numSamples = 2048;

    juce::dsp::ProcessSpec makeSpec (int numChannels = 1)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (numSamples);
        spec.numChannels = static_cast<juce::uint32> (numChannels);
        return spec;
    }

    constexpr std::array<tyg::VoicingType, 3> allVoicings {
        tyg::VoicingType::gnaw, tyg::VoicingType::wool, tyg::VoicingType::razor
    };
}

TEST_CASE ("Voicing: 0% blend is a passthrough within a small margin (dry-path DryWetMixer delay compensation)", "[voicing][dsp]")
{
    // Not bit-exact: the dry path runs through a Thiran (fractional-
    // interpolation-capable) delay line inside DryWetMixer even when the
    // requested delay is an exact integer sample count, so a tiny amount of
    // allpass ripple is expected - this checks level transparency, the way
    // GainProcessingTests.cpp's passthrough test does for the full chain.
    for (const auto voicing : allVoicings)
    {
        tyg::Voicing voicingUnderTest;
        voicingUnderTest.prepare (makeSpec(), 0.0f);
        voicingUnderTest.setVoicing (voicing);
        voicingUnderTest.setDrive (1.0f);
        voicingUnderTest.setTone (0.5f);

        juce::AudioBuffer<float> buffer (1, numSamples);
        TestHelpers::fillWithSine (buffer, testSampleRate, 1000.0, 0.5f);

        juce::AudioBuffer<float> reference;
        reference.makeCopyOf (buffer);

        juce::dsp::AudioBlock<float> block (buffer);
        voicingUnderTest.process (block);

        const auto outRms = TestHelpers::rms (buffer);
        const auto refRms = TestHelpers::rms (reference);

        INFO ("voicing index = " << static_cast<int> (voicing));
        CHECK (juce::Decibels::gainToDecibels (outRms / refRms) == Catch::Approx (0.0).margin (0.2));
    }
}

TEST_CASE ("Voicing: reports positive, plausible oversampling latency after prepare()", "[voicing][dsp]")
{
    tyg::Voicing voicing;
    CHECK (voicing.getLatencySamples() == 0);

    voicing.prepare (makeSpec(), 1.0f);
    CHECK (voicing.getLatencySamples() > 0);
    CHECK (voicing.getLatencySamples() < 1000);
}

TEST_CASE ("Voicing: no NaN/Inf and no runaway output at extreme drive for every voicing", "[voicing][dsp][robustness]")
{
    // The waveshapers themselves (tanh/hard-clip) are always bounded to
    // [-1, 1], but the post-shaper mid-hump/scoop peak filter can still
    // legitimately push a transient above unity when it's boosting (e.g.
    // Razor's +5dB hump) - that's ordinary EQ-after-clipper behaviour, not a
    // bug, so this checks for finiteness and the absence of a runaway
    // feedback-loop-style blow-up rather than a strict unity ceiling.
    for (const auto voicingType : allVoicings)
    {
        tyg::Voicing voicing;
        voicing.prepare (makeSpec(), 1.0f);
        voicing.setVoicing (voicingType);
        voicing.setDrive (1.0f); // maximum drive
        voicing.setTone (0.5f);

        // A hot input (well above 0dBFS) is exactly when an unbounded
        // waveshaper would misbehave.
        juce::AudioBuffer<float> buffer (1, numSamples);
        TestHelpers::fillWithSine (buffer, testSampleRate, 300.0, 4.0f);

        juce::dsp::AudioBlock<float> block (buffer);
        CHECK_NOTHROW (voicing.process (block));
        CHECK (TestHelpers::allSamplesFinite (buffer));

        const auto* data = buffer.getReadPointer (0);

        for (int sample = 0; sample < numSamples; ++sample)
            CHECK (std::abs (data[sample]) <= 10.0f);
    }
}

TEST_CASE ("Voicing: higher drive increases harmonic energy for every voicing", "[voicing][dsp]")
{
    // A pure sine driven harder through a nonlinearity gains energy outside
    // its fundamental (harmonics), so a simple, topology-agnostic proxy for
    // "more drive audibly changes the signal" is that the *difference*
    // between the low-drive and high-drive outputs is clearly non-zero.
    for (const auto voicingType : allVoicings)
    {
        tyg::Voicing lowDriveVoicing;
        lowDriveVoicing.prepare (makeSpec(), 1.0f);
        lowDriveVoicing.setVoicing (voicingType);
        lowDriveVoicing.setDrive (0.0f);
        lowDriveVoicing.setTone (0.5f);

        tyg::Voicing highDriveVoicing;
        highDriveVoicing.prepare (makeSpec(), 1.0f);
        highDriveVoicing.setVoicing (voicingType);
        highDriveVoicing.setDrive (1.0f);
        highDriveVoicing.setTone (0.5f);

        juce::AudioBuffer<float> lowDriveBuffer (1, numSamples);
        TestHelpers::fillWithSine (lowDriveBuffer, testSampleRate, 300.0, 0.7f);
        juce::dsp::AudioBlock<float> lowDriveBlock (lowDriveBuffer);
        lowDriveVoicing.process (lowDriveBlock);

        juce::AudioBuffer<float> highDriveBuffer (1, numSamples);
        TestHelpers::fillWithSine (highDriveBuffer, testSampleRate, 300.0, 0.7f);
        juce::dsp::AudioBlock<float> highDriveBlock (highDriveBuffer);
        highDriveVoicing.process (highDriveBlock);

        juce::AudioBuffer<float> difference;
        difference.makeCopyOf (highDriveBuffer);
        difference.addFrom (0, 0, lowDriveBuffer, 0, 0, numSamples, -1.0f);

        INFO ("voicing index = " << static_cast<int> (voicingType));
        CHECK (TestHelpers::rms (difference) > 0.01);
    }
}

TEST_CASE ("Voicing: no NaN/Inf across a denormal-range sweep for every voicing", "[voicing][dsp][robustness]")
{
    for (const auto voicingType : allVoicings)
    {
        tyg::Voicing voicing;
        voicing.prepare (makeSpec (2), 1.0f);
        voicing.setVoicing (voicingType);
        voicing.setDrive (1.0f);
        voicing.setTone (1.0f);

        juce::AudioBuffer<float> buffer (2, numSamples);
        const auto denormalValue = std::numeric_limits<float>::denorm_min() * 4.0f;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* data = buffer.getWritePointer (channel);

            for (int sample = 0; sample < numSamples; ++sample)
                data[sample] = (sample % 2 == 0) ? denormalValue : -denormalValue;
        }

        juce::dsp::AudioBlock<float> block (buffer);
        CHECK_NOTHROW (voicing.process (block));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

TEST_CASE ("Voicing: switching voicing mid-stream never produces NaN/Inf", "[voicing][dsp][robustness]")
{
    tyg::Voicing voicing;
    voicing.prepare (makeSpec (2), 1.0f);
    voicing.setDrive (0.8f);
    voicing.setTone (0.3f);

    juce::AudioBuffer<float> buffer (2, numSamples);

    for (int iteration = 0; iteration < 12; ++iteration)
    {
        voicing.setVoicing (allVoicings[static_cast<size_t> (iteration) % allVoicings.size()]);
        TestHelpers::fillWithSine (buffer, testSampleRate, 250.0, 0.6f, static_cast<juce::int64> (iteration) * numSamples);

        juce::dsp::AudioBlock<float> block (buffer);
        CHECK_NOTHROW (voicing.process (block));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}
