#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_CASE ("Denormal-range input produces no NaN/Inf output", "[robustness]")
{
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto* inputGainParam = processor.apvts.getParameter (ParamIDs::inputGain);
    auto* outputGainParam = processor.apvts.getParameter (ParamIDs::outputGain);
    REQUIRE (inputGainParam != nullptr);
    REQUIRE (outputGainParam != nullptr);

    // Exercise the gain multiply with a non-unity gain so denormals actually
    // propagate through the multiplication rather than being a no-op.
    inputGainParam->setValueNotifyingHost (inputGainParam->convertTo0to1 (12.0f));
    outputGainParam->setValueNotifyingHost (outputGainParam->convertTo0to1 (12.0f));

    constexpr int numSamples = 512;
    juce::AudioBuffer<float> buffer (2, numSamples);

    const auto denormalValue = std::numeric_limits<float>::denorm_min() * 4.0f;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        auto* data = buffer.getWritePointer (channel);

        for (int sample = 0; sample < numSamples; ++sample)
            data[sample] = (sample % 2 == 0) ? denormalValue : -denormalValue;
    }

    juce::MidiBuffer midi;

    CHECK_NOTHROW (processor.processBlock (buffer, midi));
    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("Zero-sample buffer does not crash processBlock", "[robustness]")
{
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 0);
    juce::MidiBuffer midi;

    CHECK_NOTHROW (processor.processBlock (buffer, midi));
    CHECK (buffer.getNumSamples() == 0);
    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("Zero-sample buffer does not crash when bypassed", "[robustness]")
{
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto* bypassParam = processor.apvts.getParameter (ParamIDs::bypass);
    REQUIRE (bypassParam != nullptr);
    bypassParam->setValueNotifyingHost (1.0f);

    juce::AudioBuffer<float> buffer (2, 0);
    juce::MidiBuffer midi;

    CHECK_NOTHROW (processor.processBlock (buffer, midi));
}
