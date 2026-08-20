#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "presets/Localisation.h"

#include <BinaryData.h>

#include <algorithm>
#include <cmath>

namespace
{
    // ----- Vector-editor layout metrics (issue #25) -----------------------
    // All values are design constants, not measurements of pre-rendered art
    // (there is none): the editor computes its own design size from these
    // plus the control tables in the constructor, and
    // tests/gui/EditorLayoutTests.cpp asserts the resulting geometry
    // (containment, no overlap, full parameter coverage) on the real
    // component tree, so a change here can never silently clip a control.
    constexpr int outerMargin = 10;
    constexpr int presetBarHeight = 30;
    constexpr int bandGap = 8;

    constexpr int panelPadding = 10;
    constexpr int panelBottomPadding = 8;
    constexpr int rowGap = 8;

    // A knob slot: attached label above (JUCE 8.0.14 Label::
    // componentMovedOrResized sizes an above-attached label to
    // borderTopAndBottom + 6 + fontHeight ~ 22 px for the 14 px suite
    // serif, so 24 reserved keeps it clear of the row above), then the
    // rotary area, then the value box baked into the slider's own bounds.
    constexpr int labelHeight = 24;
    constexpr int knobSize = 60;
    constexpr int textBoxHeight = 16;
    constexpr int knobSlotWidth = 80;
    constexpr int toggleSlotWidth = 66;
    constexpr int toggleHeight = 24;
    constexpr int slotGap = 6; // trimmed off the right of every slot
    constexpr int rowHeight = labelHeight + knobSize + textBoxHeight;

    // Right-hand meter bay on the four metered panels.
    constexpr int meterBayWidth = 150;
    constexpr int meterWidth = 134;
    constexpr int meterHeight = 96;

    // Peak metering floor: the dB value a silent block maps to, matching
    // basilica::gui::NeedleMeter::restingDbFor (Scale::peakLevelDb).
    constexpr float peakMeterFloorDb = -100.0f;

    // M2 i18n frame: selects German (resources/i18n/de.txt) or falls
    // through to English, once, at editor construction - see
    // Localisation.h's docs. `presetBar` is a member initialised via the
    // constructor's initialiser list, and its own constructor already calls
    // TRANS() on every button label - member initialisers run in
    // declaration order, so this helper (called from presetBar's own
    // initialiser expression below) is what guarantees installLocalisation()
    // runs before presetBar exists, not a call in the constructor *body*,
    // which would run too late.
    basilica::presets::PresetManager& initLocalisationThenGetPresetManager (CryptaAudioProcessor& processor)
    {
        basilica::presets::installLocalisation (BinaryData::de_txt, BinaryData::de_txtSize);
        return processor.presetManager;
    }
}

//==============================================================================
const juce::Identifier& CryptaAudioProcessorEditor::getScaleStatePropertyId() noexcept
{
    static const juce::Identifier id ("editorScale");
    return id;
}

double CryptaAudioProcessorEditor::readPersistedScale (const juce::ValueTree& state) noexcept
{
    if (! state.hasProperty (getScaleStatePropertyId()))
        return defaultEditorScale;

    const auto stored = (double) state.getProperty (getScaleStatePropertyId());

    // A hand-edited or corrupt session must not be able to produce a
    // zero-sized (or absurdly huge) window.
    if (! std::isfinite (stored) || stored <= 0.0)
        return defaultEditorScale;

    return juce::jlimit (minimumEditorScale, maximumEditorScale, stored);
}

//==============================================================================
CryptaAudioProcessorEditor::CryptaAudioProcessorEditor (CryptaAudioProcessor& processorToEdit)
    : juce::AudioProcessorEditor (&processorToEdit),
      audioProcessor (processorToEdit),
      presetBar (initLocalisationThenGetPresetManager (processorToEdit))
{
    // Propagates to every child, including the preset bar's stock buttons
    // and any menus/dialogs they open.
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (content);

    // FOCUS ORDER (WCAG 2.4.3): children are created and added in signal-
    // flow/reading order - preset bar, then Input, Noise Gate, Crossover,
    // Low Band, Drive Engine, Mid Band, High Band, Cabinet, EQ, Output
    // (exactly the order processBlock() applies them in), left-to-right
    // within each row. JUCE's default traverser follows this creation
    // order; do not reorder. The three visual bands below regroup those
    // panels for LAYOUT only - `bands` never reorders the children.
    content.addAndMakeVisible (presetBar);

    // --- Band 1: Input | Noise Gate | Crossover ---------------------------
    auto& input = addPanel ("Input");
    addKnob (input, ParamIDs::inputGain, "Input Gain");
    addToggle (input, ParamIDs::bypass, "Bypass");
    inputMeter = &addMeter (input, "Input peak level meter", "IN",
                            basilica::gui::NeedleMeter::Scale::peakLevelDb);

    auto& gate = addPanel ("Noise Gate");
    addToggle (gate, ParamIDs::gateEnabled, "Gate");
    addKnob (gate, ParamIDs::gateMode, "Mode");
    addKnob (gate, ParamIDs::gateThreshold, "Threshold");
    addKnob (gate, ParamIDs::gateRatio, "Ratio");
    addKnob (gate, ParamIDs::gateAttack, "Attack");
    addKnob (gate, ParamIDs::gateRelease, "Release");

    addRow (gate);
    addKnob (gate, ParamIDs::gateHysteresis, "Hysteresis");
    addKnob (gate, ParamIDs::gateHold, "Hold");
    addKnob (gate, ParamIDs::gateScHpf, "SC Highpass");
    addKnob (gate, ParamIDs::gateRange, "Range");

    gateMeter = &addMeter (gate, "Gate gain reduction meter", "GATE",
                           basilica::gui::NeedleMeter::Scale::gainReductionDb);

    auto& crossover = addPanel ("Crossover");
    addKnob (crossover, ParamIDs::splitLowHz, "Split Low");
    addKnob (crossover, ParamIDs::splitHighHz, "Split High");

    bands.push_back ({ &input, &gate, &crossover });

    // --- Band 2: Low Band | Drive Engine | Mid Band | High Band ------------
    auto& low = addPanel ("Low Band");
    addKnob (low, ParamIDs::lowCompDetector, "Detector");
    addKnob (low, ParamIDs::lowCompThreshold, "Threshold");
    addKnob (low, ParamIDs::lowCompRatio, "Ratio");
    addKnob (low, ParamIDs::lowCompKnee, "Knee");
    addKnob (low, ParamIDs::lowCompAttack, "Attack");
    addKnob (low, ParamIDs::lowCompRelease, "Release");

    addRow (low);
    addToggle (low, ParamIDs::lowCompAutoRelease, "Auto Rel");
    addToggle (low, ParamIDs::lowCompAutoMakeup, "Auto Mkup");
    addKnob (low, ParamIDs::lowCompMakeup, "Makeup");
    addKnob (low, ParamIDs::lowCompMix, "Mix");
    addKnob (low, ParamIDs::lowLevel, "Low Level");

    lowCompMeter = &addMeter (low, "Low band compressor gain reduction meter", "COMP",
                              basilica::gui::NeedleMeter::Scale::gainReductionDb);

    auto& engine = addPanel ("Drive Engine");
    addKnob (engine, ParamIDs::driveEngine, "Engine");

    auto& mid = addPanel ("Mid Band");
    addKnob (mid, ParamIDs::midDrive, "Mid Drive");
    addKnob (mid, ParamIDs::midLevel, "Mid Level");

    auto& high = addPanel ("High Band");
    addKnob (high, ParamIDs::highTightHz, "Tight");
    addKnob (high, ParamIDs::highVoicing, "Voicing");
    addKnob (high, ParamIDs::highDrive, "High Drive");
    addKnob (high, ParamIDs::highBias, "Bias");

    addRow (high);
    addKnob (high, ParamIDs::highTone, "Tone");
    addKnob (high, ParamIDs::highBlend, "Blend");
    addKnob (high, ParamIDs::highLevel, "High Level");

    bands.push_back ({ &low, &engine, &mid, &high });

    // --- Band 3: Cabinet | EQ | Output -------------------------------------
    auto& cabinet = addPanel ("Cabinet");
    addToggle (cabinet, ParamIDs::irEnabled, "Cab");
    addKnob (cabinet, ParamIDs::irMix, "Cab Mix");

    auto& eq = addPanel ("EQ");
    addToggle (eq, ParamIDs::eqEnabled, "EQ");
    addKnob (eq, ParamIDs::eqLowShelfFreq, "Low Freq");
    addKnob (eq, ParamIDs::eqLowShelfGain, "Low Gain");
    addKnob (eq, ParamIDs::eqPeak1Freq, "Peak 1 Freq");
    addKnob (eq, ParamIDs::eqPeak1Gain, "Peak 1 Gain");
    addKnob (eq, ParamIDs::eqPeak1Q, "Peak 1 Q");

    addRow (eq);
    addKnob (eq, ParamIDs::eqPeak2Freq, "Peak 2 Freq");
    addKnob (eq, ParamIDs::eqPeak2Gain, "Peak 2 Gain");
    addKnob (eq, ParamIDs::eqPeak2Q, "Peak 2 Q");
    addKnob (eq, ParamIDs::eqHighShelfFreq, "High Freq");
    addKnob (eq, ParamIDs::eqHighShelfGain, "High Gain");

    auto& output = addPanel ("Output");
    addToggle (output, ParamIDs::outputClip, "Clip");
    addKnob (output, ParamIDs::clipCeiling, "Ceiling");
    addKnob (output, ParamIDs::outputGain, "Output Gain");
    outputMeter = &addMeter (output, "Output peak level meter", "OUT",
                             basilica::gui::NeedleMeter::Scale::peakLevelDb);

    bands.push_back ({ &cabinet, &eq, &output });

    // --- Design size: computed from the control tables above ---------------
    int contentWidth = 0;
    int contentHeight = presetBarHeight;

    for (size_t i = 0; i < bands.size(); ++i)
    {
        contentWidth = std::max (contentWidth, bandRequiredWidth (i));
        contentHeight += bandGap + bandRequiredHeight (i);
    }

    designWidth = outerMargin * 2 + contentWidth;
    designHeight = outerMargin * 2 + contentHeight;

    // --- Resizing (issue #28) ---------------------------------------------
    // Read the persisted scale BEFORE touching the resize limits: JUCE
    // 8.0.14's setResizeLimits() ends with setBoundsConstrained (getBounds()),
    // which - on a still-zero-sized editor - snaps the window to the minimum
    // size and therefore fires resized(), which persists whatever scale that
    // produced. Reading first makes the restore immune to that.
    const auto persistedScale = readPersistedScale (audioProcessor.apvts.state);

    // setResizeLimits() installs (and configures) the default constrainer;
    // the fixed aspect ratio then makes every user drag a pure scale change,
    // which is exactly the contract layoutContent() relies on.
    // setResizable (true, true) attaches the ResizableCornerComponent
    // (JUCE 8.0.14, AudioProcessorEditor::setResizable).
    setResizeLimits ((int) std::lround (designWidth * minimumEditorScale),
                     (int) std::lround (designHeight * minimumEditorScale),
                     (int) std::lround (designWidth * maximumEditorScale),
                     (int) std::lround (designHeight * maximumEditorScale));

    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio ((double) designWidth / (double) designHeight);

    setResizable (true, true);

    // Restore the scale the user last left this plugin instance at, then
    // arm persistence: everything up to this point is construction noise,
    // not a user decision.
    setEditorScale (persistedScale);
    scalePersistenceArmed = true;

    // Meter polling: 30 Hz GUI-thread timer feeding the ballistic needles;
    // cryp::MeterTaps' getters are relaxed-atomic loads, so this never
    // touches (or blocks) the audio thread (issue #27).
    startTimerHz (meterRefreshHz);
}

CryptaAudioProcessorEditor::~CryptaAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

//==============================================================================
CryptaAudioProcessorEditor::Panel& CryptaAudioProcessorEditor::addPanel (const juce::String& sectionTitle)
{
    auto panel = std::make_unique<Panel>();
    panel->component = std::make_unique<basilica::gui::BusPanel> (sectionTitle);
    panel->rows.emplace_back();

    content.addAndMakeVisible (*panel->component);

    panels.push_back (std::move (panel));
    return *panels.back();
}

void CryptaAudioProcessorEditor::addRow (Panel& panel)
{
    panel.rows.emplace_back();
}

CryptaAudioProcessorEditor::Knob& CryptaAudioProcessorEditor::addKnob (Panel& panel, const char* parameterId,
                                                                       const juce::String& labelText)
{
    auto knob = std::make_unique<Knob>();

    knob->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, knobSlotWidth - slotGap, textBoxHeight);
    knob->slider.setTitle (labelText);
    knob->slider.setName (labelText);
    panel.component->addAndMakeVisible (knob->slider);

    knob->label.setText (labelText, juce::dontSendNotification);
    knob->label.setJustificationType (juce::Justification::centred);
    knob->label.attachToComponent (&knob->slider, false); // above; auto-repositions with the slider
    panel.component->addAndMakeVisible (knob->label);

    // SliderAttachment MUST be constructed before the textFromValueFunction
    // override below, not after: JUCE 8.0.14's SliderParameterAttachment
    // constructor (juce_ParameterAttachments.cpp:128) itself assigns
    // `slider.textFromValueFunction` as part of wiring the attachment -
    // setting our own function BEFORE this point would be silently
    // clobbered the moment the attachment is created.
    knob->attachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, parameterId, knob->slider);

    if (auto* param = audioProcessor.apvts.getParameter (parameterId))
    {
        // A-02 pattern: unit-carrying parameters declare their unit via
        // .withLabel() in ParameterLayout.cpp (dB/dBFS/ms/Hz/%/:1) - feed
        // it into both the value box and the accessibility value string, so
        // a screen reader hears "-60.00 dB" rather than a bare "-60.00".
        //
        // Choice parameters keep the parameter's own getText(), which
        // returns the choice NAME. Continuous ones are formatted here rather
        // than by getText(): the log-mapped frequency/time ranges are built
        // from custom NormalisableRange conversion lambdas, and JUCE 8.0.14's
        // AudioParameterFloat::getText() falls back to full float precision
        // for those - "119.9999847" in a 74 px value box, truncated with an
        // ellipsis. Decimals are chosen by magnitude instead.
        const auto unit = param->getLabel();
        const auto isChoice = dynamic_cast<const juce::AudioParameterChoice*> (param) != nullptr;

        knob->slider.textFromValueFunction = [param, unit, isChoice] (double value)
        {
            if (isChoice)
                return param->getText (param->convertTo0to1 ((float) value), 0);

            const auto magnitude = std::abs (value);
            const auto decimals = magnitude >= 100.0 ? 0 : (magnitude >= 10.0 ? 1 : 2);

            auto text = juce::String (value, decimals);

            // "-0.00 dB" is a rounding artefact, not a reading.
            if (text.startsWithChar ('-') && ! text.containsAnyOf ("123456789"))
                text = text.substring (1);

            if (unit.isEmpty())
                return text;

            // Ratio labels read as "10.0:1", every other unit as "10.0 ms".
            return unit.startsWithChar (':') ? text + unit : text + " " + unit;
        };

        knob->slider.updateText();
    }

    panel.rows.back().push_back (&knob->slider);
    knobs.push_back (std::move (knob));
    return *knobs.back();
}

CryptaAudioProcessorEditor::Toggle& CryptaAudioProcessorEditor::addToggle (Panel& panel, const char* parameterId,
                                                                           const juce::String& labelText)
{
    auto toggle = std::make_unique<Toggle>();

    // Real juce::ToggleButton on purpose: focusable and Space/Enter-
    // operable by default, and its createAccessibilityHandler() reports
    // AccessibilityRole::toggleButton (JUCE 8.0.14 juce_ToggleButton.cpp:71)
    // so it lands in the VoiceOver rotor as a toggle, not a plain button.
    toggle->button.setButtonText (labelText);
    toggle->button.setTitle (labelText);
    toggle->button.setName (labelText);
    panel.component->addAndMakeVisible (toggle->button);

    toggle->attachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, parameterId, toggle->button);

    panel.rows.back().push_back (&toggle->button);
    toggles.push_back (std::move (toggle));
    return *toggles.back();
}

basilica::gui::NeedleMeter& CryptaAudioProcessorEditor::addMeter (Panel& panel, const juce::String& accessibleTitle,
                                                                   const juce::String& faceLegend,
                                                                   basilica::gui::NeedleMeter::Scale scale)
{
    auto meter = std::make_unique<basilica::gui::NeedleMeter> (accessibleTitle, faceLegend, scale);
    panel.component->addAndMakeVisible (*meter);
    panel.meter = meter.get();

    meters.push_back (std::move (meter));
    return *meters.back();
}

//==============================================================================
void CryptaAudioProcessorEditor::timerCallback()
{
    updateMetersFromProcessor (1.0f / (float) meterRefreshHz);
}

void CryptaAudioProcessorEditor::updateMetersFromProcessor (float dtSeconds)
{
    const auto& taps = audioProcessor.getMeterTaps();

    const auto peakDb = [] (const std::atomic<float>& left, const std::atomic<float>& right)
    {
        // Stereo peaks: the louder side is what a clip-watching meter shows.
        return juce::Decibels::gainToDecibels (juce::jmax (left.load (std::memory_order_relaxed),
                                                            right.load (std::memory_order_relaxed)),
                                               peakMeterFloorDb);
    };

    if (inputMeter != nullptr)
        inputMeter->setTargetDb (peakDb (taps.inputPeakLeft, taps.inputPeakRight));

    if (outputMeter != nullptr)
        outputMeter->setTargetDb (peakDb (taps.outputPeakLeft, taps.outputPeakRight));

    // Already positive dB of gain reduction, straight from the engine's
    // per-block metering (src/dsp/MeterTaps.h).
    if (gateMeter != nullptr)
        gateMeter->setTargetDb (taps.gateGainReductionDb.load (std::memory_order_relaxed));

    if (lowCompMeter != nullptr)
        lowCompMeter->setTargetDb (taps.lowCompGainReductionDb.load (std::memory_order_relaxed));

    for (auto& meter : meters)
        meter->tick (dtSeconds);
}

//==============================================================================
int CryptaAudioProcessorEditor::slotWidthFor (const juce::Component& control) noexcept
{
    return dynamic_cast<const juce::Slider*> (&control) != nullptr ? knobSlotWidth : toggleSlotWidth;
}

int CryptaAudioProcessorEditor::rowWidth (const std::vector<juce::Component*>& row) noexcept
{
    int width = 0;

    for (const auto* control : row)
        width += slotWidthFor (*control);

    return width;
}

int CryptaAudioProcessorEditor::panelRequiredWidth (const Panel& panel) noexcept
{
    int widest = 0;

    for (const auto& row : panel.rows)
        widest = std::max (widest, rowWidth (row));

    return panelPadding * 2 + widest + (panel.meter != nullptr ? meterBayWidth : 0);
}

int CryptaAudioProcessorEditor::panelRequiredHeight (const Panel& panel) noexcept
{
    const auto numRows = (int) panel.rows.size();
    return basilica::gui::BusPanel::headerHeight
         + numRows * rowHeight + (numRows - 1) * rowGap
         + panelBottomPadding;
}

int CryptaAudioProcessorEditor::bandRequiredWidth (size_t bandIndex) const noexcept
{
    const auto& band = bands[bandIndex];
    int width = ((int) band.size() - 1) * bandGap;

    for (const auto* panel : band)
        width += panelRequiredWidth (*panel);

    return width;
}

int CryptaAudioProcessorEditor::bandRequiredHeight (size_t bandIndex) const noexcept
{
    int height = 0;

    for (const auto* panel : bands[bandIndex])
        height = std::max (height, panelRequiredHeight (*panel));

    return height;
}

//==============================================================================
double CryptaAudioProcessorEditor::getEditorScale() const noexcept
{
    return designWidth > 0 ? (double) getWidth() / (double) designWidth : defaultEditorScale;
}

void CryptaAudioProcessorEditor::setEditorScale (double newScale)
{
    const auto clamped = juce::jlimit (minimumEditorScale, maximumEditorScale,
                                       std::isfinite (newScale) ? newScale : defaultEditorScale);

    // Through the constrainer, so the aspect ratio and the size limits set
    // up in the constructor apply to a programmatic scale change exactly as
    // they do to a corner drag (JUCE 8.0.14, AudioProcessorEditor::
    // setBoundsConstrained).
    setBoundsConstrained (getBounds().withSize ((int) std::lround (designWidth * clamped),
                                                (int) std::lround (designHeight * clamped)));
}

void CryptaAudioProcessorEditor::persistScale (double scale)
{
    // Issue #28: the scale is a root-level property on the APVTS state
    // tree, which CryptaAudioProcessor::getStateInformation() serialises
    // whole - see getScaleStatePropertyId().
    //
    // Written only when it actually CHANGES, and never written at all while
    // the editor is simply sitting at the default size on a session that
    // has no stored scale: merely opening a plug-in window must not mutate
    // the session's saved state (hosts treat that as an edit, and a
    // validator comparing state across an editor open/close cycle would
    // rightly flag it).
    if (! scalePersistenceArmed)
        return;

    auto& state = audioProcessor.apvts.state;

    if (! state.hasProperty (getScaleStatePropertyId())
        && juce::approximatelyEqual (scale, defaultEditorScale))
        return;

    if (juce::approximatelyEqual (readPersistedScale (state), scale))
        return;

    state.setProperty (getScaleStatePropertyId(), scale, nullptr);
}

void CryptaAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (basilica::gui::BasilicaLookAndFeel::getEditorBackgroundColour());
}

void CryptaAudioProcessorEditor::resized()
{
    if (designWidth <= 0 || designHeight <= 0 || getWidth() <= 0 || getHeight() <= 0)
        return;

    // The content is always laid out at the design size; the window size is
    // expressed purely as a uniform scale transform on it.
    content.setBounds (0, 0, designWidth, designHeight);
    content.setTransform (juce::AffineTransform::scale ((float) getEditorScale()));

    layoutContent();

    persistScale (getEditorScale());
}

void CryptaAudioProcessorEditor::layoutContent()
{
    auto bounds = content.getLocalBounds().reduced (outerMargin);

    presetBar.setBounds (bounds.removeFromTop (presetBarHeight));

    const auto layoutPanel = [] (Panel& panel, juce::Rectangle<int> area)
    {
        panel.component->setBounds (area);

        auto panelContent = panel.component->getLocalBounds().reduced (panelPadding, 0);
        panelContent.removeFromTop (basilica::gui::BusPanel::headerHeight);

        if (panel.meter != nullptr)
        {
            auto bay = panelContent.removeFromRight (meterBayWidth);
            panel.meter->setBounds (juce::Rectangle<int> (meterWidth,
                                                          juce::jmin (meterHeight, bay.getHeight()))
                                        .withCentre (bay.getCentre()));
        }

        for (auto& row : panel.rows)
        {
            auto rowArea = panelContent.removeFromTop (rowHeight);
            rowArea.removeFromTop (labelHeight); // attached labels position themselves here

            for (auto* control : row)
            {
                auto slot = rowArea.removeFromLeft (slotWidthFor (*control)).withTrimmedRight (slotGap);

                if (dynamic_cast<juce::Slider*> (control) != nullptr)
                    control->setBounds (slot.withHeight (knobSize + textBoxHeight));
                else
                    control->setBounds (slot.withSizeKeepingCentre (slot.getWidth(), toggleHeight)
                                            .withY (rowArea.getY() + (knobSize - toggleHeight) / 2));
            }

            panelContent.removeFromTop (rowGap);
        }
    };

    for (size_t bandIndex = 0; bandIndex < bands.size(); ++bandIndex)
    {
        bounds.removeFromTop (bandGap);
        auto bandArea = bounds.removeFromTop (bandRequiredHeight (bandIndex));

        const auto& band = bands[bandIndex];

        for (size_t panelIndex = 0; panelIndex < band.size(); ++panelIndex)
        {
            // The last panel of each band absorbs the band's leftover width,
            // so every band spans the full editor width instead of leaving a
            // ragged right edge.
            const auto isLast = panelIndex + 1 == band.size();
            auto panelArea = isLast ? bandArea
                                    : bandArea.removeFromLeft (panelRequiredWidth (*band[panelIndex]));

            layoutPanel (*band[panelIndex], panelArea);

            if (! isLast)
                bandArea.removeFromLeft (bandGap);
        }
    }
}
