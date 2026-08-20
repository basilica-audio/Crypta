#include "gui/BasilicaLookAndFeel.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

// A-03 pattern (WCAG 1.4.3 Contrast (Minimum), AA): pure-function WCAG
// relative-luminance contrast checks on the exact colour pairs the vector
// editor actually renders, fetched via BasilicaLookAndFeel's static
// accessors rather than hand-copied second literals that could silently
// drift out of sync with the ones drawn. Because this editor is 100%
// vector (no baked artwork), EVERY rendered text/marking pair is covered
// here - there is no "measured asset bound" escape hatch like the
// photoreal siblings need.
namespace
{
    // WCAG 2.x relative luminance (https://www.w3.org/TR/WCAG21/#dfn-relative-luminance)
    // applied to a juce::Colour's sRGB channels.
    double relativeLuminanceChannel (juce::uint8 sRGB8)
    {
        const auto c = (double) sRGB8 / 255.0;
        return c <= 0.03928 ? c / 12.92 : std::pow ((c + 0.055) / 1.055, 2.4);
    }

    double relativeLuminance (juce::Colour colour)
    {
        return 0.2126 * relativeLuminanceChannel (colour.getRed())
             + 0.7152 * relativeLuminanceChannel (colour.getGreen())
             + 0.0722 * relativeLuminanceChannel (colour.getBlue());
    }

    // WCAG contrast ratio (https://www.w3.org/TR/WCAG21/#dfn-contrast-ratio):
    // (L1 + 0.05) / (L2 + 0.05), L1 the lighter - order-independent by
    // construction (max/min).
    double contrastRatio (juce::Colour a, juce::Colour b)
    {
        const auto lA = relativeLuminance (a);
        const auto lB = relativeLuminance (b);
        const auto lighter = std::max (lA, lB);
        const auto darker = std::min (lA, lB);
        return (lighter + 0.05) / (darker + 0.05);
    }

    void checkTextPair (juce::Colour text, juce::Colour background, const char* what)
    {
        INFO (what << ": text " << text.toDisplayString (true).toStdString()
                   << " on " << background.toDisplayString (true).toStdString()
                   << " = " << contrastRatio (text, background) << ":1");

        // WCAG 1.4.3 (AA): the suite's 14 px serif is normal-size text
        // (below the ~18.66 px / 14 pt-bold "large text" threshold), so the
        // stricter 4.5:1 floor applies.
        CHECK (contrastRatio (text, background) >= 4.5);

        // The background must be fully opaque - a translucent surface would
        // make the REAL rendered contrast depend on whatever sits
        // underneath, defeating the guarantee.
        CHECK (background.isOpaque());
    }
}

TEST_CASE ("WCAG contrast ratio helper matches known reference values", "[gui][a11y]")
{
    // Black vs white is the canonical maximum-contrast pair: exactly 21:1.
    CHECK (contrastRatio (juce::Colours::black, juce::Colours::white) == Catch::Approx (21.0).margin (0.01));

    // Identical colours are always exactly 1:1.
    CHECK (contrastRatio (juce::Colours::grey, juce::Colours::grey) == Catch::Approx (1.0).margin (0.001));

    // Symmetric in its arguments.
    const auto a = juce::Colour (0xfff0d38c);
    const auto b = juce::Colour (0xff17110c);
    CHECK (contrastRatio (a, b) == Catch::Approx (contrastRatio (b, a)).margin (1.0e-9));
}

TEST_CASE ("Every rendered text pair of the vector editor clears WCAG AA 4.5:1", "[gui][a11y]")
{
    using LNF = basilica::gui::BasilicaLookAndFeel;

    // Control labels + toggle legends over the section-panel fill.
    checkTextPair (LNF::getLabelTextColour(), LNF::getPanelBackgroundColour(),
                   "label/legend on panel");

    // Engraved section headers over the panel fill.
    checkTextPair (LNF::getPanelHeaderTextColour(), LNF::getPanelBackgroundColour(),
                   "section header on panel");

    // Slider value boxes: gold value text on the recessed chip.
    checkTextPair (LNF::getValueTextColour(), LNF::getValueBoxBackgroundColour(),
                   "value text on value box");

    // Preset-bar button lettering on the button face.
    checkTextPair (LNF::getButtonTextColour(), LNF::getButtonFaceColour(),
                   "button text on button face");

    // Needle-meter scale markings/numerals + legend on the meter face.
    checkTextPair (LNF::getMeterMarkingColour(), LNF::getMeterFaceColour(),
                   "meter markings on meter face");

    // The peak meters' warning-zone numerals (0 dBFS and above) are drawn in
    // the warning colour on the same face, so they carry the same text bar.
    checkTextPair (LNF::getMeterWarningColour(), LNF::getMeterFaceColour(),
                   "meter warning numerals on meter face");
}

TEST_CASE ("Non-text state indicators clear WCAG 1.4.11's 3:1 against their background", "[gui][a11y]")
{
    using LNF = basilica::gui::BasilicaLookAndFeel;

    // The knob pointer IS the control-state display ("readability of
    // control state", CLAUDE.md) - hold it to the full 4.5:1 text bar, not
    // just the 3:1 non-text floor.
    INFO ("knob pointer on knob face = "
          << contrastRatio (LNF::getKnobPointerColour(), LNF::getKnobFaceColour()) << ":1");
    CHECK (contrastRatio (LNF::getKnobPointerColour(), LNF::getKnobFaceColour()) >= 4.5);
    CHECK (LNF::getKnobFaceColour().isOpaque());

    // Both needle colours (normal and over-level) against their face.
    CHECK (contrastRatio (LNF::getMeterNeedleColour(), LNF::getMeterFaceColour()) >= 4.5);
    CHECK (contrastRatio (LNF::getMeterWarningColour(), LNF::getMeterFaceColour()) >= 4.5);

    // The over-level needle colour must be distinguishable FROM the normal
    // needle colour, since the two encode different states on the same face.
    // This is a drift-proofing heuristic, NOT a WCAG success criterion: no SC
    // sets a contrast floor between two foreground colours. The actual
    // WCAG 1.4.1 (Use of Colour) guarantee is that the warning state is
    // ALSO carried by needle position - asserted directly in
    // tests/gui/NeedleMeterTests.cpp ("entering the warning zone always
    // moves the needle"), so a colour-blind user never depends on the hue
    // change alone.
    INFO ("warning needle vs normal needle = "
          << contrastRatio (LNF::getMeterWarningColour(), LNF::getMeterNeedleColour()) << ":1");
    CHECK (contrastRatio (LNF::getMeterWarningColour(), LNF::getMeterNeedleColour()) >= 2.5);

    // ...and they are separated in hue as well as in luminance.
    const auto hueSeparation = std::abs (LNF::getMeterWarningColour().getHue()
                                         - LNF::getMeterNeedleColour().getHue()) * 360.0f;
    INFO ("warning/normal needle hue separation = " << hueSeparation << " degrees");
    CHECK (hueSeparation >= 20.0f);

    // The focus ring (WCAG 2.4.7 indicator) against the panel fill and the
    // editor background - 3:1 per 1.4.11 Non-text Contrast. The dark halo
    // painted beneath it (see paintFocusRing) keeps it legible over the
    // gold-lit lamp toggles too.
    CHECK (contrastRatio (LNF::getFocusRingColour(), LNF::getPanelBackgroundColour()) >= 3.0);
    CHECK (contrastRatio (LNF::getFocusRingColour(), LNF::getEditorBackgroundColour()) >= 3.0);

    // Both fills the editor actually paints under controls are opaque.
    CHECK (LNF::getPanelBackgroundColour().isOpaque());
    CHECK (LNF::getEditorBackgroundColour().isOpaque());
    CHECK (LNF::getMeterFaceColour().isOpaque());
}

TEST_CASE ("The embedded suite serif is used for every rendered glyph", "[gui][a11y]")
{
    using LNF = basilica::gui::BasilicaLookAndFeel;

    // EB Garamond, loaded from BinaryData (resources/fonts/) - never a
    // system font, so the editor renders identically on macOS and Windows.
    const auto regular = LNF::getSerifFont (14.0f);
    const auto semiBold = LNF::getSerifFont (17.0f, true);

    CHECK (regular.getHeight() == Catch::Approx (14.0f));
    CHECK (semiBold.getHeight() == Catch::Approx (17.0f));

    REQUIRE (regular.getTypefacePtr() != nullptr);
    REQUIRE (semiBold.getTypefacePtr() != nullptr);

    CHECK (regular.getTypefacePtr()->getName().containsIgnoreCase ("Garamond"));
    CHECK (semiBold.getTypefacePtr()->getName().containsIgnoreCase ("Garamond"));

    // The two faces are genuinely different objects - a silent fallback to
    // one face for both weights would flatten the typographic hierarchy.
    CHECK (regular.getTypefacePtr() != semiBold.getTypefacePtr());
}
