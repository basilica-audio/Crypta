#include "NeedleDial.h"

#include <cmath>

namespace basilica::gui
{
    NeedleDial::NeedleDial (juce::Image faceSpriteIn, juce::String accessibleTitle,
                            float pivotXFractionIn, float pivotYFractionIn,
                            float needleLengthFractionIn, std::vector<Tick> tickTableIn)
        : faceSprite (std::move (faceSpriteIn)), title (std::move (accessibleTitle)),
          tickTable (std::move (tickTableIn)),
          pivotXFraction (pivotXFractionIn), pivotYFraction (pivotYFractionIn),
          needleLengthFraction (needleLengthFractionIn)
    {
        setTitle (title);
        setInterceptsMouseClicks (false, false); // display-only instrument
    }

    NeedleDial::~NeedleDial() = default;

    float NeedleDial::tickAngleDegreesForDb (float db, const std::vector<Tick>& tickTable) noexcept
    {
        if (tickTable.empty())
            return 0.0f;

        if (db <= tickTable.front().db)
            return tickTable.front().deg;

        if (db >= tickTable.back().db)
            return tickTable.back().deg;

        for (size_t i = 1; i < tickTable.size(); ++i)
        {
            if (db <= tickTable[i].db)
            {
                const auto& lo = tickTable[i - 1];
                const auto& hi = tickTable[i];
                const auto span = hi.db - lo.db;
                const auto t = span > 0.0f ? (db - lo.db) / span : 0.0f;
                return lo.deg + t * (hi.deg - lo.deg);
            }
        }

        return tickTable.back().deg;
    }

    float NeedleDial::stepBallistics (float currentSmoothed, float target, float dtSeconds, float tauSeconds) noexcept
    {
        if (tauSeconds <= 0.0f || dtSeconds <= 0.0f)
            return target;

        const auto alpha = 1.0f - std::exp (-dtSeconds / tauSeconds);
        return currentSmoothed + (target - currentSmoothed) * alpha;
    }

    void NeedleDial::tick (float dtSeconds) noexcept
    {
        const auto target = targetDb.load (std::memory_order_relaxed);
        const auto next = stepBallistics (smoothedDb, target, dtSeconds, ballisticsTauSeconds);

        if (! juce::approximatelyEqual (next, smoothedDb))
        {
            smoothedDb = next;
            repaint();
        }
    }

    void NeedleDial::setImmediateDbForPreview (float db) noexcept
    {
        targetDb.store (db, std::memory_order_relaxed);
        smoothedDb = db;
        repaint();
    }

    void NeedleDial::paint (juce::Graphics& g)
    {
        if (! faceSprite.isValid())
            return;

        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

        const auto bounds = getLocalBounds().toFloat();
        const auto scale = bounds.getWidth() / (float) faceSprite.getWidth();

        // 1. The needle-free face sprite.
        g.drawImageTransformed (faceSprite, juce::AffineTransform::scale (scale));

        // 2. The live vector needle, clipped to the face so it can never
        // poke past the baked bezel.
        const auto pivot = juce::Point<float> (bounds.getWidth() * pivotXFraction,
                                               bounds.getHeight() * pivotYFraction);
        const auto needleLength = needleLengthFraction * bounds.getWidth();
        const auto angleRad = juce::degreesToRadians (tickAngleDegreesForDb (smoothedDb, tickTable));

        // Tapered needle polygon in an unrotated straight-up pose, rotated
        // about the pivot. A short tail below the pivot suggests the
        // counterweight the baked hub anchor already implies.
        const auto baseHalfWidth = juce::jmax (1.0f, bounds.getWidth() * 0.010f);
        const auto tipHalfWidth = juce::jmax (0.5f, bounds.getWidth() * 0.004f);
        const auto tailLength = needleLength * 0.12f;

        juce::Path needle;
        needle.startNewSubPath (-baseHalfWidth, tailLength);
        needle.lineTo (baseHalfWidth, tailLength);
        needle.lineTo (tipHalfWidth, -needleLength);
        needle.lineTo (-tipHalfWidth, -needleLength);
        needle.closeSubPath();

        const auto rotation = juce::AffineTransform::rotation (angleRad).translated (pivot.x, pivot.y);

        g.saveState();
        g.reduceClipRegion (bounds.reduced (bounds.getWidth() * 0.055f).getSmallestIntegerContainer());

        // Soft contact shadow first (key light comes from the upper left,
        // so the shadow falls down-right onto the face).
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillPath (needle, rotation.translated (bounds.getWidth() * 0.006f, bounds.getWidth() * 0.010f));

        // The needle body: near-black lacquered steel.
        g.setColour (juce::Colour (0xff231a12));
        g.fillPath (needle, rotation);

        g.restoreState();
    }

    // Read-only text value interface exposing the current ballistic-smoothed
    // reading (the tenebrae/aureate HubNeedle convention, JUCE 8.0.14
    // juce::AccessibilityTextValueInterface shape).
    class NeedleDial::ValueInterface final : public juce::AccessibilityTextValueInterface
    {
    public:
        explicit ValueInterface (const NeedleDial& ownerIn) noexcept : owner (ownerIn) {}

        bool isReadOnly() const override { return true; }

        juce::String getCurrentValueAsString() const override
        {
            return juce::String (owner.smoothedDb, 1) + " dB";
        }

        void setValueAsString (const juce::String&) override {}

    private:
        const NeedleDial& owner;
    };

    std::unique_ptr<juce::AccessibilityHandler> NeedleDial::createAccessibilityHandler()
    {
        return std::make_unique<juce::AccessibilityHandler> (
            *this,
            juce::AccessibilityRole::label,
            juce::AccessibilityActions {},
            juce::AccessibilityHandler::Interfaces { std::make_unique<ValueInterface> (*this) });
    }
}
