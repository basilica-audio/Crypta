#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "gui/NeedleMeter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>

// Metering UI (issue #27): the editor's 30 Hz GUI-thread timer is the ONLY
// reader of cryp::MeterTaps, and this file drives that exact path
// deterministically - render audio through the processor, pump the editor's
// meter update, assert the needles. A headless console binary has no running
// message loop to fire real timer callbacks, which is why
// updateMetersFromProcessor() is exposed (see PluginEditor.h).
namespace
{
    template <typename ComponentType>
    ComponentType* findDescendantByTitle (juce::Component& parent, const juce::String& title)
    {
        for (int i = 0; i < parent.getNumChildComponents(); ++i)
        {
            auto* child = parent.getChildComponent (i);

            if (auto* typed = dynamic_cast<ComponentType*> (child))
                if (typed->getTitle() == title)
                    return typed;

            if (auto* found = findDescendantByTitle<ComponentType> (*child, title))
                return found;
        }

        return nullptr;
    }

    // A steady sine at a known peak amplitude, so the expected dBFS reading
    // is arithmetic rather than a guess.
    void renderSine (CryptaAudioProcessor& processor, float peakAmplitude,
                     int numBlocks = 8, double frequencyHz = 220.0)
    {
        constexpr int blockSize = 512;
        constexpr double sampleRate = 48000.0;

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        double phase = 0.0;
        const auto phaseIncrement = juce::MathConstants<double>::twoPi * frequencyHz / sampleRate;

        for (int block = 0; block < numBlocks; ++block)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = peakAmplitude * (float) std::sin (phase);
                phase += phaseIncrement;

                for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                    buffer.setSample (channel, sample, value);
            }

            processor.processBlock (buffer, midi);
        }
    }

    void renderSilence (CryptaAudioProcessor& processor, int numBlocks = 8)
    {
        renderSine (processor, 0.0f, numBlocks);
    }
}

TEST_CASE ("The editor polls the metering backend on a 30 Hz timer", "[gui][meter]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    // Issue #27's "30/60 fps timer-driven updates": running, and at 30 Hz.
    CHECK (editor.isMeteringTimerRunning());
    CHECK (editor.getMeteringTimerIntervalMs() == 1000 / CryptaAudioProcessorEditor::meterRefreshHz);
    CHECK (CryptaAudioProcessorEditor::meterRefreshHz == 30);
}

TEST_CASE ("Input and output peak needles follow the real MeterTaps readings", "[gui][meter]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    auto* inputMeter = findDescendantByTitle<basilica::gui::NeedleMeter> (editor, "Input peak level meter");
    auto* outputMeter = findDescendantByTitle<basilica::gui::NeedleMeter> (editor, "Output peak level meter");
    REQUIRE (inputMeter != nullptr);
    REQUIRE (outputMeter != nullptr);

    // Both start parked at the silence floor rather than at 0 dBFS.
    CHECK (inputMeter->getSmoothedDb() == Catch::Approx (-100.0f));
    CHECK (outputMeter->getSmoothedDb() == Catch::Approx (-100.0f));

    // -12 dBFS sine (0.251 linear).
    constexpr float peakAmplitude = 0.25118864f;
    renderSine (processor, peakAmplitude);

    const auto tapPeakDb = juce::Decibels::gainToDecibels (
        processor.getMeterTaps().inputPeakLeft.load (std::memory_order_relaxed), -100.0f);

    INFO ("MeterTaps input peak = " << tapPeakDb << " dBFS");
    CHECK (tapPeakDb == Catch::Approx (-12.0f).margin (0.2f));

    // One timer pump per frame for a second of GUI time.
    constexpr float dt = 1.0f / 30.0f;

    for (int i = 0; i < 30; ++i)
        editor.updateMetersFromProcessor (dt);

    INFO ("input needle = " << inputMeter->getSmoothedDb() << " dBFS");
    CHECK (inputMeter->getSmoothedDb() == Catch::Approx (tapPeakDb).margin (0.1f));

    // The output needle reads the post-processing peak - a different value
    // (the default patch is not unity), but a real, finite one well above
    // the floor rather than a stuck needle.
    INFO ("output needle = " << outputMeter->getSmoothedDb() << " dBFS");
    CHECK (std::isfinite (outputMeter->getSmoothedDb()));
    CHECK (outputMeter->getSmoothedDb() > -60.0f);
    CHECK (outputMeter->getSmoothedDb() < 6.0f);

    // Silence lets both needles fall back towards the floor (slow release:
    // three seconds of GUI time at tau = 0.45 s gets most of the way).
    renderSilence (processor);

    for (int i = 0; i < 90; ++i)
        editor.updateMetersFromProcessor (dt);

    CHECK (inputMeter->getSmoothedDb() < -60.0f);
    CHECK (outputMeter->getSmoothedDb() < -60.0f);
}

TEST_CASE ("The gate needle follows the real gate gain-reduction tap", "[gui][meter]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    const auto setParam = [&processor] (const char* id, float value)
    {
        auto* parameter = processor.apvts.getParameter (id);
        REQUIRE (parameter != nullptr);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };

    // Gate on with a high threshold, then a signal well below it: the gate
    // clamps down hard and the needle must follow.
    setParam ("gateEnabled", 1.0f);
    setParam ("gateThreshold", -12.0f);

    CryptaAudioProcessorEditor editor (processor);

    auto* gateMeter = findDescendantByTitle<basilica::gui::NeedleMeter> (editor, "Gate gain reduction meter");
    REQUIRE (gateMeter != nullptr);

    // GR needles rest at 0 dB (no reduction).
    CHECK (gateMeter->getSmoothedDb() == Catch::Approx (0.0f));

    renderSine (processor, 0.05f, 40);

    const auto gateTapDb = processor.getMeterTaps().gateGainReductionDb.load (std::memory_order_relaxed);
    INFO ("gate GR tap = " << gateTapDb << " dB");
    CHECK (gateTapDb > 1.0f);

    constexpr float dt = 1.0f / 30.0f;

    for (int i = 0; i < 60; ++i)
        editor.updateMetersFromProcessor (dt);

    CHECK (gateMeter->getSmoothedDb() == Catch::Approx (gateTapDb).margin (0.1f));

    // The needle really moved: deeper reduction means a smaller angle than
    // the 0 dB rest position.
    CHECK (basilica::gui::NeedleMeter::angleDegreesForDb (
               basilica::gui::NeedleMeter::Scale::gainReductionDb, gateMeter->getSmoothedDb())
           < basilica::gui::NeedleMeter::angleDegreesForDb (
               basilica::gui::NeedleMeter::Scale::gainReductionDb, 0.0f));
}

TEST_CASE ("The low-band compressor needle follows the real compressor gain-reduction tap", "[gui][meter]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    const auto setParam = [&processor] (const char* id, float value)
    {
        auto* parameter = processor.apvts.getParameter (id);
        REQUIRE (parameter != nullptr);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };

    // Gate stays off (its default) so the signal actually reaches the low
    // band, and the compressor is driven well past its threshold.
    setParam ("lowCompThreshold", -48.0f);
    setParam ("lowCompRatio", 20.0f);

    CryptaAudioProcessorEditor editor (processor);

    auto* compMeter = findDescendantByTitle<basilica::gui::NeedleMeter>
        (editor, "Low band compressor gain reduction meter");
    REQUIRE (compMeter != nullptr);

    CHECK (compMeter->getSmoothedDb() == Catch::Approx (0.0f));

    // 60 Hz: below the 120 Hz default Split Low, so it lands in the Low
    // band the compressor actually processes (a 220 Hz tone would sit in
    // the Mid band and leave this needle legitimately at rest).
    renderSine (processor, 0.5f, 40, 60.0);

    const auto compTapDb = processor.getMeterTaps().lowCompGainReductionDb.load (std::memory_order_relaxed);
    INFO ("comp GR tap = " << compTapDb << " dB");
    CHECK (compTapDb > 0.5f);

    constexpr float dt = 1.0f / 30.0f;

    for (int i = 0; i < 60; ++i)
        editor.updateMetersFromProcessor (dt);

    CHECK (compMeter->getSmoothedDb() == Catch::Approx (compTapDb).margin (0.1f));

    CHECK (basilica::gui::NeedleMeter::angleDegreesForDb (
               basilica::gui::NeedleMeter::Scale::gainReductionDb, compMeter->getSmoothedDb())
           < basilica::gui::NeedleMeter::angleDegreesForDb (
               basilica::gui::NeedleMeter::Scale::gainReductionDb, 0.0f));
}
