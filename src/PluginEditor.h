#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <map>
#include <memory>
#include <vector>

#include "gui/NeedleDial.h"
#include "gui/PlateTypography.h"
#include "gui/SpriteKnob.h"
#include "gui/SpriteToggle.h"
#include "presets/PresetBar.h"

class CryptaAudioProcessor;

// Wave-3 photoreal "composited plate" editor (suite rollout 2026-08,
// DECISIONS.md in the gui-pipeline scaffold): the approved EMPTY master
// plate render (resources/gui/plate_crypta.png) is the baseline image, and
// every control is composited on top of it from the suite's control-sprite
// library at positions declared in resources/gui/layout-manifest.json -
// param id -> sprite type + centre + size, derived from the plugin's
// control inventory. This replaces the v0.4-era fully vector-drawn editor
// (owner decision 2026-07-15: vector GUIs rejected, photoreal mandatory).
//
// Composition layers, bottom to top:
//   1. the empty plate (paint()) - gold rim, corner filigree, vents,
//      centre ornament, screws; all baked, zero baked controls.
//   2. sprite controls (child components): SpriteKnob (static sprite +
//      rotating feathered inner disc), SpriteToggle (up sprite; mirrored
//      down state - a documented sprite-library gap workaround), and the
//      family VU pair as NeedleDial (needle-free face sprite + live vector
//      needle from the processor's lock-free MeterTaps).
//   3. typography (paint(), src/gui/PlateTypography.h): wordmark, section
//      lettering, per-control labels and thin engraved section rules, set
//      live in EB Garamond so they stay tack sharp at every scale step -
//      text baked into AI master renders never survived the render loop
//      legibly (suite typography pass, owner decision 2026-07-26).
//
// CONTROL SURFACE SCOPE: exactly the rendered inventory of
// rollout-2026-07/crypta/control-inventory.md - 33 knobs + 1 selector
// (highVoicing) + 5 toggles + 2 VU meters. Parameters added after that
// inventory (gate v0.3 extensions, low-band detector/knee/auto, Graaawl,
// driveEngine, highBias, clipCeiling) are automation/host-only for this
// GUI generation, per the same suite precedent that governs triptych's
// v0.5.0 and lancet's v0.4.0 waves. tests/gui/LayoutManifestTests.cpp
// pins those counts.
//
// FOCUS ORDER CONTRACT (WCAG 2.4.3): children are created in manifest
// order, which is signal-flow/reading order; JUCE's default traverser
// follows creation order - do not reorder.
//
// RESIZING: stepped window scaling (75/100/150/200%) like the rest of the
// photoreal family - no free resize with prerendered assets (UA
// convention; the suite skill file). The chosen scale persists in plugin
// state under the same root property the v0.4 editor used.
class CryptaAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                         private juce::Timer
{
public:
    explicit CryptaAudioProcessorEditor (CryptaAudioProcessor& processorToEdit);
    ~CryptaAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    //==========================================================================
    // Layout manifest access - tests parse the SAME embedded JSON the
    // editor builds itself from (tests/gui/LayoutManifestTests.cpp).

    struct SpriteSpec
    {
        juce::String binary;
        float width = 0, height = 0;
        float knobCx = 0, knobCy = 0, knobRadius = 0;
        float contentDiameter = 0;
        float pivotXFrac = 0, pivotYFrac = 0, needleLengthFrac = 0;
    };

    struct ControlSpec
    {
        juce::String id, type, label, tap;
        float cx = 0, cy = 0, size = 0, sweep = 270;
    };

    struct LabelSpec
    {
        juce::String text, style;
        float cx = 0, cy = 0, h = 0;
    };

    struct RuleSpec
    {
        float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    };

    struct Manifest
    {
        juce::String plateBinary;
        int plateWidth1x = 0, plateHeight1x = 0;
        std::map<juce::String, SpriteSpec> sprites;
        std::vector<basilica::gui::NeedleDial::Tick> vuTicks;
        std::vector<ControlSpec> controls;
        std::vector<LabelSpec> labels;
        std::vector<RuleSpec> rules;
    };

    static Manifest parseLayoutManifest();

    //==========================================================================
    // Stepped window scale, persisted as a root property of the APVTS
    // state tree (same storage slot as the previous editor generation, so
    // stored sessions keep round-tripping; arbitrary stored values snap to
    // the nearest step).

    static constexpr std::array<float, 4> scaleSteps { 0.75f, 1.0f, 1.5f, 2.0f };
    static constexpr int defaultScaleStepIndex = 1; // 100%

    static const juce::Identifier& getScaleStatePropertyId() noexcept;
    static int readPersistedScaleStepIndex (const juce::ValueTree& state) noexcept;

    void applyScaleStep (int newStepIndex);
    int getScaleStepIndex() const noexcept { return scaleStepIndex; }
    float getEditorScale() const noexcept { return scaleSteps[(size_t) scaleStepIndex]; }

    int getDesignWidth() const noexcept { return designWidth; }
    int getDesignHeight() const noexcept { return designHeight; }

    //==========================================================================
    // Metering seam (headless tests drive the pump directly - no message
    // loop, no real timer ticks).

    void updateMetersFromProcessor (float dtSeconds);

    bool isMeteringTimerRunning() const noexcept { return isTimerRunning(); }
    int getMeteringTimerIntervalMs() const noexcept { return getTimerInterval(); }

    static constexpr int meterRefreshHz = 30;
    static constexpr int topStripHeight1x = 34;

private:
    void timerCallback() override;
    void cycleScale();
    void buildControlsFromManifest();
    juce::Image imageForBinary (const juce::String& binaryName) const;
    void drawPlateTypography (juce::Graphics& g, float scale, float plateOriginY) const;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct KnobControl
    {
        ControlSpec spec;
        std::unique_ptr<basilica::gui::SpriteKnob> slider;
        std::unique_ptr<SliderAttachment> attachment;
    };

    struct ToggleControl
    {
        ControlSpec spec;
        std::unique_ptr<basilica::gui::SpriteToggle> button;
        std::unique_ptr<ButtonAttachment> attachment;
    };

    struct MeterControl
    {
        ControlSpec spec;
        std::unique_ptr<basilica::gui::NeedleDial> dial;
    };

    CryptaAudioProcessor& audioProcessor;

    Manifest manifest;
    juce::Image plateImage;
    basilica::gui::PlateTypography typography;

    basilica::presets::PresetBar presetBar;
    juce::TextButton scaleButton;
    int scaleStepIndex = defaultScaleStepIndex;

    std::vector<KnobControl> knobs;
    std::vector<ToggleControl> toggles;
    std::vector<MeterControl> meters;

    int designWidth = 0;
    int designHeight = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CryptaAudioProcessorEditor)
};
