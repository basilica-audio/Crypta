#include "PluginProcessor.h"
#include "params/ParameterIds.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
    // Convenience wrapper: fetches a parameter by ID and requires it to
    // exist before returning, so every SECTION below fails loudly (not with
    // a null-deref) if an ID typo ever creeps in.
    juce::RangedAudioParameter* requireParam (juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
    {
        auto* param = apvts.getParameter (id);
        REQUIRE (param != nullptr);
        return param;
    }

    // Checks that a float parameter's underlying NormalisableRange covers
    // [expectedMin, expectedMax], independent of any skew/log mapping.
    void checkFloatRange (juce::AudioProcessorValueTreeState& apvts,
                           const juce::String& id,
                           float expectedMin,
                           float expectedMax)
    {
        auto* param = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (id));
        REQUIRE (param != nullptr);

        const auto range = param->getNormalisableRange().getRange();
        CHECK (range.getStart() == Catch::Approx (expectedMin));
        CHECK (range.getEnd() == Catch::Approx (expectedMax));
    }

    // Checks a float parameter's default value in real (non-normalised)
    // units, going through convertTo0to1 so log-skewed ranges are handled
    // the same way as linear ones.
    void checkFloatDefault (juce::AudioProcessorValueTreeState& apvts,
                             const juce::String& id,
                             float expectedDefault)
    {
        auto* param = requireParam (apvts, id);
        CHECK (param->getDefaultValue() == Catch::Approx (param->convertTo0to1 (expectedDefault)).margin (1e-4));
    }

    void checkBoolDefault (juce::AudioProcessorValueTreeState& apvts, const juce::String& id, bool expectedDefault)
    {
        auto* param = requireParam (apvts, id);
        CHECK (param->getDefaultValue() == Catch::Approx (expectedDefault ? 1.0f : 0.0f));
    }
}

TEST_CASE ("Processor instantiates with the expected parameters", "[processor][parameters]")
{
    TwistYourGutsAudioProcessor processor;
    auto& apvts = processor.apvts;

    SECTION ("plugin name")
    {
        CHECK (processor.getName() == juce::String ("Twist Your Guts"));
    }

    SECTION ("all documented parameter IDs resolve")
    {
        static constexpr const char* allIds[] = {
            ParamIDs::inputGain,       ParamIDs::outputGain,      ParamIDs::bypass,
            ParamIDs::outputClip,      ParamIDs::gateEnabled,     ParamIDs::gateThreshold,
            ParamIDs::gateRatio,       ParamIDs::gateAttack,      ParamIDs::gateRelease,
            ParamIDs::crossoverFreq,   ParamIDs::lowCompThreshold, ParamIDs::lowCompRatio,
            ParamIDs::lowCompAttack,   ParamIDs::lowCompRelease,  ParamIDs::lowCompMakeup,
            ParamIDs::lowCompMix,      ParamIDs::lowLevel,        ParamIDs::highVoicing,
            ParamIDs::highDrive,       ParamIDs::highTone,        ParamIDs::highBlend,
            ParamIDs::highLevel,       ParamIDs::eqEnabled,       ParamIDs::eqLowShelfFreq,
            ParamIDs::eqLowShelfGain,  ParamIDs::eqPeak1Freq,     ParamIDs::eqPeak1Gain,
            ParamIDs::eqPeak1Q,        ParamIDs::eqPeak2Freq,     ParamIDs::eqPeak2Gain,
            ParamIDs::eqPeak2Q,        ParamIDs::eqHighShelfFreq, ParamIDs::eqHighShelfGain,
            ParamIDs::irEnabled,       ParamIDs::irMix,
        };

        for (const auto* id : allIds)
            CHECK (apvts.getParameter (id) != nullptr);
    }

    SECTION ("total parameter count matches the full v1.0 layout")
    {
        // 4 IO/global + 5 gate + 1 crossover + 7 low band + 5 high band
        // + 11 EQ + 2 IR = 35.
        CHECK (apvts.processor.getParameters().size() == 35);
    }

    SECTION ("IO / global defaults")
    {
        checkFloatDefault (apvts, ParamIDs::inputGain, 0.0f);
        checkFloatDefault (apvts, ParamIDs::outputGain, 0.0f);
        checkBoolDefault (apvts, ParamIDs::bypass, false);
        checkBoolDefault (apvts, ParamIDs::outputClip, false);

        checkFloatRange (apvts, ParamIDs::inputGain, -24.0f, 24.0f);
        checkFloatRange (apvts, ParamIDs::outputGain, -24.0f, 24.0f);
    }

    SECTION ("noise gate defaults and ranges")
    {
        checkBoolDefault (apvts, ParamIDs::gateEnabled, false);
        checkFloatDefault (apvts, ParamIDs::gateThreshold, -60.0f);
        checkFloatDefault (apvts, ParamIDs::gateRatio, 10.0f);
        checkFloatDefault (apvts, ParamIDs::gateAttack, 1.0f);
        checkFloatDefault (apvts, ParamIDs::gateRelease, 100.0f);

        checkFloatRange (apvts, ParamIDs::gateThreshold, -80.0f, 0.0f);
        checkFloatRange (apvts, ParamIDs::gateRatio, 1.0f, 20.0f);
        checkFloatRange (apvts, ParamIDs::gateAttack, 0.1f, 50.0f);
        checkFloatRange (apvts, ParamIDs::gateRelease, 5.0f, 500.0f);
    }

    SECTION ("crossover defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::crossoverFreq, 250.0f);
        checkFloatRange (apvts, ParamIDs::crossoverFreq, 60.0f, 1000.0f);
    }

    SECTION ("low band defaults and ranges")
    {
        checkFloatDefault (apvts, ParamIDs::lowCompThreshold, -18.0f);
        checkFloatDefault (apvts, ParamIDs::lowCompRatio, 4.0f);
        checkFloatDefault (apvts, ParamIDs::lowCompAttack, 10.0f);
        checkFloatDefault (apvts, ParamIDs::lowCompRelease, 120.0f);
        checkFloatDefault (apvts, ParamIDs::lowCompMakeup, 0.0f);
        checkFloatDefault (apvts, ParamIDs::lowCompMix, 100.0f);
        checkFloatDefault (apvts, ParamIDs::lowLevel, 0.0f);

        checkFloatRange (apvts, ParamIDs::lowCompThreshold, -60.0f, 0.0f);
        checkFloatRange (apvts, ParamIDs::lowCompRatio, 1.0f, 20.0f);
        checkFloatRange (apvts, ParamIDs::lowCompAttack, 0.1f, 100.0f);
        checkFloatRange (apvts, ParamIDs::lowCompRelease, 10.0f, 1000.0f);
        checkFloatRange (apvts, ParamIDs::lowCompMakeup, -12.0f, 24.0f);
        checkFloatRange (apvts, ParamIDs::lowCompMix, 0.0f, 100.0f);
        checkFloatRange (apvts, ParamIDs::lowLevel, -24.0f, 12.0f);
    }

    SECTION ("high band defaults and ranges, including the voicing choice")
    {
        auto* voicingParam = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (ParamIDs::highVoicing));
        REQUIRE (voicingParam != nullptr);
        CHECK (voicingParam->choices.size() == 3);
        CHECK (voicingParam->choices[0] == juce::String ("Gnaw"));
        CHECK (voicingParam->choices[1] == juce::String ("Wool"));
        CHECK (voicingParam->choices[2] == juce::String ("Razor"));
        CHECK (voicingParam->getIndex() == 0);

        checkFloatDefault (apvts, ParamIDs::highDrive, 50.0f);
        checkFloatDefault (apvts, ParamIDs::highTone, 50.0f);
        checkFloatDefault (apvts, ParamIDs::highBlend, 100.0f);
        checkFloatDefault (apvts, ParamIDs::highLevel, 0.0f);

        checkFloatRange (apvts, ParamIDs::highDrive, 0.0f, 100.0f);
        checkFloatRange (apvts, ParamIDs::highTone, 0.0f, 100.0f);
        checkFloatRange (apvts, ParamIDs::highBlend, 0.0f, 100.0f);
        checkFloatRange (apvts, ParamIDs::highLevel, -24.0f, 12.0f);
    }

    SECTION ("EQ defaults and ranges")
    {
        checkBoolDefault (apvts, ParamIDs::eqEnabled, false);

        checkFloatDefault (apvts, ParamIDs::eqLowShelfFreq, 100.0f);
        checkFloatDefault (apvts, ParamIDs::eqLowShelfGain, 0.0f);
        checkFloatDefault (apvts, ParamIDs::eqPeak1Freq, 500.0f);
        checkFloatDefault (apvts, ParamIDs::eqPeak1Gain, 0.0f);
        checkFloatDefault (apvts, ParamIDs::eqPeak1Q, 0.7f);
        checkFloatDefault (apvts, ParamIDs::eqPeak2Freq, 2500.0f);
        checkFloatDefault (apvts, ParamIDs::eqPeak2Gain, 0.0f);
        checkFloatDefault (apvts, ParamIDs::eqPeak2Q, 0.7f);
        checkFloatDefault (apvts, ParamIDs::eqHighShelfFreq, 8000.0f);
        checkFloatDefault (apvts, ParamIDs::eqHighShelfGain, 0.0f);

        checkFloatRange (apvts, ParamIDs::eqLowShelfFreq, 40.0f, 400.0f);
        checkFloatRange (apvts, ParamIDs::eqLowShelfGain, -18.0f, 18.0f);
        checkFloatRange (apvts, ParamIDs::eqPeak1Freq, 100.0f, 2000.0f);
        checkFloatRange (apvts, ParamIDs::eqPeak1Gain, -18.0f, 18.0f);
        checkFloatRange (apvts, ParamIDs::eqPeak1Q, 0.2f, 5.0f);
        checkFloatRange (apvts, ParamIDs::eqPeak2Freq, 500.0f, 8000.0f);
        checkFloatRange (apvts, ParamIDs::eqPeak2Gain, -18.0f, 18.0f);
        checkFloatRange (apvts, ParamIDs::eqPeak2Q, 0.2f, 5.0f);
        checkFloatRange (apvts, ParamIDs::eqHighShelfFreq, 2000.0f, 16000.0f);
        checkFloatRange (apvts, ParamIDs::eqHighShelfGain, -18.0f, 18.0f);
    }

    SECTION ("IR loader defaults and range")
    {
        checkBoolDefault (apvts, ParamIDs::irEnabled, false);
        checkFloatDefault (apvts, ParamIDs::irMix, 100.0f);
        checkFloatRange (apvts, ParamIDs::irMix, 0.0f, 100.0f);
    }

    SECTION ("parameters have the documented default values (legacy IO check)")
    {
        auto* inputGainParam = requireParam (apvts, ParamIDs::inputGain);
        auto* outputGainParam = requireParam (apvts, ParamIDs::outputGain);
        auto* bypassParam = requireParam (apvts, ParamIDs::bypass);

        // 0 dB for both gains, normalised against the -24..+24 range.
        CHECK (inputGainParam->getDefaultValue() == Catch::Approx (inputGainParam->convertTo0to1 (0.0f)));
        CHECK (outputGainParam->getDefaultValue() == Catch::Approx (outputGainParam->convertTo0to1 (0.0f)));

        // Not bypassed by default.
        CHECK (bypassParam->getDefaultValue() == Catch::Approx (0.0f));

        // Margin accounts for the negligible (sub-microdB) floating-point
        // quantisation noise introduced by the 0.01 dB NormalisableRange
        // interval snapping the default value - not an audible difference.
        CHECK (*apvts.getRawParameterValue (ParamIDs::inputGain) == Catch::Approx (0.0f).margin (1e-4));
        CHECK (*apvts.getRawParameterValue (ParamIDs::outputGain) == Catch::Approx (0.0f).margin (1e-4));
        CHECK (*apvts.getRawParameterValue (ParamIDs::bypass) == Catch::Approx (0.0f).margin (1e-4));
    }

    SECTION ("bypass parameter is wired as the plugin's host-facing bypass parameter")
    {
        CHECK (processor.getBypassParameter() == apvts.getParameter (ParamIDs::bypass));
    }

    SECTION ("reports positive latency once prepared (issue #42's oversampled high-band voicing)")
    {
        CHECK (processor.getLatencySamples() == 0);

        processor.prepareToPlay (48000.0, 512);
        CHECK (processor.getLatencySamples() > 0);
    }
}
