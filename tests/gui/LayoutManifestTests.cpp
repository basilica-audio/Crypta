#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>

#include <BinaryData.h>

#include <set>

// The wave-3 layout manifest (resources/gui/layout-manifest.json) is the
// single source of the composited editor's control surface - these tests
// pin it against BOTH sides it has to agree with: the rendered control
// inventory (rollout-2026-07/crypta/control-inventory.md: 33 knobs + 1
// selector + 5 toggles + 2 VU meters) and the live APVTS parameter set.
namespace
{
    CryptaAudioProcessorEditor::Manifest parsedManifest()
    {
        return CryptaAudioProcessorEditor::parseLayoutManifest();
    }

    int countOfType (const CryptaAudioProcessorEditor::Manifest& m, const juce::String& type)
    {
        int count = 0;

        for (const auto& control : m.controls)
            if (control.type == type)
                ++count;

        return count;
    }
}

TEST_CASE ("Manifest control counts match the rendered control inventory", "[gui][layout]")
{
    const auto manifest = parsedManifest();

    CHECK (countOfType (manifest, "knob") == 33);
    CHECK (countOfType (manifest, "selector") == 1);
    CHECK (countOfType (manifest, "toggle") == 5);
    CHECK (countOfType (manifest, "vu") == 2);
    CHECK ((int) manifest.controls.size() == 41);
}

TEST_CASE ("Every manifest control id resolves to an APVTS parameter of the matching kind", "[gui][layout]")
{
    CryptaAudioProcessor processor;
    const auto manifest = parsedManifest();

    for (const auto& control : manifest.controls)
    {
        if (control.type == "vu")
        {
            CHECK ((control.tap == "inputPeak" || control.tap == "outputPeak"));
            continue;
        }

        auto* parameter = processor.apvts.getParameter (control.id);

        INFO ("manifest id: " << control.id);
        REQUIRE (parameter != nullptr);

        if (control.type == "toggle")
            CHECK (dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr);
        else if (control.type == "selector")
            CHECK (dynamic_cast<juce::AudioParameterChoice*> (parameter) != nullptr);
        else
            CHECK (dynamic_cast<juce::AudioParameterBool*> (parameter) == nullptr);
    }
}

TEST_CASE ("Manifest ids are unique and every control sits inside the plate", "[gui][layout]")
{
    const auto manifest = parsedManifest();

    REQUIRE (manifest.plateWidth1x > 0);
    REQUIRE (manifest.plateHeight1x > 0);

    std::set<juce::String> seen;

    for (const auto& control : manifest.controls)
    {
        INFO ("manifest id: " << control.id);
        CHECK (seen.insert (control.id).second);
        CHECK (control.size > 0.0f);

        const auto half = control.size * 0.5f;
        CHECK (control.cx - half >= 0.0f);
        CHECK (control.cx + half <= (float) manifest.plateWidth1x);
        CHECK (control.cy - half >= 0.0f);
        CHECK (control.cy + half <= (float) manifest.plateHeight1x);
    }
}

TEST_CASE ("Manifest references only binary resources that exist, and non-meter controls carry labels", "[gui][layout]")
{
    const auto manifest = parsedManifest();

    const auto resourceExists = [] (const juce::String& name)
    {
        int size = 0;
        return BinaryData::getNamedResource (name.toRawUTF8(), size) != nullptr && size > 0;
    };

    CHECK (resourceExists (manifest.plateBinary));

    REQUIRE (! manifest.sprites.empty());

    for (const auto& [name, sprite] : manifest.sprites)
    {
        INFO ("sprite: " << name);
        CHECK (resourceExists (sprite.binary));
        CHECK (sprite.width > 0.0f);
        CHECK (sprite.height > 0.0f);
    }

    for (const auto& control : manifest.controls)
    {
        INFO ("manifest id: " << control.id);
        CHECK (control.label.isNotEmpty());
    }

    REQUIRE (manifest.vuTicks.size() >= 2);
    CHECK (manifest.vuTicks.front().db < manifest.vuTicks.back().db);
}

TEST_CASE ("The editor instantiates exactly one live control per manifest entry", "[gui][layout]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    CryptaAudioProcessorEditor editor (processor);
    const auto manifest = parsedManifest();

    int liveControls = 0;

    for (int i = 0; i < editor.getNumChildComponents(); ++i)
    {
        auto* child = editor.getChildComponent (i);

        if (dynamic_cast<basilica::gui::SpriteKnob*> (child) != nullptr
            || dynamic_cast<basilica::gui::SpriteToggle*> (child) != nullptr
            || dynamic_cast<basilica::gui::NeedleDial*> (child) != nullptr)
            ++liveControls;
    }

    CHECK (liveControls == (int) manifest.controls.size());
}
