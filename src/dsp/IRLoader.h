#pragma once

#include <juce_dsp/juce_dsp.h>

#include <mutex>

// Cab-sim IR loader (issue #42), the final stage before output. Wraps
// juce::dsp::Convolution (JUCE 8.0.14, juce_dsp/frequency/juce_Convolution.h)
// configured for zero added latency (the default `Convolution()` /
// `Convolution(Latency{0})` constructor), with an irMix DryWetMixer wrapped
// around it so the loaded IR can be blended rather than fully replacing the
// signal.
//
// Safe-by-default without any bundled factory IR: juce::dsp::Convolution
// with no loadImpulseResponse() call yet made falls back to an internal
// single-sample identity impulse response (JUCE 8.0.14
// ConvolutionEngineFactory::makeImpulseBuffer(), juce_Convolution.cpp).
// *However*, that internal fallback's assumed source sample rate is
// hardcoded to the ProcessSpec default (44100 Hz) and is never updated by
// prepare() - only by an explicit loadImpulseResponse() call - so at any
// *other* session sample rate, Convolution would silently resample that
// single-sample impulse against a mismatched rate, smearing/attenuating it
// (juce_Convolution.cpp's resampleImpulseResponse() only skips resampling
// when the rates match exactly) and quietly colouring the signal even with
// no IR ever loaded. prepare() below closes that gap by explicitly loading
// a correctly-rate-tagged identity impulse response itself, so "no IR
// loaded" is a guaranteed bit-exact passthrough at every session sample
// rate, not only at 44100 Hz.
//
// Bundling license-clean factory cab IRs is its own ear-tuning-gated
// milestone (see docs/manual.md); loadImpulseResponse() here is the DSP-side
// seam a future GUI file browser (or preset system) will call.
//
// Threading: per JUCE's docs, Convolution::loadImpulseResponse() is
// wait-free and safe to call from the audio thread, but this class's
// loadImpulseResponse() is intended to be called from the message thread
// (e.g. in response to a GUI file-picker or preset load) - it takes
// ownership of (moves) the supplied buffer, so the caller must not also
// touch it on the audio thread afterwards.
//
// Cross-thread hardening (ported from basilica-audio/Nave PR #28, which hit
// this exact bug class in CabConvolutionEngine - see
// tests/CrossThreadReprepareTests.cpp for the full write-up). prepare() and
// loadImpulseResponse() both ultimately call
// juce::dsp::Convolution::loadImpulseResponse(), whose background hand-off
// (juce::dsp::BackgroundMessageQueue::push(), JUCE 8.0.14
// juce_Convolution.cpp) is documented "only safe to call from a single
// thread at a time." CryptaAudioProcessor has two call sites for these two
// methods that are not guaranteed to run on the same thread:
// prepareToPlay() (called by the host on a thread the VST3/AU contract only
// guarantees is *not* the audio thread - not that it is JUCE's message
// thread) and the public CryptaAudioProcessor::loadImpulseResponse(), a
// seam intended for a future GUI file picker / preset system but already
// reachable today as public API. `messageThreadMutex` below serialises the
// two, structurally removing the race rather than narrowing its window. It
// is a std::recursive_mutex only because loadImpulseResponse() is safe to
// call reentrantly from within prepare() were that ever needed - taken by
// prepare() and loadImpulseResponse() only; NEVER by process()/reset()/
// setWetMixProportion(), which stay lock- and allocation-free for the
// audio thread.
namespace cryp
{
    class IRLoader
    {
    public:
        IRLoader() = default;

        // `initialWetMixProportion01` must be the current irMix value
        // (0..1) *before* prepare() runs the internal DryWetMixer's
        // reset(): see the same gotcha documented on cryp::Voicing::prepare().
        // Message-thread-only (see the class-level threading comment above) -
        // serialised against loadImpulseResponse() via messageThreadMutex.
        void prepare (const juce::dsp::ProcessSpec& spec, float initialWetMixProportion01);
        void reset();

        void setWetMixProportion (float wetMixProportion01) noexcept { mixer.setWetMixProportion (wetMixProportion01); }

        // Loads a new impulse response. Not real-time safe by contract of
        // this wrapper (even though the underlying JUCE call is wait-free) -
        // call from the message thread only. `irSampleRate` is the sample
        // rate the buffer's samples were captured/generated at; Convolution
        // resamples internally to match the session's sample rate.
        // Serialised against prepare() via messageThreadMutex - see the
        // class-level threading comment above.
        void loadImpulseResponse (juce::AudioBuffer<float> irBuffer, double irSampleRate);

        // Restores the safe-by-default state: the correctly-rate-tagged
        // single-sample identity impulse response prepare() installs, i.e. a
        // bit-exact passthrough, and a reported tail length of 0. This is what
        // an IR slot's "None" entry calls (issue #21's slot mechanics) - the
        // convolution engine has no "unload" of its own, so "no IR" has to be
        // expressed as an explicit identity load. Message-thread only, and
        // serialised against prepare()/loadImpulseResponse() by the same
        // mutex. Safe to call before prepare(): it then falls back to the
        // ProcessSpec default rate, exactly as an unprepared Convolution
        // would.
        void clearImpulseResponse();

        // Issue #58: the currently-loaded IR's own duration in seconds
        // (numSamples / irSampleRate at the time it was loaded - a duration
        // is invariant under Convolution's internal resampling to the
        // session rate, so this doesn't need to track the session sample
        // rate separately). 0.0 while only the safe-by-default single-sample
        // identity IR is installed (i.e. before any real IR has been
        // loaded). CryptaAudioProcessor::getTailLengthSeconds() reports this
        // to the host so bounce/freeze/render-tail decisions account for the
        // actual convolution tail once a real cab IR is loaded, rather than
        // a hardcoded 0.
        double getTailLengthSeconds() const noexcept { return loadedIrTailSeconds; }

        // In-place: convolution + irMix dry/wet blend. Callers should skip
        // calling this entirely when irEnabled is off, for a guaranteed
        // bit-exact bypass rather than relying on mix==0.
        void process (juce::dsp::AudioBlock<float>& block) noexcept
        {
            mixer.pushDrySamples (juce::dsp::AudioBlock<const float> (block));

            juce::dsp::ProcessContextReplacing<float> context (block);
            convolution.process (context);

            mixer.mixWetSamples (block);
        }

    private:
        juce::dsp::Convolution convolution;
        juce::dsp::DryWetMixer<float> mixer;

        // See getTailLengthSeconds() above. 0.0 until a real IR is loaded
        // (the identity IR installed by prepare() has a negligible/zero
        // tail by design).
        double loadedIrTailSeconds = 0.0;

        // The rate prepare() was last called with; see clearImpulseResponse().
        // Defaults to juce::dsp::ProcessSpec's own conventional default so a
        // pre-prepare() call behaves no worse than the JUCE fallback it
        // replaces.
        double preparedSampleRate = 44100.0;

        // Serialises prepare() against loadImpulseResponse() - see the
        // class-level threading comment above. Recursive rather than plain,
        // matching Nave's CabConvolutionEngine pattern, so a future
        // message-thread-only method calling another one of this class's
        // message-thread-only methods (there is none today, but Nave's
        // equivalent grew that shape over time) does not self-deadlock.
        // Never locked by process()/reset()/setWetMixProportion().
        mutable std::recursive_mutex messageThreadMutex;
    };
}
