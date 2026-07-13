#include "PluginEditor.h"
#include "PluginProcessor.h"

TwistYourGutsAudioProcessorEditor::TwistYourGutsAudioProcessorEditor (TwistYourGutsAudioProcessor& processorToEdit)
    : juce::AudioProcessorEditor (&processorToEdit),
      genericEditor (processorToEdit)
{
    addAndMakeVisible (genericEditor);
    setResizable (true, true);
    setSize (genericEditor.getWidth(), genericEditor.getHeight());
}

TwistYourGutsAudioProcessorEditor::~TwistYourGutsAudioProcessorEditor() = default;

void TwistYourGutsAudioProcessorEditor::resized()
{
    genericEditor.setBounds (getLocalBounds());
}
