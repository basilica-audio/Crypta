#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/SplitGap.h"
#include "params/ParameterIds.h"
#include "params/ParameterLayout.h"

#include <BinaryData.h>

#include <cmath>

namespace
{
    // ~20ms smoothing ramp for gain changes: fast enough to feel responsive,
    // slow enough to avoid zipper noise on parameter automation.
    constexpr double gainRampDurationSeconds = 0.02;

    // Issue #87: fixed, sample-rate-independent crossfade time for the
    // wet/dry bypass blend (see bypassWetMix's docs in PluginProcessor.h).
    // 20 ms matches the plugin's existing gain-smoothing ramp: long enough
    // that the blend itself never reads as a click, short enough that
    // engaging/disengaging bypass still feels instantaneous to a player.
    constexpr double bypassCrossfadeDurationSeconds = 0.02;

    //==========================================================================
    // v0.1.x -> v0.2.0 structural state migration (docs/design-brief.md's
    // "State migration" guarantee #7): v1 sessions serialize a single
    // "crossoverFreq" PARAM element (id="crossoverFreq") - that parameter ID
    // no longer exists in v0.2.0's ParameterLayout, replaced by the
    // splitLowHz/splitHighHz pair. Best-effort, lossy, one-directional: the
    // old single split value is closer in role to the new splitHighHz (the
    // v2 high-band edge) than to splitLowHz, since v1 never had a dedicated
    // mid band - clamped into splitHighHz's new 300-2000 Hz range on import.
    // v1's shipped default crossoverFreq is 250 Hz, i.e. below that new
    // floor, so the single most common migration path (an untouched v1
    // session) lands exactly at the 300 Hz floor - see
    // tests/StateMigrationTests.cpp's dedicated test for that path.
    //
    // splitLowHz itself is deliberately NOT injected here: it simply falls
    // back to its own v0.2.0 ParameterLayout default (120 Hz) via
    // AudioProcessorValueTreeState::replaceState()'s normal "unmentioned
    // parameter ID keeps its current/default value" behaviour - no v1 value
    // exists to migrate it from in the first place. Same for every new
    // mid-band/Tight parameter.
    constexpr float legacySplitHighFloorHz = 300.0f;
    constexpr float legacySplitHighCeilingHz = 2000.0f;
    constexpr float legacyCrossoverDefaultHz = 250.0f;

    void migrateLegacySingleCrossover (juce::XmlElement& stateXml)
    {
        for (auto* paramXml : stateXml.getChildIterator())
        {
            if (! paramXml->hasTagName ("PARAM") || paramXml->getStringAttribute ("id") != "crossoverFreq")
                continue;

            const auto legacyHz = static_cast<float> (paramXml->getDoubleAttribute ("value", legacyCrossoverDefaultHz));
            const auto migratedSplitHighHz = juce::jlimit (legacySplitHighFloorHz, legacySplitHighCeilingHz, legacyHz);

            // A v0.2.0+ session's own saved state never contains a
            // "crossoverFreq" element at all (the parameter no longer
            // exists), so this only ever fires for genuine v1 state - but
            // guard against a malformed/hand-edited file claiming both
            // anyway, rather than overwriting an explicit splitHighHz value
            // that's already present.
            if (stateXml.getChildByAttribute ("id", "splitHighHz") == nullptr)
            {
                auto* splitHighXml = new juce::XmlElement ("PARAM");
                splitHighXml->setAttribute ("id", "splitHighHz");
                splitHighXml->setAttribute ("value", static_cast<double> (migratedSplitHighHz));
                stateXml.addChildElement (splitHighXml);
            }

            break;
        }
    }

    //==========================================================================
    // v0.2.0 -> v0.3.0 state schema migration (brief §4, "State migration
    // plan (schema v1 -> v2)").
    //
    // v0.3.0 adds three engine selectors whose APVTS defaults name the NEW
    // circuit-derived engines, so that a genuinely fresh instance boots into
    // them. Saved state must not inherit that: a v0.1/v0.2 session says
    // nothing about driveEngine/lowCompDetector/gateMode, and
    // APVTS::replaceState() leaves an unmentioned parameter at its (new)
    // default - which would silently change the sound of every existing
    // session. Injecting the legacy value for exactly those three IDs is what
    // keeps old sessions bit-identical.
    //
    // The marker is a `stateVersion` attribute on the APVTS root element.
    // Attributes survive the copyState()/createXml() round-trip, and a v0.2.0
    // reader ignores an attribute it does not know, so writing it costs no
    // backward compatibility.
    constexpr const char* stateVersionAttribute = "stateVersion";
    constexpr int currentStateVersion = 2;

    // Index of the legacy (v0.2.0-equivalent) option in each engine selector's
    // choice list - "Classic", "Classic Peak" and "Classic" respectively, all
    // of which are element 0 (see src/params/ParameterLayout.cpp).
    constexpr double legacyEngineChoiceIndex = 0.0;

    void injectLegacyEngineParam (juce::XmlElement& stateXml, const char* parameterId)
    {
        // Defensive in the same way migrateLegacySingleCrossover() is: never
        // overwrite a value that is already explicitly present, even in a
        // hand-edited or malformed file.
        if (stateXml.getChildByAttribute ("id", parameterId) != nullptr)
            return;

        auto* paramXml = new juce::XmlElement ("PARAM");
        paramXml->setAttribute ("id", parameterId);
        paramXml->setAttribute ("value", legacyEngineChoiceIndex);
        stateXml.addChildElement (paramXml);
    }

    void migrateToStateV2 (juce::XmlElement& stateXml)
    {
        // Any state carrying a stateVersion is v0.3.0-or-later and already
        // says what it means about the engines - leave it alone.
        if (stateXml.hasAttribute (stateVersionAttribute))
            return;

        injectLegacyEngineParam (stateXml, ParamIDs::driveEngine);
        injectLegacyEngineParam (stateXml, ParamIDs::lowCompDetector);
        injectLegacyEngineParam (stateXml, ParamIDs::gateMode);
    }

    //==========================================================================
    // M2 preset system (.scaffold/specs/preset-system-m2.md,
    // docs/preset-system-notes.md's replication recipe from basilica-audio/
    // nave's pilot). The small, Crypta-specific config surface
    // basilica::presets::PresetManager needs - everything else about the
    // preset system is fully generic and portable across the suite.
    basilica::presets::PresetManagerConfig makePresetManagerConfig()
    {
        // JucePlugin_CFBundleIdentifier expands to a raw (unquoted) token
        // sequence, not a string literal - JUCE_STRINGIFY() is the
        // documented way to turn it into one. Always "com.yvesvogl.crypta"
        // here (BUNDLE_ID in CMakeLists.txt), matching the "plugin" field
        // baked into every presets/factory/*.json file.
        basilica::presets::PresetManagerConfig config;
        config.pluginId = JUCE_STRINGIFY (JucePlugin_CFBundleIdentifier);
        config.pluginName = JucePlugin_Name;
        config.manufacturerName = "Yves Vogl";
        config.pluginVersion = JucePlugin_VersionString;
        // userPresetsDirectoryOverrideForTests intentionally left
        // default-constructed (empty) - production instances always use the
        // real platform-standard preset location (see PresetManager.h).

        // Preset-path half of the v0.3.0 engine migration (brief §4 step 3).
        // The session path is migrateToStateV2() above; this is the same idea
        // for presets, which never go through setStateInformation().
        //
        // Without this, a v0.1/v0.2 user preset - which cannot name the three
        // engine selectors - would pick up their new Circuit/Smooth RMS/
        // Modern defaults from applyParsedPreset()'s reset-then-apply, and a
        // user's tuned preset would quietly change character. It also covers
        // the user-saved "Default" that shadows the factory one, which is the
        // preset a fresh session of an existing user actually boots into.
        config.legacyParameterCutoffVersion = "0.3.0";
        config.legacyParameterDefaults = {
            { ParamIDs::driveEngine, 0.0f },     // Classic
            { ParamIDs::lowCompDetector, 0.0f }, // Classic Peak
            { ParamIDs::gateMode, 0.0f },        // Classic
        };

        return config;
    }

    // BinaryData symbol names are derived from the presets/factory/*.json
    // file names passed to juce_add_binary_data() in CMakeLists.txt (dots
    // become underscores) - this list must stay in sync with that SOURCES
    // list. Order here only affects factory-preset iteration order before
    // getAllPresets() re-sorts alphabetically, so it isn't otherwise
    // significant.
    std::vector<basilica::presets::FactoryPresetAsset> makeFactoryPresetAssets()
    {
        return {
            { BinaryData::default_json, BinaryData::default_jsonSize },
            { BinaryData::glueAndGrind_json, BinaryData::glueAndGrind_jsonSize },
            { BinaryData::subLock_json, BinaryData::subLock_jsonSize },
            { BinaryData::throat_json, BinaryData::throat_jsonSize },
            { BinaryData::fuzzWall_json, BinaryData::fuzzWall_jsonSize },
            { BinaryData::cutThrough_json, BinaryData::cutThrough_jsonSize },
            { BinaryData::definitionOnly_json, BinaryData::definitionOnly_jsonSize },
            { BinaryData::cleanLowLoudTop_json, BinaryData::cleanLowLoudTop_jsonSize },
            { BinaryData::cabColoredGrind_json, BinaryData::cabColoredGrind_jsonSize },

            // v0.3.0 Circuit-engine showcases.
            { BinaryData::circuitFoundation_json, BinaryData::circuitFoundation_jsonSize },
            { BinaryData::circuitGrind_json, BinaryData::circuitGrind_jsonSize },
            { BinaryData::circuitKnife_json, BinaryData::circuitKnife_jsonSize },
        };
    }
}

//==============================================================================
CryptaAudioProcessor::CryptaAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout()),
      presetManager (apvts, makePresetManagerConfig(), makeFactoryPresetAssets())
{
    inputGainDb = apvts.getRawParameterValue (ParamIDs::inputGain);
    outputGainDb = apvts.getRawParameterValue (ParamIDs::outputGain);
    bypassFlag = apvts.getRawParameterValue (ParamIDs::bypass);
    outputClipEnabled = apvts.getRawParameterValue (ParamIDs::outputClip);
    splitLowHzParam = apvts.getRawParameterValue (ParamIDs::splitLowHz);
    splitHighHzParam = apvts.getRawParameterValue (ParamIDs::splitHighHz);
    lowLevelDb = apvts.getRawParameterValue (ParamIDs::lowLevel);
    midLevelDb = apvts.getRawParameterValue (ParamIDs::midLevel);
    highLevelDb = apvts.getRawParameterValue (ParamIDs::highLevel);
    bypassParameter = apvts.getParameter (ParamIDs::bypass);

    gateEnabled = apvts.getRawParameterValue (ParamIDs::gateEnabled);
    gateThresholdDb = apvts.getRawParameterValue (ParamIDs::gateThreshold);
    gateRatio = apvts.getRawParameterValue (ParamIDs::gateRatio);
    gateAttackMs = apvts.getRawParameterValue (ParamIDs::gateAttack);
    gateReleaseMs = apvts.getRawParameterValue (ParamIDs::gateRelease);

    lowCompThresholdDb = apvts.getRawParameterValue (ParamIDs::lowCompThreshold);
    lowCompRatio = apvts.getRawParameterValue (ParamIDs::lowCompRatio);
    lowCompAttackMs = apvts.getRawParameterValue (ParamIDs::lowCompAttack);
    lowCompReleaseMs = apvts.getRawParameterValue (ParamIDs::lowCompRelease);
    lowCompMakeupDb = apvts.getRawParameterValue (ParamIDs::lowCompMakeup);
    lowCompMixPercent = apvts.getRawParameterValue (ParamIDs::lowCompMix);

    lowGrowlEnabled = apvts.getRawParameterValue (ParamIDs::lowGrowl);
    lowGrowlAmountPercent = apvts.getRawParameterValue (ParamIDs::lowGrowlAmount);
    lowGrowlTonePercent = apvts.getRawParameterValue (ParamIDs::lowGrowlTone);

    midDrivePercent = apvts.getRawParameterValue (ParamIDs::midDrive);

    highTightHzParam = apvts.getRawParameterValue (ParamIDs::highTightHz);
    highVoicingChoice = apvts.getRawParameterValue (ParamIDs::highVoicing);
    highDrivePercent = apvts.getRawParameterValue (ParamIDs::highDrive);
    highTonePercent = apvts.getRawParameterValue (ParamIDs::highTone);
    highBlendPercent = apvts.getRawParameterValue (ParamIDs::highBlend);

    eqEnabled = apvts.getRawParameterValue (ParamIDs::eqEnabled);
    eqLowShelfFreqHz = apvts.getRawParameterValue (ParamIDs::eqLowShelfFreq);
    eqLowShelfGainDb = apvts.getRawParameterValue (ParamIDs::eqLowShelfGain);
    eqPeak1FreqHz = apvts.getRawParameterValue (ParamIDs::eqPeak1Freq);
    eqPeak1GainDb = apvts.getRawParameterValue (ParamIDs::eqPeak1Gain);
    eqPeak1Q = apvts.getRawParameterValue (ParamIDs::eqPeak1Q);
    eqPeak2FreqHz = apvts.getRawParameterValue (ParamIDs::eqPeak2Freq);
    eqPeak2GainDb = apvts.getRawParameterValue (ParamIDs::eqPeak2Gain);
    eqPeak2Q = apvts.getRawParameterValue (ParamIDs::eqPeak2Q);
    eqHighShelfFreqHz = apvts.getRawParameterValue (ParamIDs::eqHighShelfFreq);
    eqHighShelfGainDb = apvts.getRawParameterValue (ParamIDs::eqHighShelfGain);

    irEnabled = apvts.getRawParameterValue (ParamIDs::irEnabled);
    irMixPercent = apvts.getRawParameterValue (ParamIDs::irMix);

    driveEngineChoice = apvts.getRawParameterValue (ParamIDs::driveEngine);
    highBiasPercent = apvts.getRawParameterValue (ParamIDs::highBias);

    lowCompDetectorChoice = apvts.getRawParameterValue (ParamIDs::lowCompDetector);
    lowCompKneeDb = apvts.getRawParameterValue (ParamIDs::lowCompKnee);
    lowCompAutoReleaseFlag = apvts.getRawParameterValue (ParamIDs::lowCompAutoRelease);
    lowCompAutoMakeupFlag = apvts.getRawParameterValue (ParamIDs::lowCompAutoMakeup);

    gateModeChoice = apvts.getRawParameterValue (ParamIDs::gateMode);
    gateHysteresisDb = apvts.getRawParameterValue (ParamIDs::gateHysteresis);
    gateHoldMs = apvts.getRawParameterValue (ParamIDs::gateHold);
    gateScHpfHz = apvts.getRawParameterValue (ParamIDs::gateScHpf);
    gateRangeDb = apvts.getRawParameterValue (ParamIDs::gateRange);

    clipCeilingDb = apvts.getRawParameterValue (ParamIDs::clipCeiling);

    jassert (inputGainDb != nullptr);
    jassert (outputGainDb != nullptr);
    jassert (bypassFlag != nullptr);
    jassert (outputClipEnabled != nullptr);
    jassert (splitLowHzParam != nullptr);
    jassert (splitHighHzParam != nullptr);
    jassert (lowLevelDb != nullptr);
    jassert (midLevelDb != nullptr);
    jassert (highLevelDb != nullptr);
    jassert (bypassParameter != nullptr);

    jassert (gateEnabled != nullptr);
    jassert (gateThresholdDb != nullptr);
    jassert (gateRatio != nullptr);
    jassert (gateAttackMs != nullptr);
    jassert (gateReleaseMs != nullptr);

    jassert (lowCompThresholdDb != nullptr);
    jassert (lowCompRatio != nullptr);
    jassert (lowCompAttackMs != nullptr);
    jassert (lowCompReleaseMs != nullptr);
    jassert (lowCompMakeupDb != nullptr);
    jassert (lowCompMixPercent != nullptr);

    jassert (lowGrowlEnabled != nullptr);
    jassert (lowGrowlAmountPercent != nullptr);
    jassert (lowGrowlTonePercent != nullptr);

    jassert (midDrivePercent != nullptr);

    jassert (highTightHzParam != nullptr);
    jassert (highVoicingChoice != nullptr);
    jassert (highDrivePercent != nullptr);
    jassert (highTonePercent != nullptr);
    jassert (highBlendPercent != nullptr);

    jassert (eqEnabled != nullptr);
    jassert (eqLowShelfFreqHz != nullptr);
    jassert (eqLowShelfGainDb != nullptr);
    jassert (eqPeak1FreqHz != nullptr);
    jassert (eqPeak1GainDb != nullptr);
    jassert (eqPeak1Q != nullptr);
    jassert (eqPeak2FreqHz != nullptr);
    jassert (eqPeak2GainDb != nullptr);
    jassert (eqPeak2Q != nullptr);
    jassert (eqHighShelfFreqHz != nullptr);
    jassert (eqHighShelfGainDb != nullptr);

    jassert (irEnabled != nullptr);
    jassert (irMixPercent != nullptr);

    jassert (driveEngineChoice != nullptr);
    jassert (highBiasPercent != nullptr);

    jassert (lowCompDetectorChoice != nullptr);
    jassert (lowCompKneeDb != nullptr);
    jassert (lowCompAutoReleaseFlag != nullptr);
    jassert (lowCompAutoMakeupFlag != nullptr);

    jassert (gateModeChoice != nullptr);
    jassert (gateHysteresisDb != nullptr);
    jassert (gateHoldMs != nullptr);
    jassert (gateScHpfHz != nullptr);
    jassert (gateRangeDb != nullptr);

    jassert (clipCeilingDb != nullptr);

    // M2 default resolution: user "Default" preset > factory "Default"
    // preset > the ParameterLayout defaults apvts was just constructed with
    // above (see PresetManager::applyStartupDefault()'s docs).
    presetManager.applyStartupDefault();
}

CryptaAudioProcessor::~CryptaAudioProcessor() = default;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout CryptaAudioProcessor::createParameterLayout()
{
    return cryp::createParameterLayout();
}

//==============================================================================
const juce::String CryptaAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool CryptaAudioProcessor::acceptsMidi() const
{
    return false;
}

bool CryptaAudioProcessor::producesMidi() const
{
    return false;
}

bool CryptaAudioProcessor::isMidiEffect() const
{
    return false;
}

double CryptaAudioProcessor::getTailLengthSeconds() const
{
    // Issue #58: report the IR loader's actual loaded-IR duration instead of
    // a hardcoded 0 - unaffected by the IR loader's v0.2.0 relocation to the
    // Mid+High branch, since this just reports the currently loaded IR's own
    // duration regardless of where in the chain it sits.
    return irLoader.getTailLengthSeconds();
}

int CryptaAudioProcessor::getNumPrograms()
{
    return 1;
}

int CryptaAudioProcessor::getCurrentProgram()
{
    return 0;
}

void CryptaAudioProcessor::setCurrentProgram (int)
{
}

const juce::String CryptaAudioProcessor::getProgramName (int)
{
    return {};
}

void CryptaAudioProcessor::changeProgramName (int, const juce::String&)
{
}

//==============================================================================
void CryptaAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32> (getTotalNumOutputChannels());

    inputGainProcessor.setRampDurationSeconds (gainRampDurationSeconds);
    inputGainProcessor.prepare (spec);
    inputGainProcessor.setGainDecibels (inputGainDb->load (std::memory_order_relaxed));

    outputGainProcessor.setRampDurationSeconds (gainRampDurationSeconds);
    outputGainProcessor.prepare (spec);
    outputGainProcessor.setGainDecibels (outputGainDb->load (std::memory_order_relaxed));

    // Full-band input noise gate.
    gate.prepare (spec);

    // v0.2.0: two cascaded LR4 crossovers. lowSplit peels off the Low band;
    // midHighSplit further splits the remainder into Mid and High - see
    // docs/design-brief.md's Topology section.
    lowSplit.prepare (spec);
    lowSplit.setCutoffFrequency (splitLowHzParam->load (std::memory_order_relaxed));

    midHighSplit.prepare (spec);
    midHighSplit.setCutoffFrequency (cryp::clampSplitHighHz (splitLowHzParam->load (std::memory_order_relaxed),
                                                              splitHighHzParam->load (std::memory_order_relaxed)));

    // v0.2.0: Low band phase-alignment allpass (see PluginProcessor.h's
    // member docs / src/dsp/PhaseAlignFilter.h) - always tied to the same
    // effective cutoff as midHighSplit above.
    lowBandPhaseAlign.prepare (spec);
    lowBandPhaseAlign.setCutoffFrequency (cryp::clampSplitHighHz (splitLowHzParam->load (std::memory_order_relaxed),
                                                                   splitHighHzParam->load (std::memory_order_relaxed)));

    // Low-band parallel compressor. The DryWetMixer inside needs its mix
    // proportion primed *before* prepare() runs its internal reset() (JUCE
    // 8.0.14 gotcha - see docs/architecture.md), so the current lowCompMix
    // value is read and passed in here rather than set afterwards.
    lowCompressor.prepare (spec, lowCompMixPercent->load (std::memory_order_relaxed) / 100.0f);

    // v0.4.0 Graaawl branch on the low band. Its controls are pushed in
    // before prepare() so the internal gain smoother starts at the value the
    // session actually has (rather than ramping up from zero on the first
    // block of a session that was saved with Graaawl on) - the same class of
    // priming the DryWetMixer stages above need.
    lowGrowlStage.setEnabled (lowGrowlEnabled->load (std::memory_order_relaxed) >= 0.5f);
    lowGrowlStage.setAmount (lowGrowlAmountPercent->load (std::memory_order_relaxed) / 100.0f);
    lowGrowlStage.setTone (lowGrowlTonePercent->load (std::memory_order_relaxed) / 100.0f);
    lowGrowlStage.prepare (spec);

    // Mid band: staged drive, no DryWetMixer (no blend control - see
    // cryp::MidBand's class docs), so no priming gotcha here.
    midBand.prepare (spec);
    midBand.setDrive (midDrivePercent->load (std::memory_order_relaxed) / 100.0f);

    // High band: Tight pre-drive HPF + oversampled distortion voicing. Same
    // DryWetMixer-priming requirement for highBlend as the low-band
    // compressor above.
    highVoicing.prepare (spec, highBlendPercent->load (std::memory_order_relaxed) / 100.0f);
    highVoicing.setTightHz (highTightHzParam->load (std::memory_order_relaxed));

    // v0.3.0 Circuit engine. Always prepared, whichever engine is currently
    // selected, so that switching is a branch rather than an allocation - and
    // so the crossfade can render both.
    //
    // Every control is pushed in BEFORE prepare(), for the same reason
    // cryp::LowGrowl's and the DryWetMixer stages' values are above:
    // CircuitDrive::prepare() snaps the engine's per-block control ramps to
    // whatever values it finds, and any value it does not find is ramped up
    // to from a constructed default across the first block instead. That ramp
    // is exactly one block long however long the block is, so an unsnapped
    // ramp makes the first ~10-20 ms of a render a function of the host's
    // buffer size - which is precisely how an offline bounce came to differ
    // from realtime playback (issue #34; measured at a -20.2 dB null over the
    // first 1000 samples, 0.045 peak difference, between a 512-sample and a
    // 1024-sample render of the same passage). See
    // tests/OfflineRealtimeNullTests.cpp.
    circuitDrive.setSplitHighHz (cryp::clampSplitHighHz (splitLowHzParam->load (std::memory_order_relaxed),
                                                          splitHighHzParam->load (std::memory_order_relaxed)));
    circuitDrive.setHighTightHz (highTightHzParam->load (std::memory_order_relaxed));
    circuitDrive.setMidDrive (midDrivePercent->load (std::memory_order_relaxed) / 100.0f);
    circuitDrive.setVoicing (static_cast<cryp::VoicingType> (
        juce::jlimit (0, 2, static_cast<int> (highVoicingChoice->load (std::memory_order_relaxed)))));
    circuitDrive.setHighDrive (highDrivePercent->load (std::memory_order_relaxed) / 100.0f);
    circuitDrive.setHighTone (highTonePercent->load (std::memory_order_relaxed) / 100.0f);
    circuitDrive.setHighBlend (highBlendPercent->load (std::memory_order_relaxed) / 100.0f);
    circuitDrive.setHighBias (highBiasPercent->load (std::memory_order_relaxed) / 100.0f);
    circuitDrive.setMidLevelDb (midLevelDb->load (std::memory_order_relaxed));
    circuitDrive.setHighLevelDb (highLevelDb->load (std::memory_order_relaxed));

    circuitDrive.prepare (spec);

    // Seed the engine-change detector so the very first block after
    // prepareToPlay() is not mistaken for a switch.
    lastDriveEngineWasCircuit = driveEngineChoice->load (std::memory_order_relaxed) >= 0.5f;
    engineCrossfadeRemaining = 0;

    // Per-band level trims, smoothed the same way as the input/output gains
    // to avoid zipper noise on automation.
    lowGainProcessor.setRampDurationSeconds (gainRampDurationSeconds);
    lowGainProcessor.prepare (spec);
    lowGainProcessor.setGainDecibels (lowLevelDb->load (std::memory_order_relaxed));

    midGainProcessor.setRampDurationSeconds (gainRampDurationSeconds);
    midGainProcessor.prepare (spec);
    midGainProcessor.setGainDecibels (midLevelDb->load (std::memory_order_relaxed));

    highGainProcessor.setRampDurationSeconds (gainRampDurationSeconds);
    highGainProcessor.prepare (spec);
    highGainProcessor.setGainDecibels (highLevelDb->load (std::memory_order_relaxed));

    // Post-sum 4-band EQ.
    eq.prepare (spec);

    // IR loader. v0.2.0 relocates this stage to process the Mid+High
    // post-sum signal only (see processChunk()) - its own prepare()/mix
    // priming contract is unchanged from v1.
    irLoader.prepare (spec, irMixPercent->load (std::memory_order_relaxed) / 100.0f);

    // v0.3.0 safety clip and metering.
    outputClipper.prepare (spec);
    outputClipper.setCeilingDb (clipCeilingDb->load (std::memory_order_relaxed));
    meterTaps.reset();
    lowBandMeterLevel = midBandMeterLevel = highBandMeterLevel = 0.0f;
    pendingMidBandLevel = pendingHighBandLevel = 0.0f;

    // Issue #9: (re)allocate the low-band compensation delay line for the
    // new spec/max-delay bound. setMaximumDelayInSamples() may allocate, so
    // it must only ever be called here, never from processBlock().
    lowBandLatencyDelay.setMaximumDelayInSamples (maxLatencyCompensationSamples);
    lowBandLatencyDelay.prepare (spec);

    // Same contract for the Circuit-path alignment delay (see its docs in
    // PluginProcessor.h): allocation happens here, never in processBlock().
    circuitAlignDelay.setMaximumDelayInSamples (maxLatencyCompensationSamples);
    circuitAlignDelay.prepare (spec);

    // Issue #87: same contract again for the bypass dry-path delay.
    bypassDryDelay.setMaximumDelayInSamples (maxLatencyCompensationSamples);
    bypassDryDelay.prepare (spec);

    // Issue #87: the wet/dry bypass crossfade. Primed to the CURRENT bypass
    // state (rather than always starting from "wet") so a session saved
    // while bypassed, or a host that queries the bypass parameter before the
    // first processBlock(), doesn't open with an audible ramp-in from the
    // wrong side.
    bypassWetMix.reset (sampleRate, bypassCrossfadeDurationSeconds);
    bypassWetMix.setCurrentAndTargetValue (bypassFlag->load (std::memory_order_relaxed) >= 0.5f ? 0.0f : 1.0f);

    // Pre-allocate every band/scratch buffer to the promised block size so
    // processBlock() never resizes a buffer on the audio thread, even if a
    // host later sends an oversized block (handled defensively by chunking
    // in processBlock() - see processChunk()).
    preparedBlockSize = samplesPerBlock;
    lowBandBuffer.setSize (static_cast<int> (spec.numChannels), preparedBlockSize);
    remainderBandBuffer.setSize (static_cast<int> (spec.numChannels), preparedBlockSize);
    midBandBuffer.setSize (static_cast<int> (spec.numChannels), preparedBlockSize);
    highBandBuffer.setSize (static_cast<int> (spec.numChannels), preparedBlockSize);
    midHighSumBuffer.setSize (static_cast<int> (spec.numChannels), preparedBlockSize);
    engineCrossfadeBuffer.setSize (static_cast<int> (spec.numChannels), preparedBlockSize);
    bypassDryBuffer.setSize (static_cast<int> (spec.numChannels), preparedBlockSize);

    updateLatencyCompensation();
}

//==============================================================================
int CryptaAudioProcessor::computeTotalLatencySamples() const noexcept
{
    // v0.2.0: the Mid+High branch's two independently-owned but identically
    // configured oversampling stages (cryp::MidBand, cryp::Voicing) are the
    // only sources of latency in the chain (the gate, low-band compressor,
    // EQ and IR loader - default zero-latency Convolution - are all
    // zero-latency); their reported latencies are guaranteed numerically
    // equal by construction (same factor/filter type - see MidBand.h's
    // class docs), but jmax() here is a defensive, self-documenting
    // guarantee rather than relying on that equality silently holding.
    // v0.3.0 adds a third contributor: the Circuit engine's own shared
    // oversampling region. Taking the maximum across BOTH engines (rather
    // than the currently-selected one) is what makes the reported latency
    // depend only on the sample rate, so switching driveEngine never has to
    // re-report latency to the host - see circuitAlignDelay's docs in
    // PluginProcessor.h.
    return juce::jmax (midBand.getLatencySamples(),
                        juce::jmax (highVoicing.getLatencySamples(), circuitDrive.getLatencySamples()));
}

void CryptaAudioProcessor::updateLatencyCompensation()
{
    const auto totalLatencySamples = juce::jlimit (0, maxLatencyCompensationSamples, computeTotalLatencySamples());

    setLatencySamples (totalLatencySamples);

    // Pad the Circuit path up to the reported total. Zero whenever the two
    // engines already agree (every rate at or below 50 kHz, where both run
    // 4x), non-zero at 88.2 kHz and above.
    circuitAlignDelay.setMaximumDelayInSamples (maxLatencyCompensationSamples);
    circuitAlignDelay.setDelay (static_cast<float> (
        juce::jlimit (0, maxLatencyCompensationSamples, totalLatencySamples - circuitDrive.getLatencySamples())));

    // The low band bypasses oversampling entirely, so it must be delayed by
    // the same amount the Mid+High branch's oversampling stages delay it,
    // keeping all bands time-aligned when they are summed back together in
    // processChunk().
    lowBandLatencyDelay.setDelay (static_cast<float> (totalLatencySamples));

    // Issue #87: the bypass dry path bypasses the whole chain (by definition),
    // so it needs the same delay applied for the same reason the low band
    // does above - it must land exactly at the sample the plugin reports as
    // its own latency, or a host's plugin delay compensation pulls a
    // bypassed instance's output early relative to the compensated timeline.
    bypassDryDelay.setDelay (static_cast<float> (totalLatencySamples));
}

void CryptaAudioProcessor::releaseResources()
{
}

//==============================================================================
void CryptaAudioProcessor::reset()
{
    // Issue #56: clears every per-stage DSP class's own state (each already
    // exposes its own real-time-safe reset() for exactly this purpose - see
    // src/dsp/*.h) so a host transport stop/loop/rewind doesn't leave a
    // decaying tail ringing into whatever plays next. No allocation: every
    // stage's reset() only clears already-allocated storage.
    inputGainProcessor.reset();
    outputGainProcessor.reset();

    gate.reset();
    lowSplit.reset();
    midHighSplit.reset();
    lowBandPhaseAlign.reset();
    lowCompressor.reset();
    lowGrowlStage.reset();
    midBand.reset();
    highVoicing.reset();
    circuitDrive.reset();
    engineCrossfadeRemaining = 0;

    lowGainProcessor.reset();
    midGainProcessor.reset();
    highGainProcessor.reset();

    eq.reset();
    irLoader.reset();

    lowBandLatencyDelay.reset();
    circuitAlignDelay.reset();
    outputClipper.reset();

    // Issue #87: flush the dry delay line's history (a transport rewind must
    // not replay pre-rewind audio into the bypass blend) and snap the
    // crossfade to wherever it was heading, cancelling any ramp in flight -
    // the same "stop cleanly, don't leave stale motion" contract every reset
    // above already gives its own stage.
    bypassDryDelay.reset();
    bypassWetMix.setCurrentAndTargetValue (bypassWetMix.getTargetValue());

    meterTaps.reset();
    lowBandMeterLevel = midBandMeterLevel = highBandMeterLevel = 0.0f;
    pendingMidBandLevel = pendingHighBandLevel = 0.0f;
}

bool CryptaAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mono = juce::AudioChannelSet::mono();
    const auto stereo = juce::AudioChannelSet::stereo();

    const auto mainOut = layouts.getMainOutputChannelSet();
    const auto mainIn = layouts.getMainInputChannelSet();

    if (mainOut != mono && mainOut != stereo)
        return false;

    if (mainOut != mainIn)
        return false;

    return true;
}

void CryptaAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto totalNumInputChannels = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Buses are constrained to in == out (mono or stereo), so this is
    // normally a no-op, but it's cheap insurance against stray channels.
    for (auto channel = totalNumInputChannels; channel < totalNumOutputChannels; ++channel)
        buffer.clear (channel, 0, buffer.getNumSamples());

    // Issue #87: bypass is a crossfade (bypassWetMix, advanced per-sample in
    // applyBypassCrossfade()) between the wet chain below - which keeps
    // running unconditionally, bypassed or not, so none of its state
    // (filter memory, envelope followers, oversampling FIR history) ever
    // freezes - and a delayed copy of the untouched input. There is
    // deliberately no early return here any more: freezing the wet chain
    // while bypassed is exactly what made re-engaging it click (stale state
    // resuming into a live signal), which is the second half of what this
    // issue reports.
    bypassWetMix.setTargetValue (bypassFlag->load (std::memory_order_relaxed) >= 0.5f ? 0.0f : 1.0f);

    // Parameters are read once per host block (not per chunk/sample): the
    // dsp::Gain smoothers and the various stages' own control-rate updates
    // recompute cheaply enough at control rate, and re-reading the same
    // atomic per chunk would buy nothing.
    inputGainProcessor.setGainDecibels (inputGainDb->load (std::memory_order_relaxed));
    outputGainProcessor.setGainDecibels (outputGainDb->load (std::memory_order_relaxed));
    lowGainProcessor.setGainDecibels (lowLevelDb->load (std::memory_order_relaxed));
    midGainProcessor.setGainDecibels (midLevelDb->load (std::memory_order_relaxed));
    highGainProcessor.setGainDecibels (highLevelDb->load (std::memory_order_relaxed));

    const auto rawSplitLowHz = splitLowHzParam->load (std::memory_order_relaxed);
    const auto rawSplitHighHz = splitHighHzParam->load (std::memory_order_relaxed);
    const auto effectiveSplitHighHz = cryp::clampSplitHighHz (rawSplitLowHz, rawSplitHighHz);
    lowSplit.setCutoffFrequency (rawSplitLowHz);
    midHighSplit.setCutoffFrequency (effectiveSplitHighHz);
    // Must always track midHighSplit's own effective cutoff exactly - see
    // src/dsp/PhaseAlignFilter.h's class docs.
    lowBandPhaseAlign.setCutoffFrequency (effectiveSplitHighHz);

    gate.setEnabled (gateEnabled->load (std::memory_order_relaxed) >= 0.5f);
    gate.setThresholdDb (gateThresholdDb->load (std::memory_order_relaxed));
    gate.setRatio (gateRatio->load (std::memory_order_relaxed));
    gate.setAttackMs (gateAttackMs->load (std::memory_order_relaxed));
    gate.setReleaseMs (gateReleaseMs->load (std::memory_order_relaxed));

    // v0.3.0 Modern gate controls. Inert while gateMode is Classic.
    gate.setModernMode (gateModeChoice->load (std::memory_order_relaxed) >= 0.5f);
    gate.setHysteresisDb (gateHysteresisDb->load (std::memory_order_relaxed));
    gate.setHoldMs (gateHoldMs->load (std::memory_order_relaxed));
    gate.setRangeDb (gateRangeDb->load (std::memory_order_relaxed));
    gate.setSidechainHighPassHz (gateScHpfHz->load (std::memory_order_relaxed));

    lowCompressor.setThresholdDb (lowCompThresholdDb->load (std::memory_order_relaxed));
    lowCompressor.setRatio (lowCompRatio->load (std::memory_order_relaxed));
    lowCompressor.setAttackMs (lowCompAttackMs->load (std::memory_order_relaxed));
    lowCompressor.setReleaseMs (lowCompReleaseMs->load (std::memory_order_relaxed));
    lowCompressor.setWetMixProportion (lowCompMixPercent->load (std::memory_order_relaxed) / 100.0f);

    // v0.3.0 detector engine and its controls. Knee and auto-release are
    // Smooth-RMS-only; auto-makeup is read by both engines, so it is folded
    // into the makeup gain here rather than inside the detector.
    lowCompressor.setUseSmoothRmsDetector (lowCompDetectorChoice->load (std::memory_order_relaxed) >= 0.5f);
    lowCompressor.setKneeDb (lowCompKneeDb->load (std::memory_order_relaxed));
    lowCompressor.setAutoRelease (lowCompAutoReleaseFlag->load (std::memory_order_relaxed) >= 0.5f);
    lowCompressor.setAutoMakeup (lowCompAutoMakeupFlag->load (std::memory_order_relaxed) >= 0.5f);
    lowCompressor.setMakeupGainDb (
        lowCompressor.getEffectiveMakeupDb (lowCompMakeupDb->load (std::memory_order_relaxed)));

    // v0.4.0 Graaawl. Read every block like every other control; the stage
    // itself decides whether it has any work to do (and does none at all once
    // it is off and faded out - see cryp::LowGrowl::process()).
    lowGrowlStage.setEnabled (lowGrowlEnabled->load (std::memory_order_relaxed) >= 0.5f);
    lowGrowlStage.setAmount (lowGrowlAmountPercent->load (std::memory_order_relaxed) / 100.0f);
    lowGrowlStage.setTone (lowGrowlTonePercent->load (std::memory_order_relaxed) / 100.0f);

    midBand.setDrive (midDrivePercent->load (std::memory_order_relaxed) / 100.0f);

    highVoicing.setTightHz (highTightHzParam->load (std::memory_order_relaxed));
    const auto voicingIndex = static_cast<int> (highVoicingChoice->load (std::memory_order_relaxed));
    highVoicing.setVoicing (static_cast<cryp::VoicingType> (juce::jlimit (0, 2, voicingIndex)));
    highVoicing.setDrive (highDrivePercent->load (std::memory_order_relaxed) / 100.0f);
    highVoicing.setTone (highTonePercent->load (std::memory_order_relaxed) / 100.0f);
    highVoicing.setWetMixProportion (highBlendPercent->load (std::memory_order_relaxed) / 100.0f);

    // Circuit engine reads the same user-facing controls as Classic, plus the
    // two per-band level trims (which it applies internally, because its Mid
    // and High bands only exist inside its own oversampled region and are
    // summed before they come back out).
    circuitDrive.setSplitHighHz (effectiveSplitHighHz);
    circuitDrive.setMidDrive (midDrivePercent->load (std::memory_order_relaxed) / 100.0f);
    circuitDrive.setVoicing (static_cast<cryp::VoicingType> (juce::jlimit (0, 2, voicingIndex)));
    circuitDrive.setHighDrive (highDrivePercent->load (std::memory_order_relaxed) / 100.0f);
    circuitDrive.setHighTone (highTonePercent->load (std::memory_order_relaxed) / 100.0f);
    circuitDrive.setHighTightHz (highTightHzParam->load (std::memory_order_relaxed));
    circuitDrive.setHighBlend (highBlendPercent->load (std::memory_order_relaxed) / 100.0f);
    circuitDrive.setHighBias (highBiasPercent->load (std::memory_order_relaxed) / 100.0f);
    circuitDrive.setMidLevelDb (midLevelDb->load (std::memory_order_relaxed));
    circuitDrive.setHighLevelDb (highLevelDb->load (std::memory_order_relaxed));

    eq.setLowShelf (eqLowShelfFreqHz->load (std::memory_order_relaxed), eqLowShelfGainDb->load (std::memory_order_relaxed));
    eq.setPeak1 (eqPeak1FreqHz->load (std::memory_order_relaxed), eqPeak1GainDb->load (std::memory_order_relaxed), eqPeak1Q->load (std::memory_order_relaxed));
    eq.setPeak2 (eqPeak2FreqHz->load (std::memory_order_relaxed), eqPeak2GainDb->load (std::memory_order_relaxed), eqPeak2Q->load (std::memory_order_relaxed));
    eq.setHighShelf (eqHighShelfFreqHz->load (std::memory_order_relaxed), eqHighShelfGainDb->load (std::memory_order_relaxed));

    irLoader.setWetMixProportion (irMixPercent->load (std::memory_order_relaxed) / 100.0f);

    juce::dsp::AudioBlock<float> fullBlock (buffer);

    // Defensive chunking: hosts are expected to never exceed the block size
    // promised to prepareToPlay(), but if one ever did, indexing straight
    // into the band buffers (sized to preparedBlockSize) would be out of
    // bounds. Processing in chunks of at most preparedBlockSize handles that
    // case safely without ever resizing a buffer here.
    const auto chunkLimit = preparedBlockSize > 0
                                 ? static_cast<size_t> (preparedBlockSize)
                                 : juce::jmax (static_cast<size_t> (1), fullBlock.getNumSamples());

    for (size_t offset = 0; offset < fullBlock.getNumSamples(); offset += chunkLimit)
    {
        const auto chunkLength = juce::jmin (chunkLimit, fullBlock.getNumSamples() - offset);
        auto chunk = fullBlock.getSubBlock (offset, chunkLength);
        const auto chunkChannels = chunk.getNumChannels();

        // Issue #87: snapshot the untouched input before processChunk()
        // mutates `chunk` (i.e. this region of the host's own buffer) in
        // place with the wet signal.
        auto dryChunk = juce::dsp::AudioBlock<float> (bypassDryBuffer)
                             .getSubBlock (0, chunkLength)
                             .getSubsetChannelBlock (0, chunkChannels);
        dryChunk.copyFrom (juce::dsp::AudioBlock<const float> (chunk));

        processChunk (chunk);

        // Always run: the dry path must stay time-aligned with the wet
        // chain's latency at every instant, not only while bypass happens to
        // be engaged, or the delay line's history would be stale the moment
        // bypass toggles (see bypassDryDelay's docs in PluginProcessor.h).
        bypassDryDelay.process (juce::dsp::ProcessContextReplacing<float> (dryChunk));

        applyBypassCrossfade (juce::dsp::AudioBlock<const float> (dryChunk), chunk);
    }
}

void CryptaAudioProcessor::processChunk (juce::dsp::AudioBlock<float>& chunk) noexcept
{
    // Input peak, measured before anything touches the signal - including the
    // input trim, so the meter shows what the host is actually sending.
    {
        const auto peakOf = [&chunk] (size_t channel)
        {
            const auto* data = chunk.getChannelPointer (channel);
            float peak = 0.0f;

            for (size_t sample = 0; sample < chunk.getNumSamples(); ++sample)
                peak = juce::jmax (peak, std::abs (data[sample]));

            return peak;
        };

        meterTaps.inputPeakLeft.store (chunk.getNumChannels() > 0 ? peakOf (0) : 0.0f, std::memory_order_relaxed);
        meterTaps.inputPeakRight.store (chunk.getNumChannels() > 1 ? peakOf (1) : 0.0f, std::memory_order_relaxed);
    }

    inputGainProcessor.process (juce::dsp::ProcessContextReplacing<float> (chunk));

    // Full-band noise gate, ahead of the crossover splits.
    gate.process (chunk);

    const auto numChannels = chunk.getNumChannels();
    const auto numSamples = chunk.getNumSamples();

    auto lowBlock = juce::dsp::AudioBlock<float> (lowBandBuffer).getSubBlock (0, numSamples).getSubsetChannelBlock (0, numChannels);
    auto remainderBlock = juce::dsp::AudioBlock<float> (remainderBandBuffer).getSubBlock (0, numSamples).getSubsetChannelBlock (0, numChannels);
    auto midBlock = juce::dsp::AudioBlock<float> (midBandBuffer).getSubBlock (0, numSamples).getSubsetChannelBlock (0, numChannels);
    auto highBlock = juce::dsp::AudioBlock<float> (highBandBuffer).getSubBlock (0, numSamples).getSubsetChannelBlock (0, numChannels);
    auto midHighSumBlock = juce::dsp::AudioBlock<float> (midHighSumBuffer).getSubBlock (0, numSamples).getSubsetChannelBlock (0, numChannels);

    // Split #1: peel off the Low band; the remainder carries the Mid+High
    // content on to the selected drive engine.
    lowSplit.process (chunk, lowBlock, remainderBlock);

    // Low band: parallel compressor, then level trim, then the v0.2.0
    // phase-alignment allpass that makes the cascaded three-way sum flat
    // (see PluginProcessor.h's lowBandPhaseAlign member docs / class-level
    // proof in src/dsp/PhaseAlignFilter.h).
    lowCompressor.process (lowBlock);

    // v0.4.0 Graaawl (issue #36), inserted after the compressor and before the
    // level trim exactly as the issue specifies: the compressor keeps the lows
    // tight, so the growl branch sees a predictable input level and the amount
    // control means the same thing from note to note. Adds nothing to the
    // signal - not even a rounding difference - while it is switched off.
    lowGrowlStage.process (lowBlock);

    lowGainProcessor.process (juce::dsp::ProcessContextReplacing<float> (lowBlock));
    lowBandPhaseAlign.process (lowBlock);

    // Test-only observability seam (see setLowBandIsolationCaptureForTests()'s
    // docs in PluginProcessor.h) - captured here, before the delay
    // compensation below (a pure time shift, not a content change) and
    // before the IR loader (which never touches the Low band at all - this
    // capture is what makes that guarantee directly testable).
    if (lowBandIsolationCaptureForTests != nullptr)
        for (size_t channel = 0; channel < numChannels; ++channel)
            lowBandIsolationCaptureForTests->copyFrom (
                static_cast<int> (channel), 0, lowBlock.getChannelPointer (channel), static_cast<int> (numSamples));

    // Mid+High section, through whichever drive engine is selected.
    //
    // A change of engine arms a short equal-power crossfade, during which
    // BOTH engines run and are faded between - the two produce genuinely
    // different signals, so simply swapping branches would step the output.
    const auto useCircuitEngine = driveEngineChoice->load (std::memory_order_relaxed) >= 0.5f;

    if (useCircuitEngine != lastDriveEngineWasCircuit)
    {
        engineCrossfadeRemaining = engineCrossfadeLengthSamples;
        lastDriveEngineWasCircuit = useCircuitEngine;

        // Flush the engine that is coming back. Only one engine runs at a
        // time, so the other one's oversampling FIR history, crossover state
        // and blend delay lines still hold whatever was passing through it
        // when it was last selected - which, dumped into a live signal, is a
        // loud burst of unrelated audio. Measured at a 1.96 peak against a
        // 1.16 steady-state peak before this reset existed.
        //
        // Real-time safe: every reset() below only clears already-allocated
        // storage. The incoming engine then starts from silence and takes its
        // own latency to fill, which is exactly what the crossfade covers.
        if (useCircuitEngine)
        {
            circuitDrive.reset();
            circuitAlignDelay.reset();
        }
        else
        {
            midHighSplit.reset();
            midBand.reset();
            highVoicing.reset();
        }
    }

    if (engineCrossfadeRemaining > 0)
    {
        auto crossfadeBlock = juce::dsp::AudioBlock<float> (engineCrossfadeBuffer)
                                  .getSubBlock (0, numSamples)
                                  .getSubsetChannelBlock (0, numChannels);

        // Both engines consume the same input, so one of them needs its own
        // copy of the remainder.
        crossfadeBlock.copyFrom (juce::dsp::AudioBlock<const float> (remainderBlock));

        if (useCircuitEngine)
        {
            processMidHighCircuit (remainderBlock);
            processMidHighClassic (juce::dsp::AudioBlock<const float> (crossfadeBlock), midHighSumBlock);
            applyEngineCrossfade (juce::dsp::AudioBlock<const float> (midHighSumBlock),
                                   juce::dsp::AudioBlock<const float> (remainderBlock),
                                   midHighSumBlock);
        }
        else
        {
            processMidHighClassic (juce::dsp::AudioBlock<const float> (remainderBlock), midHighSumBlock);
            processMidHighCircuit (crossfadeBlock);
            applyEngineCrossfade (juce::dsp::AudioBlock<const float> (crossfadeBlock),
                                   juce::dsp::AudioBlock<const float> (midHighSumBlock),
                                   midHighSumBlock);
        }
    }
    else if (useCircuitEngine)
    {
        processMidHighCircuit (remainderBlock);
        midHighSumBlock.copyFrom (juce::dsp::AudioBlock<const float> (remainderBlock));
    }
    else
    {
        processMidHighClassic (juce::dsp::AudioBlock<const float> (remainderBlock), midHighSumBlock);
    }

    // Per-band meter levels. Low is measured here directly; Mid and High come
    // from whichever engine just ran, because the Circuit engine's two bands
    // only exist inside its own oversampled region and are already summed by
    // the time control returns. Both engines report post-drive, post-level
    // RMS, so the two read the same scale.
    {
        double sumOfSquares = 0.0;
        const auto* lowData = lowBlock.getChannelPointer (0);

        for (size_t sample = 0; sample < numSamples; ++sample)
            sumOfSquares += static_cast<double> (lowData[sample]) * static_cast<double> (lowData[sample]);

        const auto lowRms = numSamples > 0
                                ? static_cast<float> (std::sqrt (sumOfSquares / static_cast<double> (numSamples)))
                                : 0.0f;

        // ~300 ms one-pole at block rate, so the display settles instead of
        // flickering. Derived from the actual block length, so the time
        // constant does not change with the host's buffer size.
        const auto blockSeconds = static_cast<float> (numSamples)
                                   / static_cast<float> (juce::jmax (1.0, getSampleRate()));
        const auto smoothing = juce::jlimit (0.0f, 1.0f, blockSeconds / 0.3f);

        lowBandMeterLevel += smoothing * (lowRms - lowBandMeterLevel);
        midBandMeterLevel += smoothing * (pendingMidBandLevel - midBandMeterLevel);
        highBandMeterLevel += smoothing * (pendingHighBandLevel - highBandMeterLevel);
    }

    // Issue #9: time-align the low band with the latency the Mid+High
    // branch's oversampling stages introduce.
    lowBandLatencyDelay.process (juce::dsp::ProcessContextReplacing<float> (lowBlock));

    // Cab-sim IR loader (v0.2.0: relocated here, between the Mid+High sum
    // and the final three-way sum) - the Low band structurally never passes
    // through this call, matching the reference class's "low band bypasses
    // the cabsim" architecture (docs/design-brief.md). Skipped entirely when
    // disabled for a guaranteed bit-exact bypass rather than relying on
    // mix==0.
    if (irEnabled->load (std::memory_order_relaxed) >= 0.5f)
        irLoader.process (midHighSumBlock);

    // Final sum: Low (delay-compensated) + [Mid+High, post-IR].
    chunk.replaceWithSumOf (lowBlock, midHighSumBlock);

    // Post-sum 4-band EQ. Skipped entirely when disabled for a guaranteed
    // bit-exact bypass rather than relying on all-zero band gains.
    if (eqEnabled->load (std::memory_order_relaxed) >= 0.5f)
        eq.process (chunk);

    // Optional safety clip. v0.3.0 replaces v0.2.0's raw base-rate std::tanh
    // with an ADAA ceiling clip in delta form: far less aliasing, a settable
    // ceiling, and - unlike the naive antialiased form - genuinely transparent
    // below that ceiling rather than lowpassing the whole mix whenever it is
    // armed. See src/dsp/OutputClipper.h. Skipped entirely when disabled, so
    // the off state stays a bit-exact bypass.
    if (outputClipEnabled->load (std::memory_order_relaxed) >= 0.5f)
    {
        outputClipper.setCeilingDb (clipCeilingDb->load (std::memory_order_relaxed));
        outputClipper.process (chunk);
    }

    outputGainProcessor.process (juce::dsp::ProcessContextReplacing<float> (chunk));

    publishMeterTaps (chunk, numChannels, numSamples);
}

void CryptaAudioProcessor::publishMeterTaps (const juce::dsp::AudioBlock<float>& output,
                                               size_t numChannels,
                                               size_t numSamples) noexcept
{
    // Block-rate decimation is all a 30 Hz UI can use, and it keeps this off
    // the per-sample path entirely. Stores are relaxed: each slot is
    // independent and a reader that sees one update a block late is showing a
    // meter 20 ms stale, which no one can perceive.
    const auto blockPeak = [&output, numSamples] (size_t channel)
    {
        const auto* data = output.getChannelPointer (channel);
        float peak = 0.0f;

        for (size_t sample = 0; sample < numSamples; ++sample)
            peak = juce::jmax (peak, std::abs (data[sample]));

        return peak;
    };

    meterTaps.outputPeakLeft.store (numChannels > 0 ? blockPeak (0) : 0.0f, std::memory_order_relaxed);
    meterTaps.outputPeakRight.store (numChannels > 1 ? blockPeak (1) : 0.0f, std::memory_order_relaxed);

    meterTaps.lowBandLevel.store (lowBandMeterLevel, std::memory_order_relaxed);
    meterTaps.midBandLevel.store (midBandMeterLevel, std::memory_order_relaxed);
    meterTaps.highBandLevel.store (highBandMeterLevel, std::memory_order_relaxed);

    meterTaps.lowCompGainReductionDb.store (lowCompressor.getGainReductionDb(), std::memory_order_relaxed);
    meterTaps.gateGainReductionDb.store (gate.getGainReductionDb(), std::memory_order_relaxed);
}

void CryptaAudioProcessor::processMidHighClassic (const juce::dsp::AudioBlock<const float>& input,
                                                    juce::dsp::AudioBlock<float>& output) noexcept
{
    const auto numChannels = output.getNumChannels();
    const auto numSamples = output.getNumSamples();

    auto midBlock = juce::dsp::AudioBlock<float> (midBandBuffer).getSubBlock (0, numSamples).getSubsetChannelBlock (0, numChannels);
    auto highBlock = juce::dsp::AudioBlock<float> (highBandBuffer).getSubBlock (0, numSamples).getSubsetChannelBlock (0, numChannels);

    // Split #2: remainder -> Mid / High, at base rate.
    midHighSplit.process (input, midBlock, highBlock);

    // Mid band: staged drive, then level trim.
    midBand.process (midBlock);
    midGainProcessor.process (juce::dsp::ProcessContextReplacing<float> (midBlock));

    // High band: Tight pre-drive HPF (inside Voicing) -> oversampled
    // distortion voicing (Gnaw/Wool/Razor) -> drive -> tone -> blend, then
    // level trim.
    highVoicing.process (highBlock);
    highGainProcessor.process (juce::dsp::ProcessContextReplacing<float> (highBlock));

    // Band levels for the meter taps, to match what the Circuit engine
    // reports from inside its own oversampled region.
    {
        const auto rmsOf = [numSamples] (const juce::dsp::AudioBlock<float>& band)
        {
            const auto* data = band.getChannelPointer (0);
            double sumOfSquares = 0.0;

            for (size_t sample = 0; sample < numSamples; ++sample)
                sumOfSquares += static_cast<double> (data[sample]) * static_cast<double> (data[sample]);

            return numSamples > 0
                       ? static_cast<float> (std::sqrt (sumOfSquares / static_cast<double> (numSamples)))
                       : 0.0f;
        };

        pendingMidBandLevel = rmsOf (midBlock);
        pendingHighBandLevel = rmsOf (highBlock);
    }

    // Sum Mid + High into a dedicated buffer (never aliasing either addend)
    // ahead of the relocated IR loader.
    output.replaceWithSumOf (juce::dsp::AudioBlock<const float> (midBlock),
                              juce::dsp::AudioBlock<const float> (highBlock));
}

void CryptaAudioProcessor::processMidHighCircuit (juce::dsp::AudioBlock<float>& block) noexcept
{
    // In place: the Circuit engine splits, drives, level-trims and re-sums
    // Mid and High entirely inside its own oversampled region.
    circuitDrive.process (block);

    pendingMidBandLevel = circuitDrive.getMidBandLevel();
    pendingHighBandLevel = circuitDrive.getHighBandLevel();

    // Pad up to the Classic engine's (possibly larger) latency so the plugin
    // reports one sample-rate-dependent figure for both engines - see
    // circuitAlignDelay's docs in PluginProcessor.h.
    circuitAlignDelay.process (juce::dsp::ProcessContextReplacing<float> (block));
}

void CryptaAudioProcessor::applyEngineCrossfade (const juce::dsp::AudioBlock<const float>& outgoing,
                                                   const juce::dsp::AudioBlock<const float>& incoming,
                                                   juce::dsp::AudioBlock<float>& destination) noexcept
{
    const auto numChannels = destination.getNumChannels();
    const auto numSamples = destination.getNumSamples();
    constexpr auto length = static_cast<double> (engineCrossfadeLengthSamples);

    for (size_t sample = 0; sample < numSamples; ++sample)
    {
        const auto remaining = juce::jmax (0, engineCrossfadeRemaining - static_cast<int> (sample));
        const auto position = juce::jlimit (0.0, 1.0, (length - static_cast<double> (remaining)) / length);

        // Constant-gain (linear) rather than equal-power. The brief specifies
        // equal power, but that law is for UNCORRELATED sources: the two drive
        // engines are two renderings of the same programme and are strongly
        // correlated, so cos/sin sums to up to +3 dB mid-fade - measured at a
        // 1.96 peak on a 0.7 sine, which would break the |1.5| bound the same
        // brief sets for this test. Linear is the correct law here and holds
        // the sum bounded by the larger of the two inputs.
        const auto outgoingGain = static_cast<float> (1.0 - position);
        const auto incomingGain = static_cast<float> (position);

        for (size_t channel = 0; channel < numChannels; ++channel)
            destination.getChannelPointer (channel)[sample] =
                outgoingGain * outgoing.getChannelPointer (channel)[sample]
                + incomingGain * incoming.getChannelPointer (channel)[sample];
    }

    engineCrossfadeRemaining = juce::jmax (0, engineCrossfadeRemaining - static_cast<int> (numSamples));
}

void CryptaAudioProcessor::applyBypassCrossfade (const juce::dsp::AudioBlock<const float>& dry,
                                                   juce::dsp::AudioBlock<float>& wet) noexcept
{
    // Linear, not equal-power, for the same reason applyEngineCrossfade()
    // above is linear: the wet and dry signals are two renderings of the
    // same programme material and are strongly correlated (once dry is
    // delay-compensated, they are close to identical at bypassCrossfade's
    // 20ms endpoints), so an equal-power law would produce a measurable
    // mid-fade level bump rather than a transparent blend.
    const auto numChannels = wet.getNumChannels();
    const auto numSamples = wet.getNumSamples();

    for (size_t sample = 0; sample < numSamples; ++sample)
    {
        const auto wetGain = bypassWetMix.getNextValue();
        const auto dryGain = 1.0f - wetGain;

        for (size_t channel = 0; channel < numChannels; ++channel)
        {
            auto* wetData = wet.getChannelPointer (channel);
            wetData[sample] = wetGain * wetData[sample] + dryGain * dry.getChannelPointer (channel)[sample];
        }
    }
}

//==============================================================================
bool CryptaAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* CryptaAudioProcessor::createEditor()
{
    return new CryptaAudioProcessorEditor (*this);
}

//==============================================================================
juce::AudioProcessorParameter* CryptaAudioProcessor::getBypassParameter() const
{
    return bypassParameter;
}

//==============================================================================
void CryptaAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState();
    const std::unique_ptr<juce::XmlElement> xml (state.createXml());

    // Stamp the schema version so setStateInformation() can tell state that
    // predates the v0.3.0 engine selectors (and therefore needs the legacy
    // engines injected) from state that simply chose them - see
    // migrateToStateV2() above.
    xml->setAttribute (stateVersionAttribute, currentStateVersion);

    copyXmlToBinary (*xml, destData);
}

void CryptaAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState == nullptr || ! xmlState->hasTagName (apvts.state.getType()))
        return;

    // v0.1.x -> v0.2.0 structural migration (docs/design-brief.md guarantee
    // #7) - see migrateLegacySingleCrossover()'s docs above. No-op for a
    // v0.2.0+ saved state (it never contains a "crossoverFreq" element).
    migrateLegacySingleCrossover (*xmlState);

    // v0.2.0 -> v0.3.0 engine migration. Runs after the crossover migration
    // above so a v0.1 session gets both, in schema order.
    migrateToStateV2 (*xmlState);

    // The stateVersion attribute lives on the XML element, not in the
    // ValueTree's parameter children, so it is not carried into the APVTS
    // state - it is purely a serialisation-format marker, re-stamped on every
    // getStateInformation().
    apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
}

//==============================================================================
void CryptaAudioProcessor::loadImpulseResponse (juce::AudioBuffer<float> irBuffer, double irSampleRate)
{
    irLoader.loadImpulseResponse (std::move (irBuffer), irSampleRate);
}

std::vector<cryp::FactoryIRAsset> CryptaAudioProcessor::getFactoryIRAssetTable()
{
    // Four bass cabinet impulse responses (issues #21/#81), and the thing worth
    // knowing about all four is that they are GENERATED, not recorded.
    //
    // The content half of #21 stalled for one reason: bundling a cabinet IR
    // means redistributing someone else's recording inside every copy of an
    // AGPLv3 binary, a capture carries rights from the cabinet, the microphone
    // and whoever pressed record, and "free download" is not a licence. That
    // question has no cheap answer - so it is not answered here, it is
    // avoided. tools/ir-synth/cabsynth.py computes each of these from a
    // documented analytical cabinet model (analog-prototype filter sections
    // plus reflection taps derived from physical path lengths), which means
    // there is no third-party recording in the binary at all and nothing for
    // anyone to dispute. Re-run the generator, get byte-identical output,
    // compare the SHA-256 in resources/irs/manifest.json: reproducibility is
    // the provenance.
    //
    // They are MODELS, and the display names say so. Nothing below is a
    // capture of any real cabinet, speaker or microphone, and none of it may
    // ever be presented as one - hence the "Modelled" prefix on every name the
    // GUI will show, and hence the licence string being CC0-1.0 rather than
    // "Self-recorded", which would mean a capture we made.
    //
    // The per-model voicing, the full parameter list, the SHA-256 of each file
    // and the measured response of each are in resources/irs/LICENSES.md. The
    // CC0 1.0 legal code these are dedicated under is committed alongside them
    // at resources/irs/CC0-1.0.txt.
    return {
        { "Modelled 8x10 Cone", "CC0-1.0",
          "Generated by tools/ir-synth/cabsynth.py, model id 'bass-810-cone'; "
          "CC0-1.0 dedication by Yves Vogl, Basilica Audio, 2026-08-22; "
          "provenance and SHA-256 in resources/irs/LICENSES.md",
          BinaryData::modelled_8x10_cone_wav, BinaryData::modelled_8x10_cone_wavSize },

        { "Modelled 8x10 Edge", "CC0-1.0",
          "Generated by tools/ir-synth/cabsynth.py, model id 'bass-810-edge'; "
          "CC0-1.0 dedication by Yves Vogl, Basilica Audio, 2026-08-22; "
          "provenance and SHA-256 in resources/irs/LICENSES.md",
          BinaryData::modelled_8x10_edge_wav, BinaryData::modelled_8x10_edge_wavSize },

        { "Modelled 1x15 Vintage", "CC0-1.0",
          "Generated by tools/ir-synth/cabsynth.py, model id 'bass-115-vintage'; "
          "CC0-1.0 dedication by Yves Vogl, Basilica Audio, 2026-08-22; "
          "provenance and SHA-256 in resources/irs/LICENSES.md",
          BinaryData::modelled_1x15_vintage_wav, BinaryData::modelled_1x15_vintage_wavSize },

        { "Modelled 4x10 Horn", "CC0-1.0",
          "Generated by tools/ir-synth/cabsynth.py, model id 'bass-410-horn'; "
          "CC0-1.0 dedication by Yves Vogl, Basilica Audio, 2026-08-22; "
          "provenance and SHA-256 in resources/irs/LICENSES.md",
          BinaryData::modelled_4x10_horn_wav, BinaryData::modelled_4x10_horn_wavSize },
    };
}

bool CryptaAudioProcessor::loadFactoryImpulseResponse (int index)
{
    juce::AudioBuffer<float> irBuffer;
    double irSampleRate = 0.0;

    // Decode first, install second: a failed decode must leave whatever is
    // currently loaded playing, not drop the convolution into silence.
    if (! factoryIRs.decode (index, irBuffer, irSampleRate))
        return false;

    irLoader.loadImpulseResponse (std::move (irBuffer), irSampleRate);
    return true;
}

void CryptaAudioProcessor::clearImpulseResponse()
{
    irLoader.clearImpulseResponse();
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CryptaAudioProcessor();
}
