#pragma once

#include "BasilicaLookAndFeel.h"

#include <juce_gui_basics/juce_gui_basics.h>

// One faceplate panel per stage/section of the vector editor (issues #25 /
// #26): paints its own opaque near-black panel with an engraved serif header
// and a thin gold rule, and - the a11y half (issue #46, WCAG 4.1.2 grouping)
// - acts as an ACCESSIBILITY focus container with its own title, so a screen
// reader announces "Low Band, Ratio" instead of one flat 51-control list.
//
// FocusContainerType::focusContainer deliberately, NOT
// keyboardFocusContainer: in JUCE 8.0.14 (juce_Component.cpp:2879,
// setFocusContainerType) `focusContainer` sets only the isFocusContainer
// flag consumed by the accessibility hierarchy, while Tab traversal looks
// for isKeyboardFocusContainer (juce_Component.cpp:2918) - so grouping the
// sections for AT does NOT trap Tab inside one panel; keyboard focus still
// walks the entire editor in child creation order (= signal-flow order).
namespace basilica::gui
{
    class BusPanel : public juce::Component
    {
    public:
        explicit BusPanel (const juce::String& sectionTitle)
        {
            setTitle (sectionTitle);
            setDescription (sectionTitle);
            setFocusContainerType (juce::Component::FocusContainerType::focusContainer);
        }

        // The vertical space paint() reserves at the top for the engraved
        // header - PluginEditor.cpp lays control rows out below this.
        static constexpr int headerHeight = 26;

        void paint (juce::Graphics& g) override
        {
            const auto bounds = getLocalBounds().toFloat();

            g.setColour (BasilicaLookAndFeel::getPanelBackgroundColour());
            g.fillRoundedRectangle (bounds, 6.0f);

            g.setColour (BasilicaLookAndFeel::getPanelHeaderTextColour().withAlpha (0.35f));
            g.drawRoundedRectangle (bounds.reduced (0.75f), 6.0f, 1.5f);

            auto headerArea = getLocalBounds().removeFromTop (headerHeight).reduced (12, 0);

            g.setColour (BasilicaLookAndFeel::getPanelHeaderTextColour());
            g.setFont (BasilicaLookAndFeel::getSerifFont (17.0f, true));
            g.drawText (getTitle(), headerArea, juce::Justification::centredLeft);

            // Thin gold rule under the header, engraved into the plate.
            g.setColour (BasilicaLookAndFeel::getPanelHeaderTextColour().withAlpha (0.5f));
            g.fillRect ((float) headerArea.getX(), (float) headerHeight - 2.0f,
                        (float) headerArea.getWidth(), 1.0f);
        }

        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
        {
            // Explicit group role (a plain Component reports "unspecified",
            // JUCE 8.0.14 juce_Component.cpp:3307) so AT clients present
            // the section as a named group.
            return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::group);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BusPanel)
    };
}
