#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "params/ParameterIds.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <vector>

#if JUCE_MAC || JUCE_LINUX || JUCE_BSD
 #include <cstdlib>
#endif

// CPU cost under load, measured rather than guessed (issue #34's QA checklist,
// "CPU under load").
//
// HIDDEN BY DEFAULT. Every case here is tagged "[.cpu]" - the leading dot makes
// Catch2 skip it unless it is asked for by name or tag - because a wall-clock
// measurement is not a correctness assertion and has no business failing a CI
// job on a noisy shared runner. Run it deliberately:
//
//     ./Tests "[.cpu]" --success
//
// The number reported is the REALTIME FACTOR: how many seconds of audio the
// processor renders per second of CPU time on one core. 100x means the plugin
// costs 1% of one core at that configuration; a host can run roughly that many
// instances before the audio thread misses its deadline (minus the host's own
// overhead and minus whatever headroom the buffer size buys).
//
// NOTHING HERE ASSERTS ON TIME. The only assertion is that the output stayed
// finite; the cost is reported and left to a human to read.
//
// That is a deliberate reversal. The first version of this file asserted a
// floor of 5x realtime, on the reasoning that it would catch "this could not
// possibly ship" without being a regression gate. It then failed on the machine
// that wrote it, purely because something unrelated had the load average at 40 -
// and the shape of the results gave it away, with 192 kHz measuring faster than
// 88.2 kHz and 16-sample blocks faster than 256-sample ones. A wall-clock
// assertion on shared hardware measures the hardware's mood, not the plugin.
//
// So the run prints the machine's load average next to the figures. If it is
// not near zero, the numbers are contended and should be thrown away rather
// than reasoned about.
namespace
{
    struct LoadResult
    {
        double realtimeFactor = 0.0;
        double secondsRendered = 0.0;
        bool finite = true;
        float peak = 0.0f;
    };

    // Renders `secondsToRender` of continuous audio through a fully configured
    // processor and times it. The signal is a bass-register sine at a level
    // that keeps the dynamics stages working rather than idling, because a
    // compressor below its threshold and a gate holding closed are the cheap
    // cases, not the ones worth measuring.
    LoadResult measureLoad (double sampleRate,
                            int blockSize,
                            double secondsToRender,
                            const std::function<void (CryptaAudioProcessor&)>& configure)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, sampleRate, blockSize);
        processor.prepareToPlay (sampleRate, blockSize);

        configure (processor);

        const auto totalSamples = static_cast<int> (sampleRate * secondsToRender);
        const auto numBlocks = juce::jmax (1, totalSamples / blockSize);

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        // One untimed pass so any first-call lazy setup, page faults and cache
        // cold-start land outside the measurement.
        TestHelpers::fillWithSine (buffer, sampleRate, 55.0, 0.5f, 0);
        processor.processBlock (buffer, midi);

        LoadResult result;
        const auto start = std::chrono::steady_clock::now();

        for (int block = 0; block < numBlocks; ++block)
        {
            TestHelpers::fillWithSine (buffer, sampleRate, 55.0, 0.5f, block * blockSize);
            processor.processBlock (buffer, midi);
        }

        const auto elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();

        result.secondsRendered = (numBlocks * blockSize) / sampleRate;
        result.realtimeFactor = elapsed > 0.0 ? result.secondsRendered / elapsed : 0.0;
        result.finite = TestHelpers::allSamplesFinite (buffer);
        result.peak = buffer.getMagnitude (0, buffer.getNumSamples());

        return result;
    }

    void configureEverythingOn (CryptaAudioProcessor& processor)
    {
        // Every optional stage engaged: gate, both drive engines' bands, EQ,
        // growl, cab. This is the expensive corner, not the default patch.
        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::midDrive, 70.0f);
        TestHelpers::setParameter (processor, ParamIDs::highDrive, 70.0f);
        TestHelpers::setParameter (processor, ParamIDs::irEnabled, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::irMix, 100.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowGrowl, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowGrowlAmount, 80.0f);

        // A bundled cabinet, not an empty cab slot: irEnabled with no IR
        // loaded is a passthrough, which would have measured the cheap case
        // while claiming to measure the expensive one.
        processor.loadFactoryImpulseResponse (0);
    }

    // One-minute load average, so a reader can tell at a glance whether the
    // figures below are worth anything.
    double machineLoadAverage()
    {
       #if JUCE_MAC || JUCE_LINUX || JUCE_BSD
        double loads[3] = { 0.0, 0.0, 0.0 };

        if (getloadavg (loads, 3) > 0)
            return loads[0];
       #endif

        return -1.0;
    }

    void report (const juce::String& label, const LoadResult& result)
    {
        WARN (label << ": " << juce::String (result.realtimeFactor, 1)
                    << "x realtime (" << juce::String (100.0 / juce::jmax (1.0e-9, result.realtimeFactor), 2)
                    << "% of one core), " << juce::String (result.secondsRendered, 2)
                    << " s rendered, load average " << juce::String (machineLoadAverage(), 2));
    }
}

TEST_CASE ("CPU: cost across the supported sample rates", "[.cpu][performance]")
{
    for (const auto sampleRate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        const auto result = measureLoad (sampleRate, 512, 5.0, configureEverythingOn);

        report ("everything engaged @ " + juce::String (sampleRate, 0) + " Hz / 512", result);

        CHECK (result.finite);
    }
}

TEST_CASE ("CPU: cost across the block sizes a host might pick", "[.cpu][performance]")
{
    // Small buffers are where a plugin's per-block fixed cost shows up, and
    // where a host actually runs out of headroom.
    for (const auto blockSize : { 16, 32, 64, 128, 256, 512, 1024, 2048 })
    {
        const auto result = measureLoad (48000.0, blockSize, 5.0, configureEverythingOn);

        report ("everything engaged @ 48 kHz / " + juce::String (blockSize), result);

        CHECK (result.finite);
    }
}

TEST_CASE ("CPU: what each optional stage costs", "[.cpu][performance]")
{
    // The point of breaking it down: a user deciding whether they can afford
    // another instance wants to know which switch is the expensive one, and a
    // maintainer wants to know before a stage doubles in cost unnoticed.
    struct Configuration
    {
        const char* label;
        std::function<void (CryptaAudioProcessor&)> configure;
    };

    const std::vector<Configuration> configurations {
        { "defaults (nothing forced)", [] (CryptaAudioProcessor&) {} },
        { "gate only", [] (CryptaAudioProcessor& p)
          {
              TestHelpers::setParameter (p, ParamIDs::gateEnabled, 1.0f);
          } },
        { "drive only", [] (CryptaAudioProcessor& p)
          {
              TestHelpers::setParameter (p, ParamIDs::midDrive, 70.0f);
              TestHelpers::setParameter (p, ParamIDs::highDrive, 70.0f);
          } },
        { "cab IR only", [] (CryptaAudioProcessor& p)
          {
              TestHelpers::setParameter (p, ParamIDs::irEnabled, 1.0f);
              TestHelpers::setParameter (p, ParamIDs::irMix, 100.0f);
              p.loadFactoryImpulseResponse (0);
          } },
        { "everything engaged", configureEverythingOn },
    };

    for (const auto& configuration : configurations)
    {
        const auto result = measureLoad (48000.0, 512, 5.0, configuration.configure);

        report (juce::String (configuration.label) + " @ 48 kHz / 512", result);

        CHECK (result.finite);
    }
}

TEST_CASE ("CPU: a bundled factory IR does not change the cost class", "[.cpu][performance][factory]")
{
    // The bundled IRs are 4096 taps (issue #81). A partitioned convolution is
    // O(log N) per sample in the partition count rather than O(N), so a longer
    // IR should cost noticeably less than proportionally more - but "should"
    // is not a measurement, and 4096 taps is four times the length the slot
    // mechanics were originally sized for.
    const auto withoutIr = measureLoad (48000.0, 512, 5.0, [] (CryptaAudioProcessor& p)
    {
        TestHelpers::setParameter (p, ParamIDs::irEnabled, 0.0f);
    });

    const auto withIr = measureLoad (48000.0, 512, 5.0, [] (CryptaAudioProcessor& p)
    {
        TestHelpers::setParameter (p, ParamIDs::irEnabled, 1.0f);
        TestHelpers::setParameter (p, ParamIDs::irMix, 100.0f);
        p.loadFactoryImpulseResponse (0);
    });

    report ("cab bypassed  @ 48 kHz / 512", withoutIr);
    report ("factory IR 0  @ 48 kHz / 512", withIr);

    WARN ("cost ratio (bypassed / engaged realtime factor): "
          << juce::String (withoutIr.realtimeFactor / juce::jmax (1.0e-9, withIr.realtimeFactor), 2) << "x");

    CHECK (withIr.finite);
    CHECK (withoutIr.finite);
}
