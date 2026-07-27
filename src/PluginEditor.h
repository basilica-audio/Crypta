#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "presets/PresetBar.h"

class CryptaAudioProcessor;

// Minimal editor: a M2 preset bar (src/presets/PresetBar.h) docked at the
// top, wrapping JUCE's GenericAudioProcessorEditor below it so every APVTS
// parameter still gets a working control for free. A custom GUI replaces
// this in a later milestone (M3).
class CryptaAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit CryptaAudioProcessorEditor (CryptaAudioProcessor& processorToEdit);
    ~CryptaAudioProcessorEditor() override;

    void resized() override;

private:
    // M2 preset system (src/presets/PresetBar.h) - a horizontal strip
    // docked at the top of the editor. Constructed after the localisation
    // frame is installed (see the constructor) so its TRANS()'d strings (and
    // any of its own dialogs opened later) pick up the right language from
    // the very first paint.
    basilica::presets::PresetBar presetBar;

    // v0.3.0 metering readout (issue #13). Deliberately plain: labelled bars
    // driven from the processor's lock-free MeterTaps at 30 Hz, sitting
    // between the preset bar and the generic parameter editor. The photoreal
    // M3 GUI consumes the same struct later - this exists so the metering
    // BACKEND is verifiable and usable now, not to be the final display.
    class MeterRow final : public juce::Component,
                            private juce::Timer
    {
    public:
        explicit MeterRow (CryptaAudioProcessor& processorToRead);

        void paint (juce::Graphics& g) override;

    private:
        void timerCallback() override;

        CryptaAudioProcessor& processor;

        // Snapshots taken by the timer, so paint() never reads the atomics
        // (and so a repaint triggered by something else shows a consistent
        // set of values rather than a mixture of two updates).
        float inputPeakDb = -100.0f;
        float outputPeakDb = -100.0f;
        float lowCompReductionDb = 0.0f;
        float gateReductionDb = 0.0f;
    };

    MeterRow meterRow;

    juce::GenericAudioProcessorEditor genericEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CryptaAudioProcessorEditor)
};
