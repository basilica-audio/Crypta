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

    // Asserts a juce::AudioParameterChoice's default *index*. Choice
    // parameters normalise as index/(numChoices-1), so comparing the raw
    // normalised default would silently depend on the choice count - going
    // through getIndex() after resetting to the default keeps the assertion
    // about the option actually selected.
    void checkChoiceDefault (juce::AudioProcessorValueTreeState& apvts,
                              const juce::String& id,
                              int expectedDefaultIndex)
    {
        // getDefaultValue() is private on AudioParameterChoice itself, so the
        // default is read through the RangedAudioParameter base (where it is
        // public) before casting down for getIndex().
        auto* ranged = requireParam (apvts, id);
        auto* choice = dynamic_cast<juce::AudioParameterChoice*> (ranged);
        REQUIRE (choice != nullptr);

        ranged->setValueNotifyingHost (ranged->getDefaultValue());
        CHECK (choice->getIndex() == expectedDefaultIndex);
    }
}

TEST_CASE ("Processor instantiates with the expected parameters", "[processor][parameters]")
{
    CryptaAudioProcessor processor;
    auto& apvts = processor.apvts;

    SECTION ("plugin name")
    {
        CHECK (processor.getName() == juce::String ("Crypta"));
    }

    SECTION ("all documented parameter IDs resolve")
    {
        static constexpr const char* allIds[] = {
            ParamIDs::inputGain,        ParamIDs::outputGain,       ParamIDs::bypass,
            ParamIDs::outputClip,       ParamIDs::gateEnabled,      ParamIDs::gateThreshold,
            ParamIDs::gateRatio,        ParamIDs::gateAttack,       ParamIDs::gateRelease,
            ParamIDs::splitLowHz,       ParamIDs::splitHighHz,      ParamIDs::lowCompThreshold,
            ParamIDs::lowCompRatio,     ParamIDs::lowCompAttack,    ParamIDs::lowCompRelease,
            ParamIDs::lowCompMakeup,    ParamIDs::lowCompMix,       ParamIDs::lowLevel,
            ParamIDs::midDrive,         ParamIDs::midLevel,         ParamIDs::highTightHz,
            ParamIDs::highVoicing,      ParamIDs::highDrive,        ParamIDs::highTone,
            ParamIDs::highBlend,        ParamIDs::highLevel,        ParamIDs::eqEnabled,
            ParamIDs::eqLowShelfFreq,   ParamIDs::eqLowShelfGain,   ParamIDs::eqPeak1Freq,
            ParamIDs::eqPeak1Gain,      ParamIDs::eqPeak1Q,         ParamIDs::eqPeak2Freq,
            ParamIDs::eqPeak2Gain,      ParamIDs::eqPeak2Q,         ParamIDs::eqHighShelfFreq,
            ParamIDs::eqHighShelfGain,  ParamIDs::irEnabled,        ParamIDs::irMix,

            // v0.3.0 circuit-grade bass engine additions.
            ParamIDs::driveEngine,      ParamIDs::highBias,         ParamIDs::lowCompDetector,
            ParamIDs::lowCompKnee,      ParamIDs::lowCompAutoRelease, ParamIDs::lowCompAutoMakeup,
            ParamIDs::gateMode,         ParamIDs::gateHysteresis,   ParamIDs::gateHold,
            ParamIDs::gateScHpf,        ParamIDs::gateRange,        ParamIDs::clipCeiling,

            // v0.4.0 Graaawl (issue #36).
            ParamIDs::lowGrowl,         ParamIDs::lowGrowlAmount,   ParamIDs::lowGrowlTone,
        };

        for (const auto* id : allIds)
            CHECK (apvts.getParameter (id) != nullptr);
    }

    SECTION ("total parameter count matches the full v0.4.0 layout")
    {
        // v0.2.0's 39 (4 IO/global + 5 gate + 2 crossover + 7 low band
        // + 2 mid band + 6 high band + 11 EQ + 2 IR) plus v0.3.0's 12
        // circuit-engine additions (2 drive engine + 4 low comp detector
        // + 5 gate mode + 1 clip ceiling) = 51, plus v0.4.0's 3 Graaawl
        // parameters = 54.
        CHECK (apvts.processor.getParameters().size() == 54);
    }

    SECTION ("v0.4.0 Graaawl defaults are inert (issue #36)")
    {
        // The whole low band's backward compatibility rests on these three:
        // Graaawl off, and an Amount that would be silent even if something
        // turned it on. If any of them ever gained a non-neutral default, every
        // pre-v0.4.0 session and preset - none of which name these IDs - would
        // quietly change its low end on load.
        checkBoolDefault (apvts, ParamIDs::lowGrowl, false);
        checkFloatDefault (apvts, ParamIDs::lowGrowlAmount, 0.0f);
        checkFloatDefault (apvts, ParamIDs::lowGrowlTone, 50.0f);

        checkFloatRange (apvts, ParamIDs::lowGrowlAmount, 0.0f, 100.0f);
        checkFloatRange (apvts, ParamIDs::lowGrowlTone, 0.0f, 100.0f);
    }

    SECTION ("v0.3.0 engine selectors default to the new circuit engines")
    {
        // A FRESH instance boots into the new engines - that is the point of
        // the release. Existing sessions and presets never see these defaults
        // (see StateMigrationTests / PresetManagerTests for the two injection
        // paths that keep legacy work on the Classic engines).
        checkChoiceDefault (apvts, ParamIDs::driveEngine, 1);      // Circuit
        checkChoiceDefault (apvts, ParamIDs::lowCompDetector, 1);  // Smooth RMS
        checkChoiceDefault (apvts, ParamIDs::gateMode, 1);         // Modern
    }

    SECTION ("v0.3.0 engine parameter defaults and ranges")
    {
        // highBias, autoMakeup and clipCeiling are the three that must be
        // NEUTRAL: highBias 0 % is the symmetric v0.2.0 character, autoMakeup
        // off is a no-op, and a 0 dBFS ceiling is v0.2.0's implicit unity.
        checkFloatDefault (apvts, ParamIDs::highBias, 0.0f);
        checkBoolDefault (apvts, ParamIDs::lowCompAutoMakeup, false);
        checkFloatDefault (apvts, ParamIDs::clipCeiling, 0.0f);

        // The rest are engine-gated, so non-neutral defaults are safe.
        checkFloatDefault (apvts, ParamIDs::lowCompKnee, 6.0f);
        checkBoolDefault (apvts, ParamIDs::lowCompAutoRelease, true);
        checkFloatDefault (apvts, ParamIDs::gateHysteresis, 4.0f);
        checkFloatDefault (apvts, ParamIDs::gateHold, 20.0f);
        checkFloatDefault (apvts, ParamIDs::gateScHpf, 80.0f);
        checkFloatDefault (apvts, ParamIDs::gateRange, 60.0f);

        checkFloatRange (apvts, ParamIDs::highBias, 0.0f, 100.0f);
        checkFloatRange (apvts, ParamIDs::lowCompKnee, 0.0f, 18.0f);
        checkFloatRange (apvts, ParamIDs::gateHysteresis, 0.0f, 12.0f);
        checkFloatRange (apvts, ParamIDs::gateHold, 0.0f, 500.0f);
        checkFloatRange (apvts, ParamIDs::gateScHpf, 20.0f, 400.0f);
        checkFloatRange (apvts, ParamIDs::gateRange, 6.0f, 90.0f);
        checkFloatRange (apvts, ParamIDs::clipCeiling, -12.0f, 0.0f);
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

    SECTION ("crossover defaults and ranges (two cascaded LR4 splits, v0.2.0)")
    {
        checkFloatDefault (apvts, ParamIDs::splitLowHz, 120.0f);
        checkFloatRange (apvts, ParamIDs::splitLowHz, 60.0f, 400.0f);

        checkFloatDefault (apvts, ParamIDs::splitHighHz, 600.0f);
        checkFloatRange (apvts, ParamIDs::splitHighHz, 300.0f, 2000.0f);
    }

    SECTION ("low band defaults and ranges (v0.2.0 re-sourced glue-compressor ballistics)")
    {
        checkFloatDefault (apvts, ParamIDs::lowCompThreshold, -18.0f);
        checkFloatDefault (apvts, ParamIDs::lowCompRatio, 2.0f);
        checkFloatDefault (apvts, ParamIDs::lowCompAttack, 3.0f);
        checkFloatDefault (apvts, ParamIDs::lowCompRelease, 6.0f);
        checkFloatDefault (apvts, ParamIDs::lowCompMakeup, 0.0f);
        checkFloatDefault (apvts, ParamIDs::lowCompMix, 100.0f);
        checkFloatDefault (apvts, ParamIDs::lowLevel, 0.0f);

        checkFloatRange (apvts, ParamIDs::lowCompThreshold, -60.0f, 0.0f);
        checkFloatRange (apvts, ParamIDs::lowCompRatio, 1.0f, 20.0f);
        checkFloatRange (apvts, ParamIDs::lowCompAttack, 0.1f, 100.0f);
        checkFloatRange (apvts, ParamIDs::lowCompRelease, 5.0f, 1000.0f);
        checkFloatRange (apvts, ParamIDs::lowCompMakeup, -12.0f, 24.0f);
        checkFloatRange (apvts, ParamIDs::lowCompMix, 0.0f, 100.0f);
        checkFloatRange (apvts, ParamIDs::lowLevel, -24.0f, 12.0f);
    }

    SECTION ("mid band defaults and ranges (NEW in v0.2.0)")
    {
        checkFloatDefault (apvts, ParamIDs::midDrive, 30.0f);
        checkFloatDefault (apvts, ParamIDs::midLevel, 0.0f);

        checkFloatRange (apvts, ParamIDs::midDrive, 0.0f, 100.0f);
        checkFloatRange (apvts, ParamIDs::midLevel, -24.0f, 12.0f);
    }

    SECTION ("high band defaults and ranges, including Tight (NEW) and the voicing choice")
    {
        auto* voicingParam = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (ParamIDs::highVoicing));
        REQUIRE (voicingParam != nullptr);
        CHECK (voicingParam->choices.size() == 3);
        CHECK (voicingParam->choices[0] == juce::String ("Gnaw"));
        CHECK (voicingParam->choices[1] == juce::String ("Wool"));
        CHECK (voicingParam->choices[2] == juce::String ("Razor"));
        CHECK (voicingParam->getIndex() == 0);

        checkFloatDefault (apvts, ParamIDs::highTightHz, 100.0f);
        checkFloatDefault (apvts, ParamIDs::highDrive, 50.0f);
        checkFloatDefault (apvts, ParamIDs::highTone, 50.0f);
        checkFloatDefault (apvts, ParamIDs::highBlend, 100.0f);
        checkFloatDefault (apvts, ParamIDs::highLevel, 0.0f);

        checkFloatRange (apvts, ParamIDs::highTightHz, 20.0f, 500.0f);
        checkFloatRange (apvts, ParamIDs::highDrive, 0.0f, 100.0f);
        checkFloatRange (apvts, ParamIDs::highTone, 0.0f, 100.0f);
        checkFloatRange (apvts, ParamIDs::highBlend, 0.0f, 100.0f);
        checkFloatRange (apvts, ParamIDs::highLevel, -24.0f, 12.0f);
    }

    SECTION ("EQ defaults and ranges (v0.2.0 re-anchored default frequencies)")
    {
        checkBoolDefault (apvts, ParamIDs::eqEnabled, false);

        checkFloatDefault (apvts, ParamIDs::eqLowShelfFreq, 80.0f);
        checkFloatDefault (apvts, ParamIDs::eqLowShelfGain, 0.0f);
        checkFloatDefault (apvts, ParamIDs::eqPeak1Freq, 500.0f);
        checkFloatDefault (apvts, ParamIDs::eqPeak1Gain, 0.0f);
        checkFloatDefault (apvts, ParamIDs::eqPeak1Q, 0.7f);
        checkFloatDefault (apvts, ParamIDs::eqPeak2Freq, 2800.0f);
        checkFloatDefault (apvts, ParamIDs::eqPeak2Gain, 0.0f);
        checkFloatDefault (apvts, ParamIDs::eqPeak2Q, 0.7f);
        checkFloatDefault (apvts, ParamIDs::eqHighShelfFreq, 5000.0f);
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
        // The STARTUP value of outputGain is not the layout default asserted
        // above: the constructor resolves the factory Default preset
        // (PresetManager::applyStartupDefault()), and since the issue #34
        // item 1 clipping fix that preset carries an output trim so a fresh
        // instance no longer pushes a nominally tracked input past full scale.
        // The layout default stays 0 dB - it is what "reset to default" on the
        // control gives - and the startup state is the preset's.
        //
        // -3.26 dB, not the original -2.8: the trim was re-derived against the
        // broader-band suite reference programme (tests/PresetHeadroomTests.cpp),
        // on which Default measured +0.16 dBFS where the bass DI put it at
        // -0.31. A fresh instance is 0.46 dB quieter than it was, on purpose.
        CHECK (*apvts.getRawParameterValue (ParamIDs::outputGain) == Catch::Approx (-3.26f).margin (1e-4));
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
