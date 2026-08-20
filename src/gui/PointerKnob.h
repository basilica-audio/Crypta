#pragma once

#include "BasilicaLookAndFeel.h"
#include "KeyboardSteps.h"

#include <juce_gui_basics/juce_gui_basics.h>

// The vector editor's knob control: a juce::Slider whose rotary art is the
// pointer knob drawn by BasilicaLookAndFeel::drawRotarySlider() (issue #25 -
// no filmstrip/photoreal mode exists in this plugin), upgraded for keyboard
// operability exactly like silentium's KnobSlider and Miserere's PointerKnob
// (their M3 a11y follow-ups A-09/A-10, WCAG 2.1.1 + 2.4.7):
//
//   - juce::Slider::init() ships with setWantsKeyboardFocus(false) in JUCE
//     8.0.14 (juce_Slider.cpp:1461), so a plain Slider is unreachable by
//     Tab and its keyPressed() never fires. The constructor opts back in,
//     putting every knob into the editor's focus traversal (which follows
//     child creation order - see the PluginEditor constructor).
//
//   - keyPressed() adds WAI-ARIA-style stepping (Arrow 1%, Shift+Arrow
//     fine, PageUp/Down 10%, Home/End extremes) via handleSliderKeyPress()
//     - see KeyboardSteps.h for why the base-class behaviour is unusable
//     with this suite's finely-quantised parameter ranges (Crypta declares
//     0.01 dB / 0.01 ms intervals throughout, so one stock arrow press moves
//     a 80 dB gate threshold by 1/8000 of its range). For CHOICE parameters
//     (integer detents), the helper's one-interval fallback makes every
//     arrow press move exactly one detent.
//
//   - Shift-drag = fine mouse adjustment (the mouse analog of Shift+Arrow):
//     mouseDown/mouseDrag retune setMouseDragSensitivity() per the current
//     modifier state before forwarding to the base Slider implementation,
//     which reads that sensitivity live on every drag event (JUCE 8.0.14,
//     juce::Slider::Pimpl::pixelsForFullDragExtent).
//
//   - paint() draws the suite's shared focus ring (A-01 pattern,
//     basilica::gui::paintFocusRing) when the knob holds keyboard focus
//     (WCAG 2.4.7) - the ring wraps the rotary area only, not the value
//     box below it.
namespace basilica::gui
{
    class PointerKnob : public juce::Slider
    {
    public:
        PointerKnob()
            : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow),
              baseDragSensitivity (getMouseDragSensitivity())
        {
            setWantsKeyboardFocus (true);
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            return handleSliderKeyPress (*this, key) || juce::Slider::keyPressed (key);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            applyDragSensitivityFor (e.mods);
            juce::Slider::mouseDown (e);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            applyDragSensitivityFor (e.mods);
            juce::Slider::mouseDrag (e);
        }

        void paint (juce::Graphics& g) override
        {
            juce::Slider::paint (g);

            if (hasKeyboardFocus (true))
            {
                // Ring the rotary area (local bounds minus the value box).
                auto ringArea = getLocalBounds().toFloat();

                if (getTextBoxPosition() == juce::Slider::TextBoxBelow)
                    ringArea.removeFromBottom ((float) getTextBoxHeight());

                const auto side = juce::jmin (ringArea.getWidth(), ringArea.getHeight());
                paintFocusRing (g, juce::Rectangle<float> (side, side).withCentre (ringArea.getCentre()),
                                FocusRingShape::ellipse);
            }
        }

    private:
        void applyDragSensitivityFor (const juce::ModifierKeys& mods)
        {
            setMouseDragSensitivity (mods.isShiftDown() ? baseDragSensitivity * fineDragFactor
                                                        : baseDragSensitivity);
        }

        // Shift-fine drag needs 8x the pixels for a full-range sweep - the
        // suite's shared "hold Shift for finer control" factor.
        const int baseDragSensitivity;
        static constexpr int fineDragFactor = 8;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PointerKnob)
    };
}
