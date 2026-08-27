#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "presets/PresetManager.h"

#include <BinaryData.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

// M2 preset system tests (.scaffold/specs/preset-system-m2.md's "Tests"
// section - each TEST_CASE below maps to one of that section's numbered
// items, called out in the test names/comments). Adapted from
// basilica-audio/nave's pilot implementation
// (tests/PresetManagerTests.cpp, docs/preset-system-notes.md's replication
// recipe) - PresetManager/PresetBar themselves are copied verbatim; only the
// plugin-specific glue below (factory asset list, config, parameter IDs)
// is Crypta-specific.
namespace
{
    using basilica::presets::FactoryPresetAsset;
    using basilica::presets::PresetManager;
    using basilica::presets::PresetManagerConfig;

    // Mirrors PluginProcessor.cpp's own makeFactoryPresetAssets() - kept as
    // an independent copy here rather than exported from PluginProcessor.cpp
    // so this test file can construct its own, fully isolated PresetManager
    // instances (see makeIsolatedConfig() below) without depending on
    // production wiring internals.
    std::vector<FactoryPresetAsset> makeTestFactoryPresetAssets()
    {
        return {
            { BinaryData::default_json, BinaryData::default_jsonSize },
            { BinaryData::glueAndGrind_json, BinaryData::glueAndGrind_jsonSize },
            { BinaryData::subLock_json, BinaryData::subLock_jsonSize },
            { BinaryData::throat_json, BinaryData::throat_jsonSize },
            { BinaryData::fuzzWall_json, BinaryData::fuzzWall_jsonSize },
            { BinaryData::cutThrough_json, BinaryData::cutThrough_jsonSize },
            { BinaryData::definitionOnly_json, BinaryData::definitionOnly_jsonSize },
            { BinaryData::cleanLowLoudTop_json, BinaryData::cleanLowLoudTop_jsonSize },
            { BinaryData::cabColoredGrind_json, BinaryData::cabColoredGrind_jsonSize },
            { BinaryData::circuitFoundation_json, BinaryData::circuitFoundation_jsonSize },
            { BinaryData::circuitGrind_json, BinaryData::circuitGrind_jsonSize },
            { BinaryData::circuitKnife_json, BinaryData::circuitKnife_jsonSize },
        };
    }

    // A fresh, isolated scratch directory per test case, so this file never
    // reads or writes the real ~/Library/Audio/Presets/... (or Windows
    // equivalent) location on the machine running the tests - see
    // PresetManagerConfig::userPresetsDirectoryOverrideForTests. Deleted on
    // destruction.
    struct ScopedTestDirectory
    {
        ScopedTestDirectory()
            : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("CryptaPresetManagerTests")
                       .getChildFile (juce::String (juce::Time::getHighResolutionTicks())
                                       + "_" + juce::String (juce::Random::getSystemRandom().nextInt (1000000))))
        {
            dir.createDirectory();
        }

        ~ScopedTestDirectory()
        {
            dir.deleteRecursively();
        }

        JUCE_DECLARE_NON_COPYABLE (ScopedTestDirectory)

        juce::File dir;
    };

    PresetManagerConfig makeIsolatedConfig (const juce::File& userDir)
    {
        PresetManagerConfig config;
        config.pluginId = "com.yvesvogl.crypta";
        config.pluginName = "Crypta";
        config.manufacturerName = "Basilica Audio";
        config.pluginVersion = "0.2.0-test";
        config.userPresetsDirectoryOverrideForTests = userDir;

        // Mirrors PluginProcessor.cpp's production config: without this the
        // v0.3.0 legacy engine back-fill would be inactive in these tests, and
        // the T20 cases below would be asserting against a code path the real
        // plugin does not use.
        config.legacyParameterCutoffVersion = "0.3.0";
        config.legacyParameterDefaults = {
            { ParamIDs::driveEngine, 0.0f },
            { ParamIDs::lowCompDetector, 0.0f },
            { ParamIDs::gateMode, 0.0f },
        };

        return config;
    }

    // Reads a fixture from tests/fixtures. __FILE__ is absolute because
    // CMakeLists.txt globs the test sources.
    juce::File presetFixture (const juce::String& name)
    {
        return juce::File (juce::String (__FILE__)).getParentDirectory()
            .getChildFile ("fixtures").getChildFile (name);
    }

    int choiceIndexOf (CryptaAudioProcessor& processor, const char* id)
    {
        auto* param = dynamic_cast<juce::AudioParameterChoice*> (processor.apvts.getParameter (id));
        REQUIRE (param != nullptr);
        return param->getIndex();
    }

    void setParam (CryptaAudioProcessor& processor, const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    }

    float getParam (CryptaAudioProcessor& processor, const char* id)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        return param->convertFrom0to1 (param->getValue());
    }
}

//==============================================================================
// 1. Save -> load round-trip restores every parameter exactly.
TEST_CASE ("PresetManager: save -> load round-trip restores every parameter exactly", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    setParam (processor, ParamIDs::splitLowHz, 150.0f);
    setParam (processor, ParamIDs::splitHighHz, 700.0f);
    setParam (processor, ParamIDs::midDrive, 55.0f);
    setParam (processor, ParamIDs::midLevel, -3.0f);
    setParam (processor, ParamIDs::highTightHz, 200.0f);
    setParam (processor, ParamIDs::highDrive, 65.0f);

    REQUIRE (manager.saveUserPreset ("Round Trip", "Bass"));

    // Perturb every parameter away from the saved values before reloading,
    // so the assertions below can't pass by accident.
    setParam (processor, ParamIDs::splitLowHz, 120.0f);
    setParam (processor, ParamIDs::splitHighHz, 600.0f);
    setParam (processor, ParamIDs::midDrive, 30.0f);
    setParam (processor, ParamIDs::midLevel, 0.0f);
    setParam (processor, ParamIDs::highTightHz, 100.0f);
    setParam (processor, ParamIDs::highDrive, 50.0f);

    REQUIRE (manager.loadPreset ("Round Trip"));

    CHECK (getParam (processor, ParamIDs::splitLowHz) == Catch::Approx (150.0f).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::splitHighHz) == Catch::Approx (700.0f).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::midDrive) == Catch::Approx (55.0f).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::midLevel) == Catch::Approx (-3.0f).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::highTightHz) == Catch::Approx (200.0f).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::highDrive) == Catch::Approx (65.0f).margin (1.0e-3));
}

//==============================================================================
// 2. Import ignores unknown IDs, keeps defaults for missing IDs.
TEST_CASE ("PresetManager: import ignores unknown parameter IDs and keeps defaults for missing ones", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    // Move midDrive and midLevel away from their defaults so it's
    // meaningful when the import below leaves them untouched (they're
    // absent from "parameters").
    setParam (processor, ParamIDs::midDrive, 90.0f);
    setParam (processor, ParamIDs::midLevel, 8.0f);

    // A fixture JSON generated inline (not committed under tests/fixtures/)
    // to avoid brittle relative-path resolution across CI runners with
    // different working directories (macOS vs Windows ctest invocations) -
    // this is the forward/backward-compat scenario the spec's "fixture
    // JSONs in tests/" line calls for: an unknown ID ("futureParameter",
    // simulating a newer plugin version's preset) and two known IDs
    // (splitLowHz/splitHighHz), deliberately omitting midDrive/midLevel.
    const juce::String fixtureJson = R"({
        "format": "basilica-preset-1",
        "plugin": "com.yvesvogl.crypta",
        "pluginVersion": "9.9.9",
        "name": "Forward Compat Fixture",
        "category": "Bass",
        "parameters": { "splitLowHz": 200.0, "splitHighHz": 1000.0, "futureParameter": 42.0 }
    })";

    const auto fixtureFile = juce::File::createTempFile (".basilicapreset");
    REQUIRE (fixtureFile.replaceWithText (fixtureJson));

    juce::String errorMessage;
    REQUIRE (manager.importPresetFile (fixtureFile, errorMessage));
    CHECK (errorMessage.isEmpty());

    // Known IDs present in the fixture were applied...
    CHECK (getParam (processor, ParamIDs::splitLowHz) == Catch::Approx (200.0f).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::splitHighHz) == Catch::Approx (1000.0f).margin (1.0e-3));

    // ...IDs absent from the fixture were reset to their ParameterLayout
    // defaults (loadPreset()/importPresetFile() always reset-then-apply -
    // see PresetManager.h), not left at the pre-import 90%/8dB values.
    auto* midDriveParam = processor.apvts.getParameter (ParamIDs::midDrive);
    auto* midLevelParam = processor.apvts.getParameter (ParamIDs::midLevel);
    CHECK (getParam (processor, ParamIDs::midDrive) == Catch::Approx (midDriveParam->convertFrom0to1 (midDriveParam->getDefaultValue())).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::midLevel) == Catch::Approx (midLevelParam->convertFrom0to1 (midLevelParam->getDefaultValue())).margin (1.0e-3));

    fixtureFile.deleteFile();
}

//==============================================================================
// 3. Import refuses wrong-plugin and wrong-format files.
TEST_CASE ("PresetManager: import refuses a preset belonging to a different plugin", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    const juce::String wrongPluginJson = R"({
        "format": "basilica-preset-1",
        "plugin": "com.yvesvogl.nave",
        "pluginVersion": "0.2.0",
        "name": "Not Crypta's",
        "category": "Bass",
        "parameters": { "splitLowHz": 999.0 }
    })";

    const auto file = juce::File::createTempFile (".basilicapreset");
    REQUIRE (file.replaceWithText (wrongPluginJson));

    juce::String errorMessage;
    CHECK_FALSE (manager.importPresetFile (file, errorMessage));
    CHECK (errorMessage.isNotEmpty());

    // State must be left untouched - splitLowHz must NOT have picked up 999
    // (out of its own 60-400 Hz range too, which would be a separate bug on
    // top).
    CHECK (getParam (processor, ParamIDs::splitLowHz) != Catch::Approx (999.0f));

    file.deleteFile();
}

TEST_CASE ("PresetManager: import refuses a file with an incompatible format tag", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    const juce::String wrongFormatJson = R"({
        "format": "some-other-format-2",
        "plugin": "com.yvesvogl.crypta",
        "pluginVersion": "0.2.0",
        "name": "Wrong Format",
        "category": "Bass",
        "parameters": { "splitLowHz": 999.0 }
    })";

    const auto file = juce::File::createTempFile (".basilicapreset");
    REQUIRE (file.replaceWithText (wrongFormatJson));

    juce::String errorMessage;
    CHECK_FALSE (manager.importPresetFile (file, errorMessage));
    CHECK (errorMessage.isNotEmpty());

    file.deleteFile();
}

//==============================================================================
// 4. Factory presets all parse and load.
TEST_CASE ("PresetManager: every factory preset parses and loads without error", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    const auto all = manager.getAllPresets();
    const auto factoryCount = std::count_if (all.begin(), all.end(), [] (auto& e) { return e.isFactory; });

    // docs/presets.md - Default + 8 brief-table presets, plus the three
    // v0.3.0 Circuit-engine showcases.
    REQUIRE (factoryCount == 12);

    for (auto& entry : all)
    {
        if (! entry.isFactory)
            continue;

        CAPTURE (entry.name);
        CHECK (manager.loadPreset (entry.name));
        CHECK (manager.isCurrentPresetFactory());
        CHECK (manager.getCurrentPresetName() == entry.name);
    }
}

TEST_CASE ("PresetManager: factory preset content is plausible (Default is Init category, all parameters in range, no NaN/Inf/silence)", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    const auto all = manager.getAllPresets();
    const auto defaultEntry = std::find_if (all.begin(), all.end(), [] (auto& e) { return e.name == "Default"; });

    REQUIRE (defaultEntry != all.end());
    CHECK (defaultEntry->category == "Init");
    CHECK (defaultEntry->isFactory);

    juce::MidiBuffer midi;

    // 10. Preset round-trip (docs/design-brief.md's Guarantee #10): every
    // factory preset loads, parameter values land in range, and produces no
    // NaN/Inf/silence on a standard test signal.
    for (auto& entry : all)
    {
        if (! entry.isFactory)
            continue;

        REQUIRE (manager.loadPreset (entry.name));

        CHECK (getParam (processor, ParamIDs::splitLowHz) >= 60.0f);
        CHECK (getParam (processor, ParamIDs::splitLowHz) <= 400.0f);
        CHECK (getParam (processor, ParamIDs::splitHighHz) >= 300.0f);
        CHECK (getParam (processor, ParamIDs::splitHighHz) <= 2000.0f);
        CHECK (getParam (processor, ParamIDs::midDrive) >= 0.0f);
        CHECK (getParam (processor, ParamIDs::midDrive) <= 100.0f);

        juce::AudioBuffer<float> buffer (2, 512);

        for (int channel = 0; channel < 2; ++channel)
        {
            auto* data = buffer.getWritePointer (channel);

            for (int sample = 0; sample < 512; ++sample)
                data[sample] = 0.5f * std::sin (static_cast<float> (sample) * 0.05f);
        }

        CAPTURE (entry.name);
        CHECK_NOTHROW (processor.processBlock (buffer, midi));

        bool allFinite = true;
        double sumOfSquares = 0.0;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                if (! std::isfinite (data[sample]))
                    allFinite = false;

                sumOfSquares += static_cast<double> (data[sample]) * static_cast<double> (data[sample]);
            }
        }

        CHECK (allFinite);
        CHECK (sumOfSquares > 0.0); // not silent
    }
}

//==============================================================================
// 5. Default resolution order (user Default > factory Default > plain defaults).
TEST_CASE ("PresetManager: applyStartupDefault() loads the factory Default when no user Default exists", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    setParam (processor, ParamIDs::splitLowHz, 350.0f); // perturb first

    manager.applyStartupDefault();

    CHECK (manager.getCurrentPresetName() == "Default");
    CHECK (manager.isCurrentPresetFactory());
    CHECK (getParam (processor, ParamIDs::splitLowHz) == Catch::Approx (120.0f).margin (1.0e-3));
}

TEST_CASE ("PresetManager: a user Default preset wins over the factory Default", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    setParam (processor, ParamIDs::splitLowHz, 321.0f);
    REQUIRE (manager.setCurrentAsDefault()); // writes a user preset literally named "Default"

    setParam (processor, ParamIDs::splitLowHz, 60.0f); // perturb away before the resolution check

    manager.applyStartupDefault();

    CHECK (manager.getCurrentPresetName() == "Default");
    CHECK_FALSE (manager.isCurrentPresetFactory()); // resolved to the *user* Default, not the factory one
    CHECK (getParam (processor, ParamIDs::splitLowHz) == Catch::Approx (321.0f).margin (1.0e-3));
}

TEST_CASE ("PresetManager: resetDefault() removes the user Default so the factory Default resolves again", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    setParam (processor, ParamIDs::splitLowHz, 321.0f);
    REQUIRE (manager.setCurrentAsDefault());
    REQUIRE (manager.resetDefault());

    manager.applyStartupDefault();

    CHECK (manager.isCurrentPresetFactory());
    CHECK (getParam (processor, ParamIDs::splitLowHz) == Catch::Approx (120.0f).margin (1.0e-3));
}

//==============================================================================
// 6. Dirty flag: clean after load, dirty after any param change, clean after save.
TEST_CASE ("PresetManager: dirty flag lifecycle - clean after load, dirty after a change, clean after save", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    REQUIRE (manager.loadPreset ("Default"));
    CHECK_FALSE (manager.isDirty());

    setParam (processor, ParamIDs::splitLowHz, 300.0f);
    CHECK (manager.isDirty());

    REQUIRE (manager.saveUserPreset ("Dirty Flag Preset", "Bass"));
    CHECK_FALSE (manager.isDirty());
}

//==============================================================================
// 7. prev/next ordering and wrap-around.
TEST_CASE ("PresetManager: nextPreset()/previousPreset() traverse alphabetically and wrap around", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    const auto all = manager.getAllPresets();
    REQUIRE (all.size() >= 2);

    REQUIRE (manager.loadPreset (all.front().name));

    manager.nextPreset();
    CHECK (manager.getCurrentPresetName() == all[1].name);

    manager.previousPreset();
    CHECK (manager.getCurrentPresetName() == all.front().name);

    // Wrap backward from the first entry to the last.
    manager.previousPreset();
    CHECK (manager.getCurrentPresetName() == all.back().name);

    // Wrap forward from the last entry back to the first.
    manager.nextPreset();
    CHECK (manager.getCurrentPresetName() == all.front().name);
}

//==============================================================================
// Additional coverage beyond the spec's minimum list: save/rename/delete
// guards, single-file export round-trip, and bank import/export.

TEST_CASE ("PresetManager: saveUserPreset() refuses to shadow a factory preset name", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    CHECK_FALSE (manager.saveUserPreset ("Default", "Init")); // "Default" already exists as a factory preset
    CHECK_FALSE (manager.saveUserPreset ("Sub Lock", "Bass"));
}

TEST_CASE ("PresetManager: renameUserPreset() moves a user preset to a new name and preserves its parameters", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    setParam (processor, ParamIDs::splitHighHz, 1234.0f);
    REQUIRE (manager.saveUserPreset ("Old Name", "Bass"));

    REQUIRE (manager.renameUserPreset ("Old Name", "New Name"));

    setParam (processor, ParamIDs::splitHighHz, 600.0f); // perturb before reloading

    CHECK_FALSE (manager.loadPreset ("Old Name")); // gone
    REQUIRE (manager.loadPreset ("New Name"));
    CHECK (getParam (processor, ParamIDs::splitHighHz) == Catch::Approx (1234.0f).margin (1.0e-3));
}

TEST_CASE ("PresetManager: deleteUserPreset() removes a user preset but never a factory preset", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    REQUIRE (manager.saveUserPreset ("Temporary", "Bass"));
    REQUIRE (manager.deleteUserPreset ("Temporary"));
    CHECK_FALSE (manager.loadPreset ("Temporary"));

    // A factory preset name isn't a file on disk in the user directory, so
    // there's nothing to delete - deleteUserPreset() must return false, and
    // the factory preset must still load afterwards.
    CHECK_FALSE (manager.deleteUserPreset ("Default"));
    CHECK (manager.loadPreset ("Default"));
}

TEST_CASE ("PresetManager: exportPreset()/importPresetFile() single-file round-trip", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    setParam (processor, ParamIDs::highTightHz, 333.0f);
    REQUIRE (manager.saveUserPreset ("Exportable", "Bass"));

    const auto exportFile = juce::File::createTempFile (".basilicapreset");
    REQUIRE (manager.exportPreset ("Exportable", exportFile));
    REQUIRE (exportFile.existsAsFile());

    REQUIRE (manager.deleteUserPreset ("Exportable")); // remove the original before reimporting

    juce::String errorMessage;
    REQUIRE (manager.importPresetFile (exportFile, errorMessage));
    CHECK (getParam (processor, ParamIDs::highTightHz) == Catch::Approx (333.0f).margin (1.0e-3));

    exportFile.deleteFile();
}

TEST_CASE ("PresetManager: exportBank()/importBank() round-trips every user preset through a zip", "[presets]")
{
    ScopedTestDirectory sourceScratch;
    ScopedTestDirectory destScratch;

    CryptaAudioProcessor sourceProcessor;
    sourceProcessor.prepareToPlay (48000.0, 512);
    PresetManager sourceManager (sourceProcessor.apvts, makeIsolatedConfig (sourceScratch.dir), makeTestFactoryPresetAssets());

    setParam (sourceProcessor, ParamIDs::splitLowHz, 111.0f);
    REQUIRE (sourceManager.saveUserPreset ("Bank Preset A", "Bass"));

    setParam (sourceProcessor, ParamIDs::splitLowHz, 222.0f);
    REQUIRE (sourceManager.saveUserPreset ("Bank Preset B", "Bass"));

    const auto bankFile = juce::File::createTempFile (".zip");
    REQUIRE (sourceManager.exportBank (bankFile));
    REQUIRE (bankFile.existsAsFile());

    CryptaAudioProcessor destProcessor;
    destProcessor.prepareToPlay (48000.0, 512);
    PresetManager destManager (destProcessor.apvts, makeIsolatedConfig (destScratch.dir), makeTestFactoryPresetAssets());

    const auto importedCount = destManager.importBank (bankFile);
    CHECK (importedCount == 2);

    REQUIRE (destManager.loadPreset ("Bank Preset A"));
    CHECK (getParam (destProcessor, ParamIDs::splitLowHz) == Catch::Approx (111.0f).margin (1.0e-3));

    REQUIRE (destManager.loadPreset ("Bank Preset B"));
    CHECK (getParam (destProcessor, ParamIDs::splitLowHz) == Catch::Approx (222.0f).margin (1.0e-3));

    bankFile.deleteFile();
}

//==============================================================================
// 8. PresetManager never allocates or locks on the audio thread.
//
// Verified primarily *by design*: nothing in CryptaAudioProcessor::
// processBlock()/the dsp:: classes ever calls into PresetManager (see
// PluginProcessor.cpp - presetManager is only touched from the constructor
// and from PresetBar's message-thread-only UI callbacks), so there is no
// code path for this test to exercise in the first place. The one nuance is
// PresetManager::parameterChanged() (an AudioProcessorValueTreeState::
// Listener callback that JUCE does not document as guaranteed message-
// thread-only) - it is implemented as a single lock-free std::atomic<bool>
// store and nothing else (see PresetManager.h/.cpp), which this test
// exercises indirectly by driving parameter changes and processBlock() back
// to back and confirming nothing misbehaves.
TEST_CASE ("PresetManager: parameter-driven dirty tracking coexists safely with real-time audio processing", "[presets]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    REQUIRE (manager.loadPreset ("Default"));
    CHECK_FALSE (manager.isDirty());

    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;

    for (int block = 0; block < 8; ++block)
    {
        // Every parameterChanged() callback below happens interleaved with
        // real audio processing - if it ever became audio-thread-unsafe
        // (e.g. someone later added a lock or allocation to it), a helgrind/
        // TSan CI run would be the real detector; this test's job is just to
        // confirm normal operation isn't disrupted by the two coexisting.
        setParam (processor, ParamIDs::splitLowHz, 60.0f + static_cast<float> (block) * 20.0f);
        CHECK_NOTHROW (processor.processBlock (buffer, midi));
    }

    CHECK (manager.isDirty());
}

//==============================================================================
// v0.2.0 -> v0.3.0 preset migration (brief §4 step 3 / §6 T20).
//
// Presets never pass through setStateInformation(), so the session-state
// migration does not cover them. And because applyParsedPreset() resets every
// parameter to its default before applying a preset's values, a preset that
// predates the three engine selectors would pick up their NEW defaults -
// silently re-voicing tuned user work on load. These are the tests for the
// read-side back-fill that prevents that.

TEST_CASE ("T20: factory presets pin their engines explicitly", "[presets][migration]")
{
    ScopedTestDirectory scratch;

    CryptaAudioProcessor processor;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    SECTION ("Default boots the new Circuit engines")
    {
        // default.json IS the mechanism by which a fresh instance reaches the
        // Circuit engine: the processor constructor calls applyStartupDefault(),
        // which loads it. If this regressed, the release's headline feature
        // would be off by default and nothing else would fail.
        REQUIRE (manager.loadPreset ("Default"));

        CHECK (choiceIndexOf (processor, ParamIDs::driveEngine) == 1);      // Circuit
        CHECK (choiceIndexOf (processor, ParamIDs::lowCompDetector) == 1);  // Smooth RMS
        CHECK (choiceIndexOf (processor, ParamIDs::gateMode) == 1);         // Modern
    }

    SECTION ("the tuned v0.2.0 factory presets stay on the Classic engines")
    {
        // These were voiced against the v0.2.0 DSP. Moving them to the Circuit
        // engine would change how every one of them sounds, so they pin
        // Classic explicitly rather than relying on the version gate.
        for (const auto* name : { "Glue & Grind", "Sub Lock", "Throat", "Fuzz Wall",
                                   "Cut Through", "Definition Only", "Clean Low, Loud Top",
                                   "Cab-Colored Grind" })
        {
            INFO ("preset: " << name);
            REQUIRE (manager.loadPreset (name));

            CHECK (choiceIndexOf (processor, ParamIDs::driveEngine) == 0);
            CHECK (choiceIndexOf (processor, ParamIDs::lowCompDetector) == 0);
            CHECK (choiceIndexOf (processor, ParamIDs::gateMode) == 0);
        }
    }

    SECTION ("the new Circuit presets load, select the Circuit engines, and render finite")
    {
        for (const auto* name : { "Circuit Foundation", "Circuit Grind", "Circuit Knife" })
        {
            INFO ("preset: " << name);
            REQUIRE (manager.loadPreset (name));

            CHECK (choiceIndexOf (processor, ParamIDs::driveEngine) == 1);
            CHECK (choiceIndexOf (processor, ParamIDs::lowCompDetector) == 1);
            CHECK (choiceIndexOf (processor, ParamIDs::gateMode) == 1);

            processor.setPlayConfigDetails (2, 2, 48000.0, 512);
            processor.prepareToPlay (48000.0, 512);
            processor.reset();

            juce::AudioBuffer<float> buffer (2, 4096);

            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                    buffer.setSample (channel, sample,
                                       0.4f * std::sin (juce::MathConstants<float>::twoPi * 80.0f
                                                         * static_cast<float> (sample) / 48000.0f));

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> block (2, 512);

            for (int offset = 0; offset + 512 <= buffer.getNumSamples(); offset += 512)
            {
                for (int channel = 0; channel < 2; ++channel)
                    block.copyFrom (channel, 0, buffer, channel, offset, 512);

                processor.processBlock (block, midi);

                for (int channel = 0; channel < 2; ++channel)
                    for (int sample = 0; sample < 512; ++sample)
                        REQUIRE (std::isfinite (block.getSample (channel, sample)));
            }
        }
    }
}

TEST_CASE ("T20: legacy user presets are migrated onto the Classic engines", "[presets][migration]")
{
    ScopedTestDirectory scratch;

    const auto importFixture = [&scratch] (CryptaAudioProcessor& processor,
                                            PresetManager& manager,
                                            const juce::String& fixtureName)
    {
        const auto source = presetFixture (fixtureName);
        REQUIRE (source.existsAsFile());

        juce::String error;
        const auto imported = manager.importPresetFile (source, error);
        INFO ("import error: " << error);
        REQUIRE (imported);
        juce::ignoreUnused (processor);
    };

    SECTION ("a tuned v0.2.0 user preset")
    {
        CryptaAudioProcessor processor;
        PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

        importFixture (processor, manager, "preset_v020_user_tuned.json");

        CHECK (choiceIndexOf (processor, ParamIDs::driveEngine) == 0);
        CHECK (choiceIndexOf (processor, ParamIDs::lowCompDetector) == 0);
        CHECK (choiceIndexOf (processor, ParamIDs::gateMode) == 0);

        // Its own values still arrive intact.
        CHECK (getParam (processor, ParamIDs::midDrive) == Catch::Approx (45.0f).margin (0.1f));
        CHECK (getParam (processor, ParamIDs::highDrive) == Catch::Approx (65.0f).margin (0.1f));
    }

    SECTION ("a user-saved Default that shadows the factory one")
    {
        // The preset an existing user's fresh session actually boots into.
        // Without the back-fill this is the case that would silently change
        // someone's default sound.
        CryptaAudioProcessor processor;
        PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

        importFixture (processor, manager, "preset_v020_user_default_shadow.json");

        CHECK (choiceIndexOf (processor, ParamIDs::driveEngine) == 0);
        CHECK (choiceIndexOf (processor, ParamIDs::lowCompDetector) == 0);
        CHECK (choiceIndexOf (processor, ParamIDs::gateMode) == 0);
    }

    SECTION ("a preset with no pluginVersion key at all")
    {
        // Absent version counts as older than anything - the safe reading.
        CryptaAudioProcessor processor;
        PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

        importFixture (processor, manager, "preset_noversion_user.json");

        CHECK (choiceIndexOf (processor, ParamIDs::driveEngine) == 0);
        CHECK (choiceIndexOf (processor, ParamIDs::lowCompDetector) == 0);
        CHECK (choiceIndexOf (processor, ParamIDs::gateMode) == 0);
    }
}

TEST_CASE ("T20: presets from v0.3.0 onwards are taken at their word", "[presets][migration]")
{
    ScopedTestDirectory scratch;

    SECTION ("explicit Circuit values are not overridden")
    {
        CryptaAudioProcessor processor;
        PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

        const auto source = presetFixture ("preset_v030_user_circuit.json");
        REQUIRE (source.existsAsFile());

        juce::String error;
        REQUIRE (manager.importPresetFile (source, error));

        CHECK (choiceIndexOf (processor, ParamIDs::driveEngine) == 1);
        CHECK (choiceIndexOf (processor, ParamIDs::lowCompDetector) == 1);
        CHECK (choiceIndexOf (processor, ParamIDs::gateMode) == 1);
        CHECK (getParam (processor, ParamIDs::highBias) == Catch::Approx (30.0f).margin (0.1f));
    }

    SECTION ("a v0.3.0 preset that omits a key keeps that key's default, not the legacy value")
    {
        // The gate is version-based, not merely key-presence-based. A preset
        // saved by v0.3.0 that names driveEngine but omits the other two means
        // "the others are at their defaults" - i.e. the new engines - and must
        // not be dragged back to Classic.
        CryptaAudioProcessor processor;
        PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

        const auto source = presetFixture ("preset_v030_user_partial.json");
        REQUIRE (source.existsAsFile());

        juce::String error;
        REQUIRE (manager.importPresetFile (source, error));

        CHECK (choiceIndexOf (processor, ParamIDs::driveEngine) == 0);      // as stated
        CHECK (choiceIndexOf (processor, ParamIDs::lowCompDetector) == 1);  // default
        CHECK (choiceIndexOf (processor, ParamIDs::gateMode) == 1);         // default
    }
}

//==============================================================================
// Vendor identity (basilica-audio/.github#2, ADR 0001): user presets moved from
// the `Yves Vogl` manufacturer folder to `Basilica Audio`, and a user must not
// lose a preset over it. These cases pin the migration's whole contract - it
// adopts, it copies rather than moves, it never overwrites, it stays out of the
// real per-user folder during tests, and both folder shapes match the platform
// convention (asserted on whichever platform is running, so macOS and Windows
// CI each check their own).
namespace
{
    // Writes a preset document straight to disk rather than going through
    // PresetManager::saveUserPreset(), so the migration is exercised against a
    // file shaped like one an older build left behind - not one this build
    // happened to produce a moment earlier.
    void writeBrandingLegacyPresetFile (const juce::File& directory,
                                const juce::String& presetName,
                                const juce::String& category,
                                const juce::String& pluginId)
    {
        directory.createDirectory();

        auto* preset = new juce::DynamicObject();
        preset->setProperty ("format", PresetManager::presetFormatTag);
        preset->setProperty ("plugin", pluginId);
        preset->setProperty ("pluginVersion", "0.1.0-legacy");
        preset->setProperty ("name", presetName);
        preset->setProperty ("category", category);
        preset->setProperty ("parameters", juce::var (new juce::DynamicObject()));

        const auto written = directory
            .getChildFile (juce::File::createLegalFileName (presetName)
                            + PresetManager::presetFileExtension)
            .replaceWithText (juce::JSON::toString (juce::var (preset), false));

        REQUIRE (written);
    }

    bool brandingContainsUserPreset (const std::vector<PresetManager::PresetEntry>& entries,
                             const juce::String& name)
    {
        return std::any_of (entries.begin(), entries.end(),
                            [&name] (const PresetManager::PresetEntry& entry)
                            { return entry.name == name && ! entry.isFactory; });
    }

    juce::String brandingCategoryOf (const std::vector<PresetManager::PresetEntry>& entries,
                             const juce::String& name)
    {
        for (auto& entry : entries)
            if (entry.name == name)
                return entry.category;

        return {};
    }
}

TEST_CASE ("PresetManager: a preset saved under the legacy manufacturer folder still loads", "[presets][branding]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory legacyDirectory;
    ScopedTestDirectory currentDirectory;

    auto config = makeIsolatedConfig (currentDirectory.dir);
    config.legacyManufacturerName = "Yves Vogl";
    config.legacyUserPresetsDirectoryOverrideForTests = legacyDirectory.dir;

    writeBrandingLegacyPresetFile (legacyDirectory.dir, "Legacy Preset", "User", config.pluginId);

    PresetManager manager (processor.apvts, config, makeTestFactoryPresetAssets());

    REQUIRE (brandingContainsUserPreset (manager.getAllPresets(), "Legacy Preset"));
    REQUIRE (manager.loadPreset ("Legacy Preset"));
    REQUIRE (manager.getCurrentPresetName() == juce::String ("Legacy Preset"));
    REQUIRE_FALSE (manager.isCurrentPresetFactory());

    const auto fileName = juce::String ("Legacy Preset") + PresetManager::presetFileExtension;

    // Copied, not moved: an older build of this plugin - or a downgrade - still
    // finds its own presets exactly where it left them.
    REQUIRE (legacyDirectory.dir.getChildFile (fileName).existsAsFile());
    REQUIRE (currentDirectory.dir.getChildFile (fileName).existsAsFile());
}

TEST_CASE ("PresetManager: the legacy migration never overwrites a preset already in the new folder", "[presets][branding]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory legacyDirectory;
    ScopedTestDirectory currentDirectory;

    auto config = makeIsolatedConfig (currentDirectory.dir);
    config.legacyManufacturerName = "Yves Vogl";
    config.legacyUserPresetsDirectoryOverrideForTests = legacyDirectory.dir;

    writeBrandingLegacyPresetFile (legacyDirectory.dir, "Shared Name", "From Legacy", config.pluginId);
    writeBrandingLegacyPresetFile (currentDirectory.dir, "Shared Name", "From Current", config.pluginId);

    PresetManager manager (processor.apvts, config, makeTestFactoryPresetAssets());

    REQUIRE (brandingCategoryOf (manager.getAllPresets(), "Shared Name") == juce::String ("From Current"));

    // Idempotent: constructing a second manager over the same pair of folders
    // must not suddenly prefer the legacy copy either.
    PresetManager second (processor.apvts, config, makeTestFactoryPresetAssets());
    REQUIRE (brandingCategoryOf (second.getAllPresets(), "Shared Name") == juce::String ("From Current"));
}

TEST_CASE ("PresetManager: overriding only the current preset directory disables the legacy lookup", "[presets][branding]")
{
    ScopedTestDirectory currentDirectory;

    auto config = makeIsolatedConfig (currentDirectory.dir);
    config.legacyManufacturerName = "Yves Vogl";

    // Without this, a test that redirects only the current directory would read
    // - and copy from - the real presets of whoever is running the suite.
    REQUIRE (PresetManager::getLegacyUserPresetsDirectory (config) == juce::File());
}

TEST_CASE ("PresetManager: current and legacy preset folders follow the platform convention", "[presets][branding]")
{
    PresetManagerConfig config;
    config.pluginName = "Crypta";
    config.manufacturerName = "Basilica Audio";
    config.legacyManufacturerName = "Yves Vogl";

    const auto current = PresetManager::getUserPresetsDirectory (config);
    const auto legacy = PresetManager::getLegacyUserPresetsDirectory (config);

   #if JUCE_MAC
    const auto presetsRoot = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                 .getChildFile ("Library")
                                 .getChildFile ("Audio")
                                 .getChildFile ("Presets");

    REQUIRE (current == presetsRoot.getChildFile ("Basilica Audio").getChildFile ("Crypta"));
    REQUIRE (legacy == presetsRoot.getChildFile ("Yves Vogl").getChildFile ("Crypta"));
   #else
    const auto applicationData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

    REQUIRE (current == applicationData.getChildFile ("Basilica Audio")
                            .getChildFile ("Crypta").getChildFile ("Presets"));
    REQUIRE (legacy == applicationData.getChildFile ("Yves Vogl")
                            .getChildFile ("Crypta").getChildFile ("Presets"));
   #endif

    // The two are the same path shape and differ only in the manufacturer
    // component - which is what makes "copy from legacy to current" a rename of
    // one folder rather than a move between two unrelated layouts.
    REQUIRE (current != legacy);
    REQUIRE (current.getFileName() == legacy.getFileName());
}

TEST_CASE ("PresetManager: an empty legacy manufacturer name disables the migration", "[presets][branding]")
{
    PresetManagerConfig config;
    config.pluginName = "Crypta";
    config.manufacturerName = "Basilica Audio";

    REQUIRE (PresetManager::getLegacyUserPresetsDirectory (config) == juce::File());

    // And so does a legacy name that has already caught up with the current one,
    // so re-running a completed rename is a no-op rather than a self-copy.
    config.legacyManufacturerName = "Basilica Audio";
    REQUIRE (PresetManager::getLegacyUserPresetsDirectory (config) == juce::File());
}
