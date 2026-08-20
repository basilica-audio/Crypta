#pragma once

#include "ADAAShaper.h"
#include "RealtimeCoefficients.h"

#include <juce_dsp/juce_dsp.h>

#include <vector>

// "Graaawl" - the Warwick Thumb 5 low-band growl mode (issue #36).
//
// What the growl actually is, and why this is not "distort the lows".
// The Thumb's signature is not low-frequency distortion: it is an asymmetric
// upper-mid harmonic character in roughly 700 Hz - 2.2 kHz with a formant-like
// resonance near 1 kHz, sitting on top of a low end that stays tight. Running
// the sub-fundamental itself through a shaper produces intermodulation mud and
// a flabby bottom - the opposite of the instrument. So the growl is generated
// in a PARALLEL branch and band-limited to the formant region before it is
// blended back; the dry low band, and therefore the fundamental, is never
// shaped at all.
//
// Where the harmonics come from (the correction to the issue's sketch).
// The issue proposes high-passing the branch input at 300-400 Hz "so only the
// upper part of the low band feeds the shaper". That does not work in this
// topology: this stage sits INSIDE the Low band, i.e. below the LR4 Split Low
// crossover (60-400 Hz, default 120 Hz). At the default split there is nothing
// left at 350 Hz but stopband leakage - LR4 is 24 dB/octave, so 350 Hz is
// already ~37 dB down. A 350 Hz pre-highpass would feed the shaper silence.
//
// The formant energy therefore has to be GENERATED, not extracted: a bass
// fundamental driven into an asymmetric saturator produces a full harmonic
// series, and the growl band is simply where that series is listened to. For a
// 50 Hz fundamental the 700 Hz - 2.2 kHz window is harmonics 14-44; the
// asymmetry (a DC bias into the shaper) is what puts EVEN harmonics in there
// as well as odd, which is the woody/vocal half of the Thumb character rather
// than the pure grind of a symmetric curve. This is the same generative
// structure an octave-fuzz-into-a-formant-filter uses, and it is what keeps
// the sub clean: the band-pass AFTER the shaper is what guarantees the
// fundamental region is untouched, not a filter before it.
//
// Topology per block, all real-time safe once prepare() has run:
//
//   dry low band ------------------------------------------------+
//                                                                 |
//   copy -> rumble highpass (2nd order, 30 Hz: keeps DC/subsonics |
//           out of an asymmetric curve, where they would wander    |
//           the operating point)                                   |
//        -> asymmetric shaper: tanh(g*x + b) - tanh(b), ADAA-1     |
//        -> formant band-pass: 24 dB/oct HPF + resonant peak       |
//           + 24 dB/oct LPF, all tracking the Tone control          |
//        -> smoothed growl gain (amount) ----------------------- + |
//                                                                 v
//                                                            low band out
//
// Antialiasing, and why there is no oversampling stage here.
// The nonlinearity is a tanh at a fixed, moderate drive, fed by a band that is
// already low-passed at (at most) 400 Hz, and its output is band-limited to
// the formant window before it reaches the sum. A tanh's harmonic series
// decays geometrically rather than as 1/n, so the orders that would fold from
// above Nyquist are already far below the noise floor; ADAA-1 (see
// ADAAShaper.h, Parker/Zavalishin/Le Bivic DAFx-16) removes another 20-30 dB
// on top. Measured alias-to-signal for the whole stage is asserted in
// tests/LowGrowlTests.cpp.
//
// That buys a property oversampling could not: this stage adds ZERO latency,
// so turning Graaawl on and off never changes the plugin's reported latency,
// never needs a compensating delay on the dry low path, and - because
// process() returns immediately once the growl gain has ramped to zero - the
// OFF state is a structural bit-exact bypass rather than an approximation.
namespace cryp
{
    class LowGrowl
    {
    public:
        LowGrowl() = default;

        void prepare (const juce::dsp::ProcessSpec& spec);
        void reset();

        // Control-rate setters, all real-time safe (scalar stores plus a
        // SmoothedValue retarget; the coefficients they imply are recomputed
        // once per block inside process()).
        void setEnabled (bool shouldBeEnabled) noexcept
        {
            enabled = shouldBeEnabled;
            updateGrowlGainTarget();
        }

        void setAmount (float amount01) noexcept
        {
            amount = juce::jlimit (0.0f, 1.0f, amount01);
            updateGrowlGainTarget();
        }

        void setTone (float tone01) noexcept { tone = juce::jlimit (0.0f, 1.0f, tone01); }

        // Zero, by construction - see the class docs. Present so the stage
        // reads the same as every other one in the chain, and so a future
        // change that DID add latency has an obvious place to report it.
        int getLatencySamples() const noexcept { return 0; }

        // Centre of the formant band for the current Tone setting, in Hz.
        // Exposed for the spectral assertions in the tests (and for anything
        // that wants to draw it) rather than for the audio path.
        float getFormantCentreHz() const noexcept;

        // In-place: adds the band-limited growl branch onto the low band.
        // Returns immediately - touching neither the block nor any internal
        // state - once the stage is disabled AND its gain ramp has finished,
        // which is what makes "Graaawl off" bit-exact.
        void process (juce::dsp::AudioBlock<float>& block) noexcept;

    private:
        void updateGrowlGainTarget() noexcept
        {
            growlGain.setTargetValue (enabled ? amount * maximumGrowlGain : 0.0f);
        }

        void updateCoefficients() noexcept;
        void resetBranchState() noexcept;

        using Duplicator = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;

        //======================================================================
        // Fixed voicing constants. Engineering-derived starting points for the
        // ear-tuning gate issue #36 defines (a real Thumb 5 DI), not final
        // voicing decisions.

        // Into the shaper. 6.0 (+15.6 dB) drives a -12 dBFS low band well into
        // the tanh knee, which is where the series is rich enough for the
        // 14th-44th harmonics to matter, without the curve degenerating into a
        // square wave (whose 1/n series would both alias worse and lose the
        // vowel character to sheer buzz).
        static constexpr double shaperDriveGain = 6.0;

        // The asymmetry. tanh(g*x + b) is an even-plus-odd series; b = 0.35
        // puts real second-harmonic energy into the formant band at this
        // drive, which is the "woody" half of the Thumb character. b = 0 would
        // be a purely odd-harmonic grind. The constant tanh(b) is subtracted
        // again so the branch has no steady-state DC even before the highpass.
        static constexpr double shaperBias = 0.35;

        // Amount 100 % maps to +12 dB on the branch. The branch is a
        // harmonic-only signal (its fundamental is filtered away), so it needs
        // makeup to reach the dry band at all; the level it lands at stays
        // strongly input-dependent, which is the intended behaviour - the
        // growl should show up when the player digs in.
        static constexpr float maximumGrowlGain = 4.0f;

        // Formant sweep for the Tone control, log-mapped. The 0.5 default
        // lands on sqrt(800*1600) = 1131 Hz, i.e. the ~1 kHz vowel the issue
        // specifies.
        static constexpr float minimumFormantHz = 800.0f;
        static constexpr float maximumFormantHz = 1600.0f;

        // Band edges as ratios around the formant centre: at the default
        // centre this is a 707 Hz - 2206 Hz window, matching the issue's
        // 700 Hz - 2.2 kHz target.
        static constexpr float lowerCornerRatio = 1.0f / 1.6f;
        static constexpr float upperCornerRatio = 1.95f;

        // Resonance of the formant peak itself, on top of the band-pass.
        static constexpr float formantPeakGainDb = 6.0f;
        static constexpr float formantPeakQ = 1.4f;

        // Each band edge is a 4th-order Butterworth, built as the two sections
        // its pole pair prescribes (Q = 0.5412 and Q = 1.3065) rather than as
        // two identical Q = 0.7071 sections. That matters twice: the passband
        // stays maximally flat, and the stated corner really is the -3 dB
        // point, so "700 Hz - 2.2 kHz window" is a measurement rather than a
        // rough description. 24 dB/octave below the lower corner is also what
        // puts the fundamental region ~54 dB down inside the branch, which is
        // the structural half of the "sub stays clean" guarantee.
        static constexpr float butterworthQ1 = 0.5411961f;
        static constexpr float butterworthQ2 = 1.3065630f;

        // The subsonic guard ahead of the shaper is a single section, so it
        // uses the plain 2nd-order Butterworth Q.
        static constexpr float guardQ = 0.7071068f;

        // DC/subsonic guard ahead of the shaper.
        static constexpr float rumbleHighPassHz = 30.0f;

        // Gain-ramp length. Long enough that toggling Graaawl mid-note is a
        // fade rather than a step, short enough to still read as a switch.
        static constexpr double growlGainRampSeconds = 0.02;

        //======================================================================
        double sampleRate = 44100.0;
        bool enabled = false;
        float amount = 0.0f;
        float tone = 0.5f;

        // False while the branch is fully faded out and its state has been
        // flushed; the next block that needs the branch re-arms it from a
        // known-zero state, so a stale filter/shaper history from minutes ago
        // can never be dumped into a live signal.
        bool branchArmed = false;

        // Tracks the Tone setting the coefficients were last computed for, so
        // a static Tone costs one comparison per block instead of five
        // coefficient computations.
        float coefficientTone = -1.0f;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> growlGain;

        // Scratch for the parallel branch, sized in prepare(). Never resized
        // from process().
        juce::AudioBuffer<float> branchBuffer;

        Duplicator rumbleHighPass { new juce::dsp::IIR::Coefficients<float> (1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f) };
        Duplicator formantHighPass1 { new juce::dsp::IIR::Coefficients<float> (1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f) };
        Duplicator formantHighPass2 { new juce::dsp::IIR::Coefficients<float> (1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f) };
        Duplicator formantPeak { new juce::dsp::IIR::Coefficients<float> (1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f) };
        Duplicator formantLowPass1 { new juce::dsp::IIR::Coefficients<float> (1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f) };
        Duplicator formantLowPass2 { new juce::dsp::IIR::Coefficients<float> (1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f) };

        // One ADAA-1 history per channel.
        std::vector<ADAAState> shaperState;
    };
}
