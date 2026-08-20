#include "BasilicaLookAndFeel.h"

#include <BinaryData.h>

#include <cmath>

namespace basilica::gui
{
    namespace
    {
        // The suite's black/gold pairs. Gold-on-near-black measures ~12.8:1
        // (WCAG relative luminance), leaving generous drift headroom over
        // the 4.5:1 AA floor asserted in BasilicaLookAndFeelContrastTests.cpp.
        constexpr juce::uint32 suiteGold = 0xfff0d38c;      // warm gold lettering
        constexpr juce::uint32 suiteGoldBright = 0xfff7e3ae; // header/emphasis gold
        constexpr juce::uint32 signalRed = 0xffd85a3e;      // peak-meter over zone
        constexpr juce::uint32 panelBlack = 0xff17110c;     // section-panel fill
        constexpr juce::uint32 editorBlack = 0xff0d0a07;    // window fill behind panels
        constexpr juce::uint32 recessBlack = 0xff0b0805;    // recessed value chips / meter face
        constexpr juce::uint32 buttonFace = 0xff1d150e;     // preset-bar button face
        constexpr juce::uint32 knobFace = 0xff211913;       // pointer-knob body
        constexpr juce::uint32 knobRim = 0xff3a2d1f;        // pointer-knob rim ring

        const juce::Typeface::Ptr& regularTypeface()
        {
            static juce::Typeface::Ptr typeface = juce::Typeface::createSystemTypefaceFor (
                BinaryData::EBGaramondRegular_ttf, BinaryData::EBGaramondRegular_ttfSize);
            return typeface;
        }

        const juce::Typeface::Ptr& semiBoldTypeface()
        {
            static juce::Typeface::Ptr typeface = juce::Typeface::createSystemTypefaceFor (
                BinaryData::EBGaramondSemiBold_ttf, BinaryData::EBGaramondSemiBold_ttfSize);
            return typeface;
        }
    }

    BasilicaLookAndFeel::BasilicaLookAndFeel()
    {
        // Window / generic component colours.
        setColour (juce::ResizableWindow::backgroundColourId, getEditorBackgroundColour());
        setColour (juce::DocumentWindow::textColourId, getLabelTextColour());

        // Labels default to transparent backing over the opaque panel fill.
        setColour (juce::Label::textColourId, getLabelTextColour());
        setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);

        // Slider value box (TextBoxBelow) - recessed chip with gold value
        // lettering; the rotary art itself is drawn by drawRotarySlider().
        setColour (juce::Slider::textBoxTextColourId, getValueTextColour());
        setColour (juce::Slider::textBoxBackgroundColourId, getValueBoxBackgroundColour());
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (knobRim));
        setColour (juce::Slider::textBoxHighlightColourId, juce::Colour (suiteGold).withAlpha (0.35f));

        // Toggle legends.
        setColour (juce::ToggleButton::textColourId, getLabelTextColour());
        setColour (juce::ToggleButton::tickColourId, getLabelTextColour());

        // Preset bar buttons.
        setColour (juce::TextButton::buttonColourId, getButtonFaceColour());
        setColour (juce::TextButton::buttonOnColourId, getButtonFaceColour());
        setColour (juce::TextButton::textColourOffId, getButtonTextColour());
        setColour (juce::TextButton::textColourOnId, getButtonTextColour());

        // Popup menus (preset list) + alert windows + text editors (Save
        // As... prompt) - keep the same black/gold pairing.
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (buttonFace));
        setColour (juce::PopupMenu::textColourId, getButtonTextColour());
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (knobRim));
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colour (suiteGoldBright));
        setColour (juce::AlertWindow::backgroundColourId, juce::Colour (panelBlack));
        setColour (juce::AlertWindow::textColourId, getLabelTextColour());
        setColour (juce::AlertWindow::outlineColourId, juce::Colour (knobRim));
        setColour (juce::TextEditor::backgroundColourId, juce::Colour (recessBlack));
        setColour (juce::TextEditor::textColourId, getValueTextColour());
        setColour (juce::TextEditor::highlightColourId, juce::Colour (suiteGold).withAlpha (0.35f));
        setColour (juce::TextEditor::outlineColourId, juce::Colour (knobRim));
        setColour (juce::TextEditor::focusedOutlineColourId, getFocusRingColour());
        setColour (juce::CaretComponent::caretColourId, getLabelTextColour());
    }

    juce::Font BasilicaLookAndFeel::getSerifFont (float height, bool semiBold)
    {
        return juce::Font (juce::FontOptions (semiBold ? semiBoldTypeface() : regularTypeface())
                               .withHeight (height));
    }

    juce::Font BasilicaLookAndFeel::getLabelFont (juce::Label& label)
    {
        juce::ignoreUnused (label);
        return getSerifFont (14.0f);
    }

    void BasilicaLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
    {
        g.fillAll (label.findColour (juce::Label::backgroundColourId));

        if (! label.isBeingEdited())
        {
            const auto alpha = label.isEnabled() ? 1.0f : 0.5f;
            g.setColour (label.findColour (juce::Label::textColourId).withMultipliedAlpha (alpha));
            g.setFont (getLabelFont (label));
            g.drawFittedText (label.getText(), label.getBorderSize().subtractedFrom (label.getLocalBounds()),
                              label.getJustificationType(), 1, 1.0f);
        }
        else if (label.isEnabled())
        {
            g.setColour (label.findColour (juce::Label::outlineColourId));
        }

        g.setColour (label.findColour (juce::Label::outlineColourId));
        g.drawRect (label.getLocalBounds());
    }

    void BasilicaLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                                float sliderPosProportional, float rotaryStartAngle,
                                                float rotaryEndAngle, juce::Slider& slider)
    {
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
        const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
        const auto centre = bounds.getCentre();

        // --- Engraved scale ring ---------------------------------------
        // Stepped knobs (choice parameters, integer detents) get exactly
        // one tick per detent; continuous knobs get the standard 11.
        const auto range = slider.getRange();
        const auto interval = slider.getInterval();
        int tickCount = 11;

        if (interval > 0.0 && range.getLength() > 0.0)
        {
            const auto steps = (int) std::lround (range.getLength() / interval);
            if (steps >= 1 && steps <= 24)
                tickCount = steps + 1;
        }

        const auto tickOuter = radius;
        const auto tickInner = radius * 0.88f;

        g.setColour (getLabelTextColour().withAlpha (0.85f));

        for (int i = 0; i < tickCount; ++i)
        {
            const auto t = tickCount > 1 ? (float) i / (float) (tickCount - 1) : 0.0f;
            const auto angle = rotaryStartAngle + t * (rotaryEndAngle - rotaryStartAngle);
            const auto sinA = std::sin (angle);
            const auto cosA = std::cos (angle);

            juce::Path tick;
            tick.startNewSubPath (centre.x + tickInner * sinA, centre.y - tickInner * cosA);
            tick.lineTo (centre.x + tickOuter * sinA, centre.y - tickOuter * cosA);
            g.strokePath (tick, juce::PathStrokeType (i == 0 || i == tickCount - 1 ? 1.6f : 1.0f));
        }

        // --- Knob body --------------------------------------------------
        const auto knobRadius = radius * 0.78f;
        const auto knobBounds = juce::Rectangle<float> (knobRadius * 2.0f, knobRadius * 2.0f).withCentre (centre);

        g.setColour (juce::Colour (knobFace));
        g.fillEllipse (knobBounds);

        // Subtle top-light: brighter crescent at the upper edge.
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.10f),
                                                 centre.x, knobBounds.getY(),
                                                 juce::Colours::transparentBlack,
                                                 centre.x, centre.y, false));
        g.fillEllipse (knobBounds);

        g.setColour (juce::Colour (knobRim));
        g.drawEllipse (knobBounds, 1.5f);

        // --- Pointer ----------------------------------------------------
        const auto angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
        const auto pointerLength = knobRadius * 0.82f;
        const auto pointerWidth = juce::jmax (2.0f, knobRadius * 0.11f);

        juce::Path pointer;
        pointer.addRoundedRectangle (-pointerWidth / 2.0f, -knobRadius + 1.5f,
                                     pointerWidth, pointerLength, pointerWidth / 2.0f);
        pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));

        g.setColour (getKnobPointerColour());
        g.fillPath (pointer);

        // Hub dot.
        const auto hubRadius = knobRadius * 0.12f;
        g.fillEllipse (juce::Rectangle<float> (hubRadius * 2.0f, hubRadius * 2.0f).withCentre (centre));
    }

    void BasilicaLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                                bool shouldDrawButtonAsHighlighted,
                                                bool shouldDrawButtonAsDown)
    {
        auto bounds = button.getLocalBounds().toFloat().reduced (2.0f);

        // Lamp: a small jewel on the left, lit gold when on.
        const auto lampDiameter = juce::jmin (bounds.getHeight() - 6.0f, 14.0f);
        auto lampArea = bounds.removeFromLeft (lampDiameter + 8.0f);
        const auto lamp = juce::Rectangle<float> (lampDiameter, lampDiameter)
                              .withCentre ({ lampArea.getX() + lampDiameter / 2.0f + 2.0f, lampArea.getCentreY() });

        const bool on = button.getToggleState();

        if (on)
        {
            // Halo behind the lit jewel.
            g.setColour (getLabelTextColour().withAlpha (0.25f));
            g.fillEllipse (lamp.expanded (3.0f));
            g.setColour (juce::Colour (suiteGoldBright));
            g.fillEllipse (lamp);
        }
        else
        {
            g.setColour (juce::Colour (recessBlack));
            g.fillEllipse (lamp);
        }

        g.setColour (getLabelTextColour().withAlpha (on ? 1.0f : 0.7f));
        g.drawEllipse (lamp, 1.2f);

        // Legend.
        const auto highlight = shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown;
        g.setColour (button.findColour (juce::ToggleButton::textColourId)
                         .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f)
                         .brighter (highlight ? 0.15f : 0.0f));
        g.setFont (getSerifFont (14.0f));
        g.drawFittedText (button.getButtonText(), bounds.toNearestInt(), juce::Justification::centredLeft, 2);

        // WCAG 2.4.7: LookAndFeel_V4 draws no toggle focus indication.
        if (button.hasKeyboardFocus (true))
            paintFocusRing (g, button.getLocalBounds().toFloat(), FocusRingShape::roundedRectangle);
    }

    juce::Font BasilicaLookAndFeel::getTextButtonFont (juce::TextButton& button, int buttonHeight)
    {
        juce::ignoreUnused (button);
        return getSerifFont (juce::jmin (15.0f, (float) buttonHeight * 0.7f));
    }

    void BasilicaLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                                    const juce::Colour& backgroundColour,
                                                    bool shouldDrawButtonAsHighlighted,
                                                    bool shouldDrawButtonAsDown)
    {
        juce::ignoreUnused (backgroundColour);

        const auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
        constexpr float cornerSize = 4.0f;

        auto face = getButtonFaceColour();

        if (shouldDrawButtonAsDown)
            face = face.darker (0.3f);
        else if (shouldDrawButtonAsHighlighted)
            face = face.brighter (0.08f);

        g.setColour (face);
        g.fillRoundedRectangle (bounds, cornerSize);

        g.setColour (getLabelTextColour().withAlpha (button.getToggleState() ? 0.9f : 0.45f));
        g.drawRoundedRectangle (bounds, cornerSize, 1.2f);

        // The 3D saturation-boost focus cue V4 uses is invisible on these
        // dark faces - draw the suite's explicit ring instead (WCAG 2.4.7).
        if (button.hasKeyboardFocus (true))
            paintFocusRing (g, button.getLocalBounds().toFloat(), FocusRingShape::roundedRectangle);
    }

    void BasilicaLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                              bool shouldDrawButtonAsHighlighted,
                                              bool shouldDrawButtonAsDown)
    {
        juce::ignoreUnused (shouldDrawButtonAsDown);

        g.setColour (getButtonTextColour().brighter (shouldDrawButtonAsHighlighted ? 0.1f : 0.0f)
                         .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
        g.setFont (getTextButtonFont (button, button.getHeight()));
        g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (4, 2),
                          juce::Justification::centred, 2);
    }

    // ----------------------------------------------------------------------
    juce::Colour BasilicaLookAndFeel::getEditorBackgroundColour() noexcept { return juce::Colour (editorBlack); }
    juce::Colour BasilicaLookAndFeel::getPanelBackgroundColour() noexcept { return juce::Colour (panelBlack); }
    juce::Colour BasilicaLookAndFeel::getLabelTextColour() noexcept { return juce::Colour (suiteGold); }
    juce::Colour BasilicaLookAndFeel::getPanelHeaderTextColour() noexcept { return juce::Colour (suiteGoldBright); }
    juce::Colour BasilicaLookAndFeel::getValueTextColour() noexcept { return juce::Colour (suiteGold); }
    juce::Colour BasilicaLookAndFeel::getValueBoxBackgroundColour() noexcept { return juce::Colour (recessBlack); }
    juce::Colour BasilicaLookAndFeel::getButtonTextColour() noexcept { return juce::Colour (suiteGold); }
    juce::Colour BasilicaLookAndFeel::getButtonFaceColour() noexcept { return juce::Colour (buttonFace); }
    juce::Colour BasilicaLookAndFeel::getMeterFaceColour() noexcept { return juce::Colour (recessBlack); }
    juce::Colour BasilicaLookAndFeel::getMeterMarkingColour() noexcept { return juce::Colour (suiteGold); }
    juce::Colour BasilicaLookAndFeel::getMeterNeedleColour() noexcept { return juce::Colour (suiteGoldBright); }
    juce::Colour BasilicaLookAndFeel::getMeterWarningColour() noexcept { return juce::Colour (signalRed); }
    juce::Colour BasilicaLookAndFeel::getKnobFaceColour() noexcept { return juce::Colour (knobFace); }
    juce::Colour BasilicaLookAndFeel::getKnobPointerColour() noexcept { return juce::Colour (suiteGoldBright); }
    juce::Colour BasilicaLookAndFeel::getFocusRingColour() noexcept { return juce::Colour (suiteGoldBright); }
    juce::Colour BasilicaLookAndFeel::getFocusRingHaloColour() noexcept { return juce::Colour (0xcc000000); }

    void paintFocusRing (juce::Graphics& g, juce::Rectangle<float> bounds, FocusRingShape shape)
    {
        constexpr float ringInset = 2.0f;
        constexpr float haloStrokeWidth = 4.0f;
        constexpr float ringStrokeWidth = 2.0f;
        constexpr float roundedRectCornerSize = 4.0f;

        const auto ringBounds = bounds.reduced (ringInset);

        const auto drawShape = [shape] (juce::Graphics& graphics, juce::Rectangle<float> shapeBounds, float strokeWidth)
        {
            if (shape == FocusRingShape::ellipse)
                graphics.drawEllipse (shapeBounds, strokeWidth);
            else
                graphics.drawRoundedRectangle (shapeBounds, roundedRectCornerSize, strokeWidth);
        };

        // Dark halo first so the bright gold ring stays legible against
        // light or busy backgrounds too, not just the dark panels.
        g.setColour (BasilicaLookAndFeel::getFocusRingHaloColour());
        drawShape (g, ringBounds, haloStrokeWidth);

        g.setColour (BasilicaLookAndFeel::getFocusRingColour());
        drawShape (g, ringBounds, ringStrokeWidth);
    }
}
