#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Suite-reusable toggle switch for the wave-3 "composited plate" GUI
// generation: draws the family brass toggle sprite (lever UP), and for the
// opposite state the SAME sprite mirrored vertically with a slight darken.
//
// The mirror is a documented nearest-sprite workaround, not a design
// choice: the sprite library's provenance notes record "toggle down (OFF)
// state" as a known gap (no approved down-state master variant exists yet
// - blueprint section 2c). Mirroring the existing approved sprite stays
// within the campaign rule of never repainting/relighting/inventing new
// visual material; when the down-state variant render lands, this class
// takes it as a second constructor image with no other change
// (downSprite.isValid() switches the code path).
//
// leverUpWhenOn: the family convention is lever up = ON. A two-state
// AudioParameterChoice rendered as a toggle can invert that (Lancet's
// Bell/Shelf "Type": up = Bell = index 0 = button OFF), which is a VISUAL
// mapping only - accessibility state remains the plain button toggle
// state.
namespace basilica::gui
{
    class SpriteToggle : public juce::ToggleButton
    {
    public:
        explicit SpriteToggle (const juce::Image& upSpriteIn, bool leverUpWhenOnIn = true,
                               const juce::Image& downSpriteIn = {});
        ~SpriteToggle() override;

        void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override;

        // Whether the lever currently draws in the UP pose - the visual
        // mapping under test in tests/gui/SpriteToggleTests.cpp.
        static bool leverDrawsUp (bool toggleState, bool leverUpWhenOn) noexcept
        {
            return toggleState == leverUpWhenOn;
        }

    private:
        juce::Image upSprite;
        juce::Image downSprite;
        const bool leverUpWhenOn;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpriteToggle)
    };
}
