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

    // Mid band: staged drive, no DryWetMixer (no blend control - see
    // cryp::MidBand's class docs), so no priming gotcha here.
    midBand.prepare (spec);
    midBand.setDrive (midDrivePercent->load (std::memory_order_relaxed) / 100.0f);

    // High band: Tight pre-drive HPF + oversampled distortion voicing. Same
    // DryWetMixer-priming requirement for highBlend as the low-band
    // compressor above.
    highVoicing.prepare (spec, highBlendPercent->load (std::memory_order_relaxed) / 100.0f);
    highVoicing.setTightHz (highTightHzParam->load (std::memory_order_relaxed));

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

    // Issue #9: (re)allocate the low-band compensation delay line for the
    // new spec/max-delay bound. setMaximumDelayInSamples() may allocate, so
    // it must only ever be called here, never from processBlock().
    lowBandLatencyDelay.setMaximumDelayInSamples (maxLatencyCompensationSamples);
    lowBandLatencyDelay.prepare (spec);

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
    return juce::jmax (midBand.getLatencySamples(), highVoicing.getLatencySamples());
}

void CryptaAudioProcessor::updateLatencyCompensation()
{
    const auto totalLatencySamples = juce::jlimit (0, maxLatencyCompensationSamples, computeTotalLatencySamples());

    setLatencySamples (totalLatencySamples);

    // The low band bypasses oversampling entirely, so it must be delayed by
    // the same amount the Mid+High branch's oversampling stages delay it,
    // keeping all bands time-aligned when they are summed back together in
    // processChunk().
    lowBandLatencyDelay.setDelay (static_cast<float> (totalLatencySamples));
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
    midBand.reset();
    highVoicing.reset();

    lowGainProcessor.reset();
    midGainProcessor.reset();
    highGainProcessor.reset();

    eq.reset();
    irLoader.reset();

    lowBandLatencyDelay.reset();
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

    if (bypassFlag->load (std::memory_order_relaxed) >= 0.5f)
        return;

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

    lowCompressor.setThresholdDb (lowCompThresholdDb->load (std::memory_order_relaxed));
    lowCompressor.setRatio (lowCompRatio->load (std::memory_order_relaxed));
    lowCompressor.setAttackMs (lowCompAttackMs->load (std::memory_order_relaxed));
    lowCompressor.setReleaseMs (lowCompReleaseMs->load (std::memory_order_relaxed));
    lowCompressor.setMakeupGainDb (lowCompMakeupDb->load (std::memory_order_relaxed));
    lowCompressor.setWetMixProportion (lowCompMixPercent->load (std::memory_order_relaxed) / 100.0f);

    midBand.setDrive (midDrivePercent->load (std::memory_order_relaxed) / 100.0f);

    highVoicing.setTightHz (highTightHzParam->load (std::memory_order_relaxed));
    const auto voicingIndex = static_cast<int> (highVoicingChoice->load (std::memory_order_relaxed));
    highVoicing.setVoicing (static_cast<cryp::VoicingType> (juce::jlimit (0, 2, voicingIndex)));
    highVoicing.setDrive (highDrivePercent->load (std::memory_order_relaxed) / 100.0f);
    highVoicing.setTone (highTonePercent->load (std::memory_order_relaxed) / 100.0f);
    highVoicing.setWetMixProportion (highBlendPercent->load (std::memory_order_relaxed) / 100.0f);

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
        processChunk (chunk);
    }
}

void CryptaAudioProcessor::processChunk (juce::dsp::AudioBlock<float>& chunk) noexcept
{
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
    // content on to split #2.
    lowSplit.process (chunk, lowBlock, remainderBlock);

    // Split #2: remainder -> Mid / High.
    midHighSplit.process (remainderBlock, midBlock, highBlock);

    // Low band: parallel compressor, then level trim, then the v0.2.0
    // phase-alignment allpass that makes the cascaded three-way sum flat
    // (see PluginProcessor.h's lowBandPhaseAlign member docs / class-level
    // proof in src/dsp/PhaseAlignFilter.h).
    lowCompressor.process (lowBlock);
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

    // Mid band: staged drive, then level trim.
    midBand.process (midBlock);
    midGainProcessor.process (juce::dsp::ProcessContextReplacing<float> (midBlock));

    // High band: Tight pre-drive HPF (inside Voicing) -> oversampled
    // distortion voicing (Gnaw/Wool/Razor) -> drive -> tone -> blend, then
    // level trim. This (together with Mid band's own oversampling) is the
    // only source of latency in the chain.
    highVoicing.process (highBlock);
    highGainProcessor.process (juce::dsp::ProcessContextReplacing<float> (highBlock));

    // Issue #9: time-align the low band with the latency the Mid+High
    // branch's oversampling stages introduce.
    lowBandLatencyDelay.process (juce::dsp::ProcessContextReplacing<float> (lowBlock));

    // Sum Mid + High into a dedicated buffer (never aliasing either addend)
    // ahead of the relocated IR loader.
    midHighSumBlock.replaceWithSumOf (midBlock, highBlock);

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

    // Optional safety clip: a soft (tanh) limiter that only engages when the
    // user explicitly enables it, protecting against accidental hard-clipped
    // overs without colouring the signal at typical playing levels
    // (tanh(x) ~= x for |x| well below 1.0).
    if (outputClipEnabled->load (std::memory_order_relaxed) >= 0.5f)
    {
        for (size_t channel = 0; channel < numChannels; ++channel)
        {
            auto* data = chunk.getChannelPointer (channel);

            for (size_t sample = 0; sample < numSamples; ++sample)
                data[sample] = std::tanh (data[sample]);
        }
    }

    outputGainProcessor.process (juce::dsp::ProcessContextReplacing<float> (chunk));
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

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CryptaAudioProcessor();
}
