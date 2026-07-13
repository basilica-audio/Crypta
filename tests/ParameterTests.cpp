#include "PluginProcessor.h"
#include "params/ParameterIds.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE ("Processor instantiates with the expected parameters", "[processor][parameters]")
{
    TwistYourGutsAudioProcessor processor;

    SECTION ("plugin name")
    {
        CHECK (processor.getName() == juce::String ("Twist Your Guts"));
    }

    SECTION ("all parameter IDs exist")
    {
        CHECK (processor.apvts.getParameter (ParamIDs::inputGain) != nullptr);
        CHECK (processor.apvts.getParameter (ParamIDs::outputGain) != nullptr);
        CHECK (processor.apvts.getParameter (ParamIDs::bypass) != nullptr);
    }

    SECTION ("parameters have the documented default values")
    {
        auto* inputGainParam = processor.apvts.getParameter (ParamIDs::inputGain);
        auto* outputGainParam = processor.apvts.getParameter (ParamIDs::outputGain);
        auto* bypassParam = processor.apvts.getParameter (ParamIDs::bypass);

        REQUIRE (inputGainParam != nullptr);
        REQUIRE (outputGainParam != nullptr);
        REQUIRE (bypassParam != nullptr);

        // 0 dB for both gains, normalised against the -24..+24 range.
        CHECK (inputGainParam->getDefaultValue() == Catch::Approx (inputGainParam->convertTo0to1 (0.0f)));
        CHECK (outputGainParam->getDefaultValue() == Catch::Approx (outputGainParam->convertTo0to1 (0.0f)));

        // Not bypassed by default.
        CHECK (bypassParam->getDefaultValue() == Catch::Approx (0.0f));

        // Margin accounts for the negligible (sub-microdB) floating-point
        // quantisation noise introduced by the 0.01 dB NormalisableRange
        // interval snapping the default value - not an audible difference.
        CHECK (*processor.apvts.getRawParameterValue (ParamIDs::inputGain) == Catch::Approx (0.0f).margin (1e-4));
        CHECK (*processor.apvts.getRawParameterValue (ParamIDs::outputGain) == Catch::Approx (0.0f).margin (1e-4));
        CHECK (*processor.apvts.getRawParameterValue (ParamIDs::bypass) == Catch::Approx (0.0f).margin (1e-4));
    }

    SECTION ("bypass parameter is wired as the plugin's host-facing bypass parameter")
    {
        CHECK (processor.getBypassParameter() == processor.apvts.getParameter (ParamIDs::bypass));
    }

    SECTION ("reports zero latency")
    {
        processor.prepareToPlay (48000.0, 512);
        CHECK (processor.getLatencySamples() == 0);
    }
}
