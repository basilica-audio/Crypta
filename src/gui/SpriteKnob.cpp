#include "SpriteKnob.h"

#include "KeyboardSteps.h"

#include <cmath>

namespace basilica::gui
{
    SpriteKnob::SpriteKnob (const juce::Image& sprite, juce::Point<float> centreInSpritePx,
                            float radiusInSpritePx, float minAngleDegIn, float maxAngleDegIn)
        : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox),
          spriteImage (sprite),
          rotatingDisc (buildFeatheredCrop (sprite, centreInSpritePx, radiusInSpritePx, contentFraction)),
          centreInSprite (centreInSpritePx),
          centreFraction (sprite.isValid()
                              ? juce::Point<float> (centreInSpritePx.x / (float) sprite.getWidth(),
                                                    centreInSpritePx.y / (float) sprite.getHeight())
                              : juce::Point<float> (0.5f, 0.5f)),
          minAngleDeg (minAngleDegIn), maxAngleDeg (maxAngleDegIn)
    {
        setMouseDragSensitivity (normalDragSensitivity);
        setScrollWheelEnabled (true);
        setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);

        // Keyboard navigation (WCAG 2.1.1): juce::Slider::init() ships with
        // setWantsKeyboardFocus(false) in JUCE 8.0.14 (juce_Slider.cpp:1461)
        // - without opting back in, Tab never reaches the knob and
        // keyPressed() below never fires.
        setWantsKeyboardFocus (true);
    }

    SpriteKnob::~SpriteKnob() = default;

    float SpriteKnob::angleForProportionDegrees (double normalisedValue, float minAngleDeg, float maxAngleDeg) noexcept
    {
        const auto clamped = juce::jlimit (0.0, 1.0, normalisedValue);
        return minAngleDeg + (float) clamped * (maxAngleDeg - minAngleDeg);
    }

    juce::Image SpriteKnob::buildFeatheredCrop (const juce::Image& sprite, juce::Point<float> centreInSpritePx,
                                                float radiusPx, float contentFractionIn, float featherFraction)
    {
        const auto outerRadius = juce::jmax (1.0f, radiusPx * contentFractionIn);
        const auto innerRadius = outerRadius * (1.0f - juce::jlimit (0.0f, 1.0f, featherFraction));

        // +2px margin beyond 2*outerRadius: without it, the canvas's own
        // outermost pixel row sits at radius (outerRadius - 0.5) from the
        // canvas centre (pixel centres, not edges), measurably INSIDE
        // outerRadius, so a faint sliver would survive right at the crop's
        // edge (see tenebrae's MasterCropKnob, where this margin was
        // established).
        const auto canvasSize = juce::jmax (2, (int) std::ceil (outerRadius * 2.0f) + 2);

        juce::Image crop (juce::Image::ARGB, canvasSize, canvasSize, true);

        if (! sprite.isValid())
            return crop;

        const auto srcLeft = centreInSpritePx.x - (float) canvasSize * 0.5f;
        const auto srcTop = centreInSpritePx.y - (float) canvasSize * 0.5f;
        const auto centrePx = (float) canvasSize * 0.5f;

        juce::Image::BitmapData dst (crop, juce::Image::BitmapData::writeOnly);

        for (int y = 0; y < canvasSize; ++y)
        {
            for (int x = 0; x < canvasSize; ++x)
            {
                const auto dx = (float) x + 0.5f - centrePx;
                const auto dy = (float) y + 0.5f - centrePx;
                const auto r = std::sqrt (dx * dx + dy * dy);

                float alpha;
                if (r <= innerRadius)
                    alpha = 1.0f;
                else if (r >= outerRadius)
                    alpha = 0.0f;
                else
                    alpha = 1.0f - (r - innerRadius) / (outerRadius - innerRadius);

                if (alpha <= 0.0f)
                {
                    dst.setPixelColour (x, y, juce::Colours::transparentBlack);
                    continue;
                }

                const auto srcX = juce::jlimit (0, sprite.getWidth() - 1, (int) std::floor (srcLeft + (float) x));
                const auto srcY = juce::jlimit (0, sprite.getHeight() - 1, (int) std::floor (srcTop + (float) y));

                const auto colour = sprite.getPixelAt (srcX, srcY);
                dst.setPixelColour (x, y, colour.withMultipliedAlpha (alpha));
            }
        }

        return crop;
    }

    void SpriteKnob::paint (juce::Graphics& g)
    {
        if (! spriteImage.isValid())
            return;

        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

        const auto bounds = getLocalBounds().toFloat();
        const auto scale = bounds.getWidth() / (float) spriteImage.getWidth();

        // 1. The static pass: the whole sprite at its baked lighting.
        g.drawImageTransformed (spriteImage, juce::AffineTransform::scale (scale));

        // 2. The live pass: the feathered inner disc, rotated about the
        // knob's own centre.
        const auto angleDeg = angleForProportionDegrees (valueToProportionOfLength (getValue()), minAngleDeg, maxAngleDeg);
        const auto radians = juce::degreesToRadians (angleDeg);

        const auto discHalfW = (float) rotatingDisc.getWidth() * 0.5f;
        const auto discHalfH = (float) rotatingDisc.getHeight() * 0.5f;

        const auto transform = juce::AffineTransform::translation (-discHalfW, -discHalfH)
                                   .rotated (radians)
                                   .scaled (scale)
                                   .translated (centreInSprite.x * scale, centreInSprite.y * scale);

        g.drawImageTransformed (rotatingDisc, transform);

        // WCAG 2.4.7 Focus Visible: this paint() fully replaces
        // juce::Slider::paint(), so nothing else draws a keyboard-focus
        // indicator - a minimal, self-contained ring around the knob face.
        if (hasKeyboardFocus (true))
        {
            const auto focusRadius = juce::jmax (discHalfW, discHalfH) * scale + 3.0f;
            const auto centre = juce::Point<float> (centreInSprite.x * scale, centreInSprite.y * scale);

            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.drawEllipse (juce::Rectangle<float> (focusRadius * 2.0f, focusRadius * 2.0f).withCentre (centre), 1.5f);
        }
    }

    bool SpriteKnob::keyPressed (const juce::KeyPress& key)
    {
        return handleSliderKeyPress (*this, key) || juce::Slider::keyPressed (key);
    }

    void SpriteKnob::mouseDown (const juce::MouseEvent& e)
    {
        setMouseDragSensitivity (e.mods.isShiftDown() ? fineDragSensitivity : normalDragSensitivity);
        Slider::mouseDown (e);
    }

    void SpriteKnob::mouseDrag (const juce::MouseEvent& e)
    {
        setMouseDragSensitivity (e.mods.isShiftDown() ? fineDragSensitivity : normalDragSensitivity);
        Slider::mouseDrag (e);
    }
}
