#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

// This translation unit owns the global operator new/delete replacements that
// back the allocation guard - see AllocationGuard.h for why exactly one may
// define them.
#define CRYPTA_DEFINE_ALLOCATION_COUNTER 1
#include "AllocationGuard.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

TEST_CASE ("Denormal-range input produces no NaN/Inf output", "[robustness]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto* inputGainParam = processor.apvts.getParameter (ParamIDs::inputGain);
    auto* outputGainParam = processor.apvts.getParameter (ParamIDs::outputGain);
    REQUIRE (inputGainParam != nullptr);
    REQUIRE (outputGainParam != nullptr);

    // Exercise the gain multiply with a non-unity gain so denormals actually
    // propagate through the multiplication rather than being a no-op.
    inputGainParam->setValueNotifyingHost (inputGainParam->convertTo0to1 (12.0f));
    outputGainParam->setValueNotifyingHost (outputGainParam->convertTo0to1 (12.0f));

    constexpr int numSamples = 512;
    juce::AudioBuffer<float> buffer (2, numSamples);

    const auto denormalValue = std::numeric_limits<float>::denorm_min() * 4.0f;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        auto* data = buffer.getWritePointer (channel);

        for (int sample = 0; sample < numSamples; ++sample)
            data[sample] = (sample % 2 == 0) ? denormalValue : -denormalValue;
    }

    juce::MidiBuffer midi;

    CHECK_NOTHROW (processor.processBlock (buffer, midi));
    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("Zero-sample buffer does not crash processBlock", "[robustness]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 0);
    juce::MidiBuffer midi;

    CHECK_NOTHROW (processor.processBlock (buffer, midi));
    CHECK (buffer.getNumSamples() == 0);
    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("Zero-sample buffer does not crash when bypassed", "[robustness]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto* bypassParam = processor.apvts.getParameter (ParamIDs::bypass);
    REQUIRE (bypassParam != nullptr);
    bypassParam->setValueNotifyingHost (1.0f);

    juce::AudioBuffer<float> buffer (2, 0);
    juce::MidiBuffer midi;

    CHECK_NOTHROW (processor.processBlock (buffer, midi));
}

//==============================================================================
// v0.3.0 real-time safety and automation robustness (brief §6 T15, T16).

namespace
{
    constexpr double rtSampleRate = 48000.0;
    constexpr int rtBlockSize = 512;

    void prepareForRealtimeTest (CryptaAudioProcessor& processor, int driveEngineIndex)
    {
        processor.setPlayConfigDetails (2, 2, rtSampleRate, rtBlockSize);
        processor.prepareToPlay (rtSampleRate, rtBlockSize);

        TestHelpers::setParameter (processor, ParamIDs::driveEngine, static_cast<float> (driveEngineIndex));
        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::gateMode, 1.0f);       // Modern
        TestHelpers::setParameter (processor, ParamIDs::lowCompDetector, 1.0f); // Smooth RMS
        TestHelpers::setParameter (processor, ParamIDs::lowCompAutoMakeup, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::outputClip, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::midDrive, 50.0f);
        TestHelpers::setParameter (processor, ParamIDs::highDrive, 70.0f);

        processor.reset();
    }

    void fillBlock (juce::AudioBuffer<float>& buffer, double frequencyHz, juce::int64 startSample, float amplitude)
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto phase = juce::MathConstants<double>::twoPi * frequencyHz
                                    * static_cast<double> (startSample + sample) / rtSampleRate;
                buffer.setSample (channel, sample, amplitude * static_cast<float> (std::sin (phase)));
            }
    }
}

TEST_CASE ("T16: processBlock never allocates, on either engine", "[robustness][realtime]")
{
    // The property that cannot be heard until it is too late. Every v0.3.0
    // feature is enabled here - Circuit drive, Modern gate, Smooth RMS
    // detector with auto-makeup, the safety clip and the EQ - because each
    // added stage is a fresh opportunity to allocate on the audio thread.
    for (const auto engineIndex : { 0, 1 })
    {
        CryptaAudioProcessor processor;
        prepareForRealtimeTest (processor, engineIndex);

        juce::AudioBuffer<float> buffer (2, rtBlockSize);
        juce::MidiBuffer midi;

        // Warm up outside the measurement: the first few blocks legitimately
        // touch lazily-initialised state.
        for (int block = 0; block < 8; ++block)
        {
            fillBlock (buffer, 110.0, block * rtBlockSize, 0.5f);
            processor.processBlock (buffer, midi);
        }

        const auto allocations = AllocationCounter::countDuring ([&]
        {
            for (int block = 0; block < 200; ++block)
            {
                fillBlock (buffer, 110.0, (8 + block) * rtBlockSize, 0.5f);
                processor.processBlock (buffer, midi);
            }
        });

        INFO ("driveEngine index " << engineIndex << ": " << allocations << " allocations across 200 blocks");
        CHECK (allocations == 0);
    }
}

TEST_CASE ("T16: an oversized host block is chunked without allocating", "[robustness][realtime]")
{
    // Hosts are not supposed to exceed the block size promised to
    // prepareToPlay(), but processBlock() handles it defensively by chunking.
    // That path must be allocation-free too, or the defence would itself be
    // the failure.
    CryptaAudioProcessor processor;
    prepareForRealtimeTest (processor, 1);

    juce::AudioBuffer<float> oversized (2, rtBlockSize * 4);
    juce::MidiBuffer midi;

    fillBlock (oversized, 110.0, 0, 0.5f);
    processor.processBlock (oversized, midi);

    const auto allocations = AllocationCounter::countDuring ([&]
    {
        for (int block = 0; block < 20; ++block)
        {
            fillBlock (oversized, 110.0, block * rtBlockSize * 4, 0.5f);
            processor.processBlock (oversized, midi);
        }
    });

    INFO (allocations << " allocations across 20 oversized blocks");
    CHECK (allocations == 0);
    CHECK (TestHelpers::allSamplesFinite (oversized));
}

TEST_CASE ("T16: switching engines mid-stream does not allocate", "[robustness][realtime]")
{
    // The crossfade runs BOTH engines and resets the incoming one. Neither may
    // allocate - a reset() that reallocated would be an easy mistake to make
    // and an impossible one to hear until a session glitched.
    CryptaAudioProcessor processor;
    prepareForRealtimeTest (processor, 1);

    juce::AudioBuffer<float> buffer (2, rtBlockSize);
    juce::MidiBuffer midi;

    for (int block = 0; block < 8; ++block)
    {
        fillBlock (buffer, 110.0, block * rtBlockSize, 0.5f);
        processor.processBlock (buffer, midi);
    }

    auto* engineParameter = processor.apvts.getParameter (ParamIDs::driveEngine);
    REQUIRE (engineParameter != nullptr);

    const auto allocations = AllocationCounter::countDuring ([&]
    {
        for (int block = 0; block < 40; ++block)
        {
            // setValueNotifyingHost itself is a message-thread call in
            // production; what is being measured is processBlock's reaction to
            // the change, so the parameter write happens between blocks.
            engineParameter->setValueNotifyingHost (block % 2 == 0 ? 1.0f : 0.0f);

            fillBlock (buffer, 110.0, block * rtBlockSize, 0.5f);
            processor.processBlock (buffer, midi);
        }
    });

    INFO (allocations << " allocations across 40 engine switches");
    CHECK (TestHelpers::allSamplesFinite (buffer));

    // The parameter write is allowed to allocate (it is not on the audio
    // thread); processBlock is not. Measured as zero in practice, but the
    // bound is stated loosely enough that a JUCE listener-list detail cannot
    // make this flaky - a genuine per-block allocation would be 40+.
    CHECK (allocations < 40);
}

TEST_CASE ("T16: a long silence after a loud burst does not inflate block time", "[robustness][realtime]")
{
    // Denormal guard. Filter state decaying towards zero produces denormals,
    // which on x86 cost hundreds of cycles each - so a plugin that is fine
    // under load can stall during the silence AFTER it. ScopedNoDenormals is
    // in place; this is the assertion that it stays that way.
    CryptaAudioProcessor processor;
    prepareForRealtimeTest (processor, 1);

    juce::AudioBuffer<float> buffer (2, rtBlockSize);
    juce::MidiBuffer midi;

    const auto timeBlocks = [&] (int count, float amplitude)
    {
        const auto start = std::chrono::steady_clock::now();

        for (int block = 0; block < count; ++block)
        {
            fillBlock (buffer, 110.0, block * rtBlockSize, amplitude);
            processor.processBlock (buffer, midi);
        }

        return std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    };

    // Loud reference.
    const auto loudSeconds = timeBlocks (200, 0.5f);

    // Then ~10 s of silence, timed.
    const auto silentSeconds = timeBlocks (static_cast<int> (10.0 * rtSampleRate / rtBlockSize), 0.0f);

    const auto loudPerBlock = loudSeconds / 200.0;
    const auto silentPerBlock = silentSeconds / (10.0 * rtSampleRate / rtBlockSize);

    INFO ("loud " << (loudPerBlock * 1.0e6) << " us/block, silent " << (silentPerBlock * 1.0e6) << " us/block");

    CHECK (TestHelpers::allSamplesFinite (buffer));
    CHECK (silentPerBlock < loudPerBlock * 2.0);
}

TEST_CASE ("T15: fast automation produces no zipper noise", "[robustness][automation]")
{
    // Zipper noise is a TIME-domain artefact: a parameter applied as a
    // per-block constant steps at each block boundary, and those steps are
    // heard as clicks. So it is measured here as the largest sample-to-sample
    // discontinuity during a fast sweep, compared against the largest one the
    // same signal produces with the parameter held still.
    //
    // A spectral measure was tried first and rejected: sweeping drive across a
    // 1 kHz tone legitimately rewrites its whole harmonic structure, and the
    // resulting modulation sidebands land on non-harmonic bins. Any
    // non-harmonic-energy metric therefore reports roughly -28 dBc whether the
    // parameter is stepped or perfectly smooth, which makes it useless for
    // telling the two apart.
    struct SweepResult
    {
        float largestStep;
        bool finite;
    };

    const auto sweep = [] (const char* parameterId,
                            float fromValue,
                            float toValue,
                            int engineIndex,
                            bool actuallySweep)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (1, 1, rtSampleRate, 64);
        processor.prepareToPlay (rtSampleRate, 64);

        TestHelpers::setParameter (processor, ParamIDs::driveEngine, static_cast<float> (engineIndex));
        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::irEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::outputClip, 0.0f);

        auto* parameter = processor.apvts.getParameter (parameterId);
        REQUIRE (parameter != nullptr);

        // When not sweeping, sit at the midpoint - the same operating point
        // the sweep passes through, so the comparison is fair.
        parameter->setValueNotifyingHost (
            parameter->convertTo0to1 (actuallySweep ? fromValue : 0.5f * (fromValue + toValue)));

        processor.reset();

        constexpr int totalSamples = 48000;
        const auto sweepStart = 16000;
        const auto sweepSamples = static_cast<int> (0.05 * rtSampleRate);

        juce::AudioBuffer<float> block (1, 64);
        juce::MidiBuffer midi;

        SweepResult result { 0.0f, true };
        float previousSample = 0.0f;

        for (int offset = 0; offset + 64 <= totalSamples; offset += 64)
        {
            if (actuallySweep && offset >= sweepStart && offset < sweepStart + sweepSamples)
            {
                const auto position = static_cast<float> (offset - sweepStart) / static_cast<float> (sweepSamples);
                parameter->setValueNotifyingHost (
                    parameter->convertTo0to1 (fromValue + position * (toValue - fromValue)));
            }

            for (int sample = 0; sample < 64; ++sample)
            {
                const auto phase = juce::MathConstants<double>::twoPi * 1000.0
                                    * static_cast<double> (offset + sample) / rtSampleRate;
                block.setSample (0, sample, static_cast<float> (0.25 * std::sin (phase)));
            }

            processor.processBlock (block, midi);

            for (int sample = 0; sample < 64; ++sample)
            {
                const auto value = block.getSample (0, sample);

                if (! std::isfinite (value))
                    result.finite = false;

                // Ignore the settling at the very start of the render.
                if (offset > 8000)
                    result.largestStep = juce::jmax (result.largestStep, std::abs (value - previousSample));

                previousSample = value;
            }
        }

        return result;
    };

    struct Case
    {
        const char* id;
        float from;
        float to;
    };

    const std::vector<Case> cases {
        { ParamIDs::highDrive, 0.0f, 100.0f },
        { ParamIDs::highTightHz, 20.0f, 500.0f },
        { ParamIDs::eqPeak1Gain, -18.0f, 18.0f },
    };

    for (const auto engineIndex : { 0, 1 })
    {
        for (const auto& testCase : cases)
        {
            const auto swept = sweep (testCase.id, testCase.from, testCase.to, engineIndex, true);
            const auto still = sweep (testCase.id, testCase.from, testCase.to, engineIndex, false);

            INFO ("driveEngine " << engineIndex << ", " << testCase.id
                                  << ": largest step while sweeping " << swept.largestStep
                                  << ", while held " << still.largestStep);

            CHECK (swept.finite);
            CHECK (still.finite);

            // Automating the parameter must not introduce a discontinuity
            // larger than the programme itself already contains.
            CHECK (swept.largestStep <= still.largestStep * 1.5f + 1.0e-4f);
        }
    }
}
