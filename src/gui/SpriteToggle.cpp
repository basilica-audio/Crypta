#include "SpriteToggle.h"

namespace basilica::gui
{
    SpriteToggle::SpriteToggle (const juce::Image& upSpriteIn, bool leverUpWhenOnIn,
                                const juce::Image& downSpriteIn)
        : upSprite (upSpriteIn), downSprite (downSpriteIn), leverUpWhenOn (leverUpWhenOnIn)
    {
        // juce::ToggleButton is keyboard-operable (Space/Enter) and
        // reports a toggle-button accessibility role out of the box; only
        // the drawing is replaced here.
    }

    SpriteToggle::~SpriteToggle() = default;

    void SpriteToggle::paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                                    bool shouldDrawButtonAsDown)
    {
        if (! upSprite.isValid())
            return;

        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

        const auto bounds = getLocalBounds().toFloat();
        const auto drawUp = leverDrawsUp (getToggleState(), leverUpWhenOn);

        const auto scaleX = bounds.getWidth() / (float) upSprite.getWidth();
        const auto scaleY = bounds.getHeight() / (float) upSprite.getHeight();

        if (drawUp || downSprite.isValid())
        {
            const auto& sprite = drawUp ? upSprite : downSprite;
            g.drawImageTransformed (sprite, juce::AffineTransform::scale (scaleX, scaleY));
        }
        else
        {
            // Nearest-sprite workaround for the missing down-state master
            // variant (see class docs): the approved up sprite, mirrored
            // vertically, slightly darkened so the two states read apart
            // at a glance even in peripheral vision.
            const auto transform = juce::AffineTransform::verticalFlip ((float) upSprite.getHeight())
                                       .scaled (scaleX, scaleY);

            g.drawImageTransformed (upSprite, transform);

            g.saveState();
            g.setColour (juce::Colours::black.withAlpha (0.18f));
            g.reduceClipRegion (upSprite, transform);
            g.fillRect (bounds);
            g.restoreState();
        }

        if (shouldDrawButtonAsDown)
        {
            g.setColour (juce::Colours::black.withAlpha (0.12f));
            g.fillRect (bounds);
        }
        else if (shouldDrawButtonAsHighlighted)
        {
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.fillRect (bounds);
        }

        // WCAG 2.4.7 Focus Visible: paintButton() fully replaces the
        // LookAndFeel path, so the focus ring is drawn here.
        if (hasKeyboardFocus (true))
        {
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.drawRoundedRectangle (bounds.reduced (1.0f), 4.0f, 1.5f);
        }
    }
}
