#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/BandEQ.h"
#include "dsp/CircuitDrive.h"
#include "dsp/Crossover.h"
#include "dsp/IRLoader.h"
#include "dsp/MidBand.h"
#include "dsp/NoiseGateStage.h"
#include "dsp/ParallelCompressor.h"
#include "dsp/PhaseAlignFilter.h"
#include "dsp/Voicing.h"
#include "presets/PresetManager.h"

// v0.2.0 "deep-dive" rebuild (docs/design-brief.md): the v1.0 2-band
// (low/high) topology is now a cascaded 3-band split (low/mid/high). Full
// signal path: input trim -> noise gate -> LR4 split #1 (Split Low) ->
//   [low: parallel compressor -> level] +
//   [remainder -> LR4 split #2 (Split High) ->
//     [mid: drive -> level] + [high: Tight HPF -> voicing -> drive -> tone
//     -> blend -> level] -> sum -> IR loader (cab sim, Mid+High only)] ->
// delay-compensated sum (low + [mid+high post-IR]) -> 4-band EQ -> optional
// safety clip -> output trim. See docs/architecture.md for the full
// breakdown and docs/manual.md for the user-facing parameter reference.
class CryptaAudioProcessor final : public juce::AudioProcessor
{
public:
    CryptaAudioProcessor();
    ~CryptaAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    // Issue #56: flushes every per-stage DSP class's own state (filter
    // memory, envelope followers, oversampling FIR history, the low-band
    // latency-compensation delay line, ...) without deallocating anything.
    // Hosts call this on transport stop/loop/rewind - unlike
    // prepareToPlay(), which only fires on sample-rate/block-size changes -
    // so relying on prepareToPlay() alone leaves stale state ringing into
    // whatever plays next.
    void reset() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorParameter* getBypassParameter() const override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;

    // M2 preset system (.scaffold/specs/preset-system-m2.md,
    // docs/preset-system-notes.md replication recipe from basilica-audio/
    // nave's pilot implementation). Declared after apvts - construction
    // order follows declaration order, and PresetManager's constructor reads
    // apvts.processor.getParameters(), so apvts must already be fully built.
    basilica::presets::PresetManager presetManager;

    // Loads a new cab-sim impulse response into the IR loader stage. Not
    // real-time safe by contract (see cryp::IRLoader) - call from the message
    // thread only (e.g. in response to a future GUI file picker or preset
    // load), never from processBlock() or any audio-thread callback.
    void loadImpulseResponse (juce::AudioBuffer<float> irBuffer, double irSampleRate);

    // Test-only observability seam (docs/design-brief.md guarantee #3: "Low
    // band never reaches the IR loader"). When non-null, processChunk()
    // copies the Low band's own fully-processed (post-compressor, post-
    // lowLevel, post-phase-align, pre-final-sum, pre-delay-compensation)
    // output into this buffer every chunk - letting a test assert that content is bit-exact
    // identical whether the (relocated, Mid+High-only) IR loader is enabled
    // or not, which the full processBlock() output alone can't isolate
    // cleanly (finite LR4 stopband leakage of low-frequency content into the
    // Mid/High branch means even an unrelated IR-loader change nudges the
    // *combined* output by a measurable, if tiny, amount). Intended for
    // single-chunk test scenarios only (buffer size <= the size promised to
    // prepareToPlay()); no production code path ever sets this. Not real-time
    // safe to *set* (call only from the message thread, before processBlock()
    // runs) - reading/writing the pointed-to buffer from inside processChunk()
    // itself performs no allocation, so it is safe to leave set while
    // processBlock() runs in a test.
    void setLowBandIsolationCaptureForTests (juce::AudioBuffer<float>* captureBuffer) noexcept
    {
        lowBandIsolationCaptureForTests = captureBuffer;
    }

private:
    //==============================================================================
    // Latency-compensation seam (issue #9): computes the plugin's total
    // reported latency (the Mid+High branch's shared oversampling latency -
    // see computeTotalLatencySamples()) and re-arms the low-band
    // compensation delay to match. Called once from prepareToPlay().
    int computeTotalLatencySamples() const noexcept;
    void updateLatencyCompensation();

    // Processes at most `preparedBlockSize` samples: gate, two cascaded LR4
    // crossover splits, per-band dynamics/drive/voicing, per-band level,
    // Mid+High sum, IR loader, final sum, EQ, optional safety clip. Called
    // once per chunk from processBlock() so oversized host blocks (larger
    // than prepareToPlay promised) are handled defensively without ever
    // resizing a buffer on the audio thread.
    void processChunk (juce::dsp::AudioBlock<float>& chunk) noexcept;

    // The Mid+High section, once per engine. Classic reads `input` and writes
    // the summed, post-IR-ready result to `output` (using the mid/high band
    // buffers as scratch); Circuit works in place, because its two bands only
    // exist inside its own oversampled region.
    void processMidHighClassic (const juce::dsp::AudioBlock<const float>& input,
                                 juce::dsp::AudioBlock<float>& output) noexcept;
    void processMidHighCircuit (juce::dsp::AudioBlock<float>& block) noexcept;

    // Equal-power fade from `outgoing` to `incoming`, advancing (and
    // consuming) engineCrossfadeRemaining. `destination` may alias either
    // source - each sample is written only after both inputs at that index
    // have been read.
    void applyEngineCrossfade (const juce::dsp::AudioBlock<const float>& outgoing,
                                const juce::dsp::AudioBlock<const float>& incoming,
                                juce::dsp::AudioBlock<float>& destination) noexcept;

    //==============================================================================
    juce::dsp::Gain<float> inputGainProcessor;
    juce::dsp::Gain<float> outputGainProcessor;

    // Full-band input noise gate, ahead of the crossover splits.
    cryp::NoiseGateStage gate;

    // v0.2.0: two cascaded LR4 crossovers replace v1's single split.
    // lowSplit peels off the Low band; midHighSplit further splits the
    // remainder into Mid and High - the exact "cascaded (not parallel) LR4"
    // pattern docs/design-brief.md's Topology section specifies.
    cryp::Crossover lowSplit;
    cryp::Crossover midHighSplit;

    // v0.2.0: compensates the Low band for midHighSplit's own phase shift so
    // the three-way sum is flat-magnitude, not just each individual
    // crossover stage - see src/dsp/PhaseAlignFilter.h's class docs for the
    // exact algebraic proof of why this is required for a *cascaded* N-way
    // LR4 crossover (unlike a single 2-way split, which is flat by
    // construction with no compensation needed).
    cryp::PhaseAlignFilter lowBandPhaseAlign;

    // Low band: parallel compressor, then level trim.
    cryp::ParallelCompressor lowCompressor;

    // Mid band (NEW in v0.2.0): staged/cascaded drive only, then level trim.
    cryp::MidBand midBand;

    // High band: Tight pre-drive HPF (now voicing-independent) + selectable
    // oversampled distortion voicing (Gnaw/Wool/Razor), then level trim.
    cryp::Voicing highVoicing;

    // v0.3.0 Circuit engine: replaces the midBand + highVoicing pair above
    // with one shared oversampling region when driveEngine == Circuit. Both
    // engines stay prepared at all times so switching between them is a
    // branch, not a reallocation - and so the 64-sample crossfade below can
    // run BOTH for the duration of a switch.
    cryp::CircuitDrive circuitDrive;

    // Constant-gain crossfade between the two drive engines, in samples
    // remaining. Non-zero only immediately after a driveEngine change; while
    // it runs, processChunk() renders the Mid+High section through both
    // engines and fades between them, which is what keeps an automated or
    // preset-driven engine switch from producing a step discontinuity.
    //
    // 256 samples rather than the brief's 64: the incoming engine is reset at
    // the switch (see processChunk()) and therefore needs its own oversampling
    // latency - around 30-40 samples at base rate - before it is producing
    // full-level output at all. A 64-sample fade would still be handing it
    // significant gain while it was ramping up from silence. 256 samples
    // (5.3 ms at 48 kHz) keeps the incoming gain below 15 % until the engine
    // has filled, and is still short enough to read as an instant switch.
    static constexpr int engineCrossfadeLengthSamples = 256;
    int engineCrossfadeRemaining = 0;
    bool lastDriveEngineWasCircuit = true;

    // Scratch for the crossfade's second engine render. Sized with the other
    // band buffers in prepareToPlay().
    juce::AudioBuffer<float> engineCrossfadeBuffer;

    // Per-band level trims applied after each band's own dynamics/drive/
    // voicing processing and before the bands are summed back together.
    juce::dsp::Gain<float> lowGainProcessor;
    juce::dsp::Gain<float> midGainProcessor;
    juce::dsp::Gain<float> highGainProcessor;

    // Post-sum 4-band EQ and cab-sim IR loader. v0.2.0 relocates the IR
    // loader: it now processes the Mid+High post-sum signal only, never the
    // Low band (docs/design-brief.md's "IR loader" section) - structurally
    // enforced in processChunk() below, not just by convention.
    cryp::BandEQ eq;
    cryp::IRLoader irLoader;

    // Issue #9: upper bound on the latency this plugin will ever need to
    // compensate for, i.e. the largest oversampling latency the Mid+High
    // branch is expected to introduce. Generous headroom well above the
    // actual 4x-oversampling FIR latency at any supported sample rate, at
    // negligible memory cost.
    static constexpr int maxLatencyCompensationSamples = 4096;

    // Delays the low band so it stays time-aligned with the oversampled
    // Mid+High branch. None-interpolation is correct here: latency
    // compensation is always an integer number of samples, never a
    // fractionally modulated delay.
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> lowBandLatencyDelay { maxLatencyCompensationSamples };

    // Pads the Circuit engine's output up to the Classic engine's latency.
    //
    // The two engines can report different latencies: Classic is always 4x
    // oversampled, while Circuit drops to 2x above 50 kHz and to 1x above
    // 100 kHz. Rather than re-reporting latency to the host whenever
    // driveEngine changes - which hosts handle poorly mid-transport, and
    // which would make an automated engine switch shift the plugin's timing -
    // the plugin always reports the larger of the two and delays the Circuit
    // path by the difference. Reported latency is then a function of sample
    // rate alone, constant across engine switches, and the crossfade above
    // fades between two paths that are already time-aligned.
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> circuitAlignDelay { maxLatencyCompensationSamples };

    // Pre-allocated band buffers, sized to `preparedBlockSize` in
    // prepareToPlay(). Never resized in processBlock(). remainderBandBuffer
    // holds lowSplit's high output (Mid+High content) ahead of midHighSplit;
    // midHighSumBuffer holds the Mid+High sum ahead of the IR loader and the
    // final low+midHigh sum.
    juce::AudioBuffer<float> lowBandBuffer;
    juce::AudioBuffer<float> remainderBandBuffer;
    juce::AudioBuffer<float> midBandBuffer;
    juce::AudioBuffer<float> highBandBuffer;
    juce::AudioBuffer<float> midHighSumBuffer;
    int preparedBlockSize = 0;

    // See setLowBandIsolationCaptureForTests() above. Null in production.
    juce::AudioBuffer<float>* lowBandIsolationCaptureForTests = nullptr;

    // Raw atomic pointers into the APVTS-managed parameter values, resolved
    // once at construction time so processBlock() never has to search for
    // them (no allocation/locks on the audio thread).
    std::atomic<float>* inputGainDb = nullptr;
    std::atomic<float>* outputGainDb = nullptr;
    std::atomic<float>* bypassFlag = nullptr;
    std::atomic<float>* outputClipEnabled = nullptr;
    std::atomic<float>* splitLowHzParam = nullptr;
    std::atomic<float>* splitHighHzParam = nullptr;
    std::atomic<float>* lowLevelDb = nullptr;
    std::atomic<float>* midLevelDb = nullptr;
    std::atomic<float>* highLevelDb = nullptr;

    std::atomic<float>* gateEnabled = nullptr;
    std::atomic<float>* gateThresholdDb = nullptr;
    std::atomic<float>* gateRatio = nullptr;
    std::atomic<float>* gateAttackMs = nullptr;
    std::atomic<float>* gateReleaseMs = nullptr;

    std::atomic<float>* lowCompThresholdDb = nullptr;
    std::atomic<float>* lowCompRatio = nullptr;
    std::atomic<float>* lowCompAttackMs = nullptr;
    std::atomic<float>* lowCompReleaseMs = nullptr;
    std::atomic<float>* lowCompMakeupDb = nullptr;
    std::atomic<float>* lowCompMixPercent = nullptr;

    std::atomic<float>* midDrivePercent = nullptr;

    std::atomic<float>* highTightHzParam = nullptr;
    std::atomic<float>* highVoicingChoice = nullptr;
    std::atomic<float>* highDrivePercent = nullptr;
    std::atomic<float>* highTonePercent = nullptr;
    std::atomic<float>* highBlendPercent = nullptr;

    std::atomic<float>* eqEnabled = nullptr;
    std::atomic<float>* eqLowShelfFreqHz = nullptr;
    std::atomic<float>* eqLowShelfGainDb = nullptr;
    std::atomic<float>* eqPeak1FreqHz = nullptr;
    std::atomic<float>* eqPeak1GainDb = nullptr;
    std::atomic<float>* eqPeak1Q = nullptr;
    std::atomic<float>* eqPeak2FreqHz = nullptr;
    std::atomic<float>* eqPeak2GainDb = nullptr;
    std::atomic<float>* eqPeak2Q = nullptr;
    std::atomic<float>* eqHighShelfFreqHz = nullptr;
    std::atomic<float>* eqHighShelfGainDb = nullptr;

    std::atomic<float>* irEnabled = nullptr;
    std::atomic<float>* irMixPercent = nullptr;

    // v0.3.0 "circuit-grade bass engine" parameters (see brief §4 /
    // src/params/ParameterIds.h).
    std::atomic<float>* driveEngineChoice = nullptr;
    std::atomic<float>* highBiasPercent = nullptr;

    std::atomic<float>* lowCompDetectorChoice = nullptr;
    std::atomic<float>* lowCompKneeDb = nullptr;
    std::atomic<float>* lowCompAutoReleaseFlag = nullptr;
    std::atomic<float>* lowCompAutoMakeupFlag = nullptr;

    std::atomic<float>* gateModeChoice = nullptr;
    std::atomic<float>* gateHysteresisDb = nullptr;
    std::atomic<float>* gateHoldMs = nullptr;
    std::atomic<float>* gateScHpfHz = nullptr;
    std::atomic<float>* gateRangeDb = nullptr;

    std::atomic<float>* clipCeilingDb = nullptr;

    // The actual parameter object handed back from getBypassParameter() so
    // hosts can offer their own bypass UI/automation for this parameter.
    juce::RangedAudioParameter* bypassParameter = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CryptaAudioProcessor)
};
