#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "dsp/FactoryIRs.h"
#include "params/ParameterIds.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

// Factory IR slot mechanics, the licence guard, and the measured properties of
// every bundled impulse response (issues #21/#81).
//
// Crypta ships four bass cabinet IRs, and all four are GENERATED rather than
// recorded - computed by tools/ir-synth/cabsynth.py from a documented
// analytical cabinet model, so no third-party recording is redistributed inside
// the binary. See resources/irs/LICENSES.md.
//
// What these tests pin:
//   * the licence bar, enforced by code rather than by a README, so an IR added
//     later without provenance cannot silently reach a release build;
//   * that every shipped asset actually decodes through the plugin's own
//     decoder, at the expected rate, length and channel count;
//   * that every shipped asset is a *sane signal* - no clipping, no DC, no
//     non-finite sample, unity peak magnitude response, and a tail that has
//     faded to nothing rather than being cut off mid-ring. tools/ir-synth/
//     verify_irs.py measures the same properties from the files on disk; this
//     measures them again from the bytes embedded in the binary, which is the
//     copy that actually ships;
//   * that the convolution engine loads each one and that "None" restores the
//     bit-exact passthrough.

namespace
{
    constexpr double factoryIrSampleRate = 48000.0;
    constexpr int factoryIrBlockSize = 512;

    // A synthetic cab-like IR: a decaying, lowpassed burst. Distinctly not an
    // identity impulse, so a test can tell whether it was applied.
    juce::AudioBuffer<float> makeSyntheticIrBuffer (int numSamples = 256, int numChannels = 2)
    {
        juce::AudioBuffer<float> ir (numChannels, numSamples);
        float previous = 0.0f;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto excitation = (sample == 0 ? 1.0f : 0.0f)
                                     + 0.35f * std::sin (0.21f * static_cast<float> (sample))
                                           * std::exp (-static_cast<float> (sample) / 40.0f);
            previous = 0.6f * previous + 0.4f * excitation;

            for (int channel = 0; channel < numChannels; ++channel)
                ir.setSample (channel, sample, previous * std::exp (-static_cast<float> (sample) / 90.0f));
        }

        return ir;
    }

    // Encodes a buffer as a WAV in memory, i.e. exactly the byte layout a
    // BinaryData-embedded factory IR would have.
    juce::MemoryBlock encodeAsWav (const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        juce::MemoryBlock block;

        juce::WavAudioFormat format;
        auto stream = std::make_unique<juce::MemoryOutputStream> (block, false);

        const std::unique_ptr<juce::AudioFormatWriter> writer (
            format.createWriterFor (stream.get(), sampleRate, static_cast<unsigned int> (buffer.getNumChannels()), 24, {}, 0));

        REQUIRE (writer != nullptr);
        stream.release(); // the writer owns it now

        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        return block;
    }
}

//==============================================================================
TEST_CASE ("Factory IRs: the shipped table is the four generated bass cabinets", "[ir][factory][licence]")
{
    // The counterpart of the documentation test this replaces, which asserted
    // that the table was empty. It is no longer empty, and the assertion that
    // it is populated - with these names, in this order - is what a GUI slot
    // list and every saved preset referencing a slot index depend on.
    CryptaAudioProcessor processor;

    REQUIRE (processor.getNumFactoryImpulseResponses() == 4);

    const juce::StringArray expected {
        "Modelled 8x10 Cone",
        "Modelled 8x10 Edge",
        "Modelled 1x15 Vintage",
        "Modelled 4x10 Horn",
    };

    for (int index = 0; index < expected.size(); ++index)
        CHECK (processor.getFactoryImpulseResponseName (index) == expected[index]);

    // Out of range stays out of range: no name, no load, no crash.
    CHECK (processor.getFactoryImpulseResponseName (-1).isEmpty());
    CHECK (processor.getFactoryImpulseResponseName (4).isEmpty());
    CHECK (processor.loadFactoryImpulseResponse (-1) == false);
    CHECK (processor.loadFactoryImpulseResponse (4) == false);
}

TEST_CASE ("Factory IRs: every bundled name says 'Modelled'", "[ir][factory][licence]")
{
    // Not cosmetic. These are analytical models, not captures of real
    // cabinets, and the one way that becomes a real problem is a user reading
    // a slot list and assuming otherwise. The naming convention is therefore
    // load-bearing, and is asserted rather than left to review.
    for (const auto& asset : CryptaAudioProcessor::getFactoryIRAssetTable())
    {
        INFO ("asset: " << asset.name);
        CHECK (juce::String (asset.name).startsWith ("Modelled "));

        // "Self-recorded" would claim a capture that never happened. The
        // generated set is CC0, dedicated by its author.
        CHECK (juce::String (asset.licence) == juce::String ("CC0-1.0"));
        CHECK (juce::String (asset.source).contains ("cabsynth.py"));
    }
}

TEST_CASE ("Factory IRs: every table entry clears the licence bar", "[ir][factory][licence]")
{
    // The guard that matters. It is written so that it does real work the day
    // an IR is added: every entry must carry an approved licence, a non-empty
    // checkable source, a name and a payload - and the constructed library must
    // contain exactly as many assets as the table did, i.e. nothing was
    // silently dropped on the way in.
    const auto table = CryptaAudioProcessor::getFactoryIRAssetTable();

    for (const auto& asset : table)
    {
        REQUIRE (asset.name != nullptr);
        INFO ("asset: " << asset.name);

        CHECK (juce::String (asset.name).isNotEmpty());
        REQUIRE (asset.licence != nullptr);
        CHECK (cryp::isApprovedFactoryIRLicence (asset.licence));
        REQUIRE (asset.source != nullptr);
        CHECK (juce::String (asset.source).isNotEmpty());
        CHECK (asset.data != nullptr);
        CHECK (asset.dataSizeBytes > 0);
    }

    const cryp::FactoryIRLibrary library (table);
    CHECK (library.getNumRejectedAssets() == 0);
    CHECK (library.getNumAssets() == static_cast<int> (table.size()));
}

TEST_CASE ("Factory IRs: the licence allowlist accepts only verifiable licences", "[ir][factory][licence]")
{
    CHECK (cryp::isApprovedFactoryIRLicence ("CC0-1.0"));
    CHECK (cryp::isApprovedFactoryIRLicence ("Public Domain"));
    CHECK (cryp::isApprovedFactoryIRLicence ("Self-recorded"));

    // Everything that sounds permissive but is not a licence, or is a licence
    // with obligations that would follow the binary downstream.
    for (const auto* rejected : { "", "royalty free", "Royalty Free", "free for commercial use",
                                   "CC-BY-4.0", "CC BY-SA", "unknown", "Public domain (probably)" })
    {
        INFO ("licence string: '" << rejected << "'");
        CHECK (cryp::isApprovedFactoryIRLicence (rejected) == false);
    }
}

TEST_CASE ("Factory IRs: an asset without provenance is refused, not loaded", "[ir][factory][licence]")
{
    const auto wav = encodeAsWav (makeSyntheticIrBuffer(), factoryIrSampleRate);

    const std::vector<cryp::FactoryIRAsset> table {
        { "Verified cab", "CC0-1.0", "https://example.invalid/cc0-ir", wav.getData(), static_cast<int> (wav.getSize()) },
        { "Forum download", "royalty free", "someone's blog", wav.getData(), static_cast<int> (wav.getSize()) },
        { "No source", "CC0-1.0", "", wav.getData(), static_cast<int> (wav.getSize()) },
        { "No payload", "CC0-1.0", "https://example.invalid/cc0-ir", nullptr, 0 },
    };

    const cryp::FactoryIRLibrary library (table);

    CHECK (library.getNumAssets() == 1);
    CHECK (library.getNumRejectedAssets() == 3);
    CHECK (library.getName (0) == juce::String ("Verified cab"));
}

TEST_CASE ("Factory IRs: an embedded WAV decodes to the samples it was made from", "[ir][factory]")
{
    const auto source = makeSyntheticIrBuffer (192, 2);
    const auto wav = encodeAsWav (source, factoryIrSampleRate);

    juce::AudioBuffer<float> decoded;
    double decodedSampleRate = 0.0;

    REQUIRE (cryp::FactoryIRLibrary::decodeFromMemory (wav.getData(), wav.getSize(), decoded, decodedSampleRate));

    CHECK (decodedSampleRate == Catch::Approx (factoryIrSampleRate));
    CHECK (decoded.getNumChannels() == source.getNumChannels());
    CHECK (decoded.getNumSamples() == source.getNumSamples());

    // 24-bit storage, so the round-trip is accurate to about 1e-7 rather than
    // exact.
    for (int channel = 0; channel < source.getNumChannels(); ++channel)
        for (int sample = 0; sample < source.getNumSamples(); ++sample)
            REQUIRE (decoded.getSample (channel, sample) == Catch::Approx (source.getSample (channel, sample)).margin (1.0e-5));
}

TEST_CASE ("Factory IRs: garbage is refused rather than parsed", "[ir][factory][robustness]")
{
    juce::AudioBuffer<float> decoded;
    double decodedSampleRate = 0.0;

    CHECK (cryp::FactoryIRLibrary::decodeFromMemory (nullptr, 0, decoded, decodedSampleRate) == false);

    const char notAWav[] = "RIFFnot-a-wave-file-at-all........";
    CHECK (cryp::FactoryIRLibrary::decodeFromMemory (notAWav, sizeof (notAWav), decoded, decodedSampleRate) == false);
    CHECK (decodedSampleRate == 0.0);
}

TEST_CASE ("Factory IRs: a decoded IR can be installed and then cleared back to passthrough", "[ir][factory][processor]")
{
    // The slot mechanics end to end, through the public processor API a GUI IR
    // list would call: load -> the cab is audibly in circuit; clear ("None") ->
    // the plugin is back to its safe passthrough.
    const auto wav = encodeAsWav (makeSyntheticIrBuffer (256, 2), factoryIrSampleRate);

    juce::AudioBuffer<float> irBuffer;
    double irSampleRate = 0.0;
    REQUIRE (cryp::FactoryIRLibrary::decodeFromMemory (wav.getData(), wav.getSize(), irBuffer, irSampleRate));

    const auto renderThroughProcessor = [&] (bool loadIr, bool clearAfterwards)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, factoryIrSampleRate, factoryIrBlockSize);
        processor.prepareToPlay (factoryIrSampleRate, factoryIrBlockSize);

        // Isolate the IR stage: no drive, no dynamics, EQ off, IR fully wet.
        TestHelpers::setParameter (processor, ParamIDs::irEnabled, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::irMix, 100.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::midDrive, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::highDrive, 0.0f);

        if (loadIr)
            processor.loadImpulseResponse (irBuffer, irSampleRate);

        if (clearAfterwards)
            processor.clearImpulseResponse();

        juce::AudioBuffer<float> buffer (2, factoryIrBlockSize);
        juce::AudioBuffer<float> tail (2, factoryIrBlockSize);

        // juce::dsp::Convolution builds and installs a newly loaded IR on a
        // genuine background thread (JUCE 8.0.14, BackgroundMessageQueue,
        // ~10 ms polling) and only swaps it in on a later process() call - so
        // this is one of the few places where a test has to wait on wall-clock
        // time rather than simply calling process() in a tight loop. Same
        // reasoning, and the same 15 ms cadence, as tests/IRLoaderTests.cpp.
        for (int block = 0; block < 24; ++block)
        {
            juce::Thread::sleep (15);

            TestHelpers::fillWithSine (buffer, factoryIrSampleRate, 1200.0, 0.5f, block * factoryIrBlockSize);
            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
            tail.makeCopyOf (buffer);
        }

        return tail;
    };

    const auto passthrough = renderThroughProcessor (false, false);
    const auto withIr = renderThroughProcessor (true, false);
    const auto cleared = renderThroughProcessor (true, true);

    const auto differenceRms = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        juce::AudioBuffer<float> difference;
        difference.makeCopyOf (a);

        for (int channel = 0; channel < difference.getNumChannels(); ++channel)
            difference.addFrom (channel, 0, b, channel, 0, difference.getNumSamples(), -1.0f);

        return TestHelpers::rms (difference);
    };

    const auto referenceRms = TestHelpers::rms (passthrough);
    REQUIRE (referenceRms > 0.0);

    // Loading it changes the sound substantially...
    INFO ("IR difference " << differenceRms (withIr, passthrough) << " vs reference " << referenceRms);
    CHECK (differenceRms (withIr, passthrough) > 0.05 * referenceRms);

    // ...and clearing it puts the plugin back exactly where it started.
    INFO ("cleared difference " << differenceRms (cleared, passthrough));
    CHECK (differenceRms (cleared, passthrough) < 1.0e-6);
}

TEST_CASE ("Factory IRs: an over-long file is truncated rather than convolved whole", "[ir][factory][robustness]")
{
    // A cab IR is tens of milliseconds. The guard exists so a mis-selected
    // reverb impulse (or a malformed header claiming an enormous length) cannot
    // turn the cab stage into a multi-second convolution reverb with the CPU
    // and memory bill that implies.
    CHECK (cryp::FactoryIRLibrary::maximumImpulseResponseSamples == 960000);

    const auto wav = encodeAsWav (makeSyntheticIrBuffer (1024, 1), factoryIrSampleRate);

    juce::AudioBuffer<float> decoded;
    double decodedSampleRate = 0.0;
    REQUIRE (cryp::FactoryIRLibrary::decodeFromMemory (wav.getData(), wav.getSize(), decoded, decodedSampleRate));

    // Well under the ceiling, so nothing is truncated here - the assertion is
    // that a normal-length cab IR passes through untouched.
    CHECK (decoded.getNumSamples() == 1024);
}

//==============================================================================
// The bundled assets, measured as signals rather than trusted as files.
//
// tools/ir-synth/verify_irs.py measures the same properties from the .wav files
// on disk. These cases measure them again from the bytes BinaryData actually
// embedded in this binary, which is the copy a user gets - so a mangled asset,
// a stale BinaryData rebuild or a bad CMake entry fails the build rather than
// shipping.
namespace
{
    struct DecodedFactoryIr
    {
        juce::String name;
        juce::AudioBuffer<float> buffer;
        double sampleRate = 0.0;
    };

    std::vector<DecodedFactoryIr> decodeAllFactoryIrs()
    {
        const cryp::FactoryIRLibrary library (CryptaAudioProcessor::getFactoryIRAssetTable());

        std::vector<DecodedFactoryIr> decoded;

        for (int index = 0; index < library.getNumAssets(); ++index)
        {
            DecodedFactoryIr entry;
            entry.name = library.getName (index);

            if (library.decode (index, entry.buffer, entry.sampleRate))
                decoded.push_back (std::move (entry));
        }

        return decoded;
    }

    // Peak of |H(f)| over a 65536-point transform, and the DC term, both
    // relative to the same scale. Returned in linear magnitude.
    struct MagnitudeSummary
    {
        double peakResponse = 0.0;
        double dcResponse = 0.0;
    };

    MagnitudeSummary measureMagnitude (const juce::AudioBuffer<float>& buffer)
    {
        constexpr int fftOrder = 16;
        constexpr int fftSize = 1 << fftOrder;

        REQUIRE (buffer.getNumSamples() <= fftSize);

        juce::dsp::FFT fft (fftOrder);
        std::vector<float> scratch (static_cast<size_t> (fftSize) * 2, 0.0f);

        const auto* source = buffer.getReadPointer (0);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            scratch[static_cast<size_t> (i)] = source[i];

        fft.performFrequencyOnlyForwardTransform (scratch.data());

        MagnitudeSummary summary;
        for (int bin = 0; bin <= fftSize / 2; ++bin)
            summary.peakResponse = juce::jmax (summary.peakResponse,
                                                static_cast<double> (scratch[static_cast<size_t> (bin)]));

        // The DC term is |sum of samples|, computed in double rather than read
        // off the float FFT so the assertion is not limited by the transform's
        // own noise floor.
        double sum = 0.0;
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            sum += static_cast<double> (source[i]);

        summary.dcResponse = std::abs (sum);
        return summary;
    }
}

TEST_CASE ("Factory IRs: every bundled asset decodes to the documented format", "[ir][factory][content]")
{
    const auto decoded = decodeAllFactoryIrs();

    REQUIRE (decoded.size() == 4);

    for (const auto& ir : decoded)
    {
        INFO ("asset: " << ir.name);

        // Mono, 48 kHz, 4096 taps - resources/irs/LICENSES.md documents all
        // three, and a mismatch means the shipped audio is not the audio the
        // provenance record describes.
        CHECK (ir.sampleRate == Catch::Approx (48000.0));
        CHECK (ir.buffer.getNumChannels() == 1);
        CHECK (ir.buffer.getNumSamples() == 4096);
    }
}

TEST_CASE ("Factory IRs: no bundled asset clips, offsets or goes non-finite", "[ir][factory][content]")
{
    for (const auto& ir : decodeAllFactoryIrs())
    {
        INFO ("asset: " << ir.name);

        const auto* samples = ir.buffer.getReadPointer (0);
        const auto numSamples = ir.buffer.getNumSamples();

        double sum = 0.0;
        float peak = 0.0f;
        bool finite = true;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto value = samples[i];
            finite = finite && std::isfinite (value);
            peak = juce::jmax (peak, std::abs (value));
            sum += static_cast<double> (value);
        }

        CHECK (finite);

        // Nothing sitting on the full-scale code: an IR that clips in its own
        // file has already lost information before the convolver sees it.
        CHECK (peak < 1.0f);

        // A DC offset in an IR walks the convolved signal off centre and eats
        // headroom for nothing. The generator's high-pass alignment puts H(0)
        // at zero analytically; the measured residual after truncation is the
        // number that matters.
        CHECK (std::abs (sum / numSamples) < 1.0e-4);
    }
}

TEST_CASE ("Factory IRs: every bundled asset is normalised to unity peak response", "[ir][factory][content]")
{
    // The normalisation criterion is max |H(f)| == 1.0 rather than a peak-sample
    // or energy target, because it is the one that actually bounds the
    // convolution: with the maximum of the magnitude response at unity, no sine
    // at any frequency can leave the convolver louder than it entered, so the
    // IR alone cannot drive a full-scale input into clipping.
    for (const auto& ir : decodeAllFactoryIrs())
    {
        INFO ("asset: " << ir.name);

        const auto summary = measureMagnitude (ir.buffer);
        const auto peakDb = juce::Decibels::gainToDecibels (summary.peakResponse);

        CHECK (std::abs (peakDb) < 0.1f);

        // DC at least 60 dB below the response peak. Measured on the shipped
        // set: -69 dB (1x15, the lowest-tuned alignment) to -114 dB.
        const auto dcRelativeDb =
            juce::Decibels::gainToDecibels (summary.dcResponse / juce::jmax (1.0e-12, summary.peakResponse));

        INFO ("DC relative to peak: " << dcRelativeDb << " dB");
        CHECK (dcRelativeDb < -60.0);
    }
}

TEST_CASE ("Factory IRs: every bundled asset decays and closes rather than being cut off", "[ir][factory][content]")
{
    // A cabinet IR that is still ringing on its last sample convolves as a step
    // discontinuity, which reads as broadband splatter. The generator applies a
    // raised-cosine fade for exactly this reason; this asserts it worked on the
    // shipped bytes.
    for (const auto& ir : decodeAllFactoryIrs())
    {
        INFO ("asset: " << ir.name);

        const auto* samples = ir.buffer.getReadPointer (0);
        const auto numSamples = ir.buffer.getNumSamples();

        for (int i = numSamples - 8; i < numSamples; ++i)
            CHECK (std::abs (samples[i]) < 1.0e-6f);

        // Energy has collapsed into the head of the IR: the last tenth carries
        // essentially none of it.
        double totalEnergy = 0.0;
        double tailEnergy = 0.0;
        const auto tailStart = static_cast<int> (numSamples * 0.9);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto energy = static_cast<double> (samples[i]) * samples[i];
            totalEnergy += energy;

            if (i >= tailStart)
                tailEnergy += energy;
        }

        REQUIRE (totalEnergy > 0.0);
        CHECK (tailEnergy / totalEnergy < 1.0e-4);
    }
}

TEST_CASE ("Factory IRs: each bundled slot loads into the convolution engine and 'None' undoes it", "[ir][factory][processor][content]")
{
    // The end-to-end check the whole issue turns on: not "the file parses" but
    // "the convolution engine actually loads this one and it is audible", for
    // every slot, through the same public API a GUI IR list would call.
    const auto renderSlot = [] (int slotIndex, bool clearAfterwards)
    {
        CryptaAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, factoryIrSampleRate, factoryIrBlockSize);
        processor.prepareToPlay (factoryIrSampleRate, factoryIrBlockSize);

        TestHelpers::setParameter (processor, ParamIDs::irEnabled, 1.0f);
        TestHelpers::setParameter (processor, ParamIDs::irMix, 100.0f);
        TestHelpers::setParameter (processor, ParamIDs::eqEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::gateEnabled, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::midDrive, 0.0f);
        TestHelpers::setParameter (processor, ParamIDs::highDrive, 0.0f);

        if (slotIndex >= 0)
            REQUIRE (processor.loadFactoryImpulseResponse (slotIndex));

        if (clearAfterwards)
            processor.clearImpulseResponse();

        juce::AudioBuffer<float> buffer (2, factoryIrBlockSize);
        juce::AudioBuffer<float> tail (2, factoryIrBlockSize);

        // juce::dsp::Convolution (JUCE 8.0.14) prepares a newly loaded IR on
        // its own background thread and swaps it in on a later process() call,
        // so this waits on wall-clock time - same cadence as the case above and
        // as tests/IRLoaderTests.cpp.
        for (int block = 0; block < 24; ++block)
        {
            juce::Thread::sleep (15);

            TestHelpers::fillWithSine (buffer, factoryIrSampleRate, 900.0, 0.5f, block * factoryIrBlockSize);
            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
            tail.makeCopyOf (buffer);
        }

        return tail;
    };

    const auto differenceRms = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        juce::AudioBuffer<float> difference;
        difference.makeCopyOf (a);

        for (int channel = 0; channel < difference.getNumChannels(); ++channel)
            difference.addFrom (channel, 0, b, channel, 0, difference.getNumSamples(), -1.0f);

        return TestHelpers::rms (difference);
    };

    const auto passthrough = renderSlot (-1, false);
    const auto referenceRms = TestHelpers::rms (passthrough);
    REQUIRE (referenceRms > 0.0);

    const auto numSlots = CryptaAudioProcessor().getNumFactoryImpulseResponses();
    REQUIRE (numSlots == 4);

    for (int slot = 0; slot < numSlots; ++slot)
    {
        INFO ("factory slot " << slot);

        const auto engaged = renderSlot (slot, false);

        // Every slot must be audibly in circuit - a silently-failing load that
        // left the identity IR in place would otherwise pass unnoticed.
        CHECK (std::isfinite (TestHelpers::rms (engaged)));
        CHECK (differenceRms (engaged, passthrough) > 0.05 * referenceRms);

        // ...and "None" puts the plugin back exactly where it started.
        const auto cleared = renderSlot (slot, true);
        CHECK (differenceRms (cleared, passthrough) < 1.0e-6);
    }
}

TEST_CASE ("Factory IRs: the bundled voicings are measurably different from one another", "[ir][factory][content]")
{
    // Four slots that all sounded the same would be a content failure that
    // every mechanical test above would happily pass. This pins the actual
    // voicing spread: the octave-band responses recorded in
    // resources/irs/LICENSES.md differ, and by how much.
    const auto decoded = decodeAllFactoryIrs();
    REQUIRE (decoded.size() == 4);

    // Octave-band magnitude, in dB relative to the IR's own response peak -
    // the same quantity as the octave table in resources/irs/LICENSES.md, and
    // computed the same way: an RMS over the FFT bins inside the band.
    //
    // Deliberately NOT measured by running the IR through a second-order
    // band-pass, which was the first attempt here. A biquad band-pass has
    // 6 dB/octave skirts, so at 31.5 Hz most of what it integrates is leakage
    // from the 100-500 Hz region where a cabinet IR keeps nearly all of its
    // energy; two cabinets 9 dB apart down there measured 3 dB apart, because
    // the measurement was mostly reporting the octaves it was supposed to
    // reject. Selecting bins is exact and has no skirt at all.
    const auto octaveBandDb = [] (const juce::AudioBuffer<float>& buffer, double centreHz)
    {
        constexpr int fftOrder = 16;
        constexpr int fftSize = 1 << fftOrder;

        REQUIRE (buffer.getNumSamples() <= fftSize);

        juce::dsp::FFT fft (fftOrder);
        std::vector<float> scratch (static_cast<size_t> (fftSize) * 2, 0.0f);

        const auto* source = buffer.getReadPointer (0);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            scratch[static_cast<size_t> (i)] = source[i];

        fft.performFrequencyOnlyForwardTransform (scratch.data());

        const auto binHz = factoryIrSampleRate / static_cast<double> (fftSize);
        const auto halfWidth = std::sqrt (2.0); // one octave

        const auto lowBin = juce::jmax (0, static_cast<int> (std::floor ((centreHz / halfWidth) / binHz)));
        const auto highBin = juce::jmin (fftSize / 2,
                                          static_cast<int> (std::ceil ((centreHz * halfWidth) / binHz)));

        REQUIRE (highBin >= lowBin);

        double bandEnergy = 0.0;
        double peak = 0.0;

        for (int bin = 0; bin <= fftSize / 2; ++bin)
        {
            const auto magnitude = static_cast<double> (scratch[static_cast<size_t> (bin)]);
            peak = juce::jmax (peak, magnitude);

            if (bin >= lowBin && bin <= highBin)
                bandEnergy += magnitude * magnitude;
        }

        const auto bandRms = std::sqrt (bandEnergy / (highBin - lowBin + 1));
        return juce::Decibels::gainToDecibels (bandRms / juce::jmax (1.0e-12, peak), -120.0);
    };

    // The 1x15 is the dark one and the 4x10-plus-horn is the bright one; that
    // is the whole reason both are in the set.
    const auto dark = std::find_if (decoded.begin(), decoded.end(),
                                     [] (const auto& ir) { return ir.name.contains ("1x15"); });
    const auto bright = std::find_if (decoded.begin(), decoded.end(),
                                       [] (const auto& ir) { return ir.name.contains ("Horn"); });

    REQUIRE (dark != decoded.end());
    REQUIRE (bright != decoded.end());

    const auto darkTop = octaveBandDb (dark->buffer, 8000.0);
    const auto brightTop = octaveBandDb (bright->buffer, 8000.0);

    INFO ("1x15 at 8 kHz: " << darkTop << " dB, 4x10+horn at 8 kHz: " << brightTop << " dB");
    CHECK (brightTop > darkTop + 6.0);

    // Cone vs edge on the same cabinet: same low end, less top from the edge.
    const auto cone = std::find_if (decoded.begin(), decoded.end(),
                                     [] (const auto& ir) { return ir.name.contains ("8x10 Cone"); });
    const auto edge = std::find_if (decoded.begin(), decoded.end(),
                                     [] (const auto& ir) { return ir.name.contains ("8x10 Edge"); });

    REQUIRE (cone != decoded.end());
    REQUIRE (edge != decoded.end());

    const auto coneTop = octaveBandDb (cone->buffer, 4000.0);
    const auto edgeTop = octaveBandDb (edge->buffer, 4000.0);

    INFO ("8x10 cone at 4 kHz: " << coneTop << " dB, 8x10 edge at 4 kHz: " << edgeTop << " dB");
    CHECK (coneTop > edgeTop + 3.0);
}
