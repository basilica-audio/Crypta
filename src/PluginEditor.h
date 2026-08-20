#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

#include "gui/BasilicaLookAndFeel.h"
#include "gui/BusPanel.h"
#include "gui/NeedleMeter.h"
#include "gui/PointerKnob.h"
#include "presets/PresetBar.h"

class CryptaAudioProcessor;

// M3/M6 custom vector editor (issues #45 / #25), accessible parameter
// surface (issues #26 / #46), metering UI (issue #27) and resizable editor
// with persisted scale (issue #28). Replaces the v0.1-v0.3 preset bar +
// GenericAudioProcessorEditor stack.
//
// Everything is drawn at runtime by BasilicaLookAndFeel / the src/gui
// components - no photoreal PNG assets exist in this plugin (unlike the
// filmstrip/faceplate siblings): pointer knobs with engraved scale rings,
// lamp toggles, and four vector needle meters (input peak, gate gain
// reduction, low-band compressor gain reduction, output peak - exactly the
// readings cryp::MeterTaps exposes), grouped into one BusPanel per stage in
// signal-flow order: Input / Noise Gate / Crossover / Low Band / Drive
// Engine / Mid Band / High Band / Cabinet / EQ / Output.
//
// FOCUS ORDER CONTRACT (WCAG 2.4.3, suite-wide convention): JUCE's default
// traverser walks children in z-order, which equals CREATION order - the
// constructor therefore creates every control in signal-flow/reading order
// (preset bar first, then panel by panel, left-to-right within each row),
// and nothing may reorder children afterwards. Each BusPanel is an
// accessibility focus container (NOT a keyboard focus container - see
// BusPanel.h), so screen readers hear "Low Band, Ratio" while Tab still
// walks the whole editor.
//
// Controls are built data-driven from ID/label tables (see the .cpp) - all
// float AND choice parameters are PointerKnobs (choice knobs snap to their
// integer detents and announce the choice NAME), bool parameters are real
// juce::ToggleButtons (focusable and Space/Enter-operable out of the box,
// reported as toggle buttons by AT).
//
// RESIZING (issue #28): the whole surface is laid out once at a fixed design
// size inside `content`, and resizing only changes the uniform scale
// transform applied to that child - so no layout arithmetic can break at an
// odd window size. JUCE 8.0.14's AudioProcessorEditor::setScaleFactor() is
// explicitly the HOST's channel ("Can be called by a host to tell the editor
// that it should use a non-unity GUI scale", juce_AudioProcessorEditor.h:129)
// and sets a transform on the editor itself, so it is deliberately NOT
// reused for the user's own scale: the two compose (host transform on the
// editor, user scale on `content`) instead of fighting over one slot. The
// user-facing mechanism is the documented one for plug-in editors:
// setResizable (true, true) - which attaches a ResizableCornerComponent -
// plus setResizeLimits() and a fixed aspect ratio on the resulting
// constrainer (juce_AudioProcessorEditor.cpp, ComponentBoundsConstrainer::
// setFixedAspectRatio).
class CryptaAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                          private juce::Timer
{
public:
    explicit CryptaAudioProcessorEditor (CryptaAudioProcessor& processorToEdit);
    ~CryptaAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    //==============================================================================
    // Issue #28: resizable editor with the chosen scale persisted in plugin
    // state.

    // The unscaled design size every child is laid out at, computed in the
    // constructor from the layout constants + control tables in the .cpp
    // (never a hand-copied literal) and asserted against the real component
    // tree in tests/gui/EditorLayoutTests.cpp.
    int getDesignWidth() const noexcept { return designWidth; }
    int getDesignHeight() const noexcept { return designHeight; }

    static constexpr double minimumEditorScale = 0.6;
    static constexpr double maximumEditorScale = 1.8;
    static constexpr double defaultEditorScale = 1.0;

    // The property name the scale is stored under, as a child property of
    // the APVTS root state tree. Living on the APVTS state (rather than in a
    // processor member) is what makes it round-trip through the EXISTING
    // CryptaAudioProcessor::get/setStateInformation() pair for free: the
    // tree is serialised whole, and JUCE 8.0.14's APVTS only reacts to
    // property changes on its PARAM child trees (juce_AudioProcessor
    // ValueTreeState.cpp:442), so a root-level property is inert for it.
    static const juce::Identifier& getScaleStatePropertyId() noexcept;

    // Reads the persisted scale out of an APVTS state tree, clamped to the
    // supported range; falls back to defaultEditorScale when absent or
    // unparseable. Static so a test (or a future host-side query) can ask
    // without constructing an editor.
    static double readPersistedScale (const juce::ValueTree& state) noexcept;

    // Applies a new scale: resizes the editor (through the constrainer, so
    // the aspect ratio and the size limits are honoured) and persists it.
    void setEditorScale (double newScale);

    double getEditorScale() const noexcept;

    //==============================================================================
    // Issue #27 (metering UI) verification seam. The 30 Hz GUI-thread timer
    // is the only thing that reads cryp::MeterTaps; exposing the pump and
    // the timer's state lets a headless test binary - which has no running
    // message loop to fire real timer callbacks - assert the full path
    // (atomics -> ballistics -> needle) deterministically.
    void updateMetersFromProcessor (float dtSeconds);

    bool isMeteringTimerRunning() const noexcept { return isTimerRunning(); }
    int getMeteringTimerIntervalMs() const noexcept { return getTimerInterval(); }

    static constexpr int meterRefreshHz = 30;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob
    {
        basilica::gui::PointerKnob slider;
        juce::Label label;
        std::unique_ptr<SliderAttachment> attachment;
    };

    struct Toggle
    {
        juce::ToggleButton button;
        std::unique_ptr<ButtonAttachment> attachment;
    };

    // One stage faceplate: the BusPanel component plus its control rows
    // (each row a left-to-right list of the controls laid out in it) and
    // an optional needle meter in the panel's right bay.
    struct Panel
    {
        std::unique_ptr<basilica::gui::BusPanel> component;
        std::vector<std::vector<juce::Component*>> rows;
        basilica::gui::NeedleMeter* meter = nullptr; // owned via `meters`
    };

    Panel& addPanel (const juce::String& sectionTitle);
    void addRow (Panel& panel);
    Knob& addKnob (Panel& panel, const char* parameterId, const juce::String& labelText);
    Toggle& addToggle (Panel& panel, const char* parameterId, const juce::String& labelText);
    basilica::gui::NeedleMeter& addMeter (Panel& panel, const juce::String& accessibleTitle,
                                          const juce::String& faceLegend,
                                          basilica::gui::NeedleMeter::Scale scale);

    void timerCallback() override;

    void layoutContent();
    void persistScale (double scale);

    static int slotWidthFor (const juce::Component& control) noexcept;
    static int rowWidth (const std::vector<juce::Component*>& row) noexcept;
    static int panelRequiredWidth (const Panel& panel) noexcept;
    static int panelRequiredHeight (const Panel& panel) noexcept;
    int bandRequiredWidth (size_t bandIndex) const noexcept;
    int bandRequiredHeight (size_t bandIndex) const noexcept;

    CryptaAudioProcessor& audioProcessor;

    // Must be constructed before any child that paints with it and
    // installed on `this` so it propagates to every child (including the
    // preset bar's stock buttons/menus/dialogs).
    basilica::gui::BasilicaLookAndFeel lookAndFeel;

    // Everything visible lives inside this child, laid out at the fixed
    // design size; the editor only ever scales it (see the class docs).
    juce::Component content;

    // M2 preset system - constructed after the localisation frame is
    // installed (see the constructor) so its TRANS()'d strings pick up the
    // right language from the very first paint.
    basilica::presets::PresetBar presetBar;

    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<std::unique_ptr<Toggle>> toggles;
    std::vector<std::unique_ptr<basilica::gui::NeedleMeter>> meters;
    std::vector<std::unique_ptr<Panel>> panels;

    // Signal-flow bands: each band is a horizontal row of panels, stacked
    // top to bottom in the order they are appended.
    std::vector<std::vector<Panel*>> bands;

    // Meters owned by `meters`, kept as raw pointers for the timer.
    basilica::gui::NeedleMeter* inputMeter = nullptr;
    basilica::gui::NeedleMeter* gateMeter = nullptr;
    basilica::gui::NeedleMeter* lowCompMeter = nullptr;
    basilica::gui::NeedleMeter* outputMeter = nullptr;

    int designWidth = 0;
    int designHeight = 0;

    // Persistence is armed only once the constructor has finished restoring
    // the stored scale. Without this, the intermediate sizes JUCE 8.0.14's
    // setResizeLimits() produces on a still-zero-sized editor (it ends with
    // setBoundsConstrained (getBounds()), which snaps to the minimum size
    // and fires resized()) would be written to the session state.
    bool scalePersistenceArmed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CryptaAudioProcessorEditor)
};
