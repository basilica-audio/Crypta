#pragma once

// Central definition of all AudioProcessorValueTreeState parameter IDs.
// Keeping these in one place avoids typo-mismatches between the layout
// creation code, the processor's parameter lookups, and any future GUI code.
//
// FROZEN AS OF THE M1 FULL v1.0 PARAMETER LAYOUT (issue #7):
// Parameter IDs below must NEVER change once shipped - saved sessions and
// presets persist the APVTS state keyed by these string IDs, and renaming or
// removing one would silently break every user's saved state. Ranges,
// defaults, skew and display labels MAY still be refined during voicing/
// tuning milestones (M2-M4); only the IDs themselves are frozen.
//
// Several parameters declared here (everything below "IO / Global") are
// declared-but-inert as of M1: they exist in the APVTS layout and are fully
// covered by tests, but processBlock() does not yet read them. They are
// wired into the signal chain by their respective milestones (M2 dynamics,
// M3 distortion, M4 EQ/IR).

namespace ParamIDs
{
    //==============================================================================
    // IO / Global
    inline constexpr auto inputGain = "inputGain";
    inline constexpr auto outputGain = "outputGain";
    inline constexpr auto bypass = "bypass";
    inline constexpr auto outputClip = "outputClip";

    //==============================================================================
    // Noise gate (full-band, pre-crossover)
    inline constexpr auto gateEnabled = "gateEnabled";
    inline constexpr auto gateThreshold = "gateThreshold";
    inline constexpr auto gateRatio = "gateRatio";
    inline constexpr auto gateAttack = "gateAttack";
    inline constexpr auto gateRelease = "gateRelease";

    //==============================================================================
    // Crossover (two cascaded Linkwitz-Riley 4th order splits, v0.2.0
    // 2-band -> 3-band rebuild - docs/design-brief.md's "Topology" section).
    // The v1 single `crossoverFreq` ID is retired (breaking change,
    // acceptable pre-1.0 per the brief's Versioning section); old sessions
    // are migrated in CryptaAudioProcessor::setStateInformation() - see
    // its migrateLegacySingleCrossover() helper.
    inline constexpr auto splitLowHz = "splitLowHz";
    inline constexpr auto splitHighHz = "splitHighHz";

    //==============================================================================
    // Low band: compressor + level (ballistics re-sourced in v0.2.0, IDs/
    // structure unchanged from v1)
    inline constexpr auto lowCompThreshold = "lowCompThreshold";
    inline constexpr auto lowCompRatio = "lowCompRatio";
    inline constexpr auto lowCompAttack = "lowCompAttack";
    inline constexpr auto lowCompRelease = "lowCompRelease";
    inline constexpr auto lowCompMakeup = "lowCompMakeup";
    inline constexpr auto lowCompMix = "lowCompMix";
    inline constexpr auto lowLevel = "lowLevel";

    //==============================================================================
    // Mid band (NEW in v0.2.0): drive + level only - no filter/tone/blend,
    // matching the reference class's own Mid band exactly (docs/design-brief.md).
    inline constexpr auto midDrive = "midDrive";
    inline constexpr auto midLevel = "midLevel";

    //==============================================================================
    // High band: Tight (NEW in v0.2.0 - promoted from a Razor-only fixed
    // internal constant to a first-class, voicing-independent control) +
    // voicing + drive + tone + blend + level
    inline constexpr auto highTightHz = "highTightHz";
    inline constexpr auto highVoicing = "highVoicing";
    inline constexpr auto highDrive = "highDrive";
    inline constexpr auto highTone = "highTone";
    inline constexpr auto highBlend = "highBlend";
    inline constexpr auto highLevel = "highLevel";

    //==============================================================================
    // Post-sum 4-band EQ (LowShelf / Peak / Peak / HighShelf)
    inline constexpr auto eqEnabled = "eqEnabled";
    inline constexpr auto eqLowShelfFreq = "eqLowShelfFreq";
    inline constexpr auto eqLowShelfGain = "eqLowShelfGain";
    inline constexpr auto eqPeak1Freq = "eqPeak1Freq";
    inline constexpr auto eqPeak1Gain = "eqPeak1Gain";
    inline constexpr auto eqPeak1Q = "eqPeak1Q";
    inline constexpr auto eqPeak2Freq = "eqPeak2Freq";
    inline constexpr auto eqPeak2Gain = "eqPeak2Gain";
    inline constexpr auto eqPeak2Q = "eqPeak2Q";
    inline constexpr auto eqHighShelfFreq = "eqHighShelfFreq";
    inline constexpr auto eqHighShelfGain = "eqHighShelfGain";

    //==============================================================================
    // IR loader / cab sim (the IR file path itself is non-parameter state,
    // handled separately in M4)
    inline constexpr auto irEnabled = "irEnabled";
    inline constexpr auto irMix = "irMix";

    //==============================================================================
    // v0.3.0 "circuit-grade bass engine" additions (12 IDs, 39 -> 51 total).
    //
    // Three of these are engine selectors (driveEngine, lowCompDetector,
    // gateMode). Their APVTS defaults name the NEW circuit-derived engines,
    // because that is what a genuinely fresh instance should boot into - but
    // every pre-v0.3.0 session and every pre-v0.3.0 preset gets the legacy
    // value injected on load, so no existing session or preset ever changes
    // its sound. See CryptaAudioProcessor::migrateToStateV2() for the session
    // path and PresetManager's legacy engine injection for the preset path.
    //
    // The remaining nine are parameters of those new engines. Several carry
    // deliberately non-neutral defaults (knee 6 dB, hysteresis 4 dB, hold
    // 20 ms, gate sidechain highpass 80 Hz, range 60 dB): that is safe
    // precisely because they are unread unless the corresponding engine
    // selector is on its new value, which legacy state never selects.
    inline constexpr auto driveEngine = "driveEngine";
    inline constexpr auto highBias = "highBias";

    inline constexpr auto lowCompDetector = "lowCompDetector";
    inline constexpr auto lowCompKnee = "lowCompKnee";
    inline constexpr auto lowCompAutoRelease = "lowCompAutoRelease";
    inline constexpr auto lowCompAutoMakeup = "lowCompAutoMakeup";

    inline constexpr auto gateMode = "gateMode";
    inline constexpr auto gateHysteresis = "gateHysteresis";
    inline constexpr auto gateHold = "gateHold";
    inline constexpr auto gateScHpf = "gateScHpf";
    inline constexpr auto gateRange = "gateRange";

    inline constexpr auto clipCeiling = "clipCeiling";
}
