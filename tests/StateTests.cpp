#include "PluginProcessor.h"
#include "params/ParameterIds.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE ("State round-trip preserves non-default parameter values", "[state]")
{
    TwistYourGutsAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto* inputGainParam = processor.apvts.getParameter (ParamIDs::inputGain);
    auto* outputGainParam = processor.apvts.getParameter (ParamIDs::outputGain);
    auto* bypassParam = processor.apvts.getParameter (ParamIDs::bypass);

    REQUIRE (inputGainParam != nullptr);
    REQUIRE (outputGainParam != nullptr);
    REQUIRE (bypassParam != nullptr);

    inputGainParam->setValueNotifyingHost (inputGainParam->convertTo0to1 (12.0f));
    outputGainParam->setValueNotifyingHost (outputGainParam->convertTo0to1 (-8.5f));
    bypassParam->setValueNotifyingHost (1.0f);

    const auto savedInputValue = inputGainParam->getValue();
    const auto savedOutputValue = outputGainParam->getValue();
    const auto savedBypassValue = bypassParam->getValue();

    juce::MemoryBlock savedState;
    processor.getStateInformation (savedState);
    REQUIRE (savedState.getSize() > 0);

    // Reset every parameter back to its default before restoring, so the
    // round-trip assertion below can't pass by accident.
    inputGainParam->setValueNotifyingHost (inputGainParam->getDefaultValue());
    outputGainParam->setValueNotifyingHost (outputGainParam->getDefaultValue());
    bypassParam->setValueNotifyingHost (bypassParam->getDefaultValue());

    REQUIRE (inputGainParam->getValue() != Catch::Approx (savedInputValue));
    REQUIRE (outputGainParam->getValue() != Catch::Approx (savedOutputValue));
    REQUIRE (bypassParam->getValue() != Catch::Approx (savedBypassValue));

    processor.setStateInformation (savedState.getData(), static_cast<int> (savedState.getSize()));

    CHECK (inputGainParam->getValue() == Catch::Approx (savedInputValue).margin (1e-6));
    CHECK (outputGainParam->getValue() == Catch::Approx (savedOutputValue).margin (1e-6));
    CHECK (bypassParam->getValue() == Catch::Approx (savedBypassValue).margin (1e-6));
}
