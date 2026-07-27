#include "../src/PluginProcessor.h"
#include "../src/params/ParameterIds.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

// Golden-render regression harness for the v0.2.0 -> v0.3.0 state/preset
// migration (brief §6 T10). The contract v0.3.0 has to keep is that every
// pre-v0.3.0 session and user preset still renders through the *legacy*
// (Classic / Classic Peak / Classic) code paths, i.e. produces the exact same
// audio v0.2.0 produced.
//
// How the fixtures were made: the `[.generate-goldens]` test case below was
// run once against the v0.2.0 code (the branch point of feat/v0.3.0-sota-dsp,
// origin/main @ e7c0148) with CRYPTA_WRITE_GOLDENS=1 in the environment. It
// wrote, per configuration:
//   tests/fixtures/state_v020_<name>.xml  - genuine v0.2.0 APVTS state
//                                           (39 PARAM elements, and crucially
//                                           NO stateVersion attribute, which
//                                           is what marks it as legacy)
//   tests/fixtures/golden_v020_<name>.f32 - the render, raw little-endian
//                                           float32, channel-interleaved
// Both are committed. The generator is `[.hidden]`-tagged so it never runs in
// CI; regenerating goldens is a deliberate, reviewed act, not something a
// stray test run can do.
//
// Per-platform assertion (brief §6 T10(b) + the CI note): bit-exact float
// rendering does NOT hold across toolchains - MSVC's std::tanh and Apple
// libm's differ in the last ulp, and FMA/SIMD codegen differs too. macOS is
// therefore the bit-exactness golden platform (sample-exact memcmp); every
// other platform asserts an RMS null of <= -120 dB against the same goldens,
// which is far tighter than any real regression could sneak through but
// tolerant of last-ulp libm drift. The switch lives here in test code, so
// .github/workflows/* stays untouched (brief §5 blacklist).
namespace
{
    constexpr double goldenSampleRate = 48000.0;
    constexpr int goldenBlockSize = 512;

    // 0.25 s stereo per fixture (12000 samples/channel = 96 KB of float32).
    // The brief sketches a 4 s render; 0.25 s is a deliberate repo-weight
    // trade that loses no regression power - every migration failure mode
    // (an engine landing on Circuit/Smooth RMS/Modern instead of the legacy
    // path) changes the very first milliseconds of output, and the program
    // material below already sweeps a burst through gate open, compressor
    // attack/release and gate close inside the window.
    constexpr int goldenNumSamples = 12000;
    constexpr int goldenNumChannels = 2;

    // Deterministic, toolchain-independent program material: a fixed-seed
    // 32-bit LCG (spelled out here rather than using juce::Random or
    // std::mt19937 so the sequence is pinned by this file, not by a library
    // version) shaping a two-tone bass signal into bursts. All arithmetic is
    // done in double and rounded once, so the material itself is bit-identical
    // everywhere even though the *rendered* result is not.
    void fillGoldenProgram (juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        std::uint32_t lcg = 0x1BADB002u;
        const auto nextNoise = [&lcg]
        {
            lcg = lcg * 1664525u + 1013904223u;
            return static_cast<double> (lcg >> 8) / static_cast<double> (1u << 24) * 2.0 - 1.0;
        };

        const auto numSamples = buffer.getNumSamples();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto t = static_cast<double> (sample) / sampleRate;

            // Burst envelope: 60 ms of signal, 60 ms of near-silence, repeating.
            // Drives the gate through open -> hold -> close inside the window
            // and gives the low-band compressor a real attack/release cycle.
            const auto phaseInCycle = std::fmod (t, 0.12);
            const auto envelope = phaseInCycle < 0.06
                                      ? std::exp (-phaseInCycle * 18.0)
                                      : 0.0009; // just under a -60 dB gate threshold

            // Bass fundamental + an upper-harmonic partial so the crossover
            // splits actually distribute energy across all three bands.
            const auto fundamental = std::sin (juce::MathConstants<double>::twoPi * 55.0 * t);
            const auto partial = 0.45 * std::sin (juce::MathConstants<double>::twoPi * 880.0 * t);
            const auto grit = 0.08 * nextNoise();

            const auto left = envelope * (fundamental + partial + grit) * 0.7;
            // Decorrelate the right channel so per-channel filter/ballistics
            // state is genuinely exercised rather than mirrored.
            const auto right = envelope * (0.92 * fundamental - 0.5 * partial + grit) * 0.7;

            buffer.setSample (0, sample, static_cast<float> (left));

            if (buffer.getNumChannels() > 1)
                buffer.setSample (1, sample, static_cast<float> (right));
        }
    }

    juce::File fixturesDirectory()
    {
        // __FILE__ is absolute here: CMakeLists.txt feeds the Tests target via
        // file(GLOB_RECURSE ...), which always yields absolute paths. This
        // keeps the fixture lookup working without a compile definition, so
        // CMakeLists.txt needs no edit beyond the version bump (brief §5).
        return juce::File (juce::String (__FILE__)).getParentDirectory().getChildFile ("fixtures");
    }

    struct GoldenConfig
    {
        const char* name;
        int voicingIndex;
        bool outputClip;
    };

    // Brief T10(b): each voicing x comp on x gate on, clip OFF; plus T10(c)'s
    // clip-ON fixture, which is the one deliberate non-bit-identical case
    // (the safety clip moves to delta-form ADAA in v0.3.0).
    const std::vector<GoldenConfig>& goldenConfigs()
    {
        static const std::vector<GoldenConfig> configs {
            { "gnaw", 0, false },
            { "wool", 1, false },
            { "razor", 2, false },
            { "gnaw_clip", 0, true },
        };
        return configs;
    }

    void applyGoldenConfig (CryptaAudioProcessor& processor, const GoldenConfig& config)
    {
        const auto set = [&processor] (const char* id, float plainValue)
        {
            auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (processor.apvts.getParameter (id));
            REQUIRE (parameter != nullptr);
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (plainValue));
        };

        // Gate ON with a threshold the burst envelope crosses in both
        // directions, so the render exercises open/hold/close.
        set (ParamIDs::gateEnabled, 1.0f);
        set (ParamIDs::gateThreshold, -40.0f);
        set (ParamIDs::gateRatio, 10.0f);
        set (ParamIDs::gateAttack, 1.0f);
        set (ParamIDs::gateRelease, 100.0f);

        // Low-band compressor pushed well into gain reduction.
        set (ParamIDs::lowCompThreshold, -24.0f);
        set (ParamIDs::lowCompRatio, 4.0f);
        set (ParamIDs::lowCompAttack, 3.0f);
        set (ParamIDs::lowCompRelease, 6.0f);
        set (ParamIDs::lowCompMakeup, 3.0f);
        set (ParamIDs::lowCompMix, 80.0f);

        set (ParamIDs::midDrive, 55.0f);

        set (ParamIDs::highVoicing, static_cast<float> (config.voicingIndex));
        set (ParamIDs::highTightHz, 120.0f);
        set (ParamIDs::highDrive, 70.0f);
        set (ParamIDs::highTone, 60.0f);
        set (ParamIDs::highBlend, 85.0f);

        // EQ on, so the post-sum stage is part of the golden too.
        set (ParamIDs::eqEnabled, 1.0f);
        set (ParamIDs::eqPeak1Freq, 800.0f);
        set (ParamIDs::eqPeak1Gain, 4.0f);

        set (ParamIDs::outputClip, config.outputClip ? 1.0f : 0.0f);
        // Push into the clip so the clip-ON fixture actually clips.
        set (ParamIDs::outputGain, config.outputClip ? 12.0f : 0.0f);
    }

    // Renders the deterministic program through `processor` in fixed-size
    // blocks and returns the result interleaved-by-channel-buffer.
    juce::AudioBuffer<float> renderGolden (CryptaAudioProcessor& processor)
    {
        processor.setPlayConfigDetails (goldenNumChannels, goldenNumChannels, goldenSampleRate, goldenBlockSize);
        processor.prepareToPlay (goldenSampleRate, goldenBlockSize);
        processor.reset();

        juce::AudioBuffer<float> program (goldenNumChannels, goldenNumSamples);
        fillGoldenProgram (program, goldenSampleRate);

        juce::AudioBuffer<float> output (goldenNumChannels, goldenNumSamples);
        output.clear();

        juce::AudioBuffer<float> block (goldenNumChannels, goldenBlockSize);
        juce::MidiBuffer midi;

        for (int offset = 0; offset < goldenNumSamples; offset += goldenBlockSize)
        {
            const auto length = juce::jmin (goldenBlockSize, goldenNumSamples - offset);
            block.setSize (goldenNumChannels, length, false, false, true);

            for (int channel = 0; channel < goldenNumChannels; ++channel)
                block.copyFrom (channel, 0, program, channel, offset, length);

            processor.processBlock (block, midi);

            for (int channel = 0; channel < goldenNumChannels; ++channel)
                output.copyFrom (channel, offset, block, channel, 0, length);
        }

        return output;
    }

    std::vector<float> flatten (const juce::AudioBuffer<float>& buffer)
    {
        std::vector<float> flat;
        flat.reserve (static_cast<size_t> (buffer.getNumChannels() * buffer.getNumSamples()));

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                flat.push_back (buffer.getSample (channel, sample));

        return flat;
    }

    bool readGolden (const juce::File& file, std::vector<float>& destination)
    {
        juce::MemoryBlock raw;

        if (! file.existsAsFile() || ! file.loadFileAsData (raw))
            return false;

        const auto count = raw.getSize() / sizeof (float);
        destination.resize (count);
        std::memcpy (destination.data(), raw.getData(), count * sizeof (float));
        return true;
    }
}

// Regeneration entry point. Hidden (leading dot in the tag) so `Tests` never
// runs it in CI - invoke deliberately with `./build/Tests "[.generate-goldens]"`
// while the working tree is at the version the goldens should capture.
TEST_CASE ("Golden renders: regenerate v0.2.0 fixtures", "[.generate-goldens]")
{
    const auto directory = fixturesDirectory();
    REQUIRE (directory.createDirectory().wasOk());

    for (const auto& config : goldenConfigs())
    {
        CryptaAudioProcessor processor;
        applyGoldenConfig (processor, config);

        // Capture the state BEFORE rendering, so the committed XML is exactly
        // the state the golden was produced from.
        juce::MemoryBlock stateBlock;
        processor.getStateInformation (stateBlock);

        const std::unique_ptr<juce::XmlElement> stateXml (
            juce::AudioProcessor::getXmlFromBinary (stateBlock.getData(), static_cast<int> (stateBlock.getSize())));
        REQUIRE (stateXml != nullptr);

        const auto stateFile = directory.getChildFile (juce::String ("state_v020_") + config.name + ".xml");
        REQUIRE (stateFile.replaceWithText (stateXml->toString()));

        const auto rendered = renderGolden (processor);
        const auto flat = flatten (rendered);

        const auto goldenFile = directory.getChildFile (juce::String ("golden_v020_") + config.name + ".f32");
        REQUIRE (goldenFile.replaceWithData (flat.data(), flat.size() * sizeof (float)));
    }
}

TEST_CASE ("Golden renders: the committed v0.2.0 fixtures exist and are well-formed", "[state][migration][golden]")
{
    const auto directory = fixturesDirectory();

    for (const auto& config : goldenConfigs())
    {
        const auto stateFile = directory.getChildFile (juce::String ("state_v020_") + config.name + ".xml");
        const auto goldenFile = directory.getChildFile (juce::String ("golden_v020_") + config.name + ".f32");

        INFO ("fixture: " << config.name);
        REQUIRE (stateFile.existsAsFile());
        REQUIRE (goldenFile.existsAsFile());

        const std::unique_ptr<juce::XmlElement> stateXml (juce::XmlDocument::parse (stateFile));
        REQUIRE (stateXml != nullptr);
        REQUIRE (stateXml->hasTagName ("PARAMETERS"));

        // The defining property of a legacy fixture: no stateVersion attribute.
        // If this ever fails, the fixture was regenerated against v0.3.0+ code
        // and the migration test below has silently stopped testing migration.
        REQUIRE_FALSE (stateXml->hasAttribute ("stateVersion"));

        std::vector<float> golden;
        REQUIRE (readGolden (goldenFile, golden));
        REQUIRE (golden.size() == static_cast<size_t> (goldenNumChannels * goldenNumSamples));
    }
}

TEST_CASE ("Golden renders: legacy v0.2.0 sessions still render identically under v0.3.0", "[state][migration][golden]")
{
    // The headline migration contract (brief T10(b)): load genuine v0.2.0
    // state, render, and compare against what v0.2.0 itself produced. If the
    // engine migration ever stops injecting Classic/Classic Peak/Classic -
    // or if a "Classic path preserved verbatim" refactor is not actually
    // verbatim - this is the test that fails.
    const auto directory = fixturesDirectory();

    for (const auto& config : goldenConfigs())
    {
        // The clip-on fixture is the documented exception (brief §4 step 4a):
        // v0.3.0 deliberately replaces the raw base-rate tanh with a
        // delta-form ADAA ceiling clip. It gets its own looser contract in the
        // next test case rather than bit-exactness.
        if (config.outputClip)
            continue;

        INFO ("fixture: " << config.name);

        const auto stateFile = directory.getChildFile (juce::String ("state_v020_") + config.name + ".xml");
        const std::unique_ptr<juce::XmlElement> stateXml (juce::XmlDocument::parse (stateFile));
        REQUIRE (stateXml != nullptr);

        juce::MemoryBlock stateBlock;
        juce::AudioProcessor::copyXmlToBinary (*stateXml, stateBlock);

        CryptaAudioProcessor processor;
        processor.setStateInformation (stateBlock.getData(), static_cast<int> (stateBlock.getSize()));

        // Sanity: the migration really did fire on this fixture. Without
        // this, a bug that made the fixtures look v0.3.0-shaped would turn
        // the comparison below into a tautology.
        const auto engineIndex = [&processor] (const char* id)
        {
            auto* choice = dynamic_cast<juce::AudioParameterChoice*> (processor.apvts.getParameter (id));
            REQUIRE (choice != nullptr);
            return choice->getIndex();
        };

        REQUIRE (engineIndex (ParamIDs::driveEngine) == 0);
        REQUIRE (engineIndex (ParamIDs::lowCompDetector) == 0);
        REQUIRE (engineIndex (ParamIDs::gateMode) == 0);

        const auto rendered = flatten (renderGolden (processor));

        std::vector<float> golden;
        REQUIRE (readGolden (directory.getChildFile (juce::String ("golden_v020_") + config.name + ".f32"), golden));
        REQUIRE (rendered.size() == golden.size());

#if JUCE_MAC
        // macOS is the bit-exactness golden platform: the goldens were
        // generated here, so anything short of sample-exact is a real change.
        CHECK (std::memcmp (rendered.data(), golden.data(), golden.size() * sizeof (float)) == 0);
#else
        // Elsewhere, assert a -120 dB RMS null instead. Cross-toolchain
        // bit-exactness is unattainable (MSVC vs Apple libm std::tanh, FMA and
        // SIMD codegen differences), but -120 dB is orders of magnitude below
        // any regression that would matter, so this still catches a migration
        // landing on the wrong engine.
        double sumOfSquares = 0.0;

        for (size_t index = 0; index < golden.size(); ++index)
        {
            const auto difference = static_cast<double> (rendered[index]) - static_cast<double> (golden[index]);
            sumOfSquares += difference * difference;
        }

        const auto nullRms = std::sqrt (sumOfSquares / static_cast<double> (golden.size()));
        const auto nullDb = juce::Decibels::gainToDecibels (nullRms, -200.0);
        INFO ("null RMS: " << nullDb << " dB");
        CHECK (nullDb <= -120.0);
#endif
    }
}

TEST_CASE ("Golden renders: the engaged safety clip stays within its documented -40 dB null", "[state][migration][golden]")
{
    // Brief §4 step 4(a) / T10(c): with the safety clip ENGAGED, v0.3.0 is
    // deliberately not bit-identical to v0.2.0 - the raw base-rate std::tanh
    // is replaced by a delta-form ADAA ceiling clip, which is a documented
    // defect fix (less aliasing, and transparent below the ceiling instead of
    // colouring everything). The contract is a bounded difference, not zero:
    // differences are confined to the clipped / soft-knee region.
    const auto directory = fixturesDirectory();

    const auto stateFile = directory.getChildFile ("state_v020_gnaw_clip.xml");
    const std::unique_ptr<juce::XmlElement> stateXml (juce::XmlDocument::parse (stateFile));
    REQUIRE (stateXml != nullptr);

    juce::MemoryBlock stateBlock;
    juce::AudioProcessor::copyXmlToBinary (*stateXml, stateBlock);

    CryptaAudioProcessor processor;
    processor.setStateInformation (stateBlock.getData(), static_cast<int> (stateBlock.getSize()));

    const auto rendered = flatten (renderGolden (processor));

    std::vector<float> golden;
    REQUIRE (readGolden (directory.getChildFile ("golden_v020_gnaw_clip.f32"), golden));
    REQUIRE (rendered.size() == golden.size());

    double differenceSquares = 0.0;
    double goldenSquares = 0.0;

    for (size_t index = 0; index < golden.size(); ++index)
    {
        const auto difference = static_cast<double> (rendered[index]) - static_cast<double> (golden[index]);
        differenceSquares += difference * difference;
        goldenSquares += static_cast<double> (golden[index]) * static_cast<double> (golden[index]);
    }

    const auto count = static_cast<double> (golden.size());
    const auto nullDb = juce::Decibels::gainToDecibels (std::sqrt (differenceSquares / count), -200.0);
    const auto signalDb = juce::Decibels::gainToDecibels (std::sqrt (goldenSquares / count), -200.0);

    INFO ("null RMS: " << nullDb << " dB, signal RMS: " << signalDb << " dB");

    // Relative to the programme, not absolute - the fixture is rendered hot
    // (+12 dB output trim) precisely so the clipper is doing real work.
    CHECK ((nullDb - signalDb) <= -40.0);
    CHECK (std::isfinite (nullDb));
}
