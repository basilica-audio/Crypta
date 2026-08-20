#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>

// Vector needle meter for the M3/M6 editor (issues #27 / #25), ported from
// basilica-audio/Miserere's M3 NeedleMeter and generalised to the TWO scales
// Crypta's metering backend (src/dsp/MeterTaps.h, issue #13) exposes:
//
//   Scale::gainReductionDb - positive dB of gain reduction (0 = none), for
//       the noise gate and the low-band parallel compressor. The needle
//       rests at the right-hand extreme at 0 dB and sweeps left as reduction
//       deepens - classic GR-meter behaviour.
//
//   Scale::peakLevelDb - dBFS block peak, for the input and output meters.
//       The needle rests at the left-hand extreme at the -60 dBFS floor and
//       sweeps right as level rises; the 0 dBFS-and-above ticks and the
//       needle itself switch to the warning colour once the reading reaches
//       full scale (WCAG 1.4.1: the change is redundant with needle POSITION,
//       never colour-only).
//
// Fully vector-drawn (face, engraved arc, tick marks, numerals, legend,
// needle - all juce::Path/Graphics at runtime): the design descends from the
// suite's HubNeedle/AnalogMeter family (aureate -> requiem -> silentium), but
// where those rotate a Blender-rendered needle sprite over a baked dial face,
// Crypta has no photoreal assets at all, so both the face and the needle are
// drawn here.
//
// Threading model (same as HubNeedle): setTargetDb() is a plain relaxed
// atomic store, safe from any thread; ballistic smoothing runs on the GUI
// thread via tick(), driven by the editor's own timer (never a Timer owned
// here), so headless tests can advance it deterministically without a
// running message loop. Nothing here ever touches the audio thread - the
// editor's timer reads CryptaAudioProcessor::getMeterTaps()'s relaxed
// atomics and pushes the result in.
//
// Accessibility (A-07 pattern, WCAG 4.1.2): exposes a read-only
// AccessibilityTextValueInterface with the current smoothed reading as a
// dB string, queryable on demand - deliberately NOT announced on every
// repaint (see silentium's AnalogMeter docs for why auto-announce is the
// wrong behaviour for a meter). The component is display-only: it never
// takes keyboard focus and never intercepts mouse events.
namespace basilica::gui
{
    class NeedleMeter : public juce::Component
    {
    public:
        enum class Scale
        {
            gainReductionDb,
            peakLevelDb
        };

        // accessibleTitle: e.g. "Gate gain reduction meter".
        // faceLegend: the short engraved legend on the face, e.g. "GATE".
        NeedleMeter (juce::String accessibleTitle, juce::String faceLegend, Scale meterScale);
        ~NeedleMeter() override;

        // Thread-safe (plain relaxed atomic store): the instantaneous
        // reading in dB. Non-finite values are stored as-is and sanitised at
        // the ballistics step.
        void setTargetDb (float newTargetDb) noexcept
        {
            targetDb.store (newTargetDb, std::memory_order_relaxed);
        }

        // Advances the ballistic smoothing by dtSeconds and repaints if the
        // smoothed value changed meaningfully - called from the editor's
        // timer (see PluginEditor.cpp).
        void tick (float dtSeconds) noexcept;

        // Test/preview-only: seeds both the raw target and the smoothed
        // reading immediately, bypassing the ramp (headless test binaries
        // have no message loop to pump real ticks through).
        void setImmediateDbForPreview (float db) noexcept;

        float getSmoothedDb() const noexcept { return smoothedDb; }
        Scale getScale() const noexcept { return scale; }

        void paint (juce::Graphics& g) override;
        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

        // One-pole ballistic integration step, pure/static so it is
        // directly unit-testable without a running timer. Non-finite
        // targets resolve to `restingValue` (needle at rest) instead of
        // poisoning the smoothed state with NaN/inf - which is why the
        // resting value is a parameter and not a hard-coded 0: 0 dB is
        // "no reduction" on a GR scale but FULL SCALE on a peak scale.
        static float stepBallistics (float currentSmoothed, float target,
                                     float dtSeconds, float tauSeconds,
                                     float restingValue = 0.0f) noexcept;

        // dB -> needle angle in degrees, clockwise from straight-up (12
        // o'clock). Piecewise-linear over the scale's engraved tick table,
        // clamped beyond both ends; strictly monotonic (decreasing for
        // gainReductionDb, increasing for peakLevelDb).
        static float angleDegreesForDb (Scale meterScale, float db) noexcept;

        // The reading a meter of this scale shows with no signal at all:
        // 0 dB of reduction, or the -100 dBFS silence floor.
        static float restingDbFor (Scale meterScale) noexcept;

        // Ballistics: symmetric on the GR scales (the compressor's own
        // attack/release is what the reading is meant to show), fast-attack/
        // slow-release on the peak scales so a transient is actually visible
        // at 30 Hz refresh.
        static float attackTauSecondsFor (Scale meterScale) noexcept;
        static float releaseTauSecondsFor (Scale meterScale) noexcept;

        static constexpr float gainReductionTauSeconds = 0.18f;
        static constexpr float peakAttackTauSeconds = 0.02f;
        static constexpr float peakReleaseTauSeconds = 0.45f;

        // dBFS at which the peak scale's warning zone starts.
        static constexpr float peakWarningThresholdDb = 0.0f;

    private:
        class ValueInterface;

        const juce::String title;
        const juce::String legend;
        const Scale scale;

        std::atomic<float> targetDb;
        float smoothedDb;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeedleMeter)
    };
}
