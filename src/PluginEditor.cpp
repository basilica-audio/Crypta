#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "presets/Localisation.h"

#include <BinaryData.h>

namespace
{
    constexpr int presetBarHeight = 28;
    constexpr int meterRowHeight = 22;
    constexpr int margin = 4;

    // M2 i18n frame (.scaffold/specs/preset-system-m2.md): selects German
    // (resources/i18n/de.txt) or falls through to English, once, at editor
    // construction - see Localisation.h's docs. `presetBar` is a member
    // initialised via the constructor's initialiser list, and its own
    // constructor already calls TRANS() on every button label - member
    // initialisers run in declaration order regardless of the order they're
    // written in, so this helper (called from presetBar's own initialiser
    // expression below) is what actually guarantees installLocalisation()
    // runs before presetBar exists, not a installLocalisation() call in the
    // constructor *body*, which would run too late.
    basilica::presets::PresetManager& initLocalisationThenGetPresetManager (CryptaAudioProcessor& processor)
    {
        basilica::presets::installLocalisation (BinaryData::de_txt, BinaryData::de_txtSize);
        return processor.presetManager;
    }
}

CryptaAudioProcessorEditor::CryptaAudioProcessorEditor (CryptaAudioProcessor& processorToEdit)
    : juce::AudioProcessorEditor (&processorToEdit),
      presetBar (initLocalisationThenGetPresetManager (processorToEdit)),
      meterRow (processorToEdit),
      genericEditor (processorToEdit)
{
    addAndMakeVisible (presetBar);
    addAndMakeVisible (meterRow);
    addAndMakeVisible (genericEditor);

    setResizable (true, true);
    setSize (genericEditor.getWidth(),
              presetBarHeight + margin + meterRowHeight + margin + genericEditor.getHeight());
}

//==============================================================================
CryptaAudioProcessorEditor::MeterRow::MeterRow (CryptaAudioProcessor& processorToRead)
    : processor (processorToRead)
{
    setInterceptsMouseClicks (false, false);

    // 30 Hz. The taps are block-decimated, so polling faster would only
    // re-read the same values.
    startTimerHz (30);
}

void CryptaAudioProcessorEditor::MeterRow::timerCallback()
{
    const auto& taps = processor.getMeterTaps();

    const auto toDb = [] (float linear)
    {
        return juce::Decibels::gainToDecibels (linear, -100.0f);
    };

    // Peaks are stereo; the row shows the louder side, which is what a
    // clip-watching meter is for.
    inputPeakDb = toDb (juce::jmax (taps.inputPeakLeft.load (std::memory_order_relaxed),
                                     taps.inputPeakRight.load (std::memory_order_relaxed)));
    outputPeakDb = toDb (juce::jmax (taps.outputPeakLeft.load (std::memory_order_relaxed),
                                      taps.outputPeakRight.load (std::memory_order_relaxed)));

    lowCompReductionDb = taps.lowCompGainReductionDb.load (std::memory_order_relaxed);
    gateReductionDb = taps.gateGainReductionDb.load (std::memory_order_relaxed);

    repaint();
}

void CryptaAudioProcessorEditor::MeterRow::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().reduced (margin, 0);

    if (bounds.isEmpty())
        return;

    g.setFont (juce::FontOptions (12.0f));

    // Four equal cells: input peak, output peak, low-comp GR, gate GR.
    const auto cellWidth = bounds.getWidth() / 4;

    struct Cell
    {
        const char* label;
        float valueDb;
        float minimumDb;
        float maximumDb;
        bool isReduction;
    };

    const Cell cells[] = {
        { "IN", inputPeakDb, -60.0f, 6.0f, false },
        { "OUT", outputPeakDb, -60.0f, 6.0f, false },
        { "COMP GR", lowCompReductionDb, 0.0f, 24.0f, true },
        { "GATE GR", gateReductionDb, 0.0f, 60.0f, true },
    };

    for (const auto& cell : cells)
    {
        auto cellBounds = bounds.removeFromLeft (cellWidth).reduced (2, 2);

        auto labelBounds = cellBounds.removeFromLeft (56);
        g.setColour (juce::Colours::grey);
        g.drawText (cell.label, labelBounds, juce::Justification::centredLeft, false);

        g.setColour (juce::Colours::darkgrey.withAlpha (0.4f));
        g.fillRect (cellBounds);

        const auto proportion = juce::jlimit (
            0.0f, 1.0f, (cell.valueDb - cell.minimumDb) / (cell.maximumDb - cell.minimumDb));

        auto fill = cellBounds.withWidth (juce::roundToInt (static_cast<float> (cellBounds.getWidth()) * proportion));

        // Peaks turn red as they approach full scale; gain reduction is drawn
        // in a neutral colour, since more of it is not a warning.
        const auto overThreshold = ! cell.isReduction && cell.valueDb > -1.0f;
        g.setColour (overThreshold ? juce::Colours::orangered : juce::Colours::lightgrey);
        g.fillRect (fill);

        g.setColour (juce::Colours::white);
        g.drawText (juce::String (cell.valueDb, 1) + " dB", cellBounds, juce::Justification::centredRight, false);
    }
}

CryptaAudioProcessorEditor::~CryptaAudioProcessorEditor() = default;

void CryptaAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();

    presetBar.setBounds (bounds.removeFromTop (presetBarHeight));
    bounds.removeFromTop (margin);

    meterRow.setBounds (bounds.removeFromTop (meterRowHeight));
    bounds.removeFromTop (margin);

    genericEditor.setBounds (bounds);
}
