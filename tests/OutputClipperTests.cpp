#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "dsp/OutputClipper.h"
#include "params/ParameterIds.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

// The v0.3.0 safety clip (brief §6 T13).
//
// The interesting assertions here are the transparency ones. It is easy to
// write an antialiased clipper that also quietly lowpasses everything passing
// through it - that is exactly what naive ADAA-1 does at base rate, and it is
// the defect the delta form exists to avoid. So the tests below deliberately
// spend most of their effort on what the clipper does when it is NOT clipping.
namespace
{
    constexpr double clipperSampleRate = 48000.0;

    void configureClipTest (CryptaAudioProcessor& processor, bool clipEnabled, float ceilingDb)
    {
        processor.setPlayConfigDetails (1, 1, clipperSampleRate, 512);
        processor.prepareToPlay (clipperSampleRate, 512);

        // Everything except the clip out of the way, so the measurement is of
        // the clipper alone.
        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::irEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::midDrive, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::highDrive, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::outputClip, clipEnabled ? 1.0f : 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::clipCeiling, ceilingDb);

        processor.reset();
    }
}

TEST_CASE ("T13: the clipper honours its ceiling", "[clipper]")
{
    cryp::OutputClipper clipper;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = clipperSampleRate;
    spec.maximumBlockSize = 512;
    spec.numChannels = 1;
    clipper.prepare (spec);

    for (const auto ceilingDb : { 0.0f, -3.0f, -6.0f, -12.0f })
    {
        clipper.setCeilingDb (ceilingDb);
        clipper.reset();

        const auto ceiling = juce::Decibels::decibelsToGain (ceilingDb);

        // +6 dBFS of 1 kHz, i.e. comfortably over every ceiling tested.
        juce::AudioBuffer<float> buffer (1, 4800);
        TestHelpers::fillWithSine (buffer, clipperSampleRate, 1000.0, 2.0f);

        juce::dsp::AudioBlock<float> block (buffer);
        clipper.process (block);

        const auto peak = buffer.getMagnitude (0, buffer.getNumSamples());

        INFO ("ceiling " << ceilingDb << " dBFS (" << ceiling << " linear), peak " << peak);
        CHECK (peak <= ceiling * 1.012f);
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

TEST_CASE ("T13: the clipper is transparent below its ceiling", "[clipper]")
{
    // The assertion that guards the delta form. A naive ADAA-1 of the full
    // clipping curve degenerates to a two-tap average on sub-ceiling material
    // - about -8.3 dB at 18 kHz at this sample rate, plus half a sample of
    // delay, applied to the whole mix whenever the clip is merely ARMED. The
    // residual form is transparent by construction instead, and this is what
    // would catch a regression back to the naive form.
    cryp::OutputClipper clipper;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = clipperSampleRate;
    spec.maximumBlockSize = 512;
    spec.numChannels = 1;
    clipper.prepare (spec);
    clipper.setCeilingDb (0.0f);

    constexpr int fftOrder = 16;
    constexpr int fftSize = 1 << fftOrder;

    SECTION ("magnitude response is flat to 20 kHz")
    {
        // What is asserted is FLATNESS, not unity. A tanh-based soft clip is
        // never exactly unity - at -12 dBFS the residual x^3/3 already costs
        // about 0.13 dB - and that is the soft knee doing its job, not a
        // defect. The defect this guards against is frequency-DEPENDENT loss:
        // naive ADAA-1 would show roughly -8 dB at 18 kHz here while leaving
        // 40 Hz untouched, so the spread across the band is the tell.
        double minimumDeviation = 1.0e9;
        double maximumDeviation = -1.0e9;

        for (const auto frequency : { 40.0, 1000.0, 8000.0, 14000.0, 18000.0, 20000.0 })
        {
            const auto snapped = TestHelpers::snapToBin (frequency, clipperSampleRate, fftSize);

            // -12 dBFS: far enough below the ceiling that the residual is
            // negligible, which is the ordinary operating condition.
            juce::AudioBuffer<float> processed (1, fftSize);
            TestHelpers::fillWithSine (processed, clipperSampleRate, snapped, 0.25f);

            juce::AudioBuffer<float> reference (1, fftSize);
            reference.makeCopyOf (processed);

            clipper.reset();
            juce::dsp::AudioBlock<float> block (processed);
            clipper.process (block);

            const auto processedDb = TestHelpers::peakMagnitudeDb (
                TestHelpers::powerSpectrum (processed, 0, fftOrder), clipperSampleRate, fftSize, snapped);
            const auto referenceDb = TestHelpers::peakMagnitudeDb (
                TestHelpers::powerSpectrum (reference, 0, fftOrder), clipperSampleRate, fftSize, snapped);

            const auto deviation = processedDb - referenceDb;
            minimumDeviation = juce::jmin (minimumDeviation, deviation);
            maximumDeviation = juce::jmax (maximumDeviation, deviation);

            INFO ("at " << frequency << " Hz: " << deviation << " dB");

            // No individual point may be far off either.
            CHECK (std::abs (deviation) <= 0.25);
        }

        // Measured spread: 0.13 dB across 40 Hz - 20 kHz, against the 8.3 dB
        // of droop the naive form would produce at 18 kHz alone. The small
        // residual tilt is the compensator's own first-difference term acting
        // on the (tiny) sub-ceiling residual, and it runs in the opposite
        // direction to the soft knee, so the top octave comes out marginally
        // CLOSER to unity than the bottom.
        INFO ("deviation spread across 40 Hz - 20 kHz: " << (maximumDeviation - minimumDeviation) << " dB");
        CHECK ((maximumDeviation - minimumDeviation) <= 0.2);
    }

    SECTION ("broadband material nulls against its own input")
    {
        // Catches the half-sample delay as well as the droop: a time-domain
        // null is sensitive to both.
        juce::AudioBuffer<float> processed (1, 24000);
        std::uint32_t lcg = 0x5EEDu;

        for (int sample = 0; sample < processed.getNumSamples(); ++sample)
        {
            lcg = lcg * 1664525u + 1013904223u;
            // -30 dBFS. The null floor here is set by tanh's own third-order
            // term (x^2/3 relative), so the level has to be stated for the
            // target to mean anything: at -30 dBFS that term sits near
            // -70 dB, leaving room for the -60 dB contract.
            const auto value = static_cast<double> (lcg >> 8) / static_cast<double> (1u << 24) * 2.0 - 1.0;
            processed.setSample (0, sample, static_cast<float> (value * 0.03));
        }

        juce::AudioBuffer<float> reference (1, processed.getNumSamples());
        reference.makeCopyOf (processed);

        clipper.reset();
        juce::dsp::AudioBlock<float> block (processed);
        clipper.process (block);

        double differenceSquares = 0.0;
        double signalSquares = 0.0;

        for (int sample = 0; sample < processed.getNumSamples(); ++sample)
        {
            const auto difference = static_cast<double> (processed.getSample (0, sample))
                                     - static_cast<double> (reference.getSample (0, sample));
            differenceSquares += difference * difference;
            signalSquares += static_cast<double> (reference.getSample (0, sample))
                              * static_cast<double> (reference.getSample (0, sample));
        }

        const auto nullDb = 10.0 * std::log10 (juce::jmax (1.0e-30, differenceSquares / signalSquares));
        INFO ("sub-ceiling null: " << nullDb << " dB");
        CHECK (nullDb <= -60.0);
    }
}

TEST_CASE ("T13: the clipper aliases less than the plain tanh it replaces", "[clipper][aliasing]")
{
    constexpr int fftOrder = 16;
    constexpr int fftSize = 1 << fftOrder;

    const auto tone = TestHelpers::snapToBin (5000.0, clipperSampleRate, fftSize);

    // +2.3 dB over the ceiling: a realistic accidental over, which is what a
    // SAFETY clip is for. (At extreme overdrive - 10 dB or more past the
    // ceiling - the hard bound that guarantees the ceiling engages on most
    // samples and the antialiasing advantage goes away. That is a deliberate
    // ordering of priorities: a safety clip that lets 15 % through is not a
    // safety clip, and heavy clipping belongs in the drive stages, which are
    // oversampled and ADAA'd for exactly that purpose.)
    juce::AudioBuffer<float> antialiased (1, fftSize);
    TestHelpers::fillWithSine (antialiased, clipperSampleRate, tone, 1.3f);

    juce::AudioBuffer<float> plain (1, fftSize);
    plain.makeCopyOf (antialiased);

    // v0.2.0's clipper, verbatim.
    for (int sample = 0; sample < fftSize; ++sample)
        plain.setSample (0, sample, std::tanh (plain.getSample (0, sample)));

    cryp::OutputClipper clipper;
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = clipperSampleRate;
    spec.maximumBlockSize = 512;
    spec.numChannels = 1;
    clipper.prepare (spec);
    clipper.setCeilingDb (0.0f);

    juce::dsp::AudioBlock<float> block (antialiased);
    clipper.process (block);

    const auto plainDb = TestHelpers::aliasToSignalRatioDb (
        TestHelpers::powerSpectrum (plain, 0, fftOrder), clipperSampleRate, fftSize, tone);
    const auto adaaDb = TestHelpers::aliasToSignalRatioDb (
        TestHelpers::powerSpectrum (antialiased, 0, fftOrder), clipperSampleRate, fftSize, tone);

    INFO ("plain tanh " << plainDb << " dB, delta-form ADAA " << adaaDb << " dB");
    CHECK (adaaDb <= plainDb - 12.0);
}

TEST_CASE ("T13: the clipper survives NaN and Inf", "[clipper][robustness]")
{
    // The ADAA core carries its previous input in state, so a single NaN
    // reaching it would poison every subsequent sample, not just its own.
    cryp::OutputClipper clipper;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = clipperSampleRate;
    spec.maximumBlockSize = 512;
    spec.numChannels = 1;
    clipper.prepare (spec);
    clipper.setCeilingDb (0.0f);

    juce::AudioBuffer<float> buffer (1, 512);
    TestHelpers::fillWithSine (buffer, clipperSampleRate, 1000.0, 0.5f);

    buffer.setSample (0, 10, std::numeric_limits<float>::quiet_NaN());
    buffer.setSample (0, 20, std::numeric_limits<float>::infinity());
    buffer.setSample (0, 30, -std::numeric_limits<float>::infinity());
    buffer.setSample (0, 40, 1.0e30f);

    juce::dsp::AudioBlock<float> block (buffer);
    clipper.process (block);

    CHECK (TestHelpers::allSamplesFinite (buffer));
    CHECK (buffer.getMagnitude (0, buffer.getNumSamples()) <= 1.012f);
}

TEST_CASE ("T13: turning the clip off is a bit-exact bypass", "[clipper]")
{
    // The off state must cost nothing and change nothing - it is skipped
    // entirely rather than run at a transparent setting.
    CryptaAudioProcessor clipped;
    configureClipTest (clipped, false, 0.0f);

    juce::AudioBuffer<float> buffer (1, 4096);
    TestHelpers::fillWithSine (buffer, clipperSampleRate, 220.0, 0.4f);

    juce::AudioBuffer<float> reference (1, 4096);
    reference.makeCopyOf (buffer);

    TestHelpers::renderThrough (clipped, buffer);

    CryptaAudioProcessor untouched;
    configureClipTest (untouched, false, -12.0f); // a different ceiling, which must be unread
    TestHelpers::renderThrough (untouched, reference);

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        REQUIRE (buffer.getSample (0, sample) == reference.getSample (0, sample));
}

TEST_CASE ("T13: the Clip Ceiling parameter reaches the audio", "[clipper]")
{
    // End-to-end: the parameter is wired, and a lower ceiling really does
    // produce a lower peak.
    const auto peakFor = [] (float ceilingDb)
    {
        CryptaAudioProcessor processor;
        configureClipTest (processor, true, ceilingDb);
        TestHelpers::setParameter (processor, ParamIDs::inputGain, 18.0f);

        juce::AudioBuffer<float> buffer (1, 8192);
        TestHelpers::fillWithSine (buffer, clipperSampleRate, 220.0, 0.9f);
        TestHelpers::renderThrough (processor, buffer);

        // Skip the smoothing ramps.
        return buffer.getMagnitude (4096, 4096);
    };

    const auto atUnity = peakFor (0.0f);
    const auto atMinusSix = peakFor (-6.0f);

    INFO ("peak at 0 dBFS ceiling " << atUnity << ", at -6 dBFS " << atMinusSix);

    CHECK (atUnity <= 1.012f);
    CHECK (atMinusSix <= juce::Decibels::decibelsToGain (-6.0f) * 1.012f);
    CHECK (atMinusSix < atUnity);
}
