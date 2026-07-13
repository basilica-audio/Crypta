#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class TwistYourGutsAudioProcessor;

// Minimal editor: wraps JUCE's GenericAudioProcessorEditor so every
// APVTS parameter gets a working control for free. A custom GUI replaces
// this in a later milestone.
class TwistYourGutsAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit TwistYourGutsAudioProcessorEditor (TwistYourGutsAudioProcessor& processorToEdit);
    ~TwistYourGutsAudioProcessorEditor() override;

    void resized() override;

private:
    juce::GenericAudioProcessorEditor genericEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TwistYourGutsAudioProcessorEditor)
};
