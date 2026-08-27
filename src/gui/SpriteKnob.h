#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Suite-reusable rotary knob for the wave-3 "composited plate" GUI
// generation: the photoreal control sprite (a feathered-alpha crop from an
// approved master render, see resources/gui/ and the sprite-library
// provenance in the suite scaffold) is drawn STATIC at its baked rest
// lighting, and only a feathered inner disc of the knob's own face rotates
// with the parameter value.
//
// This is basilica-audio/tenebrae's MasterCropKnob INNER-DISC technique
// (itself generalised from silentium v0.3.9), re-hosted for standalone
// sprites instead of full-master crops: the sprite's baked outer rim and
// specular highlight never rotate, which is what structurally rules out a
// co-rotating highlight or a visible lighting seam. The rotating disc is
// cropped out of the sprite itself once at construction (message thread),
// so at the pointer's baked rest pose (12 o'clock, normalised proportion
// 0.5 for a symmetric sweep) the live layer is pixel-identical to the
// static sprite underneath.
//
// The knob's centre does NOT have to be the sprite's canvas centre: the
// stepped-selector sprite carries an engraved tick crown above the knob,
// so the knob circle sits below centre. centreInSprite/radiusInSprite are
// therefore explicit constructor geometry, measured per sprite and carried
// by the plugin's layout manifest (resources/gui/layout-manifest.json).
namespace basilica::gui
{
    class SpriteKnob : public juce::Slider
    {
    public:
        // sprite: the full control sprite (ARGB, feathered alpha).
        // centreInSpritePx / radiusInSpritePx: the knob face's own measured
        // circle within the sprite's pixel space.
        // minAngleDegIn/maxAngleDegIn: rotation sweep, clockwise degrees
        // from straight up; proportion 0.5 maps to the mid-sweep angle,
        // which for a symmetric sweep is 0 deg = the sprite's baked pose.
        SpriteKnob (const juce::Image& sprite, juce::Point<float> centreInSpritePx,
                    float radiusInSpritePx, float minAngleDegIn = -135.0f,
                    float maxAngleDegIn = 135.0f);
        ~SpriteKnob() override;

        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;

        // WCAG 2.1.1 Keyboard: WAI-ARIA-style stepping (Arrow 1%,
        // Shift+Arrow fine, PageUp/Down 10%, Home/End extremes) via
        // KeyboardSteps.h - juce::Slider's own keyPressed (JUCE 8.0.14,
        // juce_Slider.cpp:1029) steps by the raw parameter interval and
        // swallows Shift entirely.
        bool keyPressed (const juce::KeyPress& key) override;

        // Normalised slider proportion [0,1] -> absolute rotation in
        // degrees, clockwise from straight up. Exposed for unit testing.
        static float angleForProportionDegrees (double normalisedValue, float minAngleDeg,
                                                float maxAngleDeg) noexcept;

        // Where the knob's centre lands within this component's local
        // bounds, as fractions of width/height - resized()/the editor
        // layout use this to place the component so the knob centre hits
        // its manifest position exactly even though the sprite frame is
        // asymmetric (selector crown). Exposed for layout tests.
        juce::Point<float> knobCentreFraction() const noexcept { return centreFraction; }

        // Builds the feathered circular rotating disc from the sprite:
        // fully opaque inside radius*contentFraction*(1-feather), fading
        // linearly to fully transparent at radius*contentFraction.
        // Message-thread only (per-pixel work at construction), exposed as
        // an independently testable static (tests/gui/SpriteKnobTests.cpp).
        static juce::Image buildFeatheredCrop (const juce::Image& sprite, juce::Point<float> centreInSpritePx,
                                               float radiusPx, float contentFraction, float featherFraction = 0.12f);

    private:
        juce::Image spriteImage;
        juce::Image rotatingDisc;
        juce::Point<float> centreInSprite;
        juce::Point<float> centreFraction;
        const float minAngleDeg;
        const float maxAngleDeg;

        static constexpr float contentFraction = 0.94f;
        static constexpr int normalDragSensitivity = 200;
        static constexpr int fineDragSensitivity = normalDragSensitivity * 8;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpriteKnob)
    };
}
