#include "FactoryIRs.h"

namespace cryp
{
    bool isApprovedFactoryIRLicence (juce::StringRef licence) noexcept
    {
        // See the class-level rationale in FactoryIRs.h for why this list is
        // this short, and why "royalty free" is not on it.
        static const char* const approved[] = { "CC0-1.0", "Public Domain", "Self-recorded" };

        for (const auto* candidate : approved)
            if (juce::String (candidate) == licence)
                return true;

        return false;
    }

    FactoryIRLibrary::FactoryIRLibrary (std::vector<FactoryIRAsset> assetsToRegister)
    {
        assets.reserve (assetsToRegister.size());

        for (auto& asset : assetsToRegister)
        {
            const auto hasPayload = asset.data != nullptr && asset.dataSizeBytes > 0;
            const auto hasName = asset.name != nullptr && juce::String (asset.name).isNotEmpty();
            const auto hasSource = asset.source != nullptr && juce::String (asset.source).isNotEmpty();
            const auto hasLicence = asset.licence != nullptr && isApprovedFactoryIRLicence (asset.licence);

            if (hasPayload && hasName && hasSource && hasLicence)
            {
                assets.push_back (asset);
                continue;
            }

            // Reaching here means someone added an IR without clearing the
            // licence bar. The asset is dropped rather than loaded, so a
            // shipped binary cannot carry an unverified IR - and the drop is
            // counted so tests/FactoryIRTests.cpp can fail the build over it
            // instead of the plugin quietly shipping one IR fewer than
            // intended. (A jassert would be the usual JUCE reflex here; it
            // would also make the rejection path itself untestable in a debug
            // build, which is precisely the build CI runs the tests in.)
            ++rejectedAssetCount;
        }
    }

    const FactoryIRAsset* FactoryIRLibrary::getAsset (int index) const noexcept
    {
        if (index < 0 || index >= getNumAssets())
            return nullptr;

        return &assets[static_cast<size_t> (index)];
    }

    juce::String FactoryIRLibrary::getName (int index) const
    {
        if (const auto* asset = getAsset (index))
            return juce::String (asset->name);

        return {};
    }

    bool FactoryIRLibrary::decode (int index, juce::AudioBuffer<float>& destination, double& sampleRateOut) const
    {
        const auto* asset = getAsset (index);

        if (asset == nullptr)
            return false;

        return decodeFromMemory (asset->data, static_cast<size_t> (asset->dataSizeBytes), destination, sampleRateOut);
    }

    bool FactoryIRLibrary::decodeFromMemory (const void* data,
                                              size_t dataSizeBytes,
                                              juce::AudioBuffer<float>& destination,
                                              double& sampleRateOut)
    {
        sampleRateOut = 0.0;

        if (data == nullptr || dataSizeBytes == 0)
            return false;

        // Basic formats only (WAV/AIFF): a factory IR is an uncompressed
        // capture, and registering the lossy codecs here would widen the
        // parsing surface for no benefit.
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        // MemoryInputStream in non-copying mode - the bytes are BinaryData
        // (static storage) or a test fixture that outlives the call.
        auto stream = std::make_unique<juce::MemoryInputStream> (data, dataSizeBytes, false);
        const std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (std::move (stream)));

        if (reader == nullptr)
            return false;

        const auto numChannels = static_cast<int> (reader->numChannels);
        const auto numSamples = static_cast<int> (juce::jmin (reader->lengthInSamples,
                                                               static_cast<juce::int64> (maximumImpulseResponseSamples)));

        if (numChannels <= 0 || numSamples <= 0 || reader->sampleRate <= 0.0)
            return false;

        destination.setSize (numChannels, numSamples);
        destination.clear();

        if (! reader->read (&destination, 0, numSamples, 0, true, numChannels > 1))
            return false;

        sampleRateOut = reader->sampleRate;
        return true;
    }
}
