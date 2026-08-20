#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "gui/BusPanel.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <memory>

// Resizable editor with stored scale (issue #28).
//
// The mechanism under test is the one JUCE 8.0.14 documents for plug-in
// editors: setResizable (true, true) attaches a ResizableCornerComponent,
// setResizeLimits() installs the default ComponentBoundsConstrainer, and
// setFixedAspectRatio() on that constrainer turns every drag into a pure
// scale change. AudioProcessorEditor::setScaleFactor() is deliberately NOT
// used for this - it is the HOST's channel for DPI scaling
// (juce_AudioProcessorEditor.h:127-131) and sets a transform on the editor
// itself; the user scale lives on the editor's content child instead, so the
// two compose rather than collide.
//
// Persistence rides on the APVTS state tree (a root-level property), so it
// round-trips through the processor's existing get/setStateInformation()
// pair - which is what the round-trip test below actually exercises, rather
// than a bespoke test-only path.
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
}

TEST_CASE ("A fresh instance opens at unity scale and its own design size", "[gui][scale]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    // Nothing persisted yet.
    CHECK_FALSE (processor.apvts.state.hasProperty (CryptaAudioProcessorEditor::getScaleStatePropertyId()));
    CHECK (CryptaAudioProcessorEditor::readPersistedScale (processor.apvts.state)
           == Catch::Approx (CryptaAudioProcessorEditor::defaultEditorScale));

    CryptaAudioProcessorEditor editor (processor);

    CHECK (editor.getEditorScale() == Catch::Approx (1.0));
    CHECK (editor.getWidth() == editor.getDesignWidth());
    CHECK (editor.getHeight() == editor.getDesignHeight());

    // The design size is computed from the layout tables, never zero.
    CHECK (editor.getDesignWidth() > 0);
    CHECK (editor.getDesignHeight() > 0);

    // Merely OPENING the window must not mutate the session's saved state -
    // hosts treat a state change as an edit. The property appears only once
    // the user actually picks a different scale.
    CHECK_FALSE (processor.apvts.state.hasProperty (CryptaAudioProcessorEditor::getScaleStatePropertyId()));

    editor.setEditorScale (1.25);
    CHECK (processor.apvts.state.hasProperty (CryptaAudioProcessorEditor::getScaleStatePropertyId()));
    CHECK (CryptaAudioProcessorEditor::readPersistedScale (processor.apvts.state)
           == Catch::Approx (1.25).margin (0.005));

    // Going back to unity is itself a choice worth storing, so the property
    // stays (and stays correct) rather than silently disappearing.
    editor.setEditorScale (1.0);
    CHECK (CryptaAudioProcessorEditor::readPersistedScale (processor.apvts.state)
           == Catch::Approx (1.0));
}

TEST_CASE ("The editor is resizable with an aspect-locked constrainer and a corner resizer", "[gui][scale]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    CHECK (editor.isResizable());

    auto* constrainer = editor.getConstrainer();
    REQUIRE (constrainer != nullptr);

    // Aspect-ratio-locked resize (issue #28's first acceptance criterion).
    CHECK (constrainer->getFixedAspectRatio()
           == Catch::Approx ((double) editor.getDesignWidth() / (double) editor.getDesignHeight()));

    // Size limits derived from the supported scale range.
    CHECK (constrainer->getMinimumWidth()
           == (int) std::lround (editor.getDesignWidth() * CryptaAudioProcessorEditor::minimumEditorScale));
    CHECK (constrainer->getMaximumWidth()
           == (int) std::lround (editor.getDesignWidth() * CryptaAudioProcessorEditor::maximumEditorScale));

    // setResizable (true, true) attached a bottom-right corner grip.
    int cornerResizers = 0;
    visitDescendants<juce::ResizableCornerComponent> (editor, [&] (juce::ResizableCornerComponent&)
    {
        ++cornerResizers;
    });

    CHECK (cornerResizers == 1);
}

TEST_CASE ("Resizing is a pure scale change: the layout never re-flows", "[gui][scale]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    // Panel geometry in the (unscaled) design space, before resizing.
    std::vector<juce::Rectangle<int>> before;
    visitDescendants<basilica::gui::BusPanel> (editor, [&] (basilica::gui::BusPanel& panel)
    {
        before.push_back (panel.getBounds());
    });

    REQUIRE (before.size() == 10);

    editor.setEditorScale (1.5);

    CHECK (editor.getWidth() == Catch::Approx ((int) std::lround (editor.getDesignWidth() * 1.5)).margin (1));
    CHECK (editor.getEditorScale() == Catch::Approx (1.5).margin (0.005));

    // Aspect ratio preserved through the constrainer.
    const auto designRatio = (double) editor.getDesignWidth() / (double) editor.getDesignHeight();
    CHECK ((double) editor.getWidth() / (double) editor.getHeight()
           == Catch::Approx (designRatio).margin (0.01));

    std::vector<juce::Rectangle<int>> after;
    visitDescendants<basilica::gui::BusPanel> (editor, [&] (basilica::gui::BusPanel& panel)
    {
        after.push_back (panel.getBounds());
    });

    REQUIRE (after.size() == before.size());

    // Identical design-space geometry: nothing was re-flowed, re-wrapped or
    // clipped - the window is simply drawn through a scale transform.
    for (size_t i = 0; i < before.size(); ++i)
    {
        INFO ("panel " << i);
        CHECK (after[i] == before[i]);
    }

    // Shrinking behaves the same way.
    editor.setEditorScale (0.75);
    CHECK (editor.getEditorScale() == Catch::Approx (0.75).margin (0.005));

    std::vector<juce::Rectangle<int>> shrunk;
    visitDescendants<basilica::gui::BusPanel> (editor, [&] (basilica::gui::BusPanel& panel)
    {
        shrunk.push_back (panel.getBounds());
    });

    for (size_t i = 0; i < before.size(); ++i)
        CHECK (shrunk[i] == before[i]);
}

TEST_CASE ("The chosen scale is clamped to the supported range", "[gui][scale]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    editor.setEditorScale (99.0);
    CHECK (editor.getEditorScale()
           == Catch::Approx (CryptaAudioProcessorEditor::maximumEditorScale).margin (0.01));

    editor.setEditorScale (0.01);
    CHECK (editor.getEditorScale()
           == Catch::Approx (CryptaAudioProcessorEditor::minimumEditorScale).margin (0.01));

    // Garbage in a hand-edited session must never produce a zero-sized or
    // absurd window.
    const auto id = CryptaAudioProcessorEditor::getScaleStatePropertyId();
    auto state = processor.apvts.state;

    state.setProperty (id, 0.0, nullptr);
    CHECK (CryptaAudioProcessorEditor::readPersistedScale (state)
           == Catch::Approx (CryptaAudioProcessorEditor::defaultEditorScale));

    state.setProperty (id, -3.0, nullptr);
    CHECK (CryptaAudioProcessorEditor::readPersistedScale (state)
           == Catch::Approx (CryptaAudioProcessorEditor::defaultEditorScale));

    state.setProperty (id, "not a number", nullptr);
    CHECK (CryptaAudioProcessorEditor::readPersistedScale (state)
           == Catch::Approx (CryptaAudioProcessorEditor::defaultEditorScale));

    state.setProperty (id, 12.0, nullptr);
    CHECK (CryptaAudioProcessorEditor::readPersistedScale (state)
           == Catch::Approx (CryptaAudioProcessorEditor::maximumEditorScale));

    state.setProperty (id, 0.05, nullptr);
    CHECK (CryptaAudioProcessorEditor::readPersistedScale (state)
           == Catch::Approx (CryptaAudioProcessorEditor::minimumEditorScale));
}

TEST_CASE ("The scale survives a full plugin-state round trip", "[gui][scale][state]")
{
    constexpr double chosenScale = 1.35;

    int savedWidth = 0, savedHeight = 0;
    juce::MemoryBlock savedState;

    {
        CryptaAudioProcessor processor;
        processor.prepareToPlay (48000.0, 512);

        auto editor = std::make_unique<CryptaAudioProcessorEditor> (processor);
        editor->setEditorScale (chosenScale);

        savedWidth = editor->getWidth();
        savedHeight = editor->getHeight();

        CHECK (savedWidth != editor->getDesignWidth()); // the test would be vacuous at unity

        // Closing the editor must not lose the setting - the state lives on
        // the processor, not on the (transient) editor.
        editor.reset();

        processor.getStateInformation (savedState);
    }

    CHECK (savedState.getSize() > 0);

    // A brand-new processor instance (a reopened session) restores it.
    CryptaAudioProcessor restored;
    restored.prepareToPlay (48000.0, 512);
    restored.setStateInformation (savedState.getData(), (int) savedState.getSize());

    CHECK (CryptaAudioProcessorEditor::readPersistedScale (restored.apvts.state)
           == Catch::Approx (chosenScale).margin (0.005));

    CryptaAudioProcessorEditor restoredEditor (restored);

    CHECK (restoredEditor.getWidth() == savedWidth);
    CHECK (restoredEditor.getHeight() == savedHeight);
    CHECK (restoredEditor.getEditorScale() == Catch::Approx (chosenScale).margin (0.005));
}

TEST_CASE ("Persisting the scale does not disturb parameter state", "[gui][scale][state]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    // A non-default parameter value that must survive alongside the scale.
    auto* driveParameter = processor.apvts.getParameter ("highDrive");
    REQUIRE (driveParameter != nullptr);
    driveParameter->setValueNotifyingHost (driveParameter->convertTo0to1 (73.5f));

    {
        CryptaAudioProcessorEditor editor (processor);
        editor.setEditorScale (0.8);
    }

    juce::MemoryBlock savedState;
    processor.getStateInformation (savedState);

    CryptaAudioProcessor restored;
    restored.prepareToPlay (48000.0, 512);
    restored.setStateInformation (savedState.getData(), (int) savedState.getSize());

    // Every parameter came back...
    CHECK ((int) restored.getParameters().size() == 54);
    CHECK (restored.apvts.getRawParameterValue ("highDrive")->load()
           == Catch::Approx (73.5f).margin (1.0e-2));

    // ...and so did the scale, as a root-level property that is NOT mistaken
    // for a parameter child (JUCE 8.0.14 APVTS only reacts to property
    // changes on its PARAM children, juce_AudioProcessorValueTreeState.cpp:442).
    CHECK (CryptaAudioProcessorEditor::readPersistedScale (restored.apvts.state)
           == Catch::Approx (0.8).margin (0.005));

    int paramChildren = 0;

    for (const auto& child : restored.apvts.state)
        if (child.hasType ("PARAM"))
            ++paramChildren;

    CHECK (paramChildren == 54);
}

TEST_CASE ("Loading a state saved before the scale existed opens at the default size", "[gui][scale][state]")
{
    // Backward compatibility: a v0.3.x session has no editorScale property
    // at all, and must open at unity rather than at a zero-sized window.
    CryptaAudioProcessor legacy;
    legacy.prepareToPlay (48000.0, 512);

    juce::MemoryBlock legacyState;
    legacy.getStateInformation (legacyState); // no editor was ever opened

    CryptaAudioProcessor restored;
    restored.prepareToPlay (48000.0, 512);
    restored.setStateInformation (legacyState.getData(), (int) legacyState.getSize());

    CHECK_FALSE (restored.apvts.state.hasProperty (CryptaAudioProcessorEditor::getScaleStatePropertyId()));

    CryptaAudioProcessorEditor editor (restored);
    CHECK (editor.getEditorScale() == Catch::Approx (1.0));
    CHECK (editor.getWidth() == editor.getDesignWidth());
}
