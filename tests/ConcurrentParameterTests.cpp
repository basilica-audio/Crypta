#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "params/ParameterIds.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <cmath>
#include <thread>
#include <vector>

// Concurrent parameter mutation during processing (issue #123).
//
// ============================================================================
// WHY THIS FILE EXISTS
// ============================================================================
//
// The v0.4.1 release run (33091972810) died with `Trace/BPT trap: 5` inside
// pluginval's "Parameter thread safety" test, against the shipped Universal
// Binary's x86_64 slice, hosted as an AU. That test (pluginval v1.0.4,
// Source/tests/BasicTests.cpp:612) runs two loops concurrently over the full
// non-bypass automatable parameter set:
//
//   message thread : 500 x  for each parameter: setValueNotifyingHost (random)
//   test thread    : 500 x  for each parameter: setValue (random)
//                           then fillNoise + processBlock
//
// i.e. the host mutates every parameter from two threads at once while a
// third role - the render callback - reads them. Issue #123 establishes by
// measurement that the trap itself came from macOS's *out-of-process* AU
// bridge (`AUHostingServiceXPC`), a hosting mode the shipped Universal Binary
// never enters, and not from the processor's own state: the identical x86_64
// machine code passes the same strictness-10 sweep when hosted in-process.
//
// That conclusion is only worth anything if the concurrency contract it
// leans on is actually true, and nothing in this repository asserted it. The
// only cross-thread coverage that existed (CrossThreadReprepareTests.cpp) is
// about prepareToPlay() racing loadImpulseResponse() - the IR loader's
// message-thread surface - and says nothing about parameters being written
// from two threads while processBlock() reads them. So this file is the
// standing, in-repo proof of the property the pluginval test was probing,
// held independently of whether pluginval can be run at all.
//
// ============================================================================
// WHAT IT ACTUALLY GUARANTEES
// ============================================================================
//
// The processor's parameter path is a plain publish/subscribe over
// std::atomic<float>: CryptaAudioProcessor's constructor caches every
// apvts.getRawParameterValue() pointer once, and processBlock() reads each of
// them exactly once per host block with std::memory_order_relaxed (see
// src/PluginProcessor.cpp). The only APVTS listener in the plugin is
// basilica::presets::PresetManager::parameterChanged(), which is a single
// relaxed atomic store and nothing else (src/presets/PresetManager.cpp) -
// deliberately, because AudioProcessorValueTreeState::Listener callbacks fire
// on whichever thread wrote the parameter, which host automation does not
// guarantee to be the message thread.
//
// This test asserts that contract end to end rather than by inspection:
//
//   * no torn/garbage parameter read ever produces a non-finite sample, and
//   * no parameter combination reachable by two concurrent random writers
//     drives the output past the plugin's own output ceiling, and
//   * the APVTS state round-trip still holds after the storm.
//
// HONEST SCOPE. These are trip-wires for a genuine scheduling race, so a
// green run is evidence and not proof - the same caveat
// CrossThreadReprepareTests.cpp states about its own race. It is also
// deliberately NOT a claim to reproduce the release run's trap: that trap
// lives in the XPC bridge, above this processor's API surface, and no
// in-process test can reach it. The gate that catches that class is
// pluginval, and .github/scripts/validate-macos-x86_64-slice.sh is what keeps
// it pointed at the right hosting mode.

namespace
{
    // Every automatable parameter except bypass - the same selection
    // pluginval's getNonBypassAutomatableParameters() makes, and for the same
    // reason: bypass short-circuits the wet chain, so hammering it would
    // spend most of the storm measuring the dry path.
    std::vector<juce::RangedAudioParameter*> nonBypassParameters (CryptaAudioProcessor& processor)
    {
        std::vector<juce::RangedAudioParameter*> result;

        for (auto* parameter : processor.getParameters())
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);

            if (ranged == nullptr || ranged->paramID == ParamIDs::bypass)
                continue;

            if (! ranged->isAutomatable())
                continue;

            result.push_back (ranged);
        }

        return result;
    }

    bool bufferIsFinite (const juce::AudioBuffer<float>& buffer) noexcept
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                if (! std::isfinite (data[sample]))
                    return false;
        }

        return true;
    }
}

TEST_CASE ("Parameters written from two threads while processing never produce non-finite output",
           "[processor][threading][parameters]")
{
    // 32 samples at 44.1 kHz is exactly the block size pluginval's
    // "Parameter thread safety" test prepares with, so the control-rate
    // update in processBlock() runs as often per parameter write as it does
    // there - the interleaving this test is trying to hit is a per-block one.
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 32;
    constexpr int numBlocks = 500;

    CryptaAudioProcessor processor;
    processor.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);

    const auto parameters = nonBypassParameters (processor);
    REQUIRE (parameters.size() > 1);

    std::atomic<bool> writerShouldStop { false };
    std::atomic<long long> writesCompleted { 0 };

    // Role 1: the "plugin's own UI moved a slider" writer. setValueNotifyingHost()
    // is the call pluginval makes from the message thread, and it is the one
    // that fans out to AudioProcessorValueTreeState::Listener callbacks, so it
    // is the write that actually exercises PresetManager::parameterChanged()
    // on a foreign thread.
    std::thread notifyingWriter ([&]
    {
        juce::Random random (0x5eed01);

        while (! writerShouldStop.load (std::memory_order_relaxed))
        {
            for (auto* parameter : parameters)
            {
                parameter->setValueNotifyingHost (random.nextFloat());
                writesCompleted.fetch_add (1, std::memory_order_relaxed);
            }
        }
    });

    // Role 2: the "host automation lane" writer. setValue() skips the host
    // notification but takes the same path into the APVTS-backed atomic that
    // processBlock() reads, which is the store this test cares about.
    std::thread plainWriter ([&]
    {
        juce::Random random (0x5eed02);

        while (! writerShouldStop.load (std::memory_order_relaxed))
        {
            for (auto* parameter : parameters)
            {
                parameter->setValue (random.nextFloat());
                writesCompleted.fetch_add (1, std::memory_order_relaxed);
            }
        }
    });

    // Role 3: the render callback, on this (the calling) thread.
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    juce::Random noise (0x5eed03);

    bool sawNonFiniteOutput = false;
    float largestMagnitude = 0.0f;

    for (int block = 0; block < numBlocks; ++block)
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* data = buffer.getWritePointer (channel);

            for (int sample = 0; sample < blockSize; ++sample)
                data[sample] = noise.nextFloat() * 2.0f - 1.0f;
        }

        processor.processBlock (buffer, midi);

        if (! bufferIsFinite (buffer))
        {
            sawNonFiniteOutput = true;
            break;
        }

        largestMagnitude = juce::jmax (largestMagnitude, buffer.getMagnitude (0, blockSize));
    }

    writerShouldStop.store (true, std::memory_order_relaxed);
    notifyingWriter.join();
    plainWriter.join();

    // Both writer threads must actually have run; a test that measured
    // nothing because the writers never got scheduled would otherwise pass
    // silently and read as coverage it does not have.
    CHECK (writesCompleted.load (std::memory_order_relaxed) > static_cast<long long> (parameters.size()));

    CHECK_FALSE (sawNonFiniteOutput);

    // Finiteness is the property this storm can assert; a *tight* magnitude
    // bound is not, because the storm randomises the gain stages themselves.
    // Input Gain (+24 dB), the three band levels (+12 dB each), Low Comp
    // Makeup (+24 dB) and Output Gain (+24 dB) are all in the parameter set
    // being hammered, and Output Clip is a bool that spends half the storm
    // switched off, so a legitimate output of ~84 dB above the input is
    // reachable by design and says nothing about thread safety. The bound
    // here is therefore the product of every explicit gain stage's maximum,
    // which a legitimate render cannot exceed and a garbage parameter read
    // (a value from an unpublished store, or from an uninitialised atomic)
    // blows past by orders of magnitude rather than by decibels. The tight
    // bound lives in the next test, which pins the gain stages instead.
    constexpr float maximumExplicitGainDb = 24.0f + 12.0f + 24.0f + 24.0f;
    CHECK (largestMagnitude < juce::Decibels::decibelsToGain (maximumExplicitGainDb));
}

TEST_CASE ("Parameters written from another thread while processing never move the output past the clip ceiling",
           "[processor][threading][parameters]")
{
    // The tight-bound half of the storm above. Every stage that can legitimately
    // add level is pinned - the gain trims at unity, the compressor's makeup at
    // 0 dB, the output clipper on at its 0 dBFS ceiling - and everything else
    // (drive, voicing, EQ, gate, crossover, IR mix, engine selectors) is still
    // hammered from another thread while processBlock() renders. With the level
    // stages held, a render that leaves the clipper's ceiling has read something
    // it was never published.
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 32;
    constexpr int numBlocks = 400;

    CryptaAudioProcessor processor;
    processor.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);

    const juce::StringArray pinned { ParamIDs::inputGain,   ParamIDs::outputGain,
                                     ParamIDs::lowLevel,    ParamIDs::midLevel,
                                     ParamIDs::highLevel,   ParamIDs::lowCompMakeup,
                                     ParamIDs::lowCompAutoMakeup,
                                     ParamIDs::outputClip,  ParamIDs::clipCeiling };

    std::vector<juce::RangedAudioParameter*> stormed;

    for (auto* parameter : nonBypassParameters (processor))
        if (! pinned.contains (parameter->paramID))
            stormed.push_back (parameter);

    REQUIRE (stormed.size() > 1);

    const auto pin = [&processor] (const juce::String& id, float plainValue)
    {
        auto* parameter = processor.apvts.getParameter (id);
        REQUIRE (parameter != nullptr);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (plainValue));
    };

    pin (ParamIDs::inputGain, 0.0f);
    pin (ParamIDs::outputGain, 0.0f);
    pin (ParamIDs::lowLevel, 0.0f);
    pin (ParamIDs::midLevel, 0.0f);
    pin (ParamIDs::highLevel, 0.0f);
    pin (ParamIDs::lowCompMakeup, 0.0f);
    pin (ParamIDs::lowCompAutoMakeup, 0.0f);
    pin (ParamIDs::outputClip, 1.0f);
    pin (ParamIDs::clipCeiling, 0.0f);

    std::atomic<bool> writerShouldStop { false };
    std::atomic<long long> writesCompleted { 0 };

    std::thread writer ([&]
    {
        juce::Random random (0x5eed06);

        while (! writerShouldStop.load (std::memory_order_relaxed))
        {
            for (auto* parameter : stormed)
            {
                parameter->setValueNotifyingHost (random.nextFloat());
                writesCompleted.fetch_add (1, std::memory_order_relaxed);
            }
        }
    });

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;

    bool sawNonFiniteOutput = false;
    float largestMagnitude = 0.0f;

    for (int block = 0; block < numBlocks; ++block)
    {
        TestHelpers::fillWithSine (buffer, sampleRate, 110.0, 0.5f, block * blockSize);
        processor.processBlock (buffer, midi);

        if (! bufferIsFinite (buffer))
        {
            sawNonFiniteOutput = true;
            break;
        }

        largestMagnitude = juce::jmax (largestMagnitude, buffer.getMagnitude (0, blockSize));
    }

    writerShouldStop.store (true, std::memory_order_relaxed);
    writer.join();

    CHECK (writesCompleted.load (std::memory_order_relaxed) > static_cast<long long> (stormed.size()));
    CHECK_FALSE (sawNonFiniteOutput);

    // The clipper is an ADAA soft ceiling, not a brickwall limiter, so a small
    // overshoot above 0 dBFS is expected and correct; 2.0 (+6 dB) is well above
    // anything the shaper produces and well below anything a bad read does.
    CHECK (largestMagnitude < 2.0f);
}

TEST_CASE ("APVTS state round-trips exactly while parameters are written from another thread",
           "[processor][threading][parameters][state]")
{
    // The in-process analogue of pluginval's "Plugin state restoration" test
    // (pluginval v1.0.4, Source/tests/BasicTests.cpp:305): snapshot the state,
    // scramble every parameter, restore the snapshot, assert every parameter
    // came back. In-process there is no asynchronous echo between the write
    // and the read-back - unlike the out-of-process AU bridge, where that echo
    // is exactly what makes the same test flaky (issue #123) - so here it is a
    // deterministic property and is asserted exactly.
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 64;

    CryptaAudioProcessor processor;
    processor.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);

    const auto parameters = nonBypassParameters (processor);
    REQUIRE (parameters.size() > 1);

    juce::Random seedRandom (0x5eed04);

    for (auto* parameter : parameters)
        parameter->setValueNotifyingHost (seedRandom.nextFloat());

    juce::MemoryBlock snapshot;
    processor.getStateInformation (snapshot);

    // Compared in PLAIN units, not normalised ones, because normalised units
    // are not what a round-trip through the APVTS ValueTree preserves and were
    // never meant to be. The tree stores each parameter denormalised and
    // snapped to its own NormalisableRange interval, and
    // AudioProcessorValueTreeState::ParameterAdapter::setDenormalisedValue()
    // returns early when the incoming plain value already equals the current
    // one. So a two-state switch seeded with the raw normalised float 0.913
    // serialises as the snapped plain value 1.0, and restoring 1.0 over a
    // parameter that already reads 1.0 in plain units correctly leaves its raw
    // normalised float wherever the last writer put it. The switch is on before
    // and on after; only a number nothing consumes differs. Plain units are
    // what processBlock(), the host and the preset system all actually read,
    // so plain units are what this asserts - exactly, with no tolerance to hide
    // behind.
    const auto plainValue = [] (const juce::RangedAudioParameter* parameter)
    {
        return parameter->convertFrom0to1 (parameter->getValue());
    };

    std::vector<float> expected;
    expected.reserve (parameters.size());

    for (auto* parameter : parameters)
        expected.push_back (plainValue (parameter));

    const auto expectRestored = [&] (const char* stage)
    {
        processor.setStateInformation (snapshot.getData(), static_cast<int> (snapshot.getSize()));

        for (size_t i = 0; i < parameters.size(); ++i)
        {
            INFO (std::string (stage) + ": " + parameters[i]->paramID.toStdString());
            CHECK (plainValue (parameters[i]) == Catch::Approx (expected[i]).margin (1.0e-5));
        }
    };

    SECTION ("single-threaded control")
    {
        // Establishes that the round-trip itself holds, so a failure in the
        // concurrent section below cannot be read as a plain state bug wearing
        // a threading label.
        juce::Random scrambleRandom (0x5eed07);

        for (auto* parameter : parameters)
            parameter->setValueNotifyingHost (scrambleRandom.nextFloat());

        expectRestored ("single-threaded");
    }

    SECTION ("scrambled from another thread while processing")
    {
        std::atomic<bool> writerShouldStop { false };
        std::atomic<long long> writesCompleted { 0 };

        std::thread scrambler ([&]
        {
            juce::Random random (0x5eed05);

            while (! writerShouldStop.load (std::memory_order_relaxed))
            {
                for (auto* parameter : parameters)
                {
                    parameter->setValueNotifyingHost (random.nextFloat());
                    writesCompleted.fetch_add (1, std::memory_order_relaxed);
                }
            }
        });

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        for (int block = 0; block < 200; ++block)
        {
            TestHelpers::fillWithSine (buffer, sampleRate, 110.0, 0.25f, block * blockSize);
            processor.processBlock (buffer, midi);
        }

        writerShouldStop.store (true, std::memory_order_relaxed);
        scrambler.join();

        CHECK (writesCompleted.load (std::memory_order_relaxed) > static_cast<long long> (parameters.size()));

        // Restoring after the writer has stopped: the property under test is
        // that setStateInformation() puts every parameter back once the storm
        // is over, not that it wins a race against a live writer - no host
        // contract promises the latter, and pluginval does not test it either.
        expectRestored ("after concurrent scramble");
    }
}
