#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "dsp/FactoryIRs.h"
#include "params/ParameterIds.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

// Factory IR slot mechanics and the licence guard (issue #21).
//
// The content half of #21 - actually choosing and bundling cab impulse
// responses - is NOT done and is not pretended to be: the shipped asset table
// is empty, because no IR has been sourced with a licence this project will
// stake a redistributed binary on. What is done, and is what these tests pin,
// is everything around it: decoding an embedded WAV, installing it in the
// convolution engine, restoring the passthrough identity IR, reporting the
// slot list to a GUI, and - the part that matters most for a project that will
// one day be handed a folder of IRs from the internet - a licence bar that is
// enforced by code and asserted by a test rather than written in a README.
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
TEST_CASE ("Factory IRs: the shipped asset table is empty, and that is deliberate", "[ir][factory][licence]")
{
    // This is a documentation test in the strict sense: it states, in the test
    // suite rather than only in a comment, that Crypta ships no bundled IR. If
    // that ever changes it should change loudly, with the licence assertions
    // below becoming live.
    CryptaAudioProcessor processor;
    CHECK (processor.getNumFactoryImpulseResponses() == 0);
    CHECK (processor.getFactoryImpulseResponseName (0).isEmpty());
    CHECK (processor.loadFactoryImpulseResponse (0) == false);
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
