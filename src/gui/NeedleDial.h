#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <vector>

// Suite-reusable analog dial for the wave-3 "composited plate" GUI
// generation: a NEEDLE-FREE dial-face sprite (the family VU face or the D2
// mini GR face, both extracted needle-free per the suite's NADEL-REGEL -
// needles are never baked) plus a live VECTOR needle drawn on top,
// rotated about the face's own measured hub pivot.
//
// Threading/ballistics contract is copied from the tenebrae/aureate
// HubNeedle family component: setTargetDb() is a relaxed atomic store,
// safe from any thread; tick(dt) advances one-pole ballistics on the GUI
// thread and is driven by the EDITOR's timer (not an internal juce::Timer),
// so headless tests can pump it deterministically;
// setImmediateDbForPreview() seeds both target and smoothed reading for
// snapshot tests, which have no running message loop.
//
// The dB -> angle mapping is a per-instance measured tick table
// (piecewise-linear, clamped at the table ends), constructor-injected
// exactly like tenebrae's generalised HubNeedle: the VU face and the mini
// GR face measure to different arcs, and hardcoding either table in the
// class would make the other instance easy to wire wrong.
namespace basilica::gui
{
    class NeedleDial : public juce::Component
    {
    public:
        struct Tick
        {
            float db;
            float deg; // clockwise from straight up (12 o'clock)
        };

        // pivotXFraction/pivotYFraction: the needle hub pivot within the
        // face sprite, as fractions of the sprite's own canvas -
        // measured once per face sprite, carried by the layout manifest.
        // needleLengthFraction: needle length as a fraction of the drawn
        // face diameter (pivot to tip).
        NeedleDial (juce::Image faceSpriteIn, juce::String accessibleTitle,
                    float pivotXFraction, float pivotYFraction,
                    float needleLengthFraction, std::vector<Tick> tickTableIn);
        ~NeedleDial() override;

        void setTargetDb (float newTargetDb) noexcept { targetDb.store (newTargetDb, std::memory_order_relaxed); }

        // Advances ballistics by dtSeconds and repaints if the smoothed
        // value moved meaningfully - called from the editor's timer.
        void tick (float dtSeconds) noexcept;

        // Test/preview-only: seeds target AND smoothed reading, bypassing
        // the ramp (headless binaries pump no real timer ticks).
        void setImmediateDbForPreview (float db) noexcept;

        float getSmoothedDbForTest() const noexcept { return smoothedDb; }

        void paint (juce::Graphics& g) override;
        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

        // One-pole ballistic integration step - pure/static so it is
        // directly unit-testable without a running timer.
        static float stepBallistics (float currentSmoothed, float target, float dtSeconds, float tauSeconds) noexcept;

        // dB -> rotation angle in degrees (clockwise from straight up),
        // piecewise-linearly interpolated across the tick table, clamped
        // beyond its ends. Static + table-as-parameter for per-instance
        // unit testing.
        static float tickAngleDegreesForDb (float db, const std::vector<Tick>& tickTable) noexcept;

        static constexpr float ballisticsTauSeconds = 0.25f;

    private:
        class ValueInterface;

        juce::Image faceSprite;
        juce::String title;
        std::vector<Tick> tickTable;

        std::atomic<float> targetDb { -60.0f };
        float smoothedDb = -60.0f;

        const float pivotXFraction;
        const float pivotYFraction;
        const float needleLengthFraction;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeedleDial)
    };
}
