#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "gui/BusPanel.h"
#include "gui/NeedleMeter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <vector>

// Vector-editor layout tests (issues #25 / #26). Unlike the photoreal
// siblings (whose EditorLayoutTests assert hand-measured pixel manifests
// against baked master renders), Crypta's geometry is COMPUTED from the
// layout constants + control tables in PluginEditor.cpp - so the manifest
// under test here is the real constructed component tree itself:
// containment, no overlap, and full parameter coverage. Any arithmetic slip
// in the layout constants shows up as a concrete clipped/colliding control.
//
// juce::ScopedJuceInitialiser_GUI is installed once for the whole test binary
// in tests/TestMain.cpp, so constructing Components is safe in this headless
// console executable with no message loop or native window/peer.
namespace
{
    template <typename ComponentType>
    void visitDescendants (juce::Component& parent, const std::function<void (ComponentType&)>& visit)
    {
        for (int i = 0; i < parent.getNumChildComponents(); ++i)
        {
            auto* child = parent.getChildComponent (i);

            if (auto* typed = dynamic_cast<ComponentType*> (child))
                visit (*typed);

            visitDescendants<ComponentType> (*child, visit);
        }
    }

    // Position in the editor's UNSCALED design space: every test here runs
    // at scale 1.0 (asserted below), where the content transform is the
    // identity, so summing parent positions is exact.
    juce::Rectangle<int> boundsInEditor (const juce::Component& component, const juce::Component& editor)
    {
        auto bounds = component.getBounds();

        for (const auto* ancestor = component.getParentComponent();
             ancestor != nullptr && ancestor != &editor;
             ancestor = ancestor->getParentComponent())
        {
            bounds += ancestor->getPosition();
        }

        return bounds;
    }

    // Signal-flow order, which is also creation order and therefore focus
    // order - see the PluginEditor class docs.
    constexpr const char* panelTitles[] = { "Input", "Noise Gate", "Crossover", "Low Band",
                                            "Drive Engine", "Mid Band", "High Band",
                                            "Cabinet", "EQ", "Output" };
}

TEST_CASE ("Every automatable parameter has exactly one attached control", "[gui][layout]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    int sliders = 0, toggles = 0;
    visitDescendants<juce::Slider> (editor, [&] (juce::Slider&) { ++sliders; });
    visitDescendants<juce::ToggleButton> (editor, [&] (juce::ToggleButton&) { ++toggles; });

    // The APVTS carries 42 float + 4 choice + 8 bool parameters = 54
    // (src/params/ParameterIds.h; v0.4.0 added the three Graaawl controls to
    // the Low Band panel). One knob per float/choice parameter, one toggle per
    // bool parameter - no parameter may be left off the surface, and no
    // control may exist without a parameter.
    CHECK ((int) processor.getParameters().size() == 54);
    CHECK (sliders == 46);
    CHECK (toggles == 8);
    CHECK (sliders + toggles == (int) processor.getParameters().size());
}

TEST_CASE ("Moving a knob moves its parameter - one wiring spot check per section", "[gui][layout]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    struct Expectation
    {
        const char* title;      // unique across the whole editor
        const char* parameterId;
        double sliderValue;     // a legal, non-default value
        float expectedRaw;      // denormalised parameter value afterwards
    };

    const Expectation expectations[] = {
        { "Input Gain", "inputGain", 6.0, 6.0f },          // Input
        { "Threshold", "gateThreshold", -42.0, -42.0f },   // Noise Gate (first match)
        { "Split Low", "splitLowHz", 90.0, 90.0f },        // Crossover
        { "Makeup", "lowCompMakeup", 4.5, 4.5f },          // Low Band
        { "Mid Drive", "midDrive", 70.0, 70.0f },          // Mid Band
        { "Tone", "highTone", 25.0, 25.0f },               // High Band
        { "Cab Mix", "irMix", 40.0, 40.0f },               // Cabinet
        { "Peak 1 Q", "eqPeak1Q", 2.5, 2.5f },             // EQ
        { "Output Gain", "outputGain", -3.0, -3.0f },      // Output
    };

    for (const auto& expectation : expectations)
    {
        juce::Slider* knob = nullptr;
        visitDescendants<juce::Slider> (editor, [&] (juce::Slider& s)
        {
            if (knob == nullptr && s.getTitle() == expectation.title)
                knob = &s;
        });

        REQUIRE (knob != nullptr);
        INFO ("knob \"" << expectation.title << "\" -> " << expectation.parameterId);

        auto* raw = processor.apvts.getRawParameterValue (expectation.parameterId);
        REQUIRE (raw != nullptr);

        knob->setValue (expectation.sliderValue, juce::sendNotificationSync);
        CHECK (raw->load() == Catch::Approx (expectation.expectedRaw).margin (1.0e-3));
    }
}

TEST_CASE ("All controls, labels and meters stay inside their panel; panels stay inside the editor", "[gui][layout]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    // The whole file reasons in unscaled design coordinates.
    REQUIRE (editor.getEditorScale() == Catch::Approx (1.0));

    const auto designBounds = juce::Rectangle<int> (editor.getDesignWidth(), editor.getDesignHeight());
    CHECK (designBounds.getWidth() > 0);
    CHECK (designBounds.getHeight() > 0);
    CHECK (editor.getLocalBounds() == designBounds);

    int panelsSeen = 0;

    visitDescendants<basilica::gui::BusPanel> (editor, [&] (basilica::gui::BusPanel& panel)
    {
        ++panelsSeen;
        INFO ("panel \"" << panel.getTitle().toStdString() << "\" bounds "
              << panel.getBounds().toString().toStdString());
        CHECK (designBounds.contains (boundsInEditor (panel, editor)));

        // Every direct child (knobs incl. their value boxes, attached
        // labels, toggles, the meter) must be fully inside the panel.
        for (int i = 0; i < panel.getNumChildComponents(); ++i)
        {
            const auto* child = panel.getChildComponent (i);
            INFO ("child \"" << child->getName().toStdString() << "\" bounds "
                  << child->getBounds().toString().toStdString()
                  << " in panel \"" << panel.getTitle().toStdString() << "\" "
                  << panel.getLocalBounds().toString().toStdString());
            CHECK (panel.getLocalBounds().contains (child->getBounds()));
        }
    });

    CHECK (panelsSeen == 10);

    // No control may sit under the section header rule either.
    int controlsBelowHeader = 0;

    visitDescendants<juce::Slider> (editor, [&] (juce::Slider& s)
    {
        ++controlsBelowHeader;
        INFO ("knob \"" << s.getTitle().toStdString() << "\" y=" << s.getY());
        CHECK (s.getY() >= basilica::gui::BusPanel::headerHeight);
    });

    CHECK (controlsBelowHeader == 46);
}

TEST_CASE ("No two interactive controls or meters overlap", "[gui][layout]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    struct Entry
    {
        juce::String name;
        juce::Rectangle<int> bounds;
    };

    std::vector<Entry> entries;

    const auto collect = [&] (juce::Component& component)
    {
        entries.push_back ({ component.getTitle(), boundsInEditor (component, editor) });
    };

    visitDescendants<juce::Slider> (editor, [&] (juce::Slider& s) { collect (s); });
    visitDescendants<juce::ToggleButton> (editor, [&] (juce::ToggleButton& t) { collect (t); });
    visitDescendants<basilica::gui::NeedleMeter> (editor, [&] (basilica::gui::NeedleMeter& m) { collect (m); });

    // 46 knobs + 8 toggles + 4 meters - the pairwise scan below must not
    // pass vacuously on an empty collection.
    REQUIRE (entries.size() == 58);

    for (size_t i = 0; i < entries.size(); ++i)
    {
        for (size_t j = i + 1; j < entries.size(); ++j)
        {
            INFO ("\"" << entries[i].name.toStdString() << "\" " << entries[i].bounds.toString().toStdString()
                  << " vs \"" << entries[j].name.toStdString() << "\" " << entries[j].bounds.toString().toStdString());
            CHECK_FALSE (entries[i].bounds.intersects (entries[j].bounds));
        }
    }
}

TEST_CASE ("Panels do not overlap each other or the preset bar", "[gui][layout]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    std::vector<juce::Rectangle<int>> panelBounds;

    visitDescendants<basilica::gui::BusPanel> (editor, [&] (basilica::gui::BusPanel& panel)
    {
        panelBounds.push_back (boundsInEditor (panel, editor));
    });

    REQUIRE (panelBounds.size() == 10);

    for (size_t i = 0; i < panelBounds.size(); ++i)
        for (size_t j = i + 1; j < panelBounds.size(); ++j)
            CHECK_FALSE (panelBounds[i].intersects (panelBounds[j]));

    // The preset bar band sits above all panels.
    basilica::presets::PresetBar* presetBar = nullptr;
    visitDescendants<basilica::presets::PresetBar> (editor, [&] (basilica::presets::PresetBar& bar)
    {
        presetBar = &bar;
    });

    REQUIRE (presetBar != nullptr);
    const auto presetBarBounds = boundsInEditor (*presetBar, editor);

    for (const auto& bounds : panelBounds)
    {
        CHECK_FALSE (presetBarBounds.intersects (bounds));
        CHECK (presetBarBounds.getBottom() <= bounds.getY());
    }
}

TEST_CASE ("Every knob's visible label text matches its accessible title (label-in-name)", "[gui][layout][a11y]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    // WCAG 2.5.3 Label in Name: the label painted next to a knob must be
    // the same string AT users hear as the control's name - a mismatch
    // breaks voice-control users ("click Input Gain" targeting a control
    // whose accessible name is something else).
    int labelledKnobs = 0;

    visitDescendants<juce::Label> (editor, [&] (juce::Label& label)
    {
        if (auto* attached = label.getAttachedComponent())
        {
            if (auto* slider = dynamic_cast<juce::Slider*> (attached))
            {
                ++labelledKnobs;
                INFO ("label \"" << label.getText().toStdString() << "\" for knob \""
                      << slider->getTitle().toStdString() << "\"");
                CHECK (label.getText() == slider->getTitle());
            }
        }
    });

    // Every one of the 46 knobs carries an attached, matching label.
    CHECK (labelledKnobs == 46);

    // Toggles carry their own legend, which must equally match their title.
    int toggleCount = 0;

    visitDescendants<juce::ToggleButton> (editor, [&] (juce::ToggleButton& toggle)
    {
        ++toggleCount;
        CHECK (toggle.getButtonText() == toggle.getTitle());
    });

    CHECK (toggleCount == 8);
}

TEST_CASE ("The section panels appear in signal-flow order, top-left to bottom-right", "[gui][layout]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    std::vector<basilica::gui::BusPanel*> ordered;
    visitDescendants<basilica::gui::BusPanel> (editor, [&] (basilica::gui::BusPanel& panel)
    {
        ordered.push_back (&panel);
    });

    REQUIRE (ordered.size() == std::size (panelTitles));

    juce::Rectangle<int> previous;

    for (size_t i = 0; i < ordered.size(); ++i)
    {
        INFO ("panel " << i << " = \"" << ordered[i]->getTitle().toStdString() << "\"");
        CHECK (ordered[i]->getTitle() == panelTitles[i]);

        const auto bounds = boundsInEditor (*ordered[i], editor);

        if (i > 0)
        {
            // Reading order: each panel starts at or below the previous
            // one's top, and if they share a band it starts to its right.
            const auto sameBand = bounds.getY() == previous.getY();
            CHECK (bounds.getY() >= previous.getY());

            if (sameBand)
                CHECK (bounds.getX() >= previous.getRight());
            else
                CHECK (bounds.getY() >= previous.getBottom());
        }

        previous = bounds;
    }
}
