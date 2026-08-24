#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

// Issue #34, acceptance bullet "Offline bounce == realtime processing
// verified" - the plugin's own half of it, proven rather than listened to.
//
// Offline-vs-realtime equivalence is not a matter of taste, it is a
// determinism property, and a subtraction decides it far better than an ear
// can. A plugin that renders differently offline than in realtime is broken
// in one of a small number of specific ways: state that depends on wall-clock
// time, on the host's buffer size, on a prepareToPlay() value that is not
// re-applied, or on a smoother that advances once per callback instead of
// once per sample. Every one of those is easy to miss by ear and impossible
// to miss in a null test. This file found one of them and characterised a
// second, and pinned down a third that belongs to JUCE rather than to Crypta
// - see the Fixed section of the changelog, the comments on prepareToPlay()
// and CircuitDrive::prepare(), checkSameAudioDifferentlyBuffered() below, and
// the note below on what is deliberately left alone.
//
// The API under test is juce::AudioProcessor::setNonRealtime() (JUCE 8.0.14,
// juce_audio_processors/processors/juce_AudioProcessor.h): the flag a host
// actually toggles when it switches from playback to a bounce, freeze or
// render. Testing anything else would be testing a fiction.
//
// Three independent things get asserted, because a host changes all three at
// once when it bounces and only a test can separate them:
//
//   1. The flag on its own changes nothing. Identical input, identical block
//      schedule, identical parameters, one pass realtime and one pass
//      non-realtime.
//   2. Re-buffering on its own changes nothing. A bounce does not hand out
//      the block size the live session used: it re-prepares at whatever size
//      it likes, hands out non-power-of-two blocks (441, 480), and ends the
//      render on a short remainder.
//   3. Doing both to the instance that was already playing, after a knob has
//      been moved, changes nothing either. That is the sequence a real host
//      performs - it does not construct a fresh plugin to render with - and
//      it is the one that catches control state prepareToPlay() fails to
//      re-arm.
//
// What this file deliberately does NOT assert: that a freshly constructed
// instance renders the same first block as a re-prepared one. It does not,
// and the difference is real - juce::dsp::Gain::prepare() snaps its smoother
// to whatever target the object is holding, and prepareToPlay() prepares the
// five cascaded gain stages (and the low-band compressor's makeup gain)
// BEFORE telling them the session's value, so a freshly constructed instance
// ramps up from silence across its first 20 ms while a re-prepared one starts
// at level. Measured at a -13.5 dB null with a 0.485 peak difference on an
// otherwise neutral chain. It is left alone here on purpose: no host renders
// into a never-prepared instance (both sides of a real bounce-vs-playback
// comparison are re-prepared, so both are at level), and correcting the
// priming order changes the first 20 ms of every render - which the committed
// v0.2.0 golden fixtures encode. Regenerating those is documented in
// tests/GoldenRenderTests.cpp as a deliberate, reviewed act, and it is not
// this change's to take.
//
// On tolerance: bit-exact, no epsilon, wherever bit-exactness is attainable -
// which is the only defensible default for a deterministic DSP path, since
// nothing in this chain is randomised, dithered or free-running, so any
// differing bit has a cause worth finding. Two things cannot meet it, and
// both are named, measured and isolated rather than absorbed into a widened
// epsilon:
//
//   - The cab-sim convolution, which is switched off for the cases that
//     assert bit-exactness and gets its own test case at the bottom of this
//     file, measured at one ULP.
//   - On Intel only, juce::dsp's snap-to-zero denormal guard, which is a
//     genuine per-callback block-size dependence rather than a rounding
//     artefact. See blockScheduleContract() below for the mechanism, the
//     evidence, and why the bound is where it is. The bit-exact contract is
//     NOT weakened on Apple Silicon, where it is proven and holds.
//
// On timing: nothing in this file waits, sleeps, or measures wall-clock time.
// That is a requirement of the test, not a convenience - a null test with a
// timing assumption in it cannot tell a plugin defect from a loaded build
// machine.
namespace
{
    //==========================================================================
    // Program material.
    //
    // A pure function of the absolute sample index, deliberately: every block
    // schedule below must see bit-identical input, so the material may not
    // carry state that a different block boundary could perturb. The noise
    // term is an integer hash of the index rather than a running LCG for
    // exactly that reason. All arithmetic is done in double and rounded once,
    // so the material itself is identical on every toolchain even though the
    // rendered result need not be.
    //
    // Shape: a bass fundamental plus its fifth, burst-gated at 120 ms with a
    // near-silent floor, over a small noise bed. That drives the noise gate
    // through open/hold/close, the low-band compressor through real attack
    // and release, and both drive engines across a wide amplitude range
    // inside a short window - i.e. every stage that carries state has its
    // state moved rather than parked.
    float programSample (int index, int channel, double sampleRate)
    {
        auto hashed = static_cast<std::uint32_t> (index) * 2654435761u
                      + static_cast<std::uint32_t> (channel) * 40503u;
        hashed ^= hashed >> 15;
        hashed *= 2246822519u;
        hashed ^= hashed >> 13;

        const auto noise = static_cast<double> (hashed >> 8) / static_cast<double> (1u << 24) * 2.0 - 1.0;

        const auto t = static_cast<double> (index) / sampleRate;
        const auto phaseInCycle = std::fmod (t, 0.12);
        const auto envelope = phaseInCycle < 0.07
                                  ? 1.0 - std::exp (-phaseInCycle * 220.0)
                                  : 0.004;

        const auto detune = channel == 0 ? 1.0 : 1.003;
        const auto tone = 0.55 * std::sin (juce::MathConstants<double>::twoPi * 55.0 * detune * t)
                          + 0.28 * std::sin (juce::MathConstants<double>::twoPi * 82.5 * detune * t)
                          + 0.05 * noise;

        return static_cast<float> (envelope * tone);
    }

    //==========================================================================
    // Parameter sets.
    //
    // Defaults are the one configuration most likely to be accidentally
    // correct, so none of these is the defaults. Each engages every optional
    // stage the plugin has - gate, low-band compressor, Graaawl, both drive
    // bands, post-sum EQ and the safety clip - and each sits on non-default
    // values throughout, so a stage that is only wrong away from its default
    // still gets caught.
    struct NamedParameter
    {
        const char* id;
        float plainValue;
    };

    using ParameterSet = std::vector<NamedParameter>;

    ParameterSet withOverrides (ParameterSet parameters, const ParameterSet& overrides)
    {
        for (const auto& override_ : overrides)
        {
            auto found = false;

            for (auto& parameter : parameters)
            {
                if (std::string (parameter.id) == override_.id)
                {
                    parameter.plainValue = override_.plainValue;
                    found = true;
                }
            }

            if (! found)
                parameters.push_back (override_);
        }

        return parameters;
    }

    ParameterSet circuitEverythingEngaged()
    {
        return {
            { ParamIDs::inputGain, 4.5f },
            { ParamIDs::outputGain, -2.5f },
            { ParamIDs::bypass, 0.0f },
            { ParamIDs::outputClip, 1.0f },
            { ParamIDs::clipCeiling, -1.5f },

            { ParamIDs::gateEnabled, 1.0f },
            { ParamIDs::gateMode, 1.0f }, // Modern
            { ParamIDs::gateThreshold, -38.0f },
            { ParamIDs::gateRatio, 6.5f },
            { ParamIDs::gateAttack, 3.0f },
            { ParamIDs::gateRelease, 140.0f },
            { ParamIDs::gateHysteresis, 5.0f },
            { ParamIDs::gateHold, 25.0f },
            { ParamIDs::gateRange, 45.0f },
            { ParamIDs::gateScHpf, 90.0f },

            { ParamIDs::splitLowHz, 135.0f },
            { ParamIDs::splitHighHz, 780.0f },

            { ParamIDs::lowCompDetector, 1.0f }, // Smooth RMS
            { ParamIDs::lowCompThreshold, -26.0f },
            { ParamIDs::lowCompRatio, 4.5f },
            { ParamIDs::lowCompAttack, 12.0f },
            { ParamIDs::lowCompRelease, 180.0f },
            { ParamIDs::lowCompKnee, 7.0f },
            { ParamIDs::lowCompAutoRelease, 1.0f },
            { ParamIDs::lowCompAutoMakeup, 1.0f },
            { ParamIDs::lowCompMakeup, 2.5f },
            { ParamIDs::lowCompMix, 65.0f },
            { ParamIDs::lowGrowl, 1.0f },
            { ParamIDs::lowGrowlAmount, 55.0f },
            { ParamIDs::lowGrowlTone, 40.0f },
            { ParamIDs::lowLevel, 1.5f },

            { ParamIDs::driveEngine, 1.0f }, // Circuit
            { ParamIDs::midDrive, 62.0f },
            { ParamIDs::midLevel, -1.5f },

            { ParamIDs::highTightHz, 420.0f },
            { ParamIDs::highVoicing, 2.0f }, // Razor
            { ParamIDs::highDrive, 71.0f },
            { ParamIDs::highTone, 58.0f },
            { ParamIDs::highBlend, 80.0f },
            { ParamIDs::highBias, 35.0f },
            { ParamIDs::highLevel, -2.0f },

            { ParamIDs::eqEnabled, 1.0f },
            { ParamIDs::eqLowShelfFreq, 95.0f },
            { ParamIDs::eqLowShelfGain, 3.5f },
            { ParamIDs::eqPeak1Freq, 420.0f },
            { ParamIDs::eqPeak1Gain, -4.0f },
            { ParamIDs::eqPeak1Q, 1.4f },
            { ParamIDs::eqPeak2Freq, 2300.0f },
            { ParamIDs::eqPeak2Gain, 2.5f },
            { ParamIDs::eqPeak2Q, 0.9f },
            { ParamIDs::eqHighShelfFreq, 6200.0f },
            { ParamIDs::eqHighShelfGain, -2.0f },

            // Off in the sets that assert bit-exactness; see the dedicated
            // convolution test case at the bottom of this file for why, and
            // for what it costs when it is on.
            { ParamIDs::irEnabled, 0.0f },
            { ParamIDs::irMix, 70.0f },
        };
    }

    // The legacy engine trio is a genuinely different path through the
    // Mid/High section, the low-band detector and the gate - not a variation
    // on the same one - so it gets swept as its own configuration rather than
    // being assumed equivalent.
    ParameterSet classicEverythingEngaged()
    {
        return withOverrides (circuitEverythingEngaged(),
                              {
                                  { ParamIDs::driveEngine, 0.0f },     // Classic
                                  { ParamIDs::lowCompDetector, 0.0f }, // Classic Peak
                                  { ParamIDs::gateMode, 0.0f },        // Classic
                                  { ParamIDs::highVoicing, 0.0f },     // Gnaw
                                  { ParamIDs::lowGrowl, 0.0f },
                              });
    }

    //==========================================================================
    // Block schedules: what a host hands out, block by block.
    //
    // A render is not a stream of identical buffers. It is whatever the host
    // felt like, bounded above by what prepareToPlay() was promised, and it
    // ends on a remainder. `fixedSchedule` is the well-behaved case;
    // `raggedSchedule` is the one that finds remainder-handling bugs.
    std::vector<int> fixedSchedule (int totalSamples, int blockSize)
    {
        std::vector<int> schedule;

        for (int remaining = totalSamples; remaining > 0; remaining -= blockSize)
            schedule.push_back (juce::jmin (blockSize, remaining));

        return schedule;
    }

    // Deterministic but irregular: block lengths walk a fixed cycle, all at or
    // below `maximumBlockSize`, including single-sample blocks. The cycle is
    // spelled out rather than randomised so a failure is reproducible from
    // the test name alone.
    std::vector<int> raggedSchedule (int totalSamples, int maximumBlockSize)
    {
        static constexpr int cycle[] = { 1, 7, 64, 3, 128, 17, 256, 2, 33, 512, 5, 96 };
        static constexpr auto cycleLength = static_cast<int> (std::size (cycle));

        std::vector<int> schedule;
        auto index = 0;

        for (int remaining = totalSamples; remaining > 0;)
        {
            const auto length = juce::jmin (juce::jmin (cycle[index % cycleLength], maximumBlockSize), remaining);
            schedule.push_back (length);
            remaining -= length;
            ++index;
        }

        return schedule;
    }

    //==========================================================================
    // Driving one prepared processor through a schedule, writing the result
    // into `output` at `startSample`.
    void pump (CryptaAudioProcessor& processor,
               double sampleRate,
               int maximumBlockSize,
               const std::vector<int>& schedule,
               juce::AudioBuffer<float>* output,
               int startSample = 0)
    {
        juce::AudioBuffer<float> block (2, maximumBlockSize);
        juce::MidiBuffer midi;

        auto offset = startSample;

        for (const auto length : schedule)
        {
            REQUIRE (length <= maximumBlockSize);
            block.setSize (2, length, false, false, true);

            for (int channel = 0; channel < 2; ++channel)
            {
                auto* data = block.getWritePointer (channel);

                for (int sample = 0; sample < length; ++sample)
                    data[sample] = programSample (offset + sample, channel, sampleRate);
            }

            processor.processBlock (block, midi);

            if (output != nullptr)
                for (int channel = 0; channel < 2; ++channel)
                    output->copyFrom (channel, offset - startSample, block, channel, 0, length);

            offset += length;
        }
    }

    // One render pass, shaped the way a host shapes one.
    //
    // Order matters and mirrors a real host: restore the session's
    // parameters, declare realtime or non-realtime, THEN prepare, then
    // process. Preparing before the parameters are in place would prime every
    // smoother and DryWetMixer from the defaults instead of from the session,
    // which is a different - and considerably less demanding - test.
    juce::AudioBuffer<float> render (double sampleRate,
                                     int preparedBlockSize,
                                     bool nonRealtime,
                                     const ParameterSet& parameters,
                                     const std::vector<int>& schedule,
                                     int totalSamples)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, sampleRate, preparedBlockSize);

        for (const auto& parameter : parameters)
            TestHelpers::setParameter (processor, parameter.id, parameter.plainValue);

        processor.setNonRealtime (nonRealtime);
        REQUIRE (processor.isNonRealtime() == nonRealtime);

        processor.prepareToPlay (sampleRate, preparedBlockSize);

        juce::AudioBuffer<float> output (2, totalSamples);
        pump (processor, sampleRate, preparedBlockSize, schedule, &output);

        return output;
    }

    //==========================================================================
    // The subtraction.
    struct NullResult
    {
        double maxAbsoluteDifference = 0.0;
        double nullDepthDb = -std::numeric_limits<double>::infinity();
        int differingSamples = 0;
        int firstDifferingSample = -1;
        bool bitExact = true;
    };

    NullResult nullAgainst (const juce::AudioBuffer<float>& reference, const juce::AudioBuffer<float>& other)
    {
        REQUIRE (reference.getNumChannels() == other.getNumChannels());
        REQUIRE (reference.getNumSamples() == other.getNumSamples());

        NullResult result;
        double differenceEnergy = 0.0;
        double referenceEnergy = 0.0;

        for (int channel = 0; channel < reference.getNumChannels(); ++channel)
        {
            const auto* a = reference.getReadPointer (channel);
            const auto* b = other.getReadPointer (channel);

            for (int sample = 0; sample < reference.getNumSamples(); ++sample)
            {
                const auto difference = static_cast<double> (a[sample]) - static_cast<double> (b[sample]);

                differenceEnergy += difference * difference;
                referenceEnergy += static_cast<double> (a[sample]) * static_cast<double> (a[sample]);

                if (a[sample] != b[sample])
                {
                    result.bitExact = false;
                    ++result.differingSamples;

                    if (result.firstDifferingSample < 0)
                        result.firstDifferingSample = sample;

                    result.maxAbsoluteDifference = juce::jmax (result.maxAbsoluteDifference, std::abs (difference));
                }
            }
        }

        if (differenceEnergy > 0.0 && referenceEnergy > 0.0)
            result.nullDepthDb = 10.0 * std::log10 (differenceEnergy / referenceEnergy);

        return result;
    }

    // Scientific notation on purpose: the interesting differences in this
    // file are around 1e-7, and a fixed-point rendering of those reads as a
    // reassuring "0.000000".
    std::string scientific (double value)
    {
        char buffer[32] = {};
        std::snprintf (buffer, sizeof (buffer), "%.4e", value);
        return buffer;
    }

    std::string describe (const NullResult& result)
    {
        std::string text = "null depth ";
        text += result.bitExact ? "-inf dB (bit-exact)"
                                : scientific (result.nullDepthDb) + " dB";
        text += ", max abs difference " + scientific (result.maxAbsoluteDifference);
        text += ", differing samples " + std::to_string (result.differingSamples);

        if (result.firstDifferingSample >= 0)
            text += ", first at " + std::to_string (result.firstDifferingSample);

        return text;
    }

    //==========================================================================
    // The contract for "the same audio, buffered differently".
    //
    // On Apple Silicon this is bit-exact and stays bit-exact. On Intel it
    // cannot be, and the reason is a real block-size dependence rather than a
    // rounding artefact - which matters, because the two call for different
    // responses and only one of them justifies a bound.
    //
    // The mechanism: juce::dsp zeroes a filter's internal state at the END of
    // every process() call whenever its magnitude has fallen below 1e-8
    // (JUCE 8.0.14, juce_dsp.h's util::snapToZero(), called once per call by
    // LinkwitzRileyFilter::process(), IIR::Filter::process(),
    // Oversampling::processSamplesUp()/Down() and BallisticsFilter::process()
    // - i.e. by both crossover splits, the phase-align allpass, the post-sum
    // EQ and every oversampled drive stage, which is why both engines are
    // affected and not only the Circuit one). JUCE_SNAP_TO_ZERO is defined as
    // a no-op on non-Intel targets (juce_FloatVectorOperations.h: the macro is
    // #if JUCE_INTEL), so on Apple Silicon nothing snaps and the state
    // trajectory is a pure function of the input. On Intel the snap events
    // land exactly on the host's block boundaries, so where the boundaries
    // are changes the state, and the render genuinely differs.
    //
    // The evidence that this is architecture and not toolchain - MSVC FP
    // contraction was the first suspicion and it is wrong:
    //
    //   - Building THIS repository for x86_64 with the same Apple clang, on
    //     the same machine, and running it under Rosetta reproduces the
    //     Windows/MSVC failures configuration for configuration: worst case
    //     -75.90 dB / 1.3437e-03 peak locally against -75.73 dB / 1.3427e-03
    //     on windows-latest. Two different compilers and two different
    //     operating systems agreeing to 0.2 dB is not a codegen difference.
    //   - Rebuilding that same x86_64 binary with JUCE_DSP_ENABLE_SNAP_TO_ZERO
    //     set to 0, changing nothing else, restores bit-exactness on all 8017
    //     assertions in this file. One flag, one mechanism.
    //   - The first differing sample lands on a block boundary: for the
    //     512-versus-32 Classic case it is sample 32 exactly.
    //
    // Why a bound rather than a fix here: switching the guard off is a
    // one-line JUCE module flag, it is safe in this plugin's audio path
    // (processBlock() installs juce::ScopedNoDenormals, so the CPU already
    // flushes denormals and the guard is redundant there), and it is proven
    // above to work - but it changes the audio the shipped Intel binary
    // produces, which is a product decision and not a test's to take. It is
    // written up on the pull request as a recommended follow-up.
    //
    // Why the bound is where it is:
    //   - -60 dB is the bar tests/GoldenRenderTests.cpp already sets for the
    //     same class of platform split, for the same stated reason.
    //   - It sits 15.7 dB below the worst figure measured on either Intel
    //     toolchain, so it is not a number chosen to make a red run go green -
    //     the measurement came first and has 15.7 dB of room.
    //   - It sits 24 dB below the smallest genuine defect this file has
    //     actually caught (the CircuitDrive block-size dependence, -36.2 dB),
    //     so nothing of that class can hide behind it.
    //   - The peak bound is a second, independent axis: an energy bound alone
    //     cannot see a single-sample excursion. 1.0e-2 is 7.4x the worst peak
    //     measured on either Intel toolchain (1.3437e-03).
    //
    // The condition below tracks JUCE_DSP_ENABLE_SNAP_TO_ZERO deliberately, so
    // that the day the guard is switched off this contract tightens back to
    // bit-exact by itself instead of quietly staying loose.
   #if JUCE_INTEL && JUCE_DSP_ENABLE_SNAP_TO_ZERO
    constexpr auto blockScheduleIsBitExact = false;
    constexpr double intelSnapNullBoundDb = -60.0;
    constexpr double intelSnapPeakBound = 1.0e-2;
   #else
    constexpr auto blockScheduleIsBitExact = true;
   #endif

    // Applies whichever of the two contracts this target is entitled to.
    void checkSameAudioDifferentlyBuffered (const NullResult& result)
    {
        if constexpr (blockScheduleIsBitExact)
        {
            CHECK (result.bitExact);
        }
        else
        {
           #if JUCE_INTEL && JUCE_DSP_ENABLE_SNAP_TO_ZERO
            CHECK (result.nullDepthDb <= intelSnapNullBoundDb);
            CHECK (result.maxAbsoluteDifference <= intelSnapPeakBound);
           #endif
        }
    }

    const char* contractName()
    {
        return blockScheduleIsBitExact
                   ? "contract: bit-exact"
                   : "contract: Intel snap-to-zero bound (<= -60 dB, <= 1.0e-2 peak)";
    }

    // Not a multiple of any block size used below (32, 441, 480, 512, 1024),
    // so every configuration ends on a short remainder block - which is where
    // remainder handling breaks if it is going to.
    constexpr int renderSamples = 11117;

    // The block size a live session would plausibly be running at, and the
    // reference every offline shape is nulled against.
    constexpr int liveBlockSize = 512;

    struct RateAndBlock
    {
        const char* name;
        double sampleRate;
        int blockSize;
    };

    // Small, non-power-of-two, and large, at both of the sample rates the
    // acceptance bullet names. 441 and 480 are the sizes hosts genuinely hand
    // out; a processor that only works on powers of two is a real and common
    // defect, and one that a single-block-size test cannot see.
    const RateAndBlock rateAndBlockMatrix[] = {
        { "44100 Hz / 32 (small)", 44100.0, 32 },
        { "44100 Hz / 441 (non-power-of-two)", 44100.0, 441 },
        { "44100 Hz / 1024 (large)", 44100.0, 1024 },
        { "48000 Hz / 32 (small)", 48000.0, 32 },
        { "48000 Hz / 480 (non-power-of-two)", 48000.0, 480 },
        { "48000 Hz / 1024 (large)", 48000.0, 1024 },
    };

    struct NamedSet
    {
        const char* name;
        ParameterSet parameters;
    };

    std::vector<NamedSet> engineSets()
    {
        return { { "Circuit engine", circuitEverythingEngaged() },
                 { "Classic engine", classicEverythingEngaged() } };
    }
}

//==============================================================================
TEST_CASE ("Offline vs realtime: setNonRealtime() alone changes nothing, bit-exactly", "[offline-realtime][determinism]")
{
    // The narrowest statement of the property, with the block schedule held
    // identical so that the ONLY difference between the two passes is the
    // flag.
    //
    // Tolerance: exact float equality, no epsilon, deliberately. Both passes
    // execute the same instructions on the same inputs in the same order.
    // Nothing in src/ reads isNonRealtime() today, and nothing may start to
    // without this test having an opinion about it - a single differing bit
    // here means either a branch on the flag or a source of state that is not
    // a function of the input, and neither of those is roundable.
    for (const auto& configuration : rateAndBlockMatrix)
    {
        for (const auto& set : engineSets())
        {
            const auto schedule = fixedSchedule (renderSamples, configuration.blockSize);

            const auto realtime = render (configuration.sampleRate, configuration.blockSize, false,
                                          set.parameters, schedule, renderSamples);
            const auto offline = render (configuration.sampleRate, configuration.blockSize, true,
                                         set.parameters, schedule, renderSamples);

            const auto result = nullAgainst (realtime, offline);

            INFO ("flag-only null [" << configuration.name << ", " << set.name << "]: " << describe (result));
            CHECK (result.bitExact);
            CHECK (TestHelpers::allSamplesFinite (offline));
        }
    }
}

//==============================================================================
TEST_CASE ("Offline vs realtime: a bounce re-buffered at a different block size renders the same audio", "[offline-realtime][determinism][chunking]")
{
    // What a user actually does: play the passage live at the session buffer
    // size, bounce it offline at whatever size the host picks, and null the
    // two. The realtime reference runs at 512; every offline pass runs at a
    // different size, including a non-power-of-two one, a 32-sample one, and
    // a ragged schedule whose block length changes on every call down to
    // single samples.
    //
    // This is the case that failed when this file was written. The Circuit
    // engine's per-block control ramps (CircuitDrive::RampedScalar) span
    // exactly one block whatever that block's length, and prepareToPlay() was
    // not priming them, so the first block of a render ramped the drive,
    // blend, bias and level controls up from their constructed defaults over
    // a span set by the host's buffer size: a 512-sample render and a
    // 1024-sample render of the same passage nulled at only -36.2 dB, with a
    // 0.083 peak difference and a -20.2 dB local null over the first 1000
    // samples. Both prepareToPlay() and CircuitDrive::prepare() were fixed
    // rather than this assertion being relaxed.
    //
    // On Intel this case runs under the bounded contract instead of the
    // bit-exact one, for a reason that is a real block-size dependence rather
    // than rounding - see checkSameAudioDifferentlyBuffered()'s write-up
    // above. The difference between the two findings is worth being precise
    // about: the CircuitDrive defect was Crypta's, was localised to the
    // render's first block, and is fixed; the snap-to-zero dependence is
    // JUCE's, is spread across the whole render, exists on Intel only, and is
    // bounded here and reported rather than switched off unilaterally.
    struct OfflineShape
    {
        const char* name;
        int preparedBlockSize;
        bool ragged;
    };

    const OfflineShape shapes[] = {
        { "32 (small)", 32, false },
        { "441 (non-power-of-two)", 441, false },
        { "1024 (large)", 1024, false },
        { "ragged 1..512", 512, true },
    };

    for (const auto sampleRate : { 44100.0, 48000.0 })
    {
        for (const auto& set : engineSets())
        {
            const auto reference = render (sampleRate, liveBlockSize, false, set.parameters,
                                           fixedSchedule (renderSamples, liveBlockSize), renderSamples);

            for (const auto& shape : shapes)
            {
                const auto schedule = shape.ragged
                                          ? raggedSchedule (renderSamples, shape.preparedBlockSize)
                                          : fixedSchedule (renderSamples, shape.preparedBlockSize);

                const auto offline = render (sampleRate, shape.preparedBlockSize, true, set.parameters,
                                             schedule, renderSamples);

                const auto result = nullAgainst (reference, offline);

                INFO ("bounce null [" << sampleRate << " Hz, realtime " << liveBlockSize
                                      << " vs offline " << shape.name << ", " << set.name << ", "
                                      << contractName() << "]: " << describe (result));
                checkSameAudioDifferentlyBuffered (result);
                CHECK (TestHelpers::allSamplesFinite (offline));
            }
        }
    }
}

//==============================================================================
TEST_CASE ("Offline vs realtime: the host's real bounce sequence, knob move included, is block-size independent", "[offline-realtime][determinism][reset]")
{
    // The sequence a host performs for real. It does not construct a new
    // plugin to render with: it takes the instance that has been playing,
    // calls setNonRealtime (true), calls prepareToPlay() again with the render
    // block size, and renders. Anything prepareToPlay() fails to re-arm
    // therefore lands in the bounce and not in the playback the bounce is
    // supposed to reproduce.
    //
    // The knob move in the middle is the point of this case rather than
    // decoration. With the controls unchanged, a re-prepared instance's
    // control ramps are already sitting at their targets and cannot ramp; it
    // is the ordinary act of adjusting something after playback and before
    // rendering that arms them. CircuitDrive::prepare() re-arms its one-shot
    // snap for exactly this reason - without it, the new drive value would
    // ramp in across the render's first block, whose length is the host's,
    // and the same bounce at two block sizes would differ.
    //
    // Both sides run the identical lifecycle, so the reference is a bounce
    // too: only the render's block size differs.
    const auto bounce = [] (double sampleRate, int bounceBlockSize, const ParameterSet& parameters)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, sampleRate, liveBlockSize);

        for (const auto& parameter : parameters)
            TestHelpers::setParameter (processor, parameter.id, parameter.plainValue);

        processor.prepareToPlay (sampleRate, liveBlockSize);
        pump (processor, sampleRate, liveBlockSize,
              fixedSchedule (8 * liveBlockSize, liveBlockSize), nullptr);

        // Transport stopped, knobs moved, render started - the ordinary way a
        // bounce happens.
        TestHelpers::setParameter (processor, ParamIDs::highDrive, 34.0f);
        TestHelpers::setParameter (processor, ParamIDs::midDrive, 88.0f);
        TestHelpers::setParameter (processor, ParamIDs::highBlend, 45.0f);
        TestHelpers::setParameter (processor, ParamIDs::highLevel, -5.0f);

        processor.setNonRealtime (true);
        processor.prepareToPlay (sampleRate, bounceBlockSize);

        juce::AudioBuffer<float> rendered (2, renderSamples);
        pump (processor, sampleRate, bounceBlockSize,
              fixedSchedule (renderSamples, bounceBlockSize), &rendered);

        return rendered;
    };

    for (const auto sampleRate : { 44100.0, 48000.0 })
    {
        for (const auto& set : engineSets())
        {
            const auto reference = bounce (sampleRate, liveBlockSize, set.parameters);

            for (const auto bounceBlockSize : { 32, 441, 1024 })
            {
                const auto rendered = bounce (sampleRate, bounceBlockSize, set.parameters);
                const auto result = nullAgainst (reference, rendered);

                INFO ("re-prepared bounce null [" << sampleRate << " Hz, bounce at " << liveBlockSize
                                                  << " vs bounce at " << bounceBlockSize << ", " << set.name
                                                  << ", " << contractName() << "]: " << describe (result));
                checkSameAudioDifferentlyBuffered (result);
                CHECK (TestHelpers::allSamplesFinite (rendered));
            }
        }
    }
}

//==============================================================================
TEST_CASE ("Offline vs realtime: the cab-sim convolution is the one stage that cannot be bit-exact, and it is one ULP", "[offline-realtime][determinism][ir]")
{
    // The exclusion, stated with numbers rather than hidden in an epsilon.
    //
    // juce::dsp::Convolution builds its partitioned FFT engine from
    // ProcessSpec::maximumBlockSize (JUCE 8.0.14, juce_Convolution.cpp,
    // ConvolutionEngineFactory::makeEngine()). Preparing at a different block
    // size therefore partitions the same convolution differently, and two
    // different partitionings of the same convolution round differently in
    // the last bit. That is a property of the library's arithmetic, not of
    // Crypta's, and it is not something this plugin can fix without pinning
    // the convolution to a fixed internal block size - a latency and CPU
    // trade that is not worth making for a last-bit difference.
    //
    // So it is measured instead. The chain here is deliberately linear around
    // the IR stage - no EQ, no clip, no drive - so that nothing downstream
    // amplifies the difference and the figure below is the convolution's own.
    //
    // The bound is a representation bound, not a chosen tolerance: 2.0e-7 is
    // two ULP of float32 at unity, and a difference has to be at least one
    // ULP to exist at all. Measured on macOS/arm64, at both rates and all
    // three block sizes: max abs difference 1.1921e-07 - which is exactly
    // 2^-23, one ULP - for a null depth of -151.0 to -155.4 dB.
    //
    // That null depth moves by a fraction of a dB from run to run, and the
    // reason is worth stating rather than smoothing over: juce::dsp::
    // Convolution installs engines from a background thread, so whether the
    // engine prepare() built is also crossfaded in on an early process() call
    // is not fixed. It changes WHICH samples round differently; it cannot
    // change by how much, because both engines carry the same identity
    // impulse response. That is exactly why the assertion below is on the
    // per-sample bound and not on the null depth - a wall-clock-sensitive
    // quantity has no business being an assertion.
    constexpr double ulpBound = 2.0e-7;

    const auto irOnlyChain = withOverrides (classicEverythingEngaged(),
                                            {
                                                { ParamIDs::irEnabled, 1.0f },
                                                { ParamIDs::irMix, 70.0f },
                                                { ParamIDs::eqEnabled, 0.0f },
                                                { ParamIDs::outputClip, 0.0f },
                                                { ParamIDs::gateEnabled, 0.0f },
                                                { ParamIDs::midDrive, 0.0f },
                                                { ParamIDs::highDrive, 0.0f },
                                                { ParamIDs::highBlend, 0.0f },
                                                { ParamIDs::lowCompMix, 0.0f },
                                            });

    for (const auto sampleRate : { 44100.0, 48000.0 })
    {
        const auto reference = render (sampleRate, liveBlockSize, false, irOnlyChain,
                                       fixedSchedule (renderSamples, liveBlockSize), renderSamples);

        for (const auto blockSize : { 32, 441, 1024 })
        {
            const auto offline = render (sampleRate, blockSize, true, irOnlyChain,
                                         fixedSchedule (renderSamples, blockSize), renderSamples);

            const auto result = nullAgainst (reference, offline);

            INFO ("convolution-only bounce null [" << sampleRate << " Hz, realtime " << liveBlockSize
                                                   << " vs offline " << blockSize << "]: " << describe (result));
            CHECK (result.maxAbsoluteDifference <= ulpBound);
            CHECK (TestHelpers::allSamplesFinite (offline));
        }
    }

    // And what that one ULP costs once the rest of the chain is back in
    // circuit, because a number nobody records is a number that can drift.
    // The post-sum EQ's biquads integrate a persistent last-bit input
    // difference into something larger - measured at -84.5 dB / 1.5564e-04 on
    // macOS/arm64, against -inf dB for the identical chain with the IR stage
    // switched off (the assertion in the re-buffering test case above, which
    // is what attributes this entirely to the convolution).
    //
    // The bound asserted here is a REGRESSION bound and is labelled as one:
    // -70 dB leaves the convolution's last-bit drift room to differ between
    // toolchains, while being far too tight for a newly block-size-dependent
    // stage to hide behind - the smallest defect this file actually found
    // measured -36.2 dB. It is not a tolerance for a known failure.
    {
        constexpr double regressionBoundDb = -70.0;
        const auto fullChain = withOverrides (circuitEverythingEngaged(), { { ParamIDs::irEnabled, 1.0f } });

        const auto reference = render (48000.0, liveBlockSize, false, fullChain,
                                       fixedSchedule (renderSamples, liveBlockSize), renderSamples);
        const auto offline = render (48000.0, 1024, true, fullChain,
                                     fixedSchedule (renderSamples, 1024), renderSamples);

        const auto result = nullAgainst (reference, offline);

        INFO ("full chain with IR engaged [48000 Hz, realtime " << liveBlockSize
                                                                << " vs offline 1024]: " << describe (result));
        CHECK (result.nullDepthDb <= regressionBoundDb);
        CHECK (TestHelpers::allSamplesFinite (offline));
    }
}
