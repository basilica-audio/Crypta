#include "dsp/Voicing.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

// MEASURED character evidence for the three Classic voicings - issues #15
// (Gnaw, op-amp hard clip), #16 (Wool, cascaded soft-clip fuzz with a mid
// scoop) and #17 (Razor, tight OD: pre-highpass, soft clip, mid hump).
//
// Why this file exists. VoicingTests.cpp already pins the behavioural
// contracts a voicing must not break (no NaN, bounded output, blend
// transparency, no coefficient-jump transient on a switch). What it does not
// do is assert that each voicing IS what its issue says it is - "hard clip",
// "mid scoop", "tight" were adjectives in the code comments and nowhere in the
// test suite, so nothing would have caught a voicing that quietly stopped
// being itself. Every claim below is a number with a tolerance.
//
// Method. A bin-centred sine through a standalone cryp::Voicing (fully wet, so
// the blend path cannot mask the measurement), 2^15-point FFT with the shared
// 4-term Blackman-Harris harness in TestHelpers.h, warm-up discarded. Harmonic
// levels are reported RELATIVE TO THE FUNDAMENTAL, which makes them
// independent of the stage's overall gain.
//
// What the numbers include. The measurement is taken at the stage output, so a
// voicing's character filter (Wool's scoop, Razor's hump) is in circuit
// alongside its shaper. That is deliberate: it is what the user hears. Where a
// filter works AGAINST an assertion it is called out at the assertion - e.g.
// Wool's 500 Hz scoop attenuates the very second harmonic the asymmetry test
// is looking for, which makes that test conservative rather than flattering.
namespace
{
    constexpr double characterSampleRate = 48000.0;
    constexpr int characterFftOrder = 15;
    constexpr int characterFftSize = 1 << characterFftOrder;
    constexpr int characterWarmUpSamples = 8192;
    constexpr int characterBlockSize = 2048;

    struct VoicingSetup
    {
        cryp::VoicingType voicing = cryp::VoicingType::gnaw;
        float drive01 = 0.5f;
        float tone01 = 1.0f;   // tone lowpass parked near its 15 kHz ceiling
        float tightHz = 20.0f; // Tight parked at its floor, out of the way
    };

    juce::AudioBuffer<float> renderThroughVoicing (const VoicingSetup& setup, double frequencyHz, float amplitude)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = characterSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (characterBlockSize);
        spec.numChannels = 1;

        cryp::Voicing voicing;
        voicing.prepare (spec, 1.0f); // fully wet
        voicing.setVoicing (setup.voicing);
        voicing.setDrive (setup.drive01);
        voicing.setTone (setup.tone01);
        voicing.setTightHz (setup.tightHz);

        const auto totalSamples = characterWarmUpSamples + characterFftSize;
        juce::AudioBuffer<float> buffer (1, totalSamples);

        for (int offset = 0; offset < totalSamples; offset += characterBlockSize)
        {
            const auto length = juce::jmin (characterBlockSize, totalSamples - offset);

            juce::AudioBuffer<float> block (1, length);
            TestHelpers::fillWithSine (block, characterSampleRate, frequencyHz, amplitude, offset);

            juce::dsp::AudioBlock<float> audioBlock (block);
            voicing.process (audioBlock);

            buffer.copyFrom (0, offset, block, 0, 0, length);
        }

        return buffer;
    }

    // Harmonic levels 1..8 in dB relative to the fundamental, plus the total
    // harmonic distortion (harmonics 2..8 against the fundamental).
    struct HarmonicProfile
    {
        std::array<double, 9> harmonicDb {}; // index 1..8 used
        double thdDb = -200.0;
    };

    HarmonicProfile measureHarmonics (const VoicingSetup& setup, double fundamentalHz, float amplitude)
    {
        const auto snapped = TestHelpers::snapToBin (fundamentalHz, characterSampleRate, characterFftSize);
        const auto rendered = renderThroughVoicing (setup, snapped, amplitude);
        const auto power = TestHelpers::powerSpectrum (rendered, 0, characterFftOrder, characterWarmUpSamples);

        HarmonicProfile profile;

        const auto fundamentalDb = TestHelpers::peakMagnitudeDb (power, characterSampleRate, characterFftSize, snapped);
        double harmonicPowerSum = 0.0;

        for (int harmonic = 1; harmonic <= 8; ++harmonic)
        {
            const auto frequency = snapped * static_cast<double> (harmonic);

            if (frequency >= characterSampleRate * 0.5)
            {
                profile.harmonicDb[static_cast<size_t> (harmonic)] = -200.0;
                continue;
            }

            const auto absoluteDb = TestHelpers::peakMagnitudeDb (power, characterSampleRate, characterFftSize, frequency);
            profile.harmonicDb[static_cast<size_t> (harmonic)] = absoluteDb - fundamentalDb;

            if (harmonic >= 2)
                harmonicPowerSum += std::pow (10.0, (absoluteDb - fundamentalDb) / 10.0);
        }

        profile.thdDb = 10.0 * std::log10 (juce::jmax (1.0e-30, harmonicPowerSum));
        return profile;
    }

    // Magnitude response of the stage at one frequency, in dB (output level at
    // that frequency minus input level at the same frequency). Driven at a low
    // amplitude with drive at its floor so every voicing's shaper is in its
    // linear region and what is left is the filtering.
    double measureResponseDb (cryp::VoicingType voicingType, double frequencyHz, float tightHz = 20.0f)
    {
        constexpr float probeAmplitude = 0.02f;

        const auto snapped = TestHelpers::snapToBin (frequencyHz, characterSampleRate, characterFftSize);

        VoicingSetup setup;
        setup.voicing = voicingType;
        setup.drive01 = 0.0f;
        setup.tone01 = 1.0f;
        setup.tightHz = tightHz;

        const auto rendered = renderThroughVoicing (setup, snapped, probeAmplitude);

        juce::AudioBuffer<float> reference (1, characterWarmUpSamples + characterFftSize);
        TestHelpers::fillWithSine (reference, characterSampleRate, snapped, probeAmplitude);

        const auto renderedPower = TestHelpers::powerSpectrum (rendered, 0, characterFftOrder, characterWarmUpSamples);
        const auto referencePower = TestHelpers::powerSpectrum (reference, 0, characterFftOrder, characterWarmUpSamples);

        return TestHelpers::peakMagnitudeDb (renderedPower, characterSampleRate, characterFftSize, snapped)
               - TestHelpers::peakMagnitudeDb (referencePower, characterSampleRate, characterFftSize, snapped);
    }
}

//==============================================================================
// #15 - Gnaw: op-amp hard clip.
TEST_CASE ("#15 Gnaw: the shaper is a symmetric hard clip - odd harmonics dominate, even harmonics are absent", "[voicing][character][gnaw]")
{
    // juce::jlimit(-1, 1, g*x) is an odd function, and an odd nonlinearity
    // generates ONLY odd harmonics. That is the measurable signature of "op-amp
    // hard clip" as opposed to any asymmetric or biased topology, and Gnaw's
    // character filter is the neutral one (0 dB), so nothing downstream muddies
    // the reading.
    const auto profile = measureHarmonics ({ cryp::VoicingType::gnaw, 0.5f, 1.0f, 20.0f }, 220.0, 0.5f);

    INFO ("H2 " << profile.harmonicDb[2] << " dBc, H3 " << profile.harmonicDb[3]
                 << " dBc, H5 " << profile.harmonicDb[5] << " dBc, THD " << profile.thdDb << " dB");

    // The odd series is strong and slowly decaying - the 1/n fall-off of a
    // clipped sine.
    CHECK (profile.harmonicDb[3] > -20.0);
    CHECK (profile.harmonicDb[5] > -30.0);

    // The even series is floor-level: 40 dB below its odd neighbours.
    CHECK (profile.harmonicDb[2] < profile.harmonicDb[3] - 40.0);
    CHECK (profile.harmonicDb[4] < profile.harmonicDb[3] - 40.0);

    // A hard clipper at half drive is a heavily distorted signal, not a
    // saturator's polish.
    CHECK (profile.thdDb > -16.0);
}

TEST_CASE ("#15 Gnaw: its character filter is genuinely neutral", "[voicing][character][gnaw]")
{
    // Gnaw is specified as the voicing WITHOUT a tonal fingerprint - all clip,
    // no EQ (gnawMidGainDb = 0). This is the null check for that claim: with
    // drive at its floor and Tight parked at 20 Hz, the stage must be flat
    // across the band the character filters of the other two voicings live in.
    for (const auto frequencyHz : { 300.0, 500.0, 900.0, 1500.0, 3000.0 })
    {
        const auto responseDb = measureResponseDb (cryp::VoicingType::gnaw, frequencyHz);
        INFO ("f = " << frequencyHz << " Hz, response " << responseDb << " dB");
        CHECK (responseDb == Catch::Approx (0.0).margin (0.5));
    }
}

//==============================================================================
// #16 - Wool: cascaded soft-clip fuzz, mid scoop.
TEST_CASE ("#16 Wool: the cascaded shaper is asymmetric - real even-harmonic content", "[voicing][character][wool]")
{
    // Wool's DC bias between its two tanh stages is what makes it a fuzz rather
    // than a symmetric overdrive: an asymmetric curve generates EVEN harmonics.
    // Measured against Gnaw, whose symmetric clip generates none.
    //
    // Conservative by construction: the second harmonic of a 220 Hz tone lands
    // at 440 Hz, i.e. right in Wool's own -6 dB mid scoop, so the asymmetry is
    // being measured through the filter that attenuates it most.
    const auto wool = measureHarmonics ({ cryp::VoicingType::wool, 0.5f, 1.0f, 20.0f }, 220.0, 0.5f);
    const auto gnaw = measureHarmonics ({ cryp::VoicingType::gnaw, 0.5f, 1.0f, 20.0f }, 220.0, 0.5f);

    INFO ("Wool H2 " << wool.harmonicDb[2] << " dBc, H3 " << wool.harmonicDb[3]
                      << " dBc, THD " << wool.thdDb << " dB; Gnaw H2 " << gnaw.harmonicDb[2] << " dBc");

    // Even harmonics are present at a level that is unambiguously signal, not
    // numerical floor...
    CHECK (wool.harmonicDb[2] > -40.0);

    // ...and enormously above the symmetric reference.
    CHECK (wool.harmonicDb[2] > gnaw.harmonicDb[2] + 30.0);

    // Cascaded soft clipping is still heavy distortion.
    CHECK (wool.thdDb > -22.0);
}

TEST_CASE ("#16 Wool: the mid scoop is real, and it is a scoop", "[voicing][character][wool]")
{
    // The specified character filter is a 500 Hz / -6 dB / Q 0.9 peak cut.
    // Measured against Gnaw's neutral filter at the same frequencies, so what
    // is left is the scoop itself rather than any shared stage.
    const auto scoopCentreDb = measureResponseDb (cryp::VoicingType::wool, 500.0)
                                - measureResponseDb (cryp::VoicingType::gnaw, 500.0);

    INFO ("scoop at 500 Hz: " << scoopCentreDb << " dB");
    CHECK (scoopCentreDb == Catch::Approx (-6.0).margin (0.75));

    // ...and it is local: an octave and a half either side of the centre the
    // cut has largely recovered, which is what makes it a scoop rather than a
    // tilt.
    const auto lowShoulderDb = measureResponseDb (cryp::VoicingType::wool, 125.0)
                                - measureResponseDb (cryp::VoicingType::gnaw, 125.0);
    const auto highShoulderDb = measureResponseDb (cryp::VoicingType::wool, 2000.0)
                                 - measureResponseDb (cryp::VoicingType::gnaw, 2000.0);

    INFO ("shoulders: 125 Hz " << lowShoulderDb << " dB, 2000 Hz " << highShoulderDb << " dB");
    CHECK (lowShoulderDb > -1.5);
    CHECK (highShoulderDb > -1.5);
}

//==============================================================================
// #17 - Razor: tight OD (pre-HPF, soft clip, mid hump).
TEST_CASE ("#17 Razor: the soft clip is milder than Gnaw's hard clip at equal drive", "[voicing][character][razor]")
{
    // "Tight overdrive" means the character comes from the filtering, not from
    // the shaper being savage: an 8x tanh against Gnaw's 40x hard clip.
    //
    // Measured at -20 dBFS rather than the -6 dBFS the other cases use. That
    // is the level the distinction actually lives at: a hard clipper's output
    // is level-INDEPENDENT once the input clears the ceiling (drive it twice as
    // hard and the harmonic ratios barely move - see the -6 dBFS rows of the
    // character table, where the two voicings converge to within 3 dB), while a
    // tanh's harmonic content keeps tracking the input. Asserting the mildness
    // claim at a hot level would therefore be asserting nothing much; at a
    // realistic playing level the gap is 20 dB.
    const auto razor = measureHarmonics ({ cryp::VoicingType::razor, 0.5f, 1.0f, 20.0f }, 220.0, 0.1f);
    const auto gnaw = measureHarmonics ({ cryp::VoicingType::gnaw, 0.5f, 1.0f, 20.0f }, 220.0, 0.1f);

    INFO ("Razor THD " << razor.thdDb << " dB, H3 " << razor.harmonicDb[3]
                        << " dBc; Gnaw THD " << gnaw.thdDb << " dB, H3 " << gnaw.harmonicDb[3] << " dBc");

    CHECK (razor.thdDb < gnaw.thdDb - 15.0);

    // A tanh is odd, so Razor is an odd-harmonic overdrive too - but a soft
    // one: the third harmonic sits well below where a hard clip puts it.
    CHECK (razor.harmonicDb[3] < gnaw.harmonicDb[3] - 15.0);

    // Still unmistakably a distortion, not a clean stage.
    CHECK (razor.harmonicDb[3] > -45.0);
}

TEST_CASE ("#17 Razor: the mid hump is real, and centred where it is specified", "[voicing][character][razor]")
{
    // Specified as a 900 Hz / +5 dB / Q 1.0 peak boost.
    const auto humpDb = measureResponseDb (cryp::VoicingType::razor, 900.0)
                         - measureResponseDb (cryp::VoicingType::gnaw, 900.0);

    INFO ("hump at 900 Hz: " << humpDb << " dB");
    CHECK (humpDb == Catch::Approx (5.0).margin (0.75));

    // The boost is a hump, not a shelf: two octaves down it is essentially
    // gone.
    const auto lowShoulderDb = measureResponseDb (cryp::VoicingType::razor, 225.0)
                                - measureResponseDb (cryp::VoicingType::gnaw, 225.0);

    INFO ("shoulder at 225 Hz: " << lowShoulderDb << " dB");
    CHECK (lowShoulderDb < 1.5);
}

TEST_CASE ("#17 Razor: the pre-drive highpass sits at the corner Tight asks for, at 12 dB/octave", "[voicing][character][razor][tight]")
{
    // The "pre-HPF" half of #17, promoted in v0.2.0 from a Razor-only fixed
    // 200 Hz constant into the voicing-independent Tight control. Two things
    // are asserted: the -3 dB point really is the requested corner, and the
    // rolloff really is the 2nd-order (12 dB/octave) slope its Q = 0.7071
    // Butterworth section implies.
    //
    // Measured on GNAW, not on Razor, even though the corner is #17's feature.
    // Tight is voicing-independent since v0.2.0 (VoicingTests.cpp asserts the
    // sweep behaves identically on all three voicings), and Gnaw is the one
    // voicing whose character filter is neutral - proven directly two test
    // cases above. Reading the corner through Razor's +5 dB / 900 Hz hump
    // instead costs about 1.2 dB of skew at the 2 kHz passband reference,
    // which would be measuring the hump, not the highpass.
    constexpr float tightHz = 100.0f;

    const auto passbandDb = measureResponseDb (cryp::VoicingType::gnaw, 2000.0, tightHz);
    const auto cornerDb = measureResponseDb (cryp::VoicingType::gnaw, 100.0, tightHz) - passbandDb;
    const auto octaveBelowDb = measureResponseDb (cryp::VoicingType::gnaw, 50.0, tightHz) - passbandDb;
    const auto twoOctavesBelowDb = measureResponseDb (cryp::VoicingType::gnaw, 25.0, tightHz) - passbandDb;

    INFO ("corner " << cornerDb << " dB, -1 oct " << octaveBelowDb << " dB, -2 oct " << twoOctavesBelowDb << " dB");

    // Butterworth highpass: -3 dB at the corner.
    CHECK (cornerDb == Catch::Approx (-3.0).margin (1.0));

    // 12 dB/octave in the stopband. Measured between the first and second
    // octave below the corner, where the response has settled into its
    // asymptote (the corner region itself is not yet on the asymptote).
    const auto slopePerOctaveDb = twoOctavesBelowDb - octaveBelowDb;
    INFO ("slope " << slopePerOctaveDb << " dB/octave");
    CHECK (slopePerOctaveDb == Catch::Approx (-12.0).margin (1.5));
}

//==============================================================================
TEST_CASE ("Voicing character table (measured, for the manual and the listening gate)", "[.character-table]")
{
    // Hidden by default (the leading dot): this prints the table that goes into
    // the PR body and docs/manual.md rather than asserting anything, so it is
    // run deliberately, not in CI.
    std::cout << "\nvoicing  drive  amp  H2(dBc)  H3(dBc)  H4(dBc)  H5(dBc)  THD(dB)\n";

    const std::array<std::pair<const char*, cryp::VoicingType>, 3> voicings {
        std::pair { "Gnaw ", cryp::VoicingType::gnaw },
        std::pair { "Wool ", cryp::VoicingType::wool },
        std::pair { "Razor", cryp::VoicingType::razor },
    };

    for (const auto& [name, voicingType] : voicings)
    {
        for (const auto drive : { 0.25f, 0.5f, 1.0f })
        for (const auto amplitude : { 0.1f, 0.5f })
        {
            const auto profile = measureHarmonics ({ voicingType, drive, 1.0f, 20.0f }, 220.0, amplitude);

            std::cout << name << "  " << std::fixed << std::setprecision (2) << drive << "  " << amplitude << "  ";

            for (int harmonic = 2; harmonic <= 5; ++harmonic)
                std::cout << std::setw (7) << std::setprecision (1) << profile.harmonicDb[static_cast<size_t> (harmonic)] << "  ";

            std::cout << std::setw (7) << profile.thdDb << "\n";
        }
    }

    std::cout << "\nvoicing  200Hz  500Hz  900Hz  2kHz (dB vs input, drive 0)\n";

    for (const auto& [name, voicingType] : voicings)
    {
        std::cout << name << "  ";

        for (const auto frequencyHz : { 200.0, 500.0, 900.0, 2000.0 })
            std::cout << std::setw (5) << std::setprecision (1) << measureResponseDb (voicingType, frequencyHz) << "  ";

        std::cout << "\n";
    }

    SUCCEED();
}
