#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "gui/BusPanel.h"
#include "gui/NeedleMeter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <vector>

// Accessible parameter surface (issues #26 / #46): asserts the actual
// AccessibilityHandler-level behaviour of the vector editor, not just that
// it constructs. juce::ScopedJuceInitialiser_GUI is installed once for the
// whole test binary in tests/TestMain.cpp, so constructing Components is
// safe in this headless console executable with no message loop or native
// window/peer.
//
// Deliberately calls createAccessibilityHandler() directly rather than
// getAccessibilityHandler(): the latter (JUCE 8.0.14
// juce_Component.cpp:3323-3326) only returns a handler once the component
// has a live native window peer, which this headless binary never has.
// createAccessibilityHandler() is public API safely callable independent of
// any live OS accessibility bridge (see its docs in juce_Component.h).
namespace
{
    // Controls live inside one BusPanel per section, which in turn lives
    // inside the editor's scaling content component - all lookups walk the
    // tree recursively.
    template <typename ComponentType>
    ComponentType* findDescendantByTitle (juce::Component& parent, const juce::String& title)
    {
        for (int i = 0; i < parent.getNumChildComponents(); ++i)
        {
            auto* child = parent.getChildComponent (i);

            if (auto* typed = dynamic_cast<ComponentType*> (child))
                if (typed->getTitle() == title)
                    return typed;

            if (auto* found = findDescendantByTitle<ComponentType> (*child, title))
                return found;
        }

        return nullptr;
    }

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

    // juce::Button::createAccessibilityHandler() (unlike juce::Slider's) is
    // declared PROTECTED (JUCE 8.0.14 juce_Button.h). Per [class.access.virt]
    // access is checked against the STATIC type naming the call, so calling
    // through juce::Component& (where it is public) compiles and virtual
    // dispatch still invokes the most-derived override. Used uniformly for
    // all component types tested here.
    std::unique_ptr<juce::AccessibilityHandler> createHandlerForTest (juce::Component& component)
    {
        return component.createAccessibilityHandler();
    }

    // Signal-flow order = creation order = focus order (WCAG 2.4.3).
    constexpr const char* panelTitles[] = { "Input", "Noise Gate", "Crossover", "Low Band",
                                            "Drive Engine", "Mid Band", "High Band",
                                            "Cabinet", "EQ", "Output" };

    basilica::gui::BusPanel* enclosingPanel (juce::Component& control)
    {
        for (auto* ancestor = control.getParentComponent(); ancestor != nullptr; ancestor = ancestor->getParentComponent())
            if (auto* panel = dynamic_cast<basilica::gui::BusPanel*> (ancestor))
                return panel;

        return nullptr;
    }
}

TEST_CASE ("Knob accessible value strings include their declared unit", "[gui][a11y]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    struct Expectation
    {
        const char* panel;
        const char* label;
        const char* unitSuffix;
    };

    // One representative per unit declared in ParameterLayout.cpp
    // (.withLabel("dB"/"dBFS"/"ms"/"Hz"/"%"/":1")), scoped to a panel
    // because labels like "Threshold" and "Ratio" repeat across sections
    // (the gate's and the low band's). The A-02 gap this guards against is
    // units being dropped entirely by the SliderAttachment's own
    // textFromValueFunction (JUCE 8.0.14 juce_ParameterAttachments.cpp:128
    // assigns it in the attachment constructor, silently clobbering
    // anything set before - PluginEditor.cpp must set its unit-suffixing
    // function AFTER constructing the attachment).
    const Expectation expectations[] = {
        { "Input", "Input Gain", "dB" },
        { "Noise Gate", "Threshold", "dB" },
        { "Noise Gate", "Attack", "ms" },
        { "Noise Gate", "Ratio", ":1" },
        { "Noise Gate", "SC Highpass", "Hz" },
        { "Crossover", "Split Low", "Hz" },
        { "Low Band", "Mix", "%" },
        { "High Band", "Tone", "%" },
        { "Output", "Ceiling", "dBFS" },
    };

    for (const auto& expectation : expectations)
    {
        auto* panel = findDescendantByTitle<basilica::gui::BusPanel> (editor, expectation.panel);
        REQUIRE (panel != nullptr);

        auto* knob = findDescendantByTitle<juce::Slider> (*panel, expectation.label);
        REQUIRE (knob != nullptr);

        const auto handler = createHandlerForTest (*knob);
        REQUIRE (handler != nullptr);

        auto* valueInterface = handler->getValueInterface();
        REQUIRE (valueInterface != nullptr);

        const auto valueText = valueInterface->getCurrentValueAsString();
        INFO ("knob \"" << expectation.panel << " / " << expectation.label
              << "\" accessible value = \"" << valueText.toStdString() << "\"");
        CHECK (valueText.endsWith (expectation.unitSuffix));
    }
}

TEST_CASE ("Choice knobs announce the current choice by NAME, not by index", "[gui][a11y]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    struct Expectation
    {
        const char* panel;
        const char* label;
        const char* defaultChoiceName; // per ParameterLayout.cpp defaults
    };

    // All four choice parameters in the layout.
    const Expectation expectations[] = {
        { "Noise Gate", "Mode", "Modern" },        // default choice index 1
        { "Low Band", "Detector", "Smooth RMS" },  // default choice index 1
        { "Drive Engine", "Engine", "Circuit" },   // default choice index 1
        { "High Band", "Voicing", "Gnaw" },        // default choice index 0
    };

    for (const auto& expectation : expectations)
    {
        auto* panel = findDescendantByTitle<basilica::gui::BusPanel> (editor, expectation.panel);
        REQUIRE (panel != nullptr);

        auto* knob = findDescendantByTitle<juce::Slider> (*panel, expectation.label);
        REQUIRE (knob != nullptr);

        const auto handler = createHandlerForTest (*knob);
        REQUIRE (handler != nullptr);

        auto* valueInterface = handler->getValueInterface();
        REQUIRE (valueInterface != nullptr);

        const auto valueText = valueInterface->getCurrentValueAsString();
        INFO ("choice knob \"" << expectation.panel << " / " << expectation.label
              << "\" accessible value = \"" << valueText.toStdString() << "\"");
        CHECK (valueText == expectation.defaultChoiceName);
    }
}

TEST_CASE ("Every interactive control is keyboard-focusable and named", "[gui][a11y]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    int slidersSeen = 0, togglesSeen = 0;

    visitDescendants<juce::Slider> (editor, [&] (juce::Slider& slider)
    {
        ++slidersSeen;
        INFO ("knob \"" << slider.getTitle().toStdString() << "\"");
        CHECK (slider.getWantsKeyboardFocus());
        CHECK (slider.getTitle().isNotEmpty());
    });

    visitDescendants<juce::ToggleButton> (editor, [&] (juce::ToggleButton& toggle)
    {
        ++togglesSeen;
        INFO ("toggle \"" << toggle.getTitle().toStdString() << "\"");
        CHECK (toggle.getWantsKeyboardFocus());
        CHECK (toggle.getTitle().isNotEmpty());
    });

    // 40 float + 4 choice parameters = 44 knobs; 7 bool parameters = 7
    // toggles (see src/params/ParameterIds.h). A zero-match walk must not
    // pass vacuously.
    CHECK (slidersSeen == 44);
    CHECK (togglesSeen == 7);

    // The preset bar's buttons are stock juce::TextButtons - focusable by
    // default, and none may have opted out. Save/Delete start DISABLED
    // (factory preset active, nothing dirty), and JUCE 8.0.14's
    // getWantsKeyboardFocus() reports false while a component is disabled
    // (juce_Component.cpp:2874) - correct WCAG behaviour (disabled controls
    // leave the tab order), so the assertion is focusable-iff-enabled.
    int presetButtonsSeen = 0, disabledPresetButtonsSeen = 0;

    visitDescendants<juce::TextButton> (editor, [&] (juce::TextButton& button)
    {
        ++presetButtonsSeen;

        if (! button.isEnabled())
            ++disabledPresetButtonsSeen;

        INFO ("preset-bar button \"" << button.getButtonText().toStdString() << "\"");
        CHECK (button.getWantsKeyboardFocus() == button.isEnabled());
    });

    CHECK (presetButtonsSeen == 8); // prev/name/next/save/save-as/delete/import/export
    CHECK (disabledPresetButtonsSeen == 2); // Save (not dirty) + Delete (factory preset)
}

TEST_CASE ("Keyboard focus order follows the signal flow", "[gui][a11y]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    // The REAL Tab order, as JUCE's keyboard focus traverser computes it
    // (JUCE 8.0.14 juce::KeyboardFocusTraverser) - not a re-derivation of
    // child order by the test.
    const auto traverser = editor.createKeyboardFocusTraverser();
    REQUIRE (traverser != nullptr);

    const auto ordered = traverser->getAllComponents (&editor);
    REQUIRE (! ordered.empty());

    std::vector<juce::String> visitedPanels;
    int interactiveControls = 0;
    int presetButtonsBeforeFirstControl = 0;
    bool seenFirstControl = false;

    for (auto* component : ordered)
    {
        const auto isKnob = dynamic_cast<juce::Slider*> (component) != nullptr;
        const auto isToggle = dynamic_cast<juce::ToggleButton*> (component) != nullptr;

        if (! (isKnob || isToggle))
        {
            if (! seenFirstControl && dynamic_cast<juce::TextButton*> (component) != nullptr)
                ++presetButtonsBeforeFirstControl;

            continue;
        }

        seenFirstControl = true;
        ++interactiveControls;

        auto* panel = enclosingPanel (*component);
        REQUIRE (panel != nullptr);

        if (visitedPanels.empty() || visitedPanels.back() != panel->getTitle())
            visitedPanels.push_back (panel->getTitle());
    }

    // Every parameter control is reachable by Tab, exactly once.
    CHECK (interactiveControls == 51);

    // The preset bar comes first (its 6 enabled buttons; Save and Delete
    // start disabled and are correctly out of the tab order).
    CHECK (presetButtonsBeforeFirstControl == 6);

    // ...and the sections are then walked in signal-flow order, each
    // entered exactly once (no interleaving, no back-tracking).
    REQUIRE (visitedPanels.size() == std::size (panelTitles));

    for (size_t i = 0; i < visitedPanels.size(); ++i)
    {
        INFO ("focus order position " << i << " = \"" << visitedPanels[i].toStdString() << "\"");
        CHECK (visitedPanels[i] == panelTitles[i]);
    }
}

TEST_CASE ("Arrow keys step knobs by a practical amount, Shift+Arrow steps finer", "[gui][a11y]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    // Input Gain: linear -24..+24 dB, 0.01 dB interval (ParameterLayout.cpp).
    // The stock Slider::Pimpl::keyPressed would step by the raw 0.01 dB
    // interval (4800 presses for a full sweep) and ignore Shift entirely -
    // see src/gui/KeyboardSteps.h.
    auto* knob = findDescendantByTitle<juce::Slider> (editor, "Input Gain");
    REQUIRE (knob != nullptr);

    knob->setValue (-6.0, juce::sendNotificationSync);

    juce::Component& knobAsComponent = *knob;

    // Plain Right = 1% of the 48 dB range = 0.48 dB.
    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
    CHECK (knob->getValue() == Catch::Approx (-5.52).margin (1.0e-3));

    // Shift+Right = 0.1% = 0.048 dB, snapped to the 0.01 dB parameter grid.
    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::rightKey,
                                                         juce::ModifierKeys::shiftModifier, 0)));
    CHECK (knob->getValue() == Catch::Approx (-5.47).margin (1.0e-3));

    // Plain Left steps back down symmetrically.
    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::leftKey)));
    CHECK (knob->getValue() == Catch::Approx (-5.95).margin (1.0e-3));

    // PageDown = 10% = 4.8 dB.
    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::pageDownKey)));
    CHECK (knob->getValue() == Catch::Approx (-10.75).margin (1.0e-3));

    // Home/End jump to the range extremes (WAI-ARIA slider pattern).
    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::homeKey)));
    CHECK (knob->getValue() == Catch::Approx (-24.0).margin (1.0e-3));
    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::endKey)));
    CHECK (knob->getValue() == Catch::Approx (24.0).margin (1.0e-3));

    // The parameter really moved with it - the keyboard path is not a
    // display-only illusion.
    auto* raw = processor.apvts.getRawParameterValue ("inputGain");
    REQUIRE (raw != nullptr);
    CHECK (raw->load() == Catch::Approx (24.0f).margin (1.0e-3));
}

TEST_CASE ("Choice knobs step exactly one detent per arrow press and announce the new choice", "[gui][a11y]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    auto* highPanel = findDescendantByTitle<basilica::gui::BusPanel> (editor, "High Band");
    REQUIRE (highPanel != nullptr);

    // Voicing: 3 choices (Gnaw/Wool/Razor). A 1%-of-range coarse step would
    // collapse to zero after interval snapping - KeyboardSteps.h's
    // one-interval fallback must turn every arrow press into exactly one
    // detent instead of silently swallowing it.
    auto* knob = findDescendantByTitle<juce::Slider> (*highPanel, "Voicing");
    REQUIRE (knob != nullptr);

    juce::Component& knobAsComponent = *knob;

    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::homeKey)));
    CHECK (knob->getValue() == Catch::Approx (0.0).margin (1.0e-6));

    const auto handler = createHandlerForTest (*knob);
    REQUIRE (handler != nullptr);
    auto* valueInterface = handler->getValueInterface();
    REQUIRE (valueInterface != nullptr);
    CHECK (valueInterface->getCurrentValueAsString() == "Gnaw");

    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
    CHECK (knob->getValue() == Catch::Approx (1.0).margin (1.0e-6));
    CHECK (valueInterface->getCurrentValueAsString() == "Wool");

    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
    CHECK (knob->getValue() == Catch::Approx (2.0).margin (1.0e-6));
    CHECK (valueInterface->getCurrentValueAsString() == "Razor");

    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::leftKey)));
    CHECK (knob->getValue() == Catch::Approx (1.0).margin (1.0e-6));

    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::endKey)));
    CHECK (knob->getValue() == Catch::Approx (2.0).margin (1.0e-6));
    CHECK (valueInterface->getCurrentValueAsString() == "Razor");

    // ...and the choice parameter itself followed along.
    auto* raw = processor.apvts.getRawParameterValue ("highVoicing");
    REQUIRE (raw != nullptr);
    CHECK (raw->load() == Catch::Approx (2.0f).margin (1.0e-6));
}

TEST_CASE ("Ctrl/Cmd-modified arrow presses are left to the host", "[gui][a11y]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    auto* knob = findDescendantByTitle<juce::Slider> (editor, "Input Gain");
    REQUIRE (knob != nullptr);

    knob->setValue (-6.0, juce::sendNotificationSync);
    juce::Component& knobAsComponent = *knob;

    CHECK_FALSE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::rightKey,
                                                             juce::ModifierKeys::ctrlModifier, 0)));
    CHECK_FALSE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::rightKey,
                                                             juce::ModifierKeys::commandModifier, 0)));
    CHECK (knob->getValue() == Catch::Approx (-6.0));
}

TEST_CASE ("Toggles expose title, checkable state, and real APVTS wiring", "[gui][a11y]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    struct Expectation
    {
        const char* panel;
        const char* title;
        const char* parameterId;
        float defaultRaw;
    };

    // All seven bool parameters.
    const Expectation expectations[] = {
        { "Input", "Bypass", "bypass", 0.0f },
        { "Noise Gate", "Gate", "gateEnabled", 0.0f },
        { "Low Band", "Auto Rel", "lowCompAutoRelease", 1.0f },
        { "Low Band", "Auto Mkup", "lowCompAutoMakeup", 0.0f },
        { "Cabinet", "Cab", "irEnabled", 0.0f },
        { "EQ", "EQ", "eqEnabled", 0.0f },
        { "Output", "Clip", "outputClip", 0.0f },
    };

    for (const auto& expectation : expectations)
    {
        auto* panel = findDescendantByTitle<basilica::gui::BusPanel> (editor, expectation.panel);
        REQUIRE (panel != nullptr);

        auto* toggle = findDescendantByTitle<juce::ToggleButton> (*panel, expectation.title);
        REQUIRE (toggle != nullptr);
        INFO ("toggle \"" << expectation.panel << " / " << expectation.title << "\"");

        CHECK (toggle->getTitle() == expectation.title);

        const auto handler = createHandlerForTest (*toggle);
        REQUIRE (handler != nullptr);

        // juce::ToggleButton's constructor calls setClickingTogglesState(true)
        // (JUCE 8.0.14 juce_ToggleButton.cpp), so the base Button handler
        // exposes checkable/checked state; its createAccessibilityHandler
        // reports AccessibilityRole::toggleButton (juce_ToggleButton.cpp:71),
        // so VoiceOver's rotor lists these as toggles, not plain buttons.
        CHECK (handler->getCurrentState().isCheckable());

        // Real APVTS wiring, never a decorative stub: flipping the toggle
        // through the state API (the same entry point mouse, Space/Return
        // and the attachment all funnel through) must move the parameter.
        auto* raw = processor.apvts.getRawParameterValue (expectation.parameterId);
        REQUIRE (raw != nullptr);
        CHECK (raw->load() == Catch::Approx (expectation.defaultRaw));

        toggle->setToggleState (expectation.defaultRaw < 0.5f, juce::sendNotificationSync);
        CHECK (raw->load() == Catch::Approx (expectation.defaultRaw < 0.5f ? 1.0f : 0.0f));

        toggle->setToggleState (expectation.defaultRaw >= 0.5f, juce::sendNotificationSync);
        CHECK (raw->load() == Catch::Approx (expectation.defaultRaw));
    }
}

TEST_CASE ("Each section is an accessibility focus container that does not trap Tab", "[gui][a11y]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    for (const auto* title : panelTitles)
    {
        auto* panel = findDescendantByTitle<basilica::gui::BusPanel> (editor, title);
        REQUIRE (panel != nullptr);
        INFO ("section panel \"" << title << "\"");

        // focusContainer (accessibility grouping: AT reads "Low Band,
        // Ratio")...
        CHECK (panel->isFocusContainer());

        // ...but NOT a keyboard focus container: JUCE 8.0.14's Tab
        // traversal looks up the nearest isKeyboardFocusContainer()
        // ancestor (juce_Component.cpp:2918), so setting that flag here
        // would trap Tab inside one section. setFocusContainerType
        // (focusContainer) sets only the accessibility-side flag
        // (juce_Component.cpp:2879).
        CHECK_FALSE (panel->isKeyboardFocusContainer());

        // Reported to AT as a named group.
        const auto handler = createHandlerForTest (*panel);
        REQUIRE (handler != nullptr);
        CHECK (handler->getRole() == juce::AccessibilityRole::group);
        CHECK (handler->getTitle() == juce::String (title));
    }

    // Every interactive control lives inside exactly one of the ten section
    // panels (non-vacuous: the walk must find all 51).
    int controlsChecked = 0;

    const auto isInsideExactlyOnePanel = [&] (juce::Component& control)
    {
        int containingPanels = 0;

        for (auto* ancestor = control.getParentComponent(); ancestor != nullptr; ancestor = ancestor->getParentComponent())
            if (dynamic_cast<basilica::gui::BusPanel*> (ancestor) != nullptr)
                ++containingPanels;

        ++controlsChecked;
        CHECK (containingPanels == 1);
    };

    visitDescendants<juce::Slider> (editor, [&] (juce::Slider& s) { isInsideExactlyOnePanel (s); });
    visitDescendants<juce::ToggleButton> (editor, [&] (juce::ToggleButton& t) { isInsideExactlyOnePanel (t); });

    CHECK (controlsChecked == 51);
}

TEST_CASE ("Needle meters expose a read-only, unit-suffixed accessible value per stage", "[gui][a11y]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    struct Expectation
    {
        const char* title;
        basilica::gui::NeedleMeter::Scale scale;
        float previewDb;
        const char* expectedValue;
    };

    const Expectation expectations[] = {
        { "Input peak level meter", basilica::gui::NeedleMeter::Scale::peakLevelDb, -8.2f, "-8.2 dBFS" },
        { "Gate gain reduction meter", basilica::gui::NeedleMeter::Scale::gainReductionDb, 6.4f, "6.4 dB" },
        { "Low band compressor gain reduction meter", basilica::gui::NeedleMeter::Scale::gainReductionDb, 3.1f, "3.1 dB" },
        { "Output peak level meter", basilica::gui::NeedleMeter::Scale::peakLevelDb, -0.4f, "-0.4 dBFS" },
    };

    int metersSeen = 0;

    for (const auto& expectation : expectations)
    {
        auto* meter = findDescendantByTitle<basilica::gui::NeedleMeter> (editor, expectation.title);
        REQUIRE (meter != nullptr);
        ++metersSeen;
        INFO ("meter \"" << expectation.title << "\"");

        CHECK (meter->getScale() == expectation.scale);

        // Display-only: never in the tab order, never eats mouse events
        // aimed at nearby controls.
        CHECK_FALSE (meter->getWantsKeyboardFocus());

        const auto handler = meter->createAccessibilityHandler();
        REQUIRE (handler != nullptr);

        auto* valueInterface = handler->getValueInterface();
        REQUIRE (valueInterface != nullptr);
        CHECK (valueInterface->isReadOnly());

        // On-demand value in dB, following the ballistic-smoothed reading.
        meter->setImmediateDbForPreview (expectation.previewDb);
        CHECK (valueInterface->getCurrentValueAsString() == juce::String (expectation.expectedValue));
    }

    CHECK (metersSeen == 4);

    // ...and there are exactly four (input peak, gate GR, low-band comp GR,
    // output peak - every reading cryp::MeterTaps exposes for display), no
    // strays.
    int totalMeters = 0;
    visitDescendants<basilica::gui::NeedleMeter> (editor, [&] (basilica::gui::NeedleMeter&) { ++totalMeters; });
    CHECK (totalMeters == 4);
}
