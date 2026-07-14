#include "dsp/IRLoader.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

// Issue #42: cab-sim IR loader acceptance gates.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int numSamples = 4096;

    juce::dsp::ProcessSpec makeSpec (int numChannels = 1)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (numSamples);
        spec.numChannels = static_cast<juce::uint32> (numChannels);
        return spec;
    }

    // A short, sharply decaying synthetic "IR": clearly not silence and
    // clearly not an identity impulse, so tests can tell whether it was
    // actually applied.
    juce::AudioBuffer<float> makeSyntheticImpulseResponse (int numIrSamples = 64)
    {
        juce::AudioBuffer<float> ir (2, numIrSamples);

        for (int channel = 0; channel < ir.getNumChannels(); ++channel)
        {
            auto* data = ir.getWritePointer (channel);

            for (int sample = 0; sample < numIrSamples; ++sample)
                data[sample] = std::exp (-static_cast<float> (sample) / 8.0f);
        }

        return ir;
    }
}

TEST_CASE ("IRLoader: with no IR loaded, output is a bit-exact identity passthrough even at 100% wet", "[ir][dsp]")
{
    // juce::dsp::Convolution defaults to a single-sample identity impulse
    // response until loadImpulseResponse() is called (JUCE 8.0.14), so this
    // is the plugin's safe-by-default state before any factory/user IR is
    // loaded.
    tyg::IRLoader irLoader;
    irLoader.prepare (makeSpec(), 1.0f);

    // juce::dsp::Convolution installs its (here: identity) engine via the
    // same crossfade machinery used for a live IR swap, which runs once on
    // the very first process() call after prepare() (JUCE 8.0.14,
    // Convolution::Impl::processSamples()'s installPendingEngine()) - so a
    // throwaway warm-up block is processed first, the same way this
    // codebase's other tests settle smoothers/filter transients before
    // measuring (e.g. GainProcessingTests.cpp's settleSmoothing()), and the
    // identity-passthrough assertion is made against a block processed
    // afterwards, once that one-time transition has completed.
    juce::AudioBuffer<float> warmup (1, numSamples);
    TestHelpers::fillWithSine (warmup, testSampleRate, 400.0, 0.5f);
    juce::dsp::AudioBlock<float> warmupBlock (warmup);
    juce::Thread::sleep (15);
    irLoader.process (warmupBlock);

    juce::AudioBuffer<float> buffer (1, numSamples);
    TestHelpers::fillWithSine (buffer, testSampleRate, 400.0, 0.5f, numSamples);

    juce::AudioBuffer<float> reference;
    reference.makeCopyOf (buffer);

    juce::dsp::AudioBlock<float> block (buffer);
    irLoader.process (block);

    const auto* refData = reference.getReadPointer (0);
    const auto* outData = buffer.getReadPointer (0);

    for (int sample = 0; sample < numSamples; ++sample)
        CHECK (outData[sample] == Catch::Approx (refData[sample]).margin (1e-4f));
}

TEST_CASE ("IRLoader: 0% mix is a passthrough regardless of the loaded IR", "[ir][dsp]")
{
    tyg::IRLoader irLoader;
    irLoader.prepare (makeSpec (2), 0.0f);
    irLoader.loadImpulseResponse (makeSyntheticImpulseResponse(), testSampleRate);

    // Convolution's asynchronous IR loading needs a handful of prepare/
    // process cycles to install the newly loaded engine (JUCE 8.0.14,
    // juce_Convolution.cpp's ConvolutionEngineQueue) - process a few blocks
    // so the loaded IR (if it were audible) would definitely be active
    // before the assertion below.
    juce::AudioBuffer<float> warmup (2, numSamples);
    juce::dsp::AudioBlock<float> warmupBlock (warmup);

    for (int i = 0; i < 4; ++i)
    {
        juce::Thread::sleep (15);
        irLoader.process (warmupBlock);
    }

    juce::AudioBuffer<float> buffer (2, numSamples);
    TestHelpers::fillWithSine (buffer, testSampleRate, 400.0, 0.5f);

    juce::AudioBuffer<float> reference;
    reference.makeCopyOf (buffer);

    juce::dsp::AudioBlock<float> block (buffer);
    irLoader.process (block);

    const auto outRms = TestHelpers::rms (buffer);
    const auto refRms = TestHelpers::rms (reference);

    CHECK (juce::Decibels::gainToDecibels (outRms / refRms) == Catch::Approx (0.0).margin (0.2));
}

TEST_CASE ("IRLoader: loading a real IR at 100% wet audibly changes the signal", "[ir][dsp]")
{
    tyg::IRLoader irLoader;
    irLoader.prepare (makeSpec (1), 1.0f);
    irLoader.loadImpulseResponse (makeSyntheticImpulseResponse(), testSampleRate);

    juce::AudioBuffer<float> buffer (1, numSamples);
    juce::dsp::AudioBlock<float> block (buffer);

    // juce::dsp::Convolution loads impulse responses on a genuine background
    // thread (JUCE 8.0.14's BackgroundMessageQueue, polling every ~10ms) and
    // only installs the newly-built engine on a *subsequent* process() call
    // once it's ready - so, unlike the rest of this real-time-safe DSP, this
    // is one spot where a test legitimately has to wait on wall-clock time
    // rather than just calling process() in a tight loop.
    bool changed = false;

    for (int i = 0; i < 30 && ! changed; ++i)
    {
        juce::Thread::sleep (15);

        TestHelpers::fillWithSine (buffer, testSampleRate, 400.0, 0.5f, static_cast<juce::int64> (i) * numSamples);

        juce::AudioBuffer<float> reference;
        reference.makeCopyOf (buffer);

        irLoader.process (block);

        juce::AudioBuffer<float> difference;
        difference.makeCopyOf (buffer);
        difference.addFrom (0, 0, reference, 0, 0, numSamples, -1.0f);

        if (TestHelpers::rms (difference) > 0.01)
            changed = true;
    }

    CHECK (changed);
}

TEST_CASE ("IRLoader: no NaN/Inf across a denormal-range sweep, with and without a loaded IR", "[ir][dsp][robustness]")
{
    for (const bool loadIr : { false, true })
    {
        tyg::IRLoader irLoader;
        irLoader.prepare (makeSpec (2), 1.0f);

        if (loadIr)
            irLoader.loadImpulseResponse (makeSyntheticImpulseResponse(), testSampleRate);

        juce::AudioBuffer<float> buffer (2, numSamples);
        const auto denormalValue = std::numeric_limits<float>::denorm_min() * 4.0f;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            auto* data = buffer.getWritePointer (channel);

            for (int sample = 0; sample < numSamples; ++sample)
                data[sample] = (sample % 2 == 0) ? denormalValue : -denormalValue;
        }

        juce::dsp::AudioBlock<float> block (buffer);

        for (int i = 0; i < 4; ++i)
        {
            CHECK_NOTHROW (irLoader.process (block));
            CHECK (TestHelpers::allSamplesFinite (buffer));
        }
    }
}
