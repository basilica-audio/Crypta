#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "dsp/MeterTaps.h"
#include "params/ParameterIds.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

// The metering backend (brief §6 T17, closes issue #13).
namespace
{
    constexpr double meterSampleRate = 48000.0;
}

TEST_CASE ("T17: the meter struct is lock-free", "[meters]")
{
    // A meter that could block would let the UI stall the audio thread, which
    // is the entire reason this is a plain struct of atomics and not a queue.
    // MeterTaps carries the same static_assert; this states it as a test so
    // the guarantee appears in the suite's output too.
    STATIC_REQUIRE (std::atomic<float>::is_always_lock_free);
}

TEST_CASE ("T17: input and output peak taps match a known signal", "[meters]")
{
    CryptaAudioProcessor processor;
    processor.setPlayConfigDetails (2, 2, meterSampleRate, 512);
    processor.prepareToPlay (meterSampleRate, 512);

    // Unity through the plugin: no drive, no gate, no EQ, no clip.
    TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
    TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 0.0f);
    TestHelpers::setParameter (processor, ParamIDs::irEnabled, 0.0f);
    TestHelpers::setParameter (processor, ParamIDs::outputClip, 0.0f);
    TestHelpers::setParameter (processor, ParamIDs::midDrive, 0.0f);
    TestHelpers::setParameter (processor, ParamIDs::highDrive, 0.0f);
    TestHelpers::setParameter (processor, ParamIDs::lowCompMix, 0.0f); // compressor out of the way
    // Pin the output trim: the startup Default preset carries -2.8 dB since
    // the issue #34 item 1 clipping fix, and this case asserts the output
    // tap against the raw input level of a unity chain.
    TestHelpers::setParameter (processor, ParamIDs::outputGain, 0.0f);
    processor.reset();

    constexpr float amplitude = 0.5f;

    juce::AudioBuffer<float> buffer (2, 4096);
    TestHelpers::fillWithSine (buffer, meterSampleRate, 220.0, amplitude);
    TestHelpers::renderThrough (processor, buffer);

    const auto& taps = processor.getMeterTaps();

    const auto inputPeakDb = juce::Decibels::gainToDecibels (
        taps.inputPeakLeft.load (std::memory_order_relaxed), -200.0f);
    const auto expectedDb = juce::Decibels::gainToDecibels (amplitude, -200.0f);

    INFO ("input peak tap " << inputPeakDb << " dBFS, expected " << expectedDb << " dBFS");
    CHECK (inputPeakDb == Catch::Approx (expectedDb).margin (0.1f));

    // The output tap should agree, since the chain is set to unity.
    const auto outputPeakDb = juce::Decibels::gainToDecibels (
        taps.outputPeakLeft.load (std::memory_order_relaxed), -200.0f);

    INFO ("output peak tap " << outputPeakDb << " dBFS");
    CHECK (outputPeakDb == Catch::Approx (expectedDb).margin (0.5f));

    // Both channels are fed, so both slots must be live - a copy-paste slip
    // that wrote the left value twice would otherwise go unnoticed.
    CHECK (taps.inputPeakRight.load (std::memory_order_relaxed) > 0.0f);
    CHECK (taps.outputPeakRight.load (std::memory_order_relaxed) > 0.0f);
}

TEST_CASE ("T17: the gate GR tap tracks the gate's actual reduction", "[meters]")
{
    CryptaAudioProcessor processor;
    processor.setPlayConfigDetails (1, 1, meterSampleRate, 512);
    processor.prepareToPlay (meterSampleRate, 512);

    TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 1.0f);
    TestHelpers::setParameter (processor, ParamIDs::gateMode, 1.0f); // Modern
    TestHelpers::setParameter (processor, ParamIDs::gateThreshold, -30.0f);
    TestHelpers::setParameter (processor, ParamIDs::gateRange, 40.0f);
    TestHelpers::setParameter (processor, ParamIDs::gateRelease, 20.0f);
    TestHelpers::setParameter (processor, ParamIDs::gateHold, 0.0f);
    TestHelpers::setParameter (processor, ParamIDs::gateScHpf, 20.0f);
    TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 0.0f);
    processor.reset();

    const auto& taps = processor.getMeterTaps();

    SECTION ("a closed gate reports the full range")
    {
        juce::AudioBuffer<float> buffer (1, 24000);
        TestHelpers::fillWithSine (buffer, meterSampleRate, 500.0, 0.0005f); // well under threshold
        TestHelpers::renderThrough (processor, buffer);

        const auto reductionDb = taps.gateGainReductionDb.load (std::memory_order_relaxed);
        INFO ("gate GR tap with the gate shut: " << reductionDb << " dB");
        CHECK (reductionDb == Catch::Approx (40.0f).margin (1.0f));
    }

    SECTION ("an open gate reports none")
    {
        juce::AudioBuffer<float> buffer (1, 24000);
        TestHelpers::fillWithSine (buffer, meterSampleRate, 500.0, 0.5f);
        TestHelpers::renderThrough (processor, buffer);

        const auto reductionDb = taps.gateGainReductionDb.load (std::memory_order_relaxed);
        INFO ("gate GR tap with the gate open: " << reductionDb << " dB");
        CHECK (reductionDb < 1.0f);
    }
}

TEST_CASE ("T17: the low-band compressor GR tap tracks its reduction", "[meters]")
{
    const auto reductionFor = [] (float thresholdDb)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (1, 1, meterSampleRate, 512);
        processor.prepareToPlay (meterSampleRate, 512);

        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowCompDetector, 1.0f); // Smooth RMS
        TestHelpers::setParameter (processor, ParamIDs::lowCompThreshold, thresholdDb);
        TestHelpers::setParameter (processor, ParamIDs::lowCompRatio, 8.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowCompKnee, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::splitLowHz, 400.0f); // put the tone in the low band
        processor.reset();

        juce::AudioBuffer<float> buffer (1, 48000);
        TestHelpers::fillWithSine (buffer, meterSampleRate, 100.0, 0.5f);
        TestHelpers::renderThrough (processor, buffer);

        return processor.getMeterTaps().lowCompGainReductionDb.load (std::memory_order_relaxed);
    };

    // A high threshold: nothing to compress.
    const auto idle = reductionFor (-3.0f);

    // A low threshold: plenty.
    const auto working = reductionFor (-36.0f);

    INFO ("low comp GR tap: idle " << idle << " dB, working " << working << " dB");

    CHECK (idle < 1.0f);
    CHECK (working > 6.0f);
    CHECK (working > idle + 5.0f);
}

TEST_CASE ("T17: per-band level taps respond to their own band", "[meters]")
{
    // Each band tap must follow its own band and not simply mirror the input.
    const auto bandLevels = [] (double toneHz)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (1, 1, meterSampleRate, 512);
        processor.prepareToPlay (meterSampleRate, 512);

        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::irEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::midDrive, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::highDrive, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::splitLowHz, 150.0f);
        TestHelpers::setParameter (processor, ParamIDs::splitHighHz, 800.0f);
        processor.reset();

        // A full second, so the ~300 ms meter smoothing settles.
        juce::AudioBuffer<float> buffer (1, static_cast<int> (meterSampleRate));
        TestHelpers::fillWithSine (buffer, meterSampleRate, toneHz, 0.5f);
        TestHelpers::renderThrough (processor, buffer);

        const auto& taps = processor.getMeterTaps();

        struct Levels
        {
            float low;
            float mid;
            float high;
        };

        return Levels { taps.lowBandLevel.load (std::memory_order_relaxed),
                         taps.midBandLevel.load (std::memory_order_relaxed),
                         taps.highBandLevel.load (std::memory_order_relaxed) };
    };

    const auto lowTone = bandLevels (50.0);    // below splitLowHz
    const auto highTone = bandLevels (4000.0); // above splitHighHz

    INFO ("50 Hz -> low " << lowTone.low << ", mid " << lowTone.mid << ", high " << lowTone.high);
    INFO ("4 kHz -> low " << highTone.low << ", mid " << highTone.mid << ", high " << highTone.high);

    // A 50 Hz tone lands in the low band.
    CHECK (lowTone.low > lowTone.high);

    // A 4 kHz tone lands in the high band.
    CHECK (highTone.high > highTone.low);

    // And the taps genuinely moved with the signal rather than sitting at a
    // constant.
    CHECK (lowTone.low > highTone.low);
    CHECK (highTone.high > lowTone.high);
}

TEST_CASE ("T17: meters are reset alongside the DSP", "[meters]")
{
    // A stale meter after a transport stop would show level that is no longer
    // there, so reset() has to clear them with everything else.
    CryptaAudioProcessor processor;
    processor.setPlayConfigDetails (1, 1, meterSampleRate, 512);
    processor.prepareToPlay (meterSampleRate, 512);
    TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);

    juce::AudioBuffer<float> buffer (1, 8192);
    TestHelpers::fillWithSine (buffer, meterSampleRate, 220.0, 0.7f);
    TestHelpers::renderThrough (processor, buffer);

    REQUIRE (processor.getMeterTaps().inputPeakLeft.load (std::memory_order_relaxed) > 0.1f);

    processor.reset();

    CHECK (processor.getMeterTaps().inputPeakLeft.load (std::memory_order_relaxed) == 0.0f);
    CHECK (processor.getMeterTaps().outputPeakLeft.load (std::memory_order_relaxed) == 0.0f);
    CHECK (processor.getMeterTaps().lowBandLevel.load (std::memory_order_relaxed) == 0.0f);
}

TEST_CASE ("Issue #12: getLowBandGainReductionDb() is the GUI-facing view of the same tap, on both detector engines", "[meters][compressor]")
{
    // The accessor a GR needle/meter polls on its timer. Two things have to
    // hold for that to be safe and useful: it must return exactly what the
    // lock-free tap holds (no second source of truth for the same number), and
    // it must be alive on BOTH detector engines - including Classic Peak, which
    // every migrated pre-v0.3.0 session runs on and which reported a flat zero
    // before this release.
    const auto measure = [] (float detectorIndex, float thresholdDb)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (1, 1, meterSampleRate, 512);
        processor.prepareToPlay (meterSampleRate, 512);

        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowCompDetector, detectorIndex);
        TestHelpers::setParameter (processor, ParamIDs::lowCompThreshold, thresholdDb);
        TestHelpers::setParameter (processor, ParamIDs::lowCompRatio, 8.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowCompKnee, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::splitLowHz, 400.0f); // put the tone in the low band
        processor.reset();

        juce::AudioBuffer<float> buffer (1, 48000);
        TestHelpers::fillWithSine (buffer, meterSampleRate, 100.0, 0.5f);
        TestHelpers::renderThrough (processor, buffer);

        struct Reading
        {
            float accessor;
            float tap;
        };

        return Reading { processor.getLowBandGainReductionDb(),
                          processor.getMeterTaps().lowCompGainReductionDb.load (std::memory_order_relaxed) };
    };

    for (const auto detectorIndex : { 0.0f, 1.0f }) // Classic Peak, Smooth RMS
    {
        const auto idle = measure (detectorIndex, -3.0f);
        const auto working = measure (detectorIndex, -36.0f);

        INFO ("detector index " << detectorIndex << ": idle " << idle.accessor
                                 << " dB, working " << working.accessor << " dB");

        // One number, two views of it.
        CHECK (idle.accessor == idle.tap);
        CHECK (working.accessor == working.tap);

        // Positive dB of reduction, and it responds to the threshold.
        CHECK (idle.accessor < 1.0f);
        CHECK (working.accessor > 6.0f);
        CHECK (working.accessor > idle.accessor + 5.0f);
    }
}
