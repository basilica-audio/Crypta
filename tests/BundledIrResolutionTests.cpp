#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "ir/BundledIrSource.h"
#include "ir/FactoryIrLibrary.h"
#include "ir/IrLibrary.h"
#include "params/ParameterIds.h"
#include "presets/IrReference.h"
#include "presets/PresetManager.h"

#include <BinaryData.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

// Resolving a preset's IR reference, from the user's library and from the
// EMBEDDED bundle (issue #111, adopting the model basilica-audio/Nave#45
// settled). The decisions these tests encode, so a failure here reads as a
// product regression rather than a mechanical one:
//
//   D1  The reference is OPTIONAL and BY CONTENT HASH. A preset without one
//       (every pre-#111 preset) loads exactly as before; nothing anywhere
//       resolves an IR by name or id, because a name lookup is the
//       silent-wrong-sound failure the hash exists to prevent.
//
//   D4  The embedded bytes ARE a resolution source. A preset that references
//       one of Crypta's own cabinets resolves on a machine whose IR folder
//       is empty - which is every machine out of the box.
//
//   D5  The user's library comes FIRST, the embedded copy only after it. The
//       ordering picks which FILE the slot points at and never which SOUND
//       comes out, because a digest can only match bytes equal to it: when
//       both sources hold the hash they hold the same audio.
//
//   D6  The bundle is a resolution source, NOT a library. It is keyed by
//       digest alone - never scanned, never listed - so the only thing it
//       can ever answer is an explicit reference to specific bytes, and its
//       cache folder is never a search root.
//
//   D7  The lookup is TOTAL. Every digest lands on exactly one outcome
//       (notReferenced / alreadyLoaded / library / bundled / notFound), and
//       the not-found outcome performs NO audio operation at all: the slot
//       keeps the samples it had, which is why it cannot produce a NaN, a
//       full-scale click, or silence where there was signal.
//
//   D8  There is still exactly ONE decode path. The embedded bytes are
//       written to a real file and loaded through the same
//       loadImpulseResponseFromFile() a library resolution uses, which
//       funnels into the same cryp::FactoryIRLibrary::decodeFromMemory()
//       every embedded factory slot uses - so the two resolution sources
//       cannot drift into sounding different.
//
// (D2/D3 numbering follows Nave's suite; the missing numbers are Nave's
// two-slot specifics, which have no Crypta counterpart.)
//
// NOTHING HERE IS WALL-CLOCK SENSITIVE. Where a test needs to prove the
// resolution reached the DSP it compares the buffer the processor recorded at
// load time (CryptaAudioProcessor::getLoadedImpulseResponse), which is set
// synchronously - unlike the audible result of a juce::dsp::Convolution load,
// which only appears once its background preparation completes and would need
// a sleep to observe.
namespace
{
    using basilica::ir::BundledIrSource;
    using basilica::ir::FactoryIrAsset;
    using basilica::presets::PresetManager;
    using basilica::presets::PresetManagerConfig;

    struct ScopedTestDirectory
    {
        explicit ScopedTestDirectory (const juce::String& label)
            : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("CryptaBundledIrResolutionTests")
                       .getChildFile (label + "_"
                                       + juce::String (juce::Time::getHighResolutionTicks())
                                       + "_" + juce::String (juce::Random::getSystemRandom().nextInt (1000000))))
        {
            dir.createDirectory();
        }

        ~ScopedTestDirectory() { dir.deleteRecursively(); }

        JUCE_DECLARE_NON_COPYABLE (ScopedTestDirectory)

        juce::File dir;
    };

    const std::vector<FactoryIrAsset>& assets() { return crypta::factoryIrAssets(); }

    const BundledIrSource& source()
    {
        static const BundledIrSource instance { assets() };
        return instance;
    }

    const FactoryIrAsset& assetNamed (const juce::String& fileName)
    {
        for (const auto& asset : assets())
            if (fileName == asset.fileName)
                return asset;

        FAIL ("no embedded asset named " << fileName.toStdString());
        return assets().front();
    }

    juce::String digestOf (const juce::String& fileName)
    {
        const auto digest = BundledIrSource::contentHashOf (assetNamed (fileName));
        REQUIRE (digest.length() == 64);
        return digest;
    }

    PresetManagerConfig makeIsolatedConfig (const juce::File& userPresetDir)
    {
        PresetManagerConfig config;
        config.pluginId = "com.yvesvogl.crypta";
        config.pluginName = "Crypta";
        config.manufacturerName = "Yves Vogl";
        config.pluginVersion = "0.3.0-test";
        config.userPresetsDirectoryOverrideForTests = userPresetDir;
        return config;
    }

    std::vector<basilica::presets::FactoryPresetAsset> factoryPresetAssets()
    {
        return {
            { BinaryData::default_json, BinaryData::default_jsonSize },
            { BinaryData::glueAndGrind_json, BinaryData::glueAndGrind_jsonSize },
            { BinaryData::cabColoredGrind_json, BinaryData::cabColoredGrind_jsonSize },
        };
    }

    // Every test that can reach the embedded source has to be told where to
    // write, or it would materialise into the developer's (and CI's) real
    // ~/Library|%APPDATA% location - the same reason the preset tests
    // override the user preset directory.
    void useFolders (CryptaAudioProcessor& processor,
                     const juce::File& libraryFolder,
                     const juce::File& cacheFolder)
    {
        processor.apvts.state.setProperty (ParamIDs::irLibraryFolderProperty,
                                           libraryFolder.getFullPathName(), nullptr);
        processor.setBundledIrCacheDirectoryForTests (cacheFolder);
        processor.refreshIrSearchRoots();
    }

    bool buffersIdentical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
            return false;

        for (int channel = 0; channel < a.getNumChannels(); ++channel)
            for (int sample = 0; sample < a.getNumSamples(); ++sample)
                if (a.getReadPointer (channel)[sample] != b.getReadPointer (channel)[sample])
                    return false;

        return true;
    }

    bool writeWavFile (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        file.deleteFile();

        juce::WavAudioFormat format;
        auto stream = file.createOutputStream();

        if (stream == nullptr)
            return false;

        const std::unique_ptr<juce::AudioFormatWriter> writer (
            format.createWriterFor (stream.get(), sampleRate,
                                    static_cast<unsigned int> (buffer.getNumChannels()), 24, {}, 0));

        if (writer == nullptr)
            return false;

        stream.release(); // the writer owns it now

        return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    juce::AudioBuffer<float> readWav (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        REQUIRE (reader != nullptr);

        juce::AudioBuffer<float> buffer (juce::jlimit (1, 2, static_cast<int> (reader->numChannels)),
                                          static_cast<int> (reader->lengthInSamples));
        reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
        return buffer;
    }

    bool fileHoldsAssetBytes (const juce::File& file, const FactoryIrAsset& asset)
    {
        juce::MemoryBlock onDisk;

        if (! file.loadFileAsData (onDisk))
            return false;

        return onDisk.getSize() == static_cast<size_t> (asset.dataSize)
            && std::memcmp (onDisk.getData(), asset.data, onDisk.getSize()) == 0;
    }

    juce::String makePresetFile (const juce::String& name, const juce::String& irJson)
    {
        juce::String text;
        text << "{\n";
        text << "  \"format\": \"basilica-preset-1\",\n";
        text << "  \"plugin\": \"com.yvesvogl.crypta\",\n";
        text << "  \"pluginVersion\": \"0.3.0\",\n";
        text << "  \"name\": \"" << name << "\",\n";
        text << "  \"category\": \"Bass\",\n";
        text << "  \"ir\": " << irJson << ",\n";
        text << "  \"parameters\": " << R"({ "splitLowHz": 120.0, "splitHighHz": 600.0,
             "midDrive": 40.0, "irEnabled": 1.0, "irMix": 70.0 })" << "\n";
        text << "}\n";
        return text;
    }

    juce::String irJsonFor (const juce::String& hash, const juce::String& name)
    {
        return "{ \"a\": { \"sha256\": \"" + hash + "\", \"name\": \"" + name + "\" } }";
    }

    // Writes a preset carrying `irJson` into `presetDir` and loads it through
    // the real production path.
    void loadPresetWithReference (PresetManager& manager,
                                  const juce::File& presetDir,
                                  const juce::String& name,
                                  const juce::String& irJson)
    {
        const auto file = presetDir.getChildFile (name + PresetManager::presetFileExtension);
        REQUIRE (file.replaceWithText (makePresetFile (name, irJson)));
        REQUIRE (manager.loadPreset (name));
    }

    // A copy of one embedded asset written under an arbitrary name, so a test
    // can put the bundled BYTES in the library without the bundled NAME.
    juce::File copyAssetInto (const juce::File& folder,
                              const FactoryIrAsset& asset,
                              const juce::String& fileName)
    {
        const auto file = folder.getChildFile (fileName);
        REQUIRE (file.replaceWithData (asset.data, static_cast<size_t> (asset.dataSize)));
        return file;
    }
}

//==============================================================================
// D1 - the reference layer itself.

TEST_CASE ("Bundled IR resolution: the content hash is stable and depends on bytes, not identity",
           "[ir][bundled][presets]")
{
    ScopedTestDirectory scratch ("hash-layer");

    juce::AudioBuffer<float> ir (1, 64);
    ir.clear();
    ir.setSample (0, 0, 1.0f);
    ir.setSample (0, 13, -0.25f);

    const auto fileA = scratch.dir.getChildFile ("cab.wav");
    REQUIRE (writeWavFile (fileA, ir, 48000.0));

    const auto hashA = basilica::presets::contentHashOfFile (fileA);
    REQUIRE (hashA.length() == 64);

    // Same bytes under another name: same digest. The identity is the bytes.
    const auto fileB = scratch.dir.getChildFile ("same bytes, other name.wav");
    REQUIRE (fileA.copyFileTo (fileB));
    CHECK (basilica::presets::contentHashOfFile (fileB) == hashA);

    // Different bytes under the same name: different digest.
    ir.setSample (0, 13, -0.26f);
    REQUIRE (writeWavFile (fileA, ir, 48000.0));
    CHECK (basilica::presets::contentHashOfFile (fileA) != hashA);

    // A file that is not there hashes to nothing, not to something.
    CHECK (basilica::presets::contentHashOfFile (scratch.dir.getChildFile ("gone.wav")).isEmpty());
}

TEST_CASE ("Bundled IR resolution: the bundled digests match the committed manifest",
           "[ir][bundled][content]")
{
    // The digest a preset records for a bundled cabinet is the digest
    // resources/irs/manifest.json prints - so a factory preset's reference
    // can be pasted straight out of the manifest and checked by eye. Pinned
    // against the literal manifest values rather than recomputed from the
    // repo files, so a regenerated (retuned!) source tree fails here instead
    // of being silently re-blessed.
    CHECK (digestOf ("modelled_8x10_cone.wav")
           == "67aa93d0e238e88b74b455c3b5322d455d61c007620ace2d2cb88b54c6a793e2");
    CHECK (digestOf ("modelled_8x10_edge.wav")
           == "551d27888c679e864342d70a76c0298a8dbbaf21cb33b81ed995f387bf63a1c3");
    CHECK (digestOf ("modelled_1x15_vintage.wav")
           == "fd7de6a4fbe4e1fe6a820b657a2dc5e6693db74236ef668c23374d7eee8182e4");
    CHECK (digestOf ("modelled_4x10_horn.wav")
           == "b7e98317d45a340cff6853392a1fde3bcf4f446a6a7e5f7a0336dabeca1537f4");
}

TEST_CASE ("Bundled IR resolution: the preset format tag does not change for the ir object",
           "[ir][bundled][presets]")
{
    // The whole backward-compatibility story (see src/presets/IrReference.h):
    // an old build reads a preset carrying "ir" as an ordinary preset and
    // ignores the key it does not understand. That only works while the
    // format tag stays exactly what old builds validate on.
    CHECK (juce::String (PresetManager::presetFormatTag) == "basilica-preset-1");
    CHECK (juce::String (basilica::presets::irReferenceKey) == "ir");
}

//==============================================================================
// D6 - the bundle is a resolution source, not a library.

TEST_CASE ("Bundled IR resolution: the embedded source indexes cabinets and nothing else",
           "[ir][bundled][content]")
{
    // Four cabinets, and only the cabinets. The provenance files travel with
    // the library (the licensing bar is a licence committed ALONGSIDE the
    // audio), so they are in the same asset list - and a preset must not be
    // able to "resolve" its IR slot to a Markdown file.
    CHECK (source().allContentHashes().size() == 4);

    for (const char* provenanceFile : { "LICENSES.md", "CC0-1.0.txt", "manifest.json" })
    {
        const auto& asset = assetNamed (provenanceFile);
        const auto digest = BundledIrSource::contentHashOf (asset);

        REQUIRE (digest.length() == 64); // the bytes are there...
        CHECK (source().findByContentHash (digest) == nullptr); // ...but unreachable as an IR
    }

    for (const auto& asset : assets())
    {
        if (! BundledIrSource::isAudioAssetName (asset.fileName))
            continue;

        const auto digest = BundledIrSource::contentHashOf (asset);
        const auto* found = source().findByContentHash (digest);

        REQUIRE (found != nullptr);
        CHECK (juce::String (found->fileName) == juce::String (asset.fileName));
    }
}

TEST_CASE ("Bundled IR resolution: the cache folder is not the library folder and is never scanned",
           "[ir][bundled]")
{
    const auto library = basilica::ir::IrLibrary::defaultDirectory();
    const auto cache = basilica::ir::IrLibrary::bundledCacheDirectory();

    // Writing the embedded copy into the user's library folder would put
    // files the user never chose into their own folder - the silent install
    // src/ir/FactoryIrLibrary.h refuses to perform.
    CHECK (cache != library);
    CHECK_FALSE (cache.isAChildOf (library));
    CHECK_FALSE (library.isAChildOf (cache));

    // Neither is it a search root: the cache answers explicit digests only.
    CryptaAudioProcessor processor;

    for (const auto& root : processor.getIrSearchRoots())
        CHECK (root != cache);
}

TEST_CASE ("Bundled IR resolution: a malformed digest is a miss in the embedded source too",
           "[ir][bundled]")
{
    for (const char* bad : { "", "   ", "not-a-hash", "abc",
                              "ZZZZ567890123456789012345678901234567890123456789012345678901234" })
        CHECK (source().findByContentHash (bad) == nullptr);

    // Well-formed but nobody's.
    CHECK (source().findByContentHash (juce::String::repeatedString ("ab", 32)) == nullptr);

    // Case and surrounding whitespace must not change the answer - a digest
    // pasted out of resources/irs/manifest.json by hand still has to resolve.
    const auto digest = digestOf ("modelled_8x10_cone.wav");
    CHECK (source().findByContentHash (digest.toUpperCase()) != nullptr);
    CHECK (source().findByContentHash ("  " + digest + "  ") != nullptr);
}

//==============================================================================
// D8 - materialising, and the single decode path.

TEST_CASE ("Bundled IR resolution: materialising writes the embedded bytes exactly, and is idempotent",
           "[ir][bundled][content]")
{
    ScopedTestDirectory cache ("materialise");

    const auto& asset = assetNamed ("modelled_8x10_cone.wav");

    const auto first = source().materialise (asset, cache.dir);
    REQUIRE (first.existsAsFile());
    CHECK (first.getFileName() == juce::String (asset.fileName));
    CHECK (fileHoldsAssetBytes (first, asset));

    // Idempotent: asked again, the same file comes back, still byte-exact.
    // Asserted on CONTENT rather than on a modification timestamp, which
    // would be a wall-clock assumption.
    const auto second = source().materialise (asset, cache.dir);
    CHECK (second == first);
    CHECK (fileHoldsAssetBytes (second, asset));

    // ...and it repairs rather than trusting what it finds: a file of the
    // right name holding the wrong bytes is exactly the case where handing it
    // to the convolver would be the silent-wrong-sound failure.
    REQUIRE (first.replaceWithText ("not audio at all"));
    CHECK_FALSE (fileHoldsAssetBytes (first, asset));

    const auto repaired = source().materialise (asset, cache.dir);
    CHECK (repaired == first);
    CHECK (fileHoldsAssetBytes (repaired, asset));
}

TEST_CASE ("Bundled IR resolution: every bundled cabinet resolves to its own bytes",
           "[ir][bundled][content]")
{
    ScopedTestDirectory cache ("all-cabs");
    ScopedTestDirectory emptyLibrary ("all-cabs-library");

    CryptaAudioProcessor processor;
    useFolders (processor, emptyLibrary.dir, cache.dir);

    int cabinets = 0;

    for (const auto& asset : assets())
    {
        if (! BundledIrSource::isAudioAssetName (asset.fileName))
            continue;

        ++cabinets;

        const auto resolved = processor.resolveIrReference (BundledIrSource::contentHashOf (asset));

        INFO ("asset: " << asset.fileName);
        CHECK (resolved.source == CryptaAudioProcessor::IrReferenceSource::bundled);
        REQUIRE (resolved.file.existsAsFile());
        CHECK (resolved.file.getFileName() == juce::String (asset.fileName));
        CHECK (fileHoldsAssetBytes (resolved.file, asset));
    }

    CHECK (cabinets == 4);
}

//==============================================================================
// D7 - totality.

TEST_CASE ("Bundled IR resolution: every lookup has exactly one defined outcome",
           "[ir][bundled][presets]")
{
    using Source = CryptaAudioProcessor::IrReferenceSource;

    ScopedTestDirectory cache ("total-cache");
    ScopedTestDirectory library ("total-library");

    CryptaAudioProcessor processor;
    useFolders (processor, library.dir, cache.dir);

    const auto bundledDigest = digestOf ("modelled_8x10_cone.wav");

    // A user IR that is NOT one of Crypta's: the bytes of a bundled cabinet
    // would resolve from the bundle even when the file was deleted, which
    // would make "the library found it" untestable.
    juce::AudioBuffer<float> userIr (1, 128);
    userIr.clear();
    userIr.setSample (0, 0, 1.0f);
    userIr.setSample (0, 41, -0.5f);

    const auto userFile = library.dir.getChildFile ("user_cab.wav");
    REQUIRE (writeWavFile (userFile, userIr, 48000.0));
    const auto userDigest = basilica::presets::contentHashOfFile (userFile);
    REQUIRE (userDigest.length() == 64);

    SECTION ("no digest at all is not a reference")
    {
        for (const char* empty : { "", "   " })
        {
            const auto resolved = processor.resolveIrReference (empty);
            CHECK (resolved.source == Source::notReferenced);
            CHECK (resolved.file == juce::File());
        }
    }

    SECTION ("a digest that is not a digest is a miss, not a non-reference")
    {
        // The preset MEANT to name an IR and failed to. Reporting that as
        // "no reference" would swallow a corrupt preset silently.
        for (const char* malformed : { "abc", "not-a-hash",
                                        "ZZZZ567890123456789012345678901234567890123456789012345678901234" })
        {
            const auto resolved = processor.resolveIrReference (malformed);
            CHECK (resolved.source == Source::notFound);
            CHECK (resolved.file == juce::File());
        }
    }

    SECTION ("a well-formed digest nobody holds is a miss")
    {
        const auto resolved = processor.resolveIrReference (juce::String::repeatedString ("cd", 32));
        CHECK (resolved.source == Source::notFound);
        CHECK (resolved.file == juce::File());
    }

    SECTION ("a digest the library holds resolves from the library")
    {
        const auto resolved = processor.resolveIrReference (userDigest);
        CHECK (resolved.source == Source::library);
        CHECK (resolved.file == userFile);
    }

    SECTION ("a digest only the bundle holds resolves from the bundle")
    {
        const auto resolved = processor.resolveIrReference (bundledDigest);
        CHECK (resolved.source == Source::bundled);
        REQUIRE (resolved.file.existsAsFile());
        CHECK (resolved.file.isAChildOf (cache.dir));
    }

    SECTION ("a digest the slot already holds is answered without going anywhere")
    {
        processor.prepareToPlay (48000.0, 512);
        REQUIRE (processor.loadImpulseResponseFromFile (userFile));

        const auto resolved = processor.resolveIrReference (userDigest);
        CHECK (resolved.source == Source::alreadyLoaded);
        CHECK (resolved.file == userFile);

        // ...and specifically not by writing anything: the short-circuit is
        // ahead of both sources.
        CHECK (cache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);
    }

    SECTION ("clearing the slot clears the short-circuit")
    {
        // The alreadyLoaded answer tracks the bytes the ENGINE holds, not a
        // path that might go stale: after "None", the same digest has to be
        // resolved from a source again.
        processor.prepareToPlay (48000.0, 512);
        REQUIRE (processor.loadImpulseResponseFromFile (userFile));
        processor.clearImpulseResponse();

        const auto resolved = processor.resolveIrReference (userDigest);
        CHECK (resolved.source == Source::library);
        CHECK (resolved.file == userFile);
    }

    SECTION ("a raw buffer load does not impersonate a reference")
    {
        // The public loadImpulseResponse (buffer, rate) has no file bytes to
        // hash, so it must not leave a digest behind for the short-circuit
        // (or a preset save) to trust.
        processor.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> raw (1, 32);
        raw.clear();
        raw.setSample (0, 0, 1.0f);
        processor.loadImpulseResponse (std::move (raw), 48000.0);

        CHECK (processor.getCurrentIrContentHash().isEmpty());

        const auto resolved = processor.resolveIrReference (userDigest);
        CHECK (resolved.source == Source::library);
    }
}

TEST_CASE ("Bundled IR resolution: a cache folder that cannot be written degrades to a miss",
           "[ir][bundled][presets]")
{
    ScopedTestDirectory scratch ("unwritable");
    ScopedTestDirectory presetDir ("unwritable-presets");
    ScopedTestDirectory emptyLibrary ("unwritable-library");

    // A cache path whose parent is a regular FILE, so createDirectory() must
    // fail. Cheaper and more portable than manipulating permissions, and it
    // exercises exactly the branch that matters: the embedded copy is there,
    // and it still cannot become a file.
    const auto blocker = scratch.dir.getChildFile ("blocker");
    REQUIRE (blocker.replaceWithText ("not a directory"));

    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    useFolders (processor, emptyLibrary.dir, blocker.getChildFile ("cache"));

    const auto bundledDigest = digestOf ("modelled_8x10_cone.wav");

    const auto resolved = processor.resolveIrReference (bundledDigest);
    CHECK (resolved.source == CryptaAudioProcessor::IrReferenceSource::notFound);
    CHECK (resolved.file == juce::File());

    // Through the real preset path: the preset still opens, the slot is
    // untouched, and the notice says so - including that Crypta itself could
    // not write its own copy out, which is the only way one of its own
    // cabinets can miss.
    juce::AudioBuffer<float> irBefore;
    irBefore.makeCopyOf (processor.getLoadedImpulseResponse());

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    loadPresetWithReference (manager, presetDir.dir, "Unwritable",
                             irJsonFor (bundledDigest, "Modelled 8x10 Cone"));

    CHECK (processor.getCurrentIrFilePath().isEmpty());
    CHECK (buffersIdentical (processor.getLoadedImpulseResponse(), irBefore));

    const auto notice = processor.getPresetIrNotice();
    INFO ("notice: " << notice.toStdString());
    CHECK (notice.contains ("Modelled 8x10 Cone"));
    CHECK (notice.contains ("could not write out its own copy"));
}

TEST_CASE ("Bundled IR resolution: a miss performs no audio operation at all",
           "[ir][bundled][presets][dsp]")
{
    ScopedTestDirectory cache ("miss-cache");
    ScopedTestDirectory presetDir ("miss-presets");
    ScopedTestDirectory library ("miss-library");

    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    CryptaAudioProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    useFolders (processor, library.dir, cache.dir);

    // A real, audible IR in the slot first. This is what "degrade audibly
    // safely" is actually about: the player has a cabinet up, opens a preset
    // that names one they do not have, and whatever was playing must go on
    // playing - not turn into silence, a NaN or a discontinuity.
    juce::AudioBuffer<float> playingIr (1, 192);
    playingIr.clear();
    playingIr.setSample (0, 0, 0.9f);
    playingIr.setSample (0, 23, -0.4f);
    playingIr.setSample (0, 64, 0.2f);

    const auto playing = library.dir.getChildFile ("playing.wav");
    REQUIRE (writeWavFile (playing, playingIr, sampleRate));
    REQUIRE (processor.loadImpulseResponseFromFile (playing));

    const auto checkIrIsUsable = [&processor] (const char* when)
    {
        const auto& ir = processor.getLoadedImpulseResponse();
        INFO (when);
        REQUIRE (ir.getNumSamples() > 0);

        float peak = 0.0f;

        for (int channel = 0; channel < ir.getNumChannels(); ++channel)
            for (int sample = 0; sample < ir.getNumSamples(); ++sample)
            {
                const auto value = ir.getReadPointer (channel)[sample];
                REQUIRE (std::isfinite (value));
                peak = juce::jmax (peak, std::abs (value));
            }

        // Not silence: there is still an impulse response to convolve with.
        CHECK (peak > 0.0f);
    };

    checkIrIsUsable ("before the preset load");

    juce::AudioBuffer<float> irBefore;
    irBefore.makeCopyOf (processor.getLoadedImpulseResponse());

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    loadPresetWithReference (manager, presetDir.dir, "Nobody Has This",
                             irJsonFor (juce::String::repeatedString ("ef", 32), "A Cab Nobody Owns"));

    // Bit-identical to what it was. That is the whole safety argument: an
    // operation that does not happen cannot produce a NaN, a discontinuity or
    // a dropout.
    CHECK (buffersIdentical (processor.getLoadedImpulseResponse(), irBefore));
    CHECK (processor.getCurrentIrFilePath() == playing.getFullPathName());
    checkIrIsUsable ("after the missing reference");

    CHECK (cache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);
    CHECK (processor.getPresetIrNotice().isNotEmpty());

    // And the plugin still renders finite audio afterwards. The bound is a
    // loose sanity ceiling rather than a level claim: Crypta is a distortion
    // processor with per-band drive and makeup, so unlike a near-unity cab
    // loader its output level is a voicing question (gated elsewhere, see
    // tests/ListeningProxyTests.cpp) - the claim HERE is only "no NaN, no
    // runaway", the synchronous IR checks above are the real assertion.
    juce::AudioBuffer<float> audio (2, blockSize);
    juce::MidiBuffer midi;

    for (int block = 0; block < 8; ++block)
    {
        TestHelpers::fillWithSine (audio, sampleRate, 220.0, 0.25f);
        processor.processBlock (audio, midi);

        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            {
                const auto value = audio.getReadPointer (channel)[sample];
                REQUIRE (std::isfinite (value));
                REQUIRE (std::abs (value) <= 4.0f);
            }
    }
}

TEST_CASE ("Bundled IR resolution: a miss on a slot that holds nothing leaves it holding nothing",
           "[ir][bundled][presets]")
{
    // The other half: a fresh instance, no user IR anywhere. The IR loader's
    // default state is the identity passthrough, which is why
    // getLoadedImpulseResponse() reports an EMPTY buffer - there is no
    // tracked IR to report. A miss must leave exactly that, rather than
    // installing something or clearing something that was never there.
    ScopedTestDirectory cache ("miss-default-cache");
    ScopedTestDirectory presetDir ("miss-default-presets");
    ScopedTestDirectory library ("miss-default-library");

    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);
    useFolders (processor, library.dir, cache.dir);

    REQUIRE (processor.getCurrentIrFilePath().isEmpty());
    REQUIRE (processor.getLoadedImpulseResponse().getNumSamples() == 0);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    loadPresetWithReference (manager, presetDir.dir, "Nobody Has This Either",
                             irJsonFor (juce::String::repeatedString ("ef", 32), "A Cab Nobody Owns"));

    CHECK (processor.getCurrentIrFilePath().isEmpty());
    CHECK (processor.getCurrentIrContentHash().isEmpty());
    CHECK (processor.getLoadedImpulseResponse().getNumSamples() == 0);
    CHECK (cache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);
    CHECK (processor.getPresetIrNotice().isNotEmpty());
}

//==============================================================================
// D4 / D5 / D8 - through the real preset path.

TEST_CASE ("Bundled IR resolution: a factory preset resolves with no library installed",
           "[ir][bundled][presets][factory][content]")
{
    // The case the model exists for: a first-run user, nothing in the IR
    // folder, opening a FACTORY preset that references a FACTORY cabinet
    // already inside the binary they just installed.
    ScopedTestDirectory cache ("factory-cache");
    ScopedTestDirectory presetDir ("factory-presets");
    ScopedTestDirectory emptyLibrary ("factory-library");

    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    useFolders (processor, emptyLibrary.dir, cache.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    REQUIRE (manager.loadPreset ("Cab-Colored Grind"));

    // Nothing to report - the whole point.
    CHECK (processor.getPresetIrNotice().isEmpty());

    // The slot holds the cabinet docs/presets.md prescribes for Cab-Colored
    // Grind, and holds it as SAMPLES, not merely as a bookkeeping entry.
    const auto& coneAsset = assetNamed ("modelled_8x10_cone.wav");

    const juce::File slot (processor.getCurrentIrFilePath());
    REQUIRE (slot.existsAsFile());
    CHECK (slot.isAChildOf (cache.dir));
    CHECK (fileHoldsAssetBytes (slot, coneAsset));
    CHECK (processor.getCurrentIrContentHash() == BundledIrSource::contentHashOf (coneAsset));
    CHECK (buffersIdentical (processor.getLoadedImpulseResponse(), readWav (slot)));

    // The reference survives a re-save: the materialised file is a real file
    // holding the referenced bytes, so capturing the slot re-derives the same
    // digest. A memory-only load could not offer this.
    REQUIRE (manager.saveUserPreset ("Cab Copy", "Bass"));

    const auto saved = presetDir.dir.getChildFile (juce::String ("Cab Copy") + PresetManager::presetFileExtension);
    REQUIRE (saved.existsAsFile());

    const auto text = saved.loadFileAsString();
    CHECK (text.contains (BundledIrSource::contentHashOf (coneAsset)));
}

TEST_CASE ("Bundled IR resolution: a factory-slot load by index records the same reference",
           "[ir][bundled][presets][factory]")
{
    // Crypta's pre-#111 way of putting a factory cabinet up - the GUI's slot
    // list calling loadFactoryImpulseResponse (index) - now records the same
    // digest a file-resolved load would, so a preset saved from it recalls
    // the cabinet anywhere this binary runs.
    ScopedTestDirectory presetDir ("index-presets");

    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadFactoryImpulseResponse (0));

    const auto hash = processor.getCurrentIrContentHash();
    REQUIRE (hash.length() == 64);

    // Index 0 is whatever the table lists first - the digest must be one of
    // the four bundled cabinets' digests, and the slot must hold real
    // samples with no file path (nothing was loaded from disk).
    CHECK (source().findByContentHash (hash) != nullptr);
    CHECK (processor.getCurrentIrFilePath().isEmpty());
    CHECK (processor.getLoadedImpulseResponse().getNumSamples() > 0);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    REQUIRE (manager.saveUserPreset ("Factory Slot", "Bass"));

    const auto saved = presetDir.dir.getChildFile (juce::String ("Factory Slot") + PresetManager::presetFileExtension);
    CHECK (saved.loadFileAsString().contains (hash));

    // ...and the identity "None" writes no reference at all: a preset saved
    // from the passthrough default is byte-identical to a pre-#111 one.
    processor.clearImpulseResponse();
    REQUIRE (manager.saveUserPreset ("No Cab", "Bass"));

    const auto noneText = presetDir.dir.getChildFile (juce::String ("No Cab") + PresetManager::presetFileExtension)
                              .loadFileAsString();
    CHECK_FALSE (noneText.contains ("\"ir\""));
    CHECK_FALSE (noneText.contains ("sha256"));
}

TEST_CASE ("Bundled IR resolution: the installed library wins over the embedded copy",
           "[ir][bundled][presets][factory][content]")
{
    // D5. Both sources hold the digest, because installing writes exactly
    // the embedded bytes - so this is a test about WHICH FILE, and the
    // sample-identity test below is what shows the choice is not audible.
    ScopedTestDirectory cache ("precedence-cache");
    ScopedTestDirectory presetDir ("precedence-presets");
    ScopedTestDirectory library ("precedence-library");

    const auto install = basilica::ir::FactoryIrLibrary::installInto (library.dir, assets());
    REQUIRE (install.succeeded());

    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    useFolders (processor, library.dir, cache.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    REQUIRE (manager.loadPreset ("Cab-Colored Grind"));

    const juce::File slot (processor.getCurrentIrFilePath());
    REQUIRE (slot.existsAsFile());
    CHECK (slot.isAChildOf (library.dir));
    CHECK_FALSE (slot.isAChildOf (cache.dir));

    // Nothing was materialised at all: the embedded source was never reached.
    CHECK (cache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);

    CHECK (processor.getPresetIrNotice().isEmpty());
}

TEST_CASE ("Bundled IR resolution: both sources put sample-identical audio into the engine",
           "[ir][bundled][presets][factory][content][dsp]")
{
    // D8. Free today, because the embedded path materialises a file and then
    // uses the ONE decode path. Pinned anyway: the day somebody adds a
    // memory decoder to save a write is the day it stops being free, and a
    // preset that sounds different depending on whether a library happens to
    // be populated is exactly the failure the model exists to prevent.
    ScopedTestDirectory libraryDir ("identical-library");
    ScopedTestDirectory installedCache ("identical-cache-a");
    ScopedTestDirectory bundledCache ("identical-cache-b");
    ScopedTestDirectory emptyLibrary ("identical-empty");
    ScopedTestDirectory presetA ("identical-presets-a");
    ScopedTestDirectory presetB ("identical-presets-b");

    REQUIRE (basilica::ir::FactoryIrLibrary::installInto (libraryDir.dir, assets()).succeeded());

    const auto renderFrom = [] (const juce::File& library,
                                 const juce::File& cache,
                                 const juce::File& presets,
                                 juce::AudioBuffer<float>& slotOut,
                                 double& rateOut)
    {
        CryptaAudioProcessor processor;
        processor.prepareToPlay (48000.0, 512);
        useFolders (processor, library, cache);

        PresetManager manager (processor.apvts, makeIsolatedConfig (presets), factoryPresetAssets());
        processor.installPresetIrCallbacks (manager);

        REQUIRE (manager.loadPreset ("Cab-Colored Grind"));
        REQUIRE (processor.getPresetIrNotice().isEmpty());

        slotOut.makeCopyOf (processor.getLoadedImpulseResponse());
        rateOut = processor.getLoadedImpulseResponseSampleRate();
    };

    juce::AudioBuffer<float> installed, bundled;
    double installedRate = 0.0, bundledRate = 0.0;

    renderFrom (libraryDir.dir, installedCache.dir, presetA.dir, installed, installedRate);
    renderFrom (emptyLibrary.dir, bundledCache.dir, presetB.dir, bundled, bundledRate);

    // The first run must genuinely have come from the library and the second
    // genuinely from the bundle, or this compares one path with itself.
    CHECK (installedCache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);
    CHECK (bundledCache.dir.getNumberOfChildFiles (juce::File::findFiles) > 0);

    REQUIRE (installed.getNumSamples() > 0);
    CHECK (buffersIdentical (installed, bundled));
    CHECK (installedRate == bundledRate);
    CHECK (installedRate == Catch::Approx (48000.0));
}

//==============================================================================
// D5 / D6 - shadowing.

TEST_CASE ("Bundled IR resolution: a bundled cabinet cannot be shadowed by name",
           "[ir][bundled][presets]")
{
    ScopedTestDirectory cache ("shadow-cache");
    ScopedTestDirectory presetDir ("shadow-presets");
    ScopedTestDirectory library ("shadow-library");

    const auto& coneAsset = assetNamed ("modelled_8x10_cone.wav");
    const auto coneDigest = BundledIrSource::contentHashOf (coneAsset);

    // An impostor: Crypta's own file NAME, somebody else's audio. Nothing
    // resolves by name, so this must not be able to stand in for the
    // reference - that would be the silent-wrong-sound failure the content
    // hash exists to prevent.
    juce::AudioBuffer<float> impostorIr (1, 96);
    impostorIr.clear();
    impostorIr.setSample (0, 0, 1.0f);
    impostorIr.setSample (0, 17, 0.8f);

    const auto impostor = library.dir.getChildFile ("modelled_8x10_cone.wav");
    REQUIRE (writeWavFile (impostor, impostorIr, 48000.0));

    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    useFolders (processor, library.dir, cache.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    loadPresetWithReference (manager, presetDir.dir, "Shadowed",
                             irJsonFor (coneDigest, "Modelled 8x10 Cone"));

    const juce::File slot (processor.getCurrentIrFilePath());
    REQUIRE (slot.existsAsFile());
    CHECK (slot.isAChildOf (cache.dir));
    CHECK (fileHoldsAssetBytes (slot, coneAsset));
    CHECK_FALSE (buffersIdentical (processor.getLoadedImpulseResponse(), impostorIr));
    CHECK (processor.getPresetIrNotice().isEmpty());
}

TEST_CASE ("Bundled IR resolution: a user's own copy of a bundled cabinet is used, whatever it is called",
           "[ir][bundled][presets]")
{
    // The other half of D5: the bytes are what is referenced, so a user who
    // keeps Crypta's cabinets under their own names in their own folder gets
    // THEIR file - the one they can see and audition - rather than a copy
    // appearing somewhere they did not choose.
    ScopedTestDirectory cache ("rename-cache");
    ScopedTestDirectory presetDir ("rename-presets");
    ScopedTestDirectory library ("rename-library");

    const auto& hornAsset = assetNamed ("modelled_4x10_horn.wav");
    const auto renamed = copyAssetInto (library.dir, hornAsset, "My Favourite Cab.wav");

    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    useFolders (processor, library.dir, cache.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    loadPresetWithReference (manager, presetDir.dir, "Renamed",
                             irJsonFor (BundledIrSource::contentHashOf (hornAsset), "Modelled 4x10 Horn"));

    CHECK (processor.getCurrentIrFilePath() == renamed.getFullPathName());
    CHECK (cache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);
    CHECK (processor.getPresetIrNotice().isEmpty());
}

TEST_CASE ("Bundled IR resolution: a retuned cabinet still misses loudly",
           "[ir][bundled][presets]")
{
    // The property the byte hash was chosen for, restated against the
    // embedded source: if a model is ever retuned, its bytes change, and a
    // preset made against the OLD bytes must not silently recall the new
    // sound. The embedded source is keyed by the same digest, so it cannot
    // rescue a stale reference either - which is the correct outcome, not a
    // gap. This is the executable half of resources/irs/LICENSES.md's
    // "never retune in place" release rule.
    ScopedTestDirectory cache ("retune-cache");
    ScopedTestDirectory presetDir ("retune-presets");
    ScopedTestDirectory library ("retune-library");

    const auto& coneAsset = assetNamed ("modelled_8x10_cone.wav");

    // A digest one nibble away from a real bundled cabinet: what a retune
    // would look like from a preset's point of view.
    auto retunedDigest = BundledIrSource::contentHashOf (coneAsset);
    retunedDigest = retunedDigest.substring (0, 63)
                  + (retunedDigest.getLastCharacter() == '0' ? "1" : "0");

    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    useFolders (processor, library.dir, cache.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    loadPresetWithReference (manager, presetDir.dir, "Retuned",
                             irJsonFor (retunedDigest, "Modelled 8x10 Cone (v1)"));

    CHECK (processor.getCurrentIrFilePath().isEmpty());
    CHECK (cache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);

    const auto notice = processor.getPresetIrNotice();
    INFO ("notice: " << notice.toStdString());
    CHECK (notice.contains ("Modelled 8x10 Cone (v1)"));

    // ...and it is NOT reported as one of Crypta's own, because that digest
    // is not one of Crypta's own - the hint would be a lie.
    CHECK_FALSE (notice.contains ("could not write out its own copy"));
}

//==============================================================================
// The notice listener.

TEST_CASE ("Bundled IR resolution: the notice reaches a listener and clears again",
           "[ir][bundled][presets]")
{
    ScopedTestDirectory cache ("notice-cache");
    ScopedTestDirectory presetDir ("notice-presets");
    ScopedTestDirectory library ("notice-library");

    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    useFolders (processor, library.dir, cache.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    std::vector<juce::String> published;
    processor.onPresetIrNotice = [&published] (const juce::String& notice) { published.push_back (notice); };

    // A miss publishes a message naming the cabinet...
    loadPresetWithReference (manager, presetDir.dir, "Missing",
                             irJsonFor (juce::String::repeatedString ("ef", 32), "Someone Else's Cab"));

    REQUIRE (published.size() == 1);
    CHECK (published.back().contains ("Someone Else's Cab"));
    CHECK (published.back() == processor.getPresetIrNotice());

    // ...a load with nothing to report publishes the CLEAR, so a listener
    // cannot keep showing a stale message...
    REQUIRE (manager.loadPreset ("Glue & Grind"));
    REQUIRE (published.size() == 2);
    CHECK (published.back().isEmpty());
    CHECK (processor.getPresetIrNotice().isEmpty());

    // ...and an unchanged notice is not re-published.
    REQUIRE (manager.loadPreset ("Glue & Grind"));
    CHECK (published.size() == 2);
}
