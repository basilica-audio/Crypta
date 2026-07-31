#include "PluginProcessor.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

// Regression coverage ported from basilica-audio/Nave PR #28
// ("std::bad_function_call during pluginval's Automation test at 96 kHz",
// fixed by serialising CabConvolutionEngine's message-thread-only methods
// behind a std::recursive_mutex - see
// `git -C nave show origin/main:src/dsp/CabConvolutionEngine.h`/`.cpp` and
// `gh pr view 28 --repo basilica-audio/Nave` for the full original
// write-up). This file is Crypta's own independent audit of the same bug
// class, not a straight port: Crypta's architecture differs from Nave's in
// three load-bearing ways, documented below.
//
// ============================================================================
// AUDIT
// ============================================================================
//
// THE BUG CLASS (from Nave). juce::dsp::Convolution::loadImpulseResponse()
// hands its work off to a background loader thread via
// juce::dsp::BackgroundMessageQueue::push() (JUCE 8.0.14,
// juce_dsp/frequency/juce_Convolution.cpp), which is documented "only safe
// to call from a single thread at a time." If two threads call
// loadImpulseResponse() on the *same* juce::dsp::Convolution instance
// concurrently, without any synchronisation between them, the queue's
// internal FixedSizeFunction command slots can be corrupted; the background
// loader thread later invokes a corrupted/empty slot, throwing
// std::bad_function_call on a thread with no reachable catch handler, which
// aborts the whole process. Nave hit this because two of its call paths into
// CabConvolutionEngine's message-thread-only setters were not guaranteed to
// run on the same thread: prepareToPlay() (host-chosen thread - the VST3/AU
// contract only guarantees it is *not* the audio thread, not that it *is*
// JUCE's own MessageManager thread) and an AsyncUpdater::handleAsyncUpdate()
// callback (always JUCE's real message thread, but triggered from a
// different, audio-thread-delivered automation path).
//
// CRYPTA'S ENTRY POINTS INTO THE SHARED juce::dsp::Convolution INSTANCE
// (cryp::IRLoader, src/dsp/IRLoader.h/.cpp), confirmed by reading every
// caller in this repository, not assumed:
//
//   1. CryptaAudioProcessor::prepareToPlay() (src/PluginProcessor.cpp,
//      calls irLoader.prepare()) - the host-chosen thread described above.
//      Same false-safety assumption Nave had: nothing in this call chain
//      guarantees it runs on JUCE's message thread.
//   2. The public CryptaAudioProcessor::loadImpulseResponse (AudioBuffer,
//      double) (src/PluginProcessor.cpp, calls irLoader.loadImpulseResponse())
//      - documented "message thread only" in both PluginProcessor.h and
//      IRLoader.h's class comments, but there is no compiler- or
//      runtime-enforced guarantee of that; it is reachable by any caller
//      that holds a CryptaAudioProcessor&, including a host driving the
//      plugin programmatically/via scripting, and is the exact DSP-side
//      seam this repo's own CLAUDE.md and IRLoader.h describe as "intended
//      to be called from the message thread (e.g. in response to a GUI
//      file-picker or preset load)" - i.e. a future GUI wiring target, not
//      yet built, but the function itself is live, public API today.
//
// HOW CRYPTA DIFFERS FROM NAVE (verified by grepping the whole src/ tree,
// not assumed from the Nave brief):
//
//   - `grep -rn "AsyncUpdater\|triggerAsyncUpdate\|std::thread\|callAsync"
//      src/` outside src/presets/PresetBar.h's unrelated GUI juce::Timer
//      returns nothing: Crypta has no AsyncUpdater-mediated reconfiguration
//      path at all, so there is no second "always the real message thread"
//      entry point the way Nave had one.
//   - `grep -rn "std::function" src/` returns nothing: no std::function
//      member anywhere in production code that could be invoked before
//      assignment (the other half of the bug class named in the original
//      brief) - this failure mode is entirely internal to JUCE's
//      BackgroundMessageQueue, not something Crypta's own code exposes
//      directly.
//   - CryptaAudioProcessor::setStateInformation() (src/PluginProcessor.cpp)
//      only calls apvts.replaceState() after the legacy-state migrations;
//      it never calls irLoader.prepare() or irLoader.loadImpulseResponse(),
//      so it is not a third entry point into IRLoader.
//   - CryptaAudioProcessor::reset() calls irLoader.reset(), and
//      releaseResources() is an empty no-op (IRLoader has no
//      releaseResources()-equivalent lifecycle hook) - neither touches
//      juce::dsp::Convolution's loadImpulseResponse() path, so neither is
//      an entry point for *this* bug class. (reset() is documented
//      real-time-safe per its own comment in PluginProcessor.cpp -
//      IRLoader::reset() must stay lock-free for that contract to hold, so
//      the fix below deliberately never takes IRLoader's mutex there.)
//   - src/presets/PresetManager.h/.cpp only reads/writes the APVTS
//      ValueTree; it never touches IRLoader or calls loadImpulseResponse()
//      anywhere.
//
// So: entry point #1 (prepareToPlay) is exactly Nave's, and entry point #2
// (the public loadImpulseResponse()) plays the role Nave's
// AsyncUpdater-triggered reconfigure played - a second, independently
// reachable caller of the same underlying convolution.loadImpulseResponse()
// with no synchronisation against #1. Unlike Nave, no AsyncUpdater hop or
// message-loop pump is needed to exercise this: entry point #2 can be
// called directly from an arbitrary std::thread, exactly as a future GUI
// file-picker callback or a scripted host would call it.
//
// THE FIX (src/dsp/IRLoader.h/.cpp): a `mutable std::recursive_mutex
// messageThreadMutex`, taken by IRLoader::prepare() and
// IRLoader::loadImpulseResponse() - the two message-thread-only methods -
// and by neither IRLoader::process() (audio thread, called every block from
// CryptaAudioProcessor::processChunk()) nor IRLoader::reset() nor
// IRLoader::setWetMixProportion() (both documented/required to stay
// real-time safe). This mirrors Nave's CabConvolutionEngine pattern
// exactly: serialise the message-thread-only surface, never touch the
// audio-thread surface.
//
// WHETHER THE RACE IS REAL: reproduced below by stress-testing
// CryptaAudioProcessor::prepareToPlay() (simulating the host's own,
// not-necessarily-message-thread reprepare loop) running concurrently, on
// its own std::thread, against CryptaAudioProcessor::loadImpulseResponse()
// with synthetic in-memory IR buffers, running on a second, independent
// std::thread. Both threads call directly into the same juce::dsp::
// Convolution instance inside IRLoader with no coordination other than the
// fix's mutex - exactly the concurrency pattern described above. See the
// PR body for the actual red-verification run (fix reverted via `git
// stash`, test re-run, evidence recorded there) - being a genuine OS
// scheduling race, any single CI run of this test is a best-effort
// trip-wire, not a 100%-reproducing repro; the real guarantee is the mutex
// itself, which makes the race structurally impossible rather than merely
// unlikely.

namespace
{
    // A short, sharply-decaying synthetic IR - clearly not silence and
    // clearly not the single-sample identity IR IRLoader installs by
    // default, so a human reading a failure knows this thread was doing
    // real work, not a degenerate no-op. Built in-memory; Crypta's
    // loadImpulseResponse() API takes a buffer directly, unlike Nave's
    // file-path-based loader, so no file I/O is needed here at all.
    juce::AudioBuffer<float> makeSyntheticImpulseResponse (int numIrSamples, float phaseSeed)
    {
        juce::AudioBuffer<float> ir (2, numIrSamples);

        for (int channel = 0; channel < ir.getNumChannels(); ++channel)
        {
            auto* data = ir.getWritePointer (channel);

            for (int sample = 0; sample < numIrSamples; ++sample)
                data[sample] = static_cast<float> (std::sin ((static_cast<float> (sample) + phaseSeed) * 0.05f))
                                * std::exp (-static_cast<float> (sample) / 200.0f);
        }

        return ir;
    }
}

TEST_CASE ("Concurrent prepareToPlay and loadImpulseResponse survive a 44.1/96/192k reprepare", "[processor][threading][irloader]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    std::atomic<bool> sawNonFiniteOutput { false };
    std::atomic<int> irLoadsCompleted { 0 };

    // Simulates a second, independent caller of the public
    // loadImpulseResponse() seam - a future GUI file-picker callback or a
    // scripted/programmatic host call - racing against the reprepare loop
    // below with no coordination other than IRLoader's mutex. Deterministic
    // per-iteration seed (the loop index) so a failure is reproducible.
    //
    // Bounded by a fixed iteration count (not a "run until the host thread
    // says stop" flag): CryptaAudioProcessor::prepareToPlay() reprepares
    // every DSP stage in the whole processor, not just IRLoader, so it is
    // not cheap, and an unthrottled racing loadImpulseResponse() loop that
    // runs for the host thread's entire duration spends far more wall-clock
    // time serialised behind the fix's mutex than the race itself needs to
    // be exercised - a self-inflicted slowdown, not evidence of a defect.
    // 200 iterations comfortably overlaps the host loop below while keeping
    // total runtime well under the brief's 60 s budget.
    std::thread irLoadThread ([&]
    {
        for (int iteration = 0; iteration < 200; ++iteration)
        {
            const auto irLengths = std::array<int, 2> { 64, 256 };
            const auto irLength = irLengths[static_cast<size_t> (iteration) % irLengths.size()];

            auto ir = makeSyntheticImpulseResponse (irLength, static_cast<float> (iteration));
            processor.loadImpulseResponse (std::move (ir), 48000.0);

            irLoadsCompleted.fetch_add (1, std::memory_order_relaxed);
        }
    });

    // Simulates the host's own prepareToPlay()-calling thread (this test's
    // calling thread), which per the VST3/AU specs is not guaranteed to be
    // JUCE's message thread: reprepares across the sample rates and block
    // sizes named in the brief and processes one block at each step,
    // mirroring pluginval's Audio processing/Automation sweep pattern (the
    // same pattern that triggered Nave's #27). 4 outer iterations x 3
    // sample rates x 2 block sizes = 24 reprepares, enough to overlap the
    // racing thread above many times over without the test becoming slow.
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        for (const double sampleRate : { 44100.0, 96000.0, 192000.0 })
        {
            for (const int blockSize : { 64, 1024 })
            {
                processor.prepareToPlay (sampleRate, blockSize);

                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;

                TestHelpers::fillWithSine (buffer, sampleRate, 220.0, 0.5f);
                processor.processBlock (buffer, midi);

                if (! TestHelpers::allSamplesFinite (buffer))
                    sawNonFiniteOutput.store (true, std::memory_order_relaxed);
            }
        }
    }

    irLoadThread.join();

    INFO ("IR loads completed on the racing thread: " << irLoadsCompleted.load());
    CHECK (irLoadsCompleted.load() == 200);
    CHECK_FALSE (sawNonFiniteOutput.load());

    // If the race in the audit above were still live, the crash happens on
    // JUCE's internal convolution background-loader thread - a third thread
    // besides these two - so reaching this line at all (rather than the
    // whole process aborting with std::bad_function_call, uncatchable by
    // any try/catch here) is itself part of what this test verifies.
}

TEST_CASE ("Concurrent prepareToPlay and loadImpulseResponse do not deadlock across many small reprepares", "[processor][threading][irloader]")
{
    // A narrower, faster companion to the test above: many rapid small
    // reprepares (64-sample blocks only) against a racing IR load thread,
    // both bounded by a fixed iteration count (not wall-clock time or a
    // "run until the other thread stops" flag - see the runtime-budget
    // comment on the previous test for why that matters here), acting as a
    // basic liveness/deadlock check on the mutex added in IRLoader (a
    // recursive_mutex taken only by prepare()/loadImpulseResponse(), never
    // by process()/reset(), so no lock-order inversion is possible against
    // the audio-thread-safe methods - this test exists to keep it that
    // way).
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 64);

    std::atomic<int> irLoadsCompleted { 0 };

    std::thread irLoadThread ([&]
    {
        for (int iteration = 0; iteration < 150; ++iteration)
        {
            auto ir = makeSyntheticImpulseResponse (32 + (iteration % 5) * 16, static_cast<float> (iteration));
            processor.loadImpulseResponse (std::move (ir), 48000.0);
            irLoadsCompleted.fetch_add (1, std::memory_order_relaxed);
        }
    });

    juce::AudioBuffer<float> buffer (2, 64);
    juce::MidiBuffer midi;

    for (int iteration = 0; iteration < 60; ++iteration)
    {
        processor.prepareToPlay (48000.0, 64);
        TestHelpers::fillWithSine (buffer, 48000.0, 220.0, 0.5f, static_cast<juce::int64> (iteration) * 64);
        processor.processBlock (buffer, midi);
    }

    irLoadThread.join();

    INFO ("IR loads completed on the racing thread: " << irLoadsCompleted.load());
    CHECK (irLoadsCompleted.load() == 150);
    CHECK (TestHelpers::allSamplesFinite (buffer));
}
