#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "params/ParameterIds.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <vector>

// Sample-rate independence, measured as "the same musical result", not as "it
// did not crash" (issue #34, the v1.0.0 measurement gate).
//
// WHY THIS IS NOT tests/SampleRateAndRobustnessTests.cpp
// -----------------------------------------------------
// That file sweeps 44.1 kHz to 192 kHz and asserts the output is finite and the
// reported latency is plausible. Both are necessary. Neither would notice a
// filter corner that lands at the wrong frequency at 96 kHz, a compressor whose
// attack is coded in samples rather than in milliseconds, or a shaper whose
// harmonic profile changes because the oversampling factor dropped. Those are
// the failures that make a plugin sound different when the session rate changes
// - which is a bug users hit when they open last year's 44.1 kHz project in a
// 48 kHz template, and it is invisible to a finiteness check.
//
// THE MEASUREMENT
// ---------------
// Everything below is a RATIO of output to input measured with the same FFT, so
// the window's scale factor and the transform length cancel and the figures from
// different rates are directly comparable. The transform length is chosen per
// rate so the analysis bin width stays at 1.35-1.47 Hz everywhere, which keeps
// the bin-snap error of the test tone below 0.73 Hz at every rate.
//
// THE TOLERANCES, AND WHERE THEY COME FROM
// ----------------------------------------
// linearToleranceDb = 0.5 dB, for the magnitude response below 2 kHz.
//
//   Every IIR section in the chain is a bilinear-transformed analogue
//   prototype, and the bilinear transform warps frequency by
//   tan(pi*f/fs)/(pi*f/fs). The worst case in this test is the highest analysed
//   tone at the lowest analysed rate: 1760 Hz at 44.1 kHz, where
//   pi*f/fs = 0.1254 and the warp ratio is 1.00524 - a 0.52 % frequency error
//   relative to the analogue prototype the coefficients came from. The steepest
//   slope anywhere in the chain is the cascaded Linkwitz-Riley pair at
//   24 dB/octave, so that frequency error can move the magnitude by at most
//   24 * log2(1.00524) = 0.18 dB. Bin-snap adds 0.73 Hz at 1760 Hz = 0.04 %,
//   or 0.01 dB through the same slope.
//
//   0.5 dB is that 0.19 dB with a factor of ~2.6 in hand for the phase-align
//   allpass, the low-band compensation delay's quantisation to whole samples,
//   and float accumulation across a 131072-point transform. It is NOT chosen to
//   make anything pass: the expected honest answer is a small fraction of it,
//   and the measured figures are printed in the INFO lines so a result that
//   creeps towards the bound is visible before it crosses it.
//
// harmonicToleranceDb = 1.5 dB, for the harmonic profile of the drive stage.
//
//   A harmonic's amplitude is a power of the amplitude presented to the shaper,
//   so any magnitude error in the filtering AHEAD of the shaper is multiplied by
//   the harmonic order. The third harmonic of the 880 Hz test tone sits at
//   2640 Hz, where the 44.1 kHz warp ratio is 1.0118 and the pre-shaper path's
//   own slope gives ~0.4 dB; cubed by the nonlinearity, that is ~1.2 dB. The
//   oversampling factor also changes with rate (4x at 44.1/48 kHz, less above),
//   which moves the alias floor underneath the harmonic being measured.
//   1.5 dB covers the 1.2 dB and nothing more.
//
// ballisticsToleranceDb = 0.5 dB, for the compressor's gain reduction measured
//   at a fixed time after a step.
//
//   Attack and release are declared in milliseconds and a one-pole coefficient
//   computed as exp(-1/(tau*fs)) is exact at any fs, so the correct answer here
//   is zero deviation and any deviation at all is a coefficient computed in
//   samples. 0.5 dB is one twelfth of the ~6 dB reduction under test - far too
//   small to be a change in feel, and far too large to be reached by anything
//   except a real rate dependency.
namespace
{
    const std::vector<double> invarianceRates { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    constexpr double referenceRate = 48000.0;
    constexpr int invarianceBlockSize = 512;

    constexpr double linearToleranceDb = 0.5;
    constexpr double harmonicToleranceDb = 1.5;
    constexpr double ballisticsToleranceDb = 0.5;

    // Analysis length per rate, chosen to hold the bin width at 1.35-1.47 Hz.
    int fftOrderFor (double sampleRate)
    {
        if (sampleRate <= 50000.0)  return 15; // 32768
        if (sampleRate <= 100000.0) return 16; // 65536
        return 17;                             // 131072
    }

    // Renders a steady sine and returns the level of a chosen spectral
    // component, relative to the same component of the input signal, in dB.
    // Because both sides use the identical window and transform, everything
    // except the plugin's own transfer cancels.
    struct ToneAnalysis
    {
        double fundamentalTransferDb = 0.0;
        std::vector<double> harmonicRelativeDb; // H2..H5 relative to H1, in dB
        double actualFrequencyHz = 0.0;
    };

    ToneAnalysis analyseTone (const std::function<void (CryptaAudioProcessor&)>& configure,
                              double sampleRate,
                              double requestedHz,
                              float amplitude,
                              int highestHarmonic)
    {
        const auto fftOrder = fftOrderFor (sampleRate);
        const auto fftSize = 1 << fftOrder;

        // Half a second of warm-up before the analysis window: past the
        // longest smoother and past the reported latency at every rate.
        const auto warmUpSamples = static_cast<int> (0.5 * sampleRate);
        const auto totalSamples = warmUpSamples + fftSize;

        const auto toneHz = TestHelpers::snapToBin (requestedHz, sampleRate, fftSize);

        juce::AudioBuffer<float> signal (2, totalSamples);
        TestHelpers::fillWithSine (signal, sampleRate, toneHz, amplitude, 0);

        juce::AudioBuffer<float> reference (2, totalSamples);
        reference.makeCopyOf (signal);

        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, sampleRate, invarianceBlockSize);
        processor.prepareToPlay (sampleRate, invarianceBlockSize);
        configure (processor);
        processor.reset();

        TestHelpers::renderThrough (processor, signal, invarianceBlockSize);

        const auto outputSpectrum = TestHelpers::powerSpectrum (signal, 0, fftOrder, warmUpSamples);
        const auto inputSpectrum = TestHelpers::powerSpectrum (reference, 0, fftOrder, warmUpSamples);

        ToneAnalysis analysis;
        analysis.actualFrequencyHz = toneHz;

        const auto outputFundamentalDb = TestHelpers::peakMagnitudeDb (outputSpectrum, sampleRate, fftSize, toneHz);
        const auto inputFundamentalDb = TestHelpers::peakMagnitudeDb (inputSpectrum, sampleRate, fftSize, toneHz);

        analysis.fundamentalTransferDb = outputFundamentalDb - inputFundamentalDb;

        for (int harmonic = 2; harmonic <= highestHarmonic; ++harmonic)
        {
            const auto harmonicHz = toneHz * static_cast<double> (harmonic);

            if (harmonicHz >= 0.45 * sampleRate)
            {
                analysis.harmonicRelativeDb.push_back (0.0);
                continue;
            }

            const auto harmonicDb = TestHelpers::peakMagnitudeDb (outputSpectrum, sampleRate, fftSize, harmonicHz);
            analysis.harmonicRelativeDb.push_back (harmonicDb - outputFundamentalDb);
        }

        return analysis;
    }

    // The transparent configuration: nothing driven, nothing gated, nothing
    // compressed. What is left is the three-band split, the phase-align
    // allpass, the delay compensation and the sum - i.e. exactly the part of
    // the chain whose rate dependency the tolerance above was derived for.
    void configureLinear (CryptaAudioProcessor& processor)
    {
        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::outputClip, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::irEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowGrowl, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::midDrive, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::highDrive, 0.0f);
        // Threshold at the top of its range: the compressor is present and
        // running, but a -12 dBFS tone never reaches it, so the measurement is
        // of the filter network rather than of a level-dependent gain.
        TestHelpers::setParameter (processor, ParamIDs::lowCompThreshold, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::lowCompAutoMakeup, 0.0f);
    }
}

//==============================================================================
TEST_CASE ("Sample rate: the magnitude response below 2 kHz is the same at 44.1/48/88.2/96/192 kHz",
           "[sample-rate][invariance]")
{
    // Musical octaves of A, from the low B-string region up to the top of where
    // a bass guitar's fundamental and first few harmonics live.
    const std::vector<double> tones { 55.0, 110.0, 220.0, 440.0, 880.0, 1760.0 };

    for (const auto toneHz : tones)
    {
        const auto reference = analyseTone (configureLinear, referenceRate, toneHz, 0.25f, 1);

        INFO ("tone " << toneHz << " Hz, 48 kHz reference transfer "
              << reference.fundamentalTransferDb << " dB");

        for (const auto rate : invarianceRates)
        {
            if (rate == referenceRate)
                continue;

            const auto measured = analyseTone (configureLinear, rate, toneHz, 0.25f, 1);
            const auto deviation = measured.fundamentalTransferDb - reference.fundamentalTransferDb;

            INFO ("at " << rate << " Hz (analysed at " << measured.actualFrequencyHz
                  << " Hz): transfer " << measured.fundamentalTransferDb
                  << " dB, deviation " << deviation << " dB, allowed " << linearToleranceDb);

            CHECK (std::abs (deviation) <= linearToleranceDb);
        }
    }
}

TEST_CASE ("Sample rate: the drive stage's harmonic profile is the same at every rate",
           "[sample-rate][invariance][voicing]")
{
    // 880 Hz sits above the default 600 Hz upper split, so the tone is in the
    // high band and the voicing's shaper is what produces the harmonics being
    // compared. Both engines, because they oversample differently.
    for (const auto engineIndex : { 0, 1 }) // Classic, Circuit
    {
        for (const auto voicingIndex : { 0, 1, 2 }) // Gnaw, Wool, Razor
        {
            const auto configure = [engineIndex, voicingIndex] (CryptaAudioProcessor& processor)
            {
                TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
                TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 0.0f);
                TestHelpers::setParameter (processor, ParamIDs::outputClip, 0.0f);
                TestHelpers::setParameter (processor, ParamIDs::driveEngine, static_cast<float> (engineIndex));
                TestHelpers::setParameter (processor, ParamIDs::highVoicing, static_cast<float> (voicingIndex));
                TestHelpers::setParameter (processor, ParamIDs::highDrive, 70.0f);
                TestHelpers::setParameter (processor, ParamIDs::highBlend, 100.0f);
            };

            const auto reference = analyseTone (configure, referenceRate, 880.0, 0.25f, 5);

            for (const auto rate : invarianceRates)
            {
                if (rate == referenceRate)
                    continue;

                const auto measured = analyseTone (configure, rate, 880.0, 0.25f, 5);

                REQUIRE (measured.harmonicRelativeDb.size() == reference.harmonicRelativeDb.size());

                for (size_t index = 0; index < reference.harmonicRelativeDb.size(); ++index)
                {
                    // Harmonics that fall below -90 dB relative to the
                    // fundamental are the shaper's symmetry floor, not signal:
                    // Gnaw's even harmonics are 69 dB down by design
                    // (tests/VoicingCharacterTests.cpp), and comparing two
                    // numbers that are both essentially absent measures the
                    // FFT's noise floor rather than the plugin.
                    if (reference.harmonicRelativeDb[index] < -90.0)
                        continue;

                    const auto deviation = measured.harmonicRelativeDb[index] - reference.harmonicRelativeDb[index];

                    INFO ("engine " << engineIndex << ", voicing " << voicingIndex
                          << ", H" << (index + 2)
                          << " at " << rate << " Hz: " << measured.harmonicRelativeDb[index]
                          << " dB vs 48 kHz " << reference.harmonicRelativeDb[index]
                          << " dB, deviation " << deviation << " dB, allowed " << harmonicToleranceDb);

                    CHECK (std::abs (deviation) <= harmonicToleranceDb);
                }
            }
        }
    }
}

TEST_CASE ("Sample rate: the low-band compressor's ballistics are in milliseconds, not in samples",
           "[sample-rate][invariance][compressor]")
{
    // A step from silence to a loud low tone, with the gain reduction read out
    // of the meter tap at a fixed WALL-CLOCK offset after the step. If any
    // coefficient were computed in samples rather than from a time constant,
    // the reduction at 30 ms would differ by more than 4x between 44.1 kHz and
    // 192 kHz; if they are all times, it is the same number at every rate.
    for (const auto detectorIndex : { 0, 1 }) // Classic Peak, Smooth RMS
    {
        std::vector<double> reductions;

        for (const auto rate : invarianceRates)
        {
            const auto blockSize = 64;

            CryptaAudioProcessor processor;
            processor.setPlayConfigDetails (2, 2, rate, blockSize);
            processor.prepareToPlay (rate, blockSize);

            TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
            TestHelpers::setParameter (processor, ParamIDs::lowCompDetector, static_cast<float> (detectorIndex));
            TestHelpers::setParameter (processor, ParamIDs::lowCompThreshold, -30.0f);
            TestHelpers::setParameter (processor, ParamIDs::lowCompRatio, 4.0f);
            TestHelpers::setParameter (processor, ParamIDs::lowCompAttack, 10.0f);
            TestHelpers::setParameter (processor, ParamIDs::lowCompAutoRelease, 0.0f);
            TestHelpers::setParameter (processor, ParamIDs::lowCompRelease, 200.0f);
            TestHelpers::setParameter (processor, ParamIDs::lowCompAutoMakeup, 0.0f);
            processor.reset();

            juce::AudioBuffer<float> block (2, blockSize);
            juce::MidiBuffer midi;

            // 100 ms of silence, then the tone. The reduction is sampled 30 ms
            // after the onset - three attack time constants, deep enough into
            // the attack to be sensitive to it and short enough to be nowhere
            // near the release.
            const auto silentBlocks = static_cast<int> (0.1 * rate / blockSize);
            const auto sampleAtBlock = static_cast<int> (0.03 * rate / blockSize);

            for (int index = 0; index < silentBlocks; ++index)
            {
                block.clear();
                processor.processBlock (block, midi);
            }

            double reductionDb = 0.0;

            for (int index = 0; index <= sampleAtBlock; ++index)
            {
                TestHelpers::fillWithSine (block, rate, 80.0, 0.5f,
                                            static_cast<juce::int64> (index) * blockSize);
                processor.processBlock (block, midi);
                reductionDb = static_cast<double> (processor.getLowBandGainReductionDb());
            }

            reductions.push_back (reductionDb);
        }

        REQUIRE (reductions.size() == invarianceRates.size());

        const auto referenceIndex = 1; // 48 kHz
        const auto referenceReduction = reductions[static_cast<size_t> (referenceIndex)];

        // The measurement has to have teeth: if nothing is being reduced there
        // is nothing for a rate dependency to change.
        INFO ("detector " << detectorIndex << ", reduction at 48 kHz " << referenceReduction << " dB");
        CHECK (referenceReduction > 3.0);

        for (size_t index = 0; index < reductions.size(); ++index)
        {
            const auto deviation = reductions[index] - referenceReduction;

            INFO ("detector " << detectorIndex << " at " << invarianceRates[index]
                  << " Hz: " << reductions[index] << " dB vs 48 kHz " << referenceReduction
                  << " dB, deviation " << deviation << " dB, allowed " << ballisticsToleranceDb);

            CHECK (std::abs (deviation) <= ballisticsToleranceDb);
        }
    }
}
