#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <vector>

// Factory impulse-response slot mechanics (issue #21).
//
// SCOPE, STATED PLAINLY: this file is the *mechanism* for bundling cab IRs in
// the binary and handing them to cryp::IRLoader.
//
// FOUR impulse responses ship with Crypta today (issue #81), registered in
// CryptaAudioProcessor::getFactoryIRAssetTable(). This comment used to say
// the asset table was EMPTY and that none had been sourced with a licence
// worth staking a distributed AGPLv3 binary on - true when it was written,
// and the reasoning behind it is still exactly right, which is why it is
// restated rather than deleted: a capture carries rights from the cabinet,
// from the microphone and from whoever pressed record, third-party packs
// routinely misstate all three, and "free download" is not a licence.
//
// That question was not answered. It was AVOIDED. The four that ship are
// GENERATED, not recorded - tools/ir-synth/cabsynth.py computes each from a
// documented analytical cabinet model, so there is no third-party recording
// inside the binary and therefore nothing to trace and no licensor to find.
// Their display names all begin with "Modelled" so a user reading a preset
// list is never left thinking otherwise. Provenance, per-file model
// parameters, checksums and the measured response of each are in
// resources/irs/LICENSES.md; the CC0 dedication is at resources/irs/CC0-1.0.txt.
//
// The licence bar below is therefore NOT dormant - it is the thing that keeps
// a future hand-added capture from slipping in beside them.
//
// The licence bar, enforced in code rather than in a README.
// Every asset registered here must name a licence from the approved list
// (isApprovedFactoryIRLicence()) *and* a non-empty, checkable source. The
// library constructor drops anything that does not - so an IR added later
// without provenance cannot silently end up in a release build, and the
// accompanying test (tests/FactoryIRTests.cpp) fails the build if the table
// ever contains an entry that would be dropped. The approved list is
// deliberately short:
//   - "CC0-1.0"        : Creative Commons Zero, i.e. dedicated to the public
//                        domain, with the source URL recorded.
//   - "Public Domain"  : an explicit public-domain dedication that is not CC0.
//   - "Self-recorded"  : captured by this project, with the cab/mic/date
//                        recorded in the source field and the recording rig
//                        owned by us. This is the only category that needs no
//                        third party to be right.
// Anything else - "free for commercial use", "royalty free", CC-BY, unknown -
// is rejected. CC-BY is rejected not because it is unusable but because it
// carries attribution obligations into every downstream binary, which is a
// decision for the project owner to take deliberately, not a default.
namespace cryp
{
    struct FactoryIRAsset
    {
        // Display name for the GUI's IR slot list.
        const char* name = nullptr;

        // Licence identifier - must be one isApprovedFactoryIRLicence()
        // accepts.
        const char* licence = nullptr;

        // Provenance: a URL for a third-party public-domain source, or the
        // capture details (cab, mic, position, date) for a self-recording.
        // Never empty.
        const char* source = nullptr;

        // Raw file bytes (a WAV or AIFF), typically a BinaryData:: symbol.
        // Not owned by this struct.
        const void* data = nullptr;
        int dataSizeBytes = 0;
    };

    bool isApprovedFactoryIRLicence (juce::StringRef licence) noexcept;

    class FactoryIRLibrary
    {
    public:
        FactoryIRLibrary() = default;

        // Assets failing the licence bar above are dropped (and assert in
        // debug builds). Construction is a startup-time operation; nothing in
        // this class is real-time safe or intended to be called from the audio
        // thread.
        explicit FactoryIRLibrary (std::vector<FactoryIRAsset> assetsToRegister);

        int getNumAssets() const noexcept { return static_cast<int> (assets.size()); }

        // Assets the licence bar rejected at construction. MUST be zero for
        // the table the plugin actually ships - a non-zero value means someone
        // added an IR without provenance and it was silently dropped, which
        // tests/FactoryIRTests.cpp fails the build over.
        int getNumRejectedAssets() const noexcept { return rejectedAssetCount; }

        // Null for an out-of-range index.
        const FactoryIRAsset* getAsset (int index) const noexcept;

        // Empty string for an out-of-range index.
        juce::String getName (int index) const;

        // Decodes asset `index` into `destination` and reports the file's own
        // sample rate. Message thread only - it allocates and runs an audio
        // format reader. False for an out-of-range index or undecodable data.
        bool decode (int index, juce::AudioBuffer<float>& destination, double& sampleRateOut) const;

        // The decoder itself, exposed so a GUI file picker (and the tests) can
        // reuse exactly the same path an embedded asset takes - including the
        // length guard below. Message thread only.
        static bool decodeFromMemory (const void* data,
                                       size_t dataSizeBytes,
                                       juce::AudioBuffer<float>& destination,
                                       double& sampleRateOut);

        // Cab IRs are tens of milliseconds; anything claiming minutes is
        // either a reverb or a malformed header, and convolving it would cost
        // real CPU and real latency-free FFT memory. 10 seconds at 96 kHz is
        // a generous ceiling that still refuses the pathological case.
        static constexpr int maximumImpulseResponseSamples = 960000;

    private:
        std::vector<FactoryIRAsset> assets;
        int rejectedAssetCount = 0;
    };
}
