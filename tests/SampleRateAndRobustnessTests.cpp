#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

// Issue #43: broadened test coverage - sample-rate sweeps (44.1-192kHz),
// mono/stereo bus configurations, extreme parameter automation, and a
// long-run NaN/Inf stability soak, exercised through the full processor so
// every M1 DSP stage (gate, crossover, parallel compressor, voicing, EQ, IR
// loader) is covered together, the way a host would actually drive it.
namespace
{
    constexpr int testBlockSize = 512;

    // Every ParamIDs float/bool/choice parameter, driven to a pseudo-random
    // value each iteration. Keeping this list in one place means new
    // parameters are easy to fold into both the extreme-automation and
    // long-run soak tests below.
    void randomiseAllParameters (juce::AudioProcessorValueTreeState& apvts, juce::Random& random)
    {
        for (auto* parameter : apvts.processor.getParameters())
            parameter->setValueNotifyingHost (random.nextFloat());
    }
}

TEST_CASE ("Sample-rate sweep: processBlock stays finite and reports plausible latency from 44.1kHz to 192kHz", "[robustness][sample-rate]")
{
    const double sampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

    for (const auto sampleRate : sampleRates)
    {
        TwistYourGutsAudioProcessor processor;
        processor.prepareToPlay (sampleRate, testBlockSize);

        INFO ("sampleRate = " << sampleRate);
        CHECK (processor.getLatencySamples() > 0);
        CHECK (processor.getLatencySamples() < 4096);

        juce::AudioBuffer<float> buffer (2, testBlockSize);
        juce::MidiBuffer midi;

        for (int i = 0; i < 8; ++i)
        {
            TestHelpers::fillWithSine (buffer, sampleRate, 220.0,
                                        0.6f, static_cast<juce::int64> (i) * testBlockSize);
            CHECK_NOTHROW (processor.processBlock (buffer, midi));
            CHECK (TestHelpers::allSamplesFinite (buffer));
        }
    }
}

TEST_CASE ("Bus configuration: mono in/out processes without crashing or producing NaN/Inf", "[robustness][bus-layout]")
{
    TwistYourGutsAudioProcessor processor;

    juce::AudioProcessor::BusesLayout monoLayout;
    monoLayout.inputBuses.add (juce::AudioChannelSet::mono());
    monoLayout.outputBuses.add (juce::AudioChannelSet::mono());

    REQUIRE (processor.checkBusesLayoutSupported (monoLayout));
    REQUIRE (processor.setBusesLayout (monoLayout));

    processor.prepareToPlay (48000.0, testBlockSize);
    CHECK (processor.getTotalNumInputChannels() == 1);
    CHECK (processor.getTotalNumOutputChannels() == 1);

    juce::AudioBuffer<float> buffer (1, testBlockSize);
    juce::MidiBuffer midi;

    for (int i = 0; i < 8; ++i)
    {
        TestHelpers::fillWithSine (buffer, 48000.0, 220.0, 0.6f, static_cast<juce::int64> (i) * testBlockSize);
        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

TEST_CASE ("Bus configuration: stereo in/out (default) processes without crashing or producing NaN/Inf", "[robustness][bus-layout]")
{
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (48000.0, testBlockSize);
    CHECK (processor.getTotalNumInputChannels() == 2);
    CHECK (processor.getTotalNumOutputChannels() == 2);

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    juce::MidiBuffer midi;

    for (int i = 0; i < 8; ++i)
    {
        TestHelpers::fillWithSine (buffer, 48000.0, 220.0, 0.6f, static_cast<juce::int64> (i) * testBlockSize);
        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

TEST_CASE ("Extreme parameter automation: randomising every parameter every block never produces NaN/Inf", "[robustness][automation]")
{
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (48000.0, testBlockSize);

    juce::Random random (2026);
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    juce::MidiBuffer midi;

    for (int i = 0; i < 60; ++i)
    {
        randomiseAllParameters (processor.apvts, random);
        TestHelpers::fillWithSine (buffer, 48000.0, 100.0 + random.nextFloat() * 8000.0,
                                    0.8f, static_cast<juce::int64> (i) * testBlockSize);

        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

TEST_CASE ("Long-run stability: continuous processing over an extended run stays finite (no slow-building blowup)", "[robustness][long-run]")
{
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (48000.0, testBlockSize);

    // Enable every stage at once (gate, EQ, IR loader all default-off) so
    // the soak actually exercises the full chain, at settings well inside
    // normal musical use rather than another extreme sweep.
    auto setParam = [&] (const char* id, float value01)
    {
        auto* parameter = processor.apvts.getParameter (id);
        REQUIRE (parameter != nullptr);
        parameter->setValueNotifyingHost (value01);
    };

    setParam (ParamIDs::gateEnabled, 1.0f);
    setParam (ParamIDs::eqEnabled, 1.0f);
    setParam (ParamIDs::irEnabled, 1.0f);

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    juce::MidiBuffer midi;

    // ~500 blocks at 512 samples/48kHz is ~5.3 seconds of continuous audio -
    // long enough to catch a slowly diverging filter state, short enough to
    // stay well under a minute even on Debug CI.
    constexpr int numBlocks = 500;

    for (int i = 0; i < numBlocks; ++i)
    {
        const auto frequencyHz = 80.0 + 40.0 * std::sin (static_cast<double> (i) * 0.05);
        TestHelpers::fillWithSine (buffer, 48000.0, frequencyHz, 0.7f, static_cast<juce::int64> (i) * testBlockSize);

        processor.processBlock (buffer, midi);

        if (i % 50 == 0)
        {
            INFO ("block " << i);
            REQUIRE (TestHelpers::allSamplesFinite (buffer));
        }
    }

    CHECK (TestHelpers::allSamplesFinite (buffer));
}
