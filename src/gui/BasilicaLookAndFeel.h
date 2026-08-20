#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Suite-wide Basilica look & feel - Crypta's M3/M6 VECTOR variant (issues
// #45 / #25), ported from basilica-audio/Miserere's M3 editor (its PR #31).
//
// Unlike the photoreal siblings (silentium/aureate/requiem, whose faceplates,
// knob filmstrips and needle sprites are Blender pre-renders), everything in
// Crypta's editor is drawn at runtime with juce::Graphics/juce::Path: pointer
// knobs with engraved scale rings, lamp toggles, per-stage needle meters, and
// the preset-bar button reskin. No photoreal PNG assets exist in this plugin;
// the suite's black/gold material language is carried entirely by the colour
// constants below.
//
// TYPOGRAPHY: EB Garamond (embedded via BinaryData, resources/fonts/ - OFL
// licensed, see resources/fonts/OFL-EBGaramond.txt; the suite serif chosen in
// silentium's v0.3.1 overhaul for holding stroke weight at 12-14px label
// sizes). All text in this editor is JUCE-drawn, so getSerifFont() is the
// single source of every rendered glyph.
//
// CONTRAST DRIFT-PROOFING (A-03 pattern, WCAG 1.4.3): every colour pair the
// LookAndFeel actually renders text/markings with is exposed as a static
// accessor and asserted >= 4.5:1 in tests/gui/BasilicaLookAndFeelContrastTests.cpp
// against the SAME colours, never a hand-copied second literal.
namespace basilica::gui
{
    class BasilicaLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        BasilicaLookAndFeel();

        juce::Font getLabelFont (juce::Label& label) override;
        void drawLabel (juce::Graphics& g, juce::Label& label) override;

        // Vector pointer knob + engraved scale ring. Stepped parameters
        // (choice knobs - the slider's interval spans >= 1 whole step of a
        // small integer range) get one engraved tick per detent; continuous
        // knobs get the standard 11-tick ring.
        void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                               float sliderPosProportional, float rotaryStartAngle,
                               float rotaryEndAngle, juce::Slider& slider) override;

        // Lamp toggle: engraved legend + gold lamp, plus an explicit focus
        // ring - LookAndFeel_V4 draws NO focus indication for toggles at
        // all (JUCE 8.0.14), so without this override keyboard focus on a
        // toggle would be invisible (WCAG 2.4.7).
        void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

        juce::Font getTextButtonFont (juce::TextButton& button, int buttonHeight) override;
        void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                   const juce::Colour& backgroundColour,
                                   bool shouldDrawButtonAsHighlighted,
                                   bool shouldDrawButtonAsDown) override;
        void drawButtonText (juce::Graphics& g, juce::TextButton& button,
                             bool shouldDrawButtonAsHighlighted,
                             bool shouldDrawButtonAsDown) override;

        // The suite serif, resolved from the embedded EB Garamond faces
        // (never a system font). Shared by every component that draws text
        // directly (BusPanel headers, NeedleMeter legends).
        static juce::Font getSerifFont (float height, bool semiBold = false);

        // ------------------------------------------------------------------
        // Rendered-colour accessors (contrast-test contract - see the
        // header comment). Each getter names the exact draw site it feeds.
        // ------------------------------------------------------------------

        // Editor window fill behind/between the section panels.
        static juce::Colour getEditorBackgroundColour() noexcept;

        // BusPanel::paint()'s opaque panel fill - control labels, toggle
        // legends and knob scale rings all render directly over this.
        static juce::Colour getPanelBackgroundColour() noexcept;

        // drawLabel()/drawToggleButton() text + engraved tick marks, drawn
        // over getPanelBackgroundColour().
        static juce::Colour getLabelTextColour() noexcept;

        // BusPanel header lettering + its thin rule, over the panel fill.
        static juce::Colour getPanelHeaderTextColour() noexcept;

        // Slider value-box text over its opaque recessed chip.
        static juce::Colour getValueTextColour() noexcept;
        static juce::Colour getValueBoxBackgroundColour() noexcept;

        // Preset-bar TextButton lettering over the opaque button face.
        static juce::Colour getButtonTextColour() noexcept;
        static juce::Colour getButtonFaceColour() noexcept;

        // NeedleMeter's engraved arc/ticks/legend + needle over its opaque
        // recessed face.
        static juce::Colour getMeterFaceColour() noexcept;
        static juce::Colour getMeterMarkingColour() noexcept;
        static juce::Colour getMeterNeedleColour() noexcept;

        // The peak meters' over-level zone: engraved above -6 dBFS on the
        // I/O scales, and the needle colour once the reading crosses 0 dBFS.
        static juce::Colour getMeterWarningColour() noexcept;

        // Pointer-knob body/pointer pair: the pointer line is the state
        // indicator, so it must stay readable against the knob face
        // ("readability of control state", CLAUDE.md).
        static juce::Colour getKnobFaceColour() noexcept;
        static juce::Colour getKnobPointerColour() noexcept;

        // Focus-ring pair used by paintFocusRing() below.
        static juce::Colour getFocusRingColour() noexcept;
        static juce::Colour getFocusRingHaloColour() noexcept;

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BasilicaLookAndFeel)
    };

    // A-01 pattern (WCAG 2.4.7 Focus Visible), copied from silentium: shared
    // keyboard-focus indicator for custom-painted controls, drawn once
    // `hasKeyboardFocus (true)` is true so the fix lives in one place.
    // Callers here: PointerKnob::paint() (ellipse), drawToggleButton() and
    // drawButtonBackground() (rounded rectangle).
    enum class FocusRingShape
    {
        ellipse,
        roundedRectangle
    };

    void paintFocusRing (juce::Graphics& g, juce::Rectangle<float> bounds, FocusRingShape shape);
}
