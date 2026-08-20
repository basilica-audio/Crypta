#include "gui/NeedleMeter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

// The vector needle meter (issues #27 / #25): pure-function coverage of the
// ballistics and both dB->angle mappings (all static, so no timer or message
// loop is needed), plus the display-only component contract.
using basilica::gui::NeedleMeter;

TEST_CASE ("Ballistics step converges monotonically towards the target without overshoot", "[gui][meter]")
{
    float smoothed = 0.0f;
    constexpr float target = 10.0f;
    constexpr float dt = 1.0f / 30.0f;

    float previous = smoothed;

    // Strict monotonicity only over the first ~1.3 s of the ramp - once the
    // gap shrinks towards float epsilon the per-tick increment legitimately
    // underflows to zero, so asserting strict `>` all the way to
    // convergence would test float rounding, not the ballistics.
    for (int i = 0; i < 40; ++i)
    {
        smoothed = NeedleMeter::stepBallistics (previous, target, dt, NeedleMeter::gainReductionTauSeconds);

        CHECK (smoothed > previous);  // strictly rising towards the target...
        CHECK (smoothed <= target);   // ...never past it
        previous = smoothed;
    }

    for (int i = 0; i < 400; ++i)
        smoothed = NeedleMeter::stepBallistics (smoothed, target, dt, NeedleMeter::gainReductionTauSeconds);

    // ~14.7 s total at 30 Hz >> tau (0.18 s): must have converged.
    CHECK (smoothed == Catch::Approx (target).margin (1.0e-3));

    // Falling back to 0 is symmetric.
    for (int i = 0; i < 400; ++i)
        smoothed = NeedleMeter::stepBallistics (smoothed, 0.0f, dt, NeedleMeter::gainReductionTauSeconds);

    CHECK (smoothed == Catch::Approx (0.0f).margin (1.0e-3));
}

TEST_CASE ("Ballistics edge cases: zero dt/tau jump straight to the target", "[gui][meter]")
{
    CHECK (NeedleMeter::stepBallistics (2.0f, 8.0f, 0.0f, 0.18f) == Catch::Approx (8.0f));
    CHECK (NeedleMeter::stepBallistics (2.0f, 8.0f, -1.0f, 0.18f) == Catch::Approx (8.0f));
    CHECK (NeedleMeter::stepBallistics (2.0f, 8.0f, 0.033f, 0.0f) == Catch::Approx (8.0f));
}

TEST_CASE ("Ballistics sanitise non-finite inputs to the scale's resting value", "[gui][meter]")
{
    constexpr auto nan = std::numeric_limits<float>::quiet_NaN();
    constexpr auto inf = std::numeric_limits<float>::infinity();

    // A NaN/inf TARGET resolves to the resting value - the step output must
    // stay finite whatever the engine hands over.
    CHECK (std::isfinite (NeedleMeter::stepBallistics (5.0f, nan, 0.033f, 0.18f)));
    CHECK (std::isfinite (NeedleMeter::stepBallistics (5.0f, inf, 0.033f, 0.18f)));
    CHECK (std::isfinite (NeedleMeter::stepBallistics (5.0f, -inf, 0.033f, 0.18f)));

    // A poisoned CURRENT state recovers to the (sanitised) target instead
    // of sticking at NaN forever.
    CHECK (NeedleMeter::stepBallistics (nan, 4.0f, 0.033f, 0.18f) == Catch::Approx (4.0f));
    CHECK (NeedleMeter::stepBallistics (nan, nan, 0.033f, 0.18f) == Catch::Approx (0.0f));

    // 0 dB means "no reduction" on a GR scale but FULL SCALE on a peak
    // scale, which is why the resting value is a parameter: a NaN peak
    // reading must rest the needle at the silence floor, never slam it to
    // 0 dBFS.
    CHECK (NeedleMeter::stepBallistics (nan, nan, 0.033f, 0.02f, -100.0f) == Catch::Approx (-100.0f));
    CHECK (NeedleMeter::restingDbFor (NeedleMeter::Scale::gainReductionDb) == Catch::Approx (0.0f));
    CHECK (NeedleMeter::restingDbFor (NeedleMeter::Scale::peakLevelDb) == Catch::Approx (-100.0f));
}

TEST_CASE ("Gain-reduction dB->angle mapping hits the engraved ticks and clamps", "[gui][meter]")
{
    constexpr auto gr = NeedleMeter::Scale::gainReductionDb;

    // The GR tick table in NeedleMeter.cpp: 0/3/6/10/20 dB GR at
    // +50/+20/-5/-28/-50 degrees (needle rests right, sweeps left).
    CHECK (NeedleMeter::angleDegreesForDb (gr, 0.0f) == Catch::Approx (50.0f));
    CHECK (NeedleMeter::angleDegreesForDb (gr, 3.0f) == Catch::Approx (20.0f));
    CHECK (NeedleMeter::angleDegreesForDb (gr, 6.0f) == Catch::Approx (-5.0f));
    CHECK (NeedleMeter::angleDegreesForDb (gr, 10.0f) == Catch::Approx (-28.0f));
    CHECK (NeedleMeter::angleDegreesForDb (gr, 20.0f) == Catch::Approx (-50.0f));

    // Linear interpolation between two ticks (1.5 dB is halfway from 0 to 3).
    CHECK (NeedleMeter::angleDegreesForDb (gr, 1.5f) == Catch::Approx (35.0f));

    // Clamped beyond the scale (and for nonsense negative "reduction").
    CHECK (NeedleMeter::angleDegreesForDb (gr, -5.0f) == Catch::Approx (50.0f));
    CHECK (NeedleMeter::angleDegreesForDb (gr, 100.0f) == Catch::Approx (-50.0f));

    // Non-finite input rests the needle instead of producing a NaN angle.
    CHECK (NeedleMeter::angleDegreesForDb (gr, std::numeric_limits<float>::quiet_NaN())
           == Catch::Approx (50.0f));

    // Strictly monotonically decreasing across the live span - deeper
    // reduction always swings further left, no plateau or reversal.
    auto previous = NeedleMeter::angleDegreesForDb (gr, 0.0f);

    for (float db = 0.25f; db <= 20.0f; db += 0.25f)
    {
        const auto angle = NeedleMeter::angleDegreesForDb (gr, db);
        CHECK (angle < previous);
        previous = angle;
    }
}

TEST_CASE ("Peak-level dB->angle mapping hits the engraved ticks and clamps", "[gui][meter]")
{
    constexpr auto peak = NeedleMeter::Scale::peakLevelDb;

    // The peak tick table in NeedleMeter.cpp: -60/-24/-12/-6/0/+6 dBFS at
    // -50/-20/+5/+25/+40/+50 degrees (needle rests left, sweeps right).
    CHECK (NeedleMeter::angleDegreesForDb (peak, -60.0f) == Catch::Approx (-50.0f));
    CHECK (NeedleMeter::angleDegreesForDb (peak, -24.0f) == Catch::Approx (-20.0f));
    CHECK (NeedleMeter::angleDegreesForDb (peak, -12.0f) == Catch::Approx (5.0f));
    CHECK (NeedleMeter::angleDegreesForDb (peak, -6.0f) == Catch::Approx (25.0f));
    CHECK (NeedleMeter::angleDegreesForDb (peak, 0.0f) == Catch::Approx (40.0f));
    CHECK (NeedleMeter::angleDegreesForDb (peak, 6.0f) == Catch::Approx (50.0f));

    // Halfway between -12 and -6 dBFS is halfway between their angles.
    CHECK (NeedleMeter::angleDegreesForDb (peak, -9.0f) == Catch::Approx (15.0f));

    // The silence floor and anything below it park the needle at the left
    // extreme; anything above +6 dBFS pins it at the right one.
    CHECK (NeedleMeter::angleDegreesForDb (peak, -100.0f) == Catch::Approx (-50.0f));
    CHECK (NeedleMeter::angleDegreesForDb (peak, 24.0f) == Catch::Approx (50.0f));
    CHECK (NeedleMeter::angleDegreesForDb (peak, std::numeric_limits<float>::quiet_NaN())
           == Catch::Approx (-50.0f));

    // Strictly monotonically INCREASING - the opposite direction to the GR
    // scale, and never a plateau.
    auto previous = NeedleMeter::angleDegreesForDb (peak, -60.0f);

    for (float db = -59.5f; db <= 6.0f; db += 0.5f)
    {
        const auto angle = NeedleMeter::angleDegreesForDb (peak, db);
        CHECK (angle > previous);
        previous = angle;
    }

    // The two scales sweep opposite ways: at their respective "loud"
    // extreme the GR needle is left and the peak needle is right.
    CHECK (NeedleMeter::angleDegreesForDb (NeedleMeter::Scale::gainReductionDb, 20.0f)
           < NeedleMeter::angleDegreesForDb (peak, 6.0f));
}

TEST_CASE ("Entering the peak warning zone always moves the needle, not just its colour", "[gui][meter][a11y]")
{
    constexpr auto peak = NeedleMeter::Scale::peakLevelDb;

    // WCAG 1.4.1 Use of Colour: the peak meters turn the needle (and the
    // 0 dBFS-and-above engraved numerals) to the warning colour at full
    // scale. That hue change must never be the ONLY cue - needle POSITION
    // has to carry the same information, so a colour-blind user reads the
    // same state. Every sub-threshold reading therefore sits strictly left
    // of every at-or-over-threshold one.
    const auto thresholdAngle = NeedleMeter::angleDegreesForDb (peak, NeedleMeter::peakWarningThresholdDb);

    for (float db = -60.0f; db < NeedleMeter::peakWarningThresholdDb; db += 0.5f)
    {
        INFO ("safe reading " << db << " dBFS");
        CHECK (NeedleMeter::angleDegreesForDb (peak, db) < thresholdAngle);
    }

    for (float db = NeedleMeter::peakWarningThresholdDb; db <= 12.0f; db += 0.5f)
    {
        INFO ("warning reading " << db << " dBFS");
        CHECK (NeedleMeter::angleDegreesForDb (peak, db) >= thresholdAngle);
    }

    // The threshold is where a peak meter's warning belongs: full scale.
    CHECK (NeedleMeter::peakWarningThresholdDb == Catch::Approx (0.0f));
}

TEST_CASE ("Peak meters use fast-attack/slow-release ballistics, GR meters symmetric ones", "[gui][meter]")
{
    constexpr auto peak = NeedleMeter::Scale::peakLevelDb;
    constexpr auto gr = NeedleMeter::Scale::gainReductionDb;
    constexpr float dt = 1.0f / 30.0f;

    // GR: identical time constants in both directions - the display is
    // meant to show the compressor's own ballistics, not add its own.
    CHECK (NeedleMeter::attackTauSecondsFor (gr) == Catch::Approx (NeedleMeter::releaseTauSecondsFor (gr)));
    CHECK (NeedleMeter::attackTauSecondsFor (gr) == Catch::Approx (NeedleMeter::gainReductionTauSeconds));

    // Peak: markedly asymmetric, so a transient is visible at 30 Hz refresh
    // and the fall-back is readable rather than a flicker.
    CHECK (NeedleMeter::attackTauSecondsFor (peak) < NeedleMeter::releaseTauSecondsFor (peak));

    // One tick of rise covers most of the gap...
    const auto risen = NeedleMeter::stepBallistics (-40.0f, -10.0f, dt,
                                                    NeedleMeter::attackTauSecondsFor (peak), -100.0f);
    const auto risenFraction = (risen - (-40.0f)) / 30.0f;
    INFO ("peak attack covers " << risenFraction << " of the gap per tick");
    CHECK (risenFraction > 0.75f);

    // ...while one tick of fall covers only a little of it.
    const auto fallen = NeedleMeter::stepBallistics (-10.0f, -40.0f, dt,
                                                     NeedleMeter::releaseTauSecondsFor (peak), -100.0f);
    const auto fallenFraction = ((-10.0f) - fallen) / 30.0f;
    INFO ("peak release covers " << fallenFraction << " of the gap per tick");
    CHECK (fallenFraction < 0.12f);
    CHECK (fallenFraction > 0.0f);
}

TEST_CASE ("tick() follows the atomic target with the right ballistics per scale", "[gui][meter]")
{
    basilica::gui::NeedleMeter grMeter ("Test gain reduction meter", "TEST",
                                        NeedleMeter::Scale::gainReductionDb);

    CHECK (grMeter.getSmoothedDb() == Catch::Approx (0.0f));

    grMeter.setTargetDb (12.0f);

    for (int i = 0; i < 300; ++i)
        grMeter.tick (1.0f / 30.0f);

    CHECK (grMeter.getSmoothedDb() == Catch::Approx (12.0f).margin (1.0e-3));

    grMeter.setTargetDb (std::numeric_limits<float>::quiet_NaN());

    for (int i = 0; i < 300; ++i)
        grMeter.tick (1.0f / 30.0f);

    CHECK (std::isfinite (grMeter.getSmoothedDb()));
    CHECK (grMeter.getSmoothedDb() == Catch::Approx (0.0f).margin (1.0e-3));

    // A fresh peak meter starts parked at the silence floor, not at 0 dBFS.
    basilica::gui::NeedleMeter peakMeter ("Test peak meter", "TEST", NeedleMeter::Scale::peakLevelDb);
    CHECK (peakMeter.getSmoothedDb() == Catch::Approx (-100.0f));

    peakMeter.setTargetDb (-6.0f);

    // A single tick already gets most of the way there (fast attack)...
    peakMeter.tick (1.0f / 30.0f);
    CHECK (peakMeter.getSmoothedDb() > -30.0f);

    for (int i = 0; i < 300; ++i)
        peakMeter.tick (1.0f / 30.0f);

    CHECK (peakMeter.getSmoothedDb() == Catch::Approx (-6.0f).margin (1.0e-3));

    // ...and a NaN reading parks it back at the floor rather than at 0 dBFS.
    peakMeter.setTargetDb (std::numeric_limits<float>::quiet_NaN());

    for (int i = 0; i < 900; ++i)
        peakMeter.tick (1.0f / 30.0f);

    CHECK (peakMeter.getSmoothedDb() == Catch::Approx (-100.0f).margin (1.0e-2));
}

TEST_CASE ("NeedleMeter is a display-only component with a titled, read-only accessible value", "[gui][meter][a11y]")
{
    basilica::gui::NeedleMeter meter ("Gate gain reduction meter", "GATE",
                                      NeedleMeter::Scale::gainReductionDb);

    CHECK (meter.getTitle() == "Gate gain reduction meter");
    CHECK (meter.getScale() == NeedleMeter::Scale::gainReductionDb);
    CHECK_FALSE (meter.getWantsKeyboardFocus());

    bool clicksSelf = true, clicksChildren = true;
    meter.getInterceptsMouseClicks (clicksSelf, clicksChildren);
    CHECK_FALSE (clicksSelf);
    CHECK_FALSE (clicksChildren);

    // createAccessibilityHandler() (not getAccessibilityHandler()) - the
    // latter needs a live native peer (JUCE 8.0.14 juce_Component.cpp:
    // 3323-3326), which this headless binary never has.
    const auto handler = meter.createAccessibilityHandler();
    REQUIRE (handler != nullptr);

    auto* valueInterface = handler->getValueInterface();
    REQUIRE (valueInterface != nullptr);
    CHECK (valueInterface->isReadOnly());
    CHECK (valueInterface->getCurrentValueAsString() == "0.0 dB");

    meter.setImmediateDbForPreview (3.5f);
    CHECK (valueInterface->getCurrentValueAsString() == "3.5 dB");

    // The peak scale announces dBFS, so a screen-reader user can tell an
    // absolute level reading from a relative reduction one.
    basilica::gui::NeedleMeter peakMeter ("Input peak level meter", "IN", NeedleMeter::Scale::peakLevelDb);
    const auto peakHandler = peakMeter.createAccessibilityHandler();
    REQUIRE (peakHandler != nullptr);

    auto* peakValue = peakHandler->getValueInterface();
    REQUIRE (peakValue != nullptr);
    CHECK (peakValue->isReadOnly());

    peakMeter.setImmediateDbForPreview (-12.3f);
    CHECK (peakValue->getCurrentValueAsString() == "-12.3 dBFS");
}
