# Architecture

## Signal flow

```mermaid
flowchart LR
    IN[Input] --> TRIM[Input Trim]
    TRIM --> GATE[Noise Gate]
    GATE --> SPLIT1[LR4 Split Low<br/>60-400 Hz, default 120 Hz]

    SPLIT1 -->|Low band| LCOMP[Parallel Compressor<br/>+ Makeup + Mix]
    LCOMP --> LLEVEL[Level]
    LLEVEL --> LALIGN[Phase-Align Allpass<br/>tied to Split High]

    SPLIT1 -->|Remainder| SPLIT2[LR4 Split High<br/>300-2000 Hz, default 600 Hz]

    SPLIT2 -->|Mid band| MDRIVE[Drive<br/>oversampled]
    MDRIVE --> MLEVEL[Level]

    SPLIT2 -->|High band| HTIGHT[Tight<br/>pre-drive HPF]
    HTIGHT --> HVOICE[Voicing:<br/>Gnaw / Wool / Razor<br/>4x oversampled]
    HVOICE --> HDRIVE[Drive]
    HDRIVE --> HTONE[Tone]
    HTONE --> HBLEND[Clean/Dist Blend]
    HBLEND --> HLEVEL[Level]

    MLEVEL --> MHSUM[Mid+High Sum]
    HLEVEL --> MHSUM
    MHSUM --> IR[IR Loader<br/>cab sim - Mid+High only]

    LALIGN --> SUM[Sum<br/>delay-compensated]
    IR --> SUM

    SUM --> EQ[4-band EQ]
    EQ --> CLIP[Safety Clip<br/>optional]
    CLIP --> OUT[Output Trim]
```

All three bands re-converge at the `Sum` stage. The Low band carries a compensation delay (matching the Mid/High branch's shared oversampling latency) and a phase-alignment allpass filter (see [Cascaded 3-band flat-sum](#cascaded-3-band-flat-sum-and-phase-alignment) below) so the three-way sum is both time-aligned and magnitude-flat.

## Module map

| Directory | Responsibility |
|---|---|
| `src/dsp` | All audio-thread DSP, each in its own class with a matching Catch2 test file: `Crossover` (LR4 split/sum, two cascaded instances - `lowSplit`, `midHighSplit`), `PhaseAlignFilter` (Low-band phase-alignment allpass - see below), `NoiseGateStage` (full-band input gate), `ParallelCompressor` (low-band dynamics), `MidBand` (mid-band staged drive, NEW in v0.2.0), `Voicing` (high-band Gnaw/Wool/Razor distortion + Tight pre-drive HPF + 4x oversampling + tone + blend), `BandEQ` (post-sum 4-band EQ), `IRLoader` (cab-sim convolution, relocated in v0.2.0 to the Mid+High branch), `SplitGap` (pure `clampSplitHighHz()` helper enforcing the minimum musical gap between the two crossover splits). **v0.3.0 adds** `ADAAShaper` (antiderivative antialiasing core, closed-form and tabulated), `CircuitDrive` (the Circuit engine: one shared oversampling region, OS-rate split, per-voicing circuit topologies), `LevelDetector` (log-domain RMS compressor detector with soft knee and program-dependent release), `GateEngine` (the Modern gate), `OutputClipper` (ADAA ceiling clip in delta form) and `MeterTaps` (lock-free metering). `RealtimeCoefficients.h` is a shared helper for updating `juce::dsp::IIR` filter coefficients from the audio thread without heap allocation (see below). No allocation, locks, or I/O once `prepareToPlay` has run. |
| `src/params` | Parameter layout and `AudioProcessorValueTreeState` definitions — parameter IDs, ranges, defaults, and value-to-DSP mapping. Single source of truth for what a preset captures. |
| `src/presets` | M2 preset system (`PresetManager`, `PresetBar`, `Localisation`) - see [Preset system](#preset-system-m2) below. |
| `src/ui` | Editor/GUI code. Talks to the processor only through `src/params` (attachments) and read-only metering data — never reaches into `src/dsp` internals directly. Placeholder generic JUCE UI (plus the M2 preset bar) until the custom vector GUI lands in a later milestone (M3). |

Dependency direction is one-way: `src/ui` → `src/params` ← `src/presets`, and `src/dsp` is driven by `src/params` values but has no upward dependency on UI, presets, or state code. This keeps the DSP core testable in isolation (see `tests/`) without instantiating any UI or persistence machinery.

## Cascaded 3-band flat-sum and phase alignment

v0.2.0 restructures the signal path from a single 2-band LR4 split to two **cascaded** LR4 splits (Low, then Mid/High of the remainder) - see `docs/design-brief.md`'s Topology section for the design rationale. A single `cryp::Crossover`'s own dual-output Low+High sum is exactly a flat-magnitude allpass-filtered version of *that stage's own input* (`juce::dsp::LinkwitzRileyFilter`'s documented property). But naively cascading two such stages does **not** automatically make the three-way sum flat relative to the *original* input - confirmed empirically while building `tests/ThreeBandFlatSumTests.cpp`: deviations up to −10 dB appeared at close `splitLowHz`/`splitHighHz` ratios (worst inside the first crossover's own transition band), because the second crossover's own phase shift is applied only to the Mid+High branch, leaving the untouched Low band "out of phase" with it at the final sum.

The fix (`src/dsp/PhaseAlignFilter.h`) is proven algebraically, not just empirically: reading JUCE 8.0.14's own `juce_LinkwitzRileyFilter.cpp` source confirms the dual-output `processSample(channel, input, outputLow, outputHigh)`'s `outputLow + outputHigh` sum uses the *exact same formula* (built only from the first internal biquad stage's state) as that same class's single-output `processSample(channel, input)` in `Type::allpass` mode. Since that allpass transform is linear:

```
Low_compensated + Mid + High
  = Allpass2(Low) + Allpass2(Remainder)   [Mid+High = Allpass2(Remainder), by midHighSplit's own reconstruction property]
  = Allpass2(Low + Remainder)             [Allpass2 is linear]
  = Allpass2(Input)                       [Low + Remainder = Input exactly, lowSplit's own reconstruction property]
```

and `Allpass2(Input)` has flat magnitude relative to `Input` by definition (an allpass filter's magnitude response is unity at every frequency). `PhaseAlignFilter` wraps a *second, physically separate* `juce::dsp::LinkwitzRileyFilter<float>` instance configured `Type::allpass`, always tied to the *same* effective cutoff as `midHighSplit` (via `cryp::clampSplitHighHz()` - see below), applied to the Low band right after its own compressor+level processing, before the final sum. This is a standard technique in professional N-way active-crossover design (phase-alignment allpass networks between cascaded crossover stages), not a Crypta-specific invention.

## Minimum-gap clamp between Split Low and Split High

`splitLowHz`'s own range (60-400 Hz) overlaps `splitHighHz`'s own range (300-2000 Hz), so the two parameters could in principle collapse the Mid band to a degenerate near-zero (or inverted) width. `src/dsp/SplitGap.h`'s `cryp::clampSplitHighHz(splitLowHz, requestedSplitHighHz)` is a pure, real-time-safe function enforcing a minimum 1/3-octave gap: `midHighSplit` (and `lowBandPhaseAlign`, which must always match it exactly) are always set to `clampSplitHighHz()`'s *effective* value, never the raw `splitHighHz` parameter directly. `PluginProcessor`, the test suite, and the two DSP-level tests exercise the same function, so the boundary behaviour is directly testable (`tests/SplitGapTests.cpp`, `tests/ThreeBandFlatSumTests.cpp`).

## Real-time-safe filter coefficient updates

`juce::dsp::IIR::Coefficients<float>::makeLowShelf`/`makePeakFilter`/`makeHighShelf`/... (the usual way to build filter coefficients) heap-allocate a new `Coefficients` object on every call - fine in `prepareToPlay()`, not fine on the audio thread when a parameter (an EQ band's frequency, the high-band voicing's mid-filter, its tone or Tight control, ...) is being automated continuously. `BandEQ` and `Voicing` both use `juce::dsp::IIR::ArrayCoefficients<float>::makeXxx()` instead, which returns the same coefficients as a stack-only `std::array` (zero allocation), and `src/dsp/RealtimeCoefficients.h` writes that array's values directly into an already-allocated `Coefficients<float>` object's raw storage (normalising by `a0` the same way `Coefficients`' own constructor does). The `Coefficients` object itself is allocated exactly once, during `prepare()`; every subsequent update on the audio thread only ever overwrites existing memory.

## IR loader safe-by-default behaviour and relocation

`juce::dsp::Convolution` falls back to an internal single-sample identity impulse response when `loadImpulseResponse()` has never been called - but that fallback's assumed source sample rate is hardcoded to JUCE's `ProcessSpec` default (44100 Hz), so at any *other* session sample rate it would otherwise get silently resampled (smeared/attenuated) against a mismatched rate. `IRLoader::prepare()` closes that gap by explicitly loading a correctly-rate-tagged identity impulse response itself, so "no IR loaded" is a guaranteed bit-exact passthrough at every session sample rate, not only at 44100 Hz - see the class-level comment in `src/dsp/IRLoader.h`. This behaviour is unchanged in v0.2.0; what changed is *where* `IRLoader` sits in the signal path - it now processes only the Mid+High post-sum signal, structurally never the Low band (see `PluginProcessor.cpp`'s `processChunk()` and `tests/LowBandIsolationTests.cpp`, which asserts the Low band's own isolated output is bit-exact identical whether the IR loader is enabled or not).

## Latency compensation

The Mid band's staged drive (`cryp::MidBand`) and the High band's voicing (`cryp::Voicing`) each run their own nonlinear shaping stage oversampled 4x (`juce::dsp::Oversampling`, FIR half-band equiripple, max quality, integer latency) - two *physically separate* `juce::dsp::Oversampling` instances, identically configured (same factor exponent, same filter type), so their reported latencies are guaranteed numerically equal by construction even though they are not literally one shared object (see `src/dsp/MidBand.h`'s class docs for why literal instance-sharing was not implemented). This oversampling is the *only* source of latency in the current signal path - the gate, both crossovers, the phase-alignment allpass, the low-band parallel compressor, the EQ, and the IR loader (configured for zero-latency convolution) are all zero-latency by construction. To keep all three bands phase-coherent at the `Sum` stage:

- The Low band path carries a matching `juce::dsp::DelayLine` (integer/no-interpolation, since the delay is always a whole number of samples) sized to `juce::jmax(midBand.getLatencySamples(), highVoicing.getLatencySamples())`.
- The High band's own clean/distorted blend (`highBlend`) is handled by a `juce::dsp::DryWetMixer` whose dry path is *also* delay-compensated (`setWetLatency`) by that same amount, so the clean and distorted high-band signals stay phase-coherent with each other too, not just with the Low band. The Mid band has no blend control (see `src/dsp/MidBand.h`), so it needs no such compensation.

`CryptaAudioProcessor::computeTotalLatencySamples()` reports that shared value to the host via `setLatencySamples()`, so host-side plugin delay compensation (PDC) accounts for the whole chain.

### The `DryWetMixer` priming gotcha (JUCE 8.0.14)

`juce::dsp::DryWetMixer::prepare()` calls `reset()` internally, which snaps its smoothed dry/wet volumes to whatever `mix` was set to *at that moment* - so if `prepare()` runs before the real mix value is set, the mixer briefly snaps to a stale default before the next `setWetMixProportion()` call retargets it, causing an audible fade-in glitch on the very first block. `ParallelCompressor::prepare()`, `Voicing::prepare()`, and `IRLoader::prepare()` all take the current mix proportion as an explicit parameter and call `setWetMixProportion()` *before* `prepare()` internally, closing this gap at the API level rather than relying on call-order discipline at every call site. `MidBand` has no `DryWetMixer` (no blend control), so this gotcha does not apply to it.

## Drive engines (v0.3.0)

`driveEngine` selects between two implementations of the Mid+High section.

**Classic** is the v0.2.0 code, unchanged: `cryp::MidBand` and `cryp::Voicing`, each owning its own 4x oversampling instance. It is preserved byte-for-byte because it is what every pre-v0.3.0 session and preset is migrated onto — `tests/GoldenRenderTests.cpp` renders committed v0.2.0 fixtures through the migrated processor and asserts sample-exact equality on macOS.

**Circuit** (`src/dsp/CircuitDrive.{h,cpp}`) collapses the two oversampling regions into one:

```
remainder (base rate)
  -> upsample once
  -> LR4 split #2, at fs*M          (cryp::Crossover, unchanged, prepared at fs*M)
  -> Mid:  ADAA tanh, dry-crossfaded by drive
     High: tight HPF -> per-voicing pre-emphasis -> ADAA clipper -> de-emphasis
           -> drive-tracked LPF -> DC blocker -> character -> tone -> blend
  -> per-band level -> sum -> downsample once
```

The saved oversampling region is what pays for the extra per-voicing filtering. The factor adapts to the host rate (4x ≤ 50 kHz, 2x ≤ 100 kHz, 1x above), on the basis that ADAA-1 contributes 20–30 dB of alias suppression on top of the oversampling headroom.

Both engines stay prepared at all times, so switching is a branch rather than a reallocation. A switch arms a 256-sample constant-gain crossfade during which **both** engines run, and **flushes the incoming engine** first: only one engine runs at a time, so the idle one's oversampling history and delay lines otherwise hold stale audio and release it as a burst. The crossfade is longer than the brief's 64 samples because the flushed engine needs its own latency to refill before it can be given significant gain.

### Antialiasing

`src/dsp/ADAAShaper.h` implements first-order antiderivative antialiasing (Parker, Zavalishin & Le Bivic, DAFx-16): `y[n] = (F1(x[n]) − F1(x[n−1])) / (x[n] − x[n−1])`, with a midpoint fallback when consecutive inputs converge and the quotient becomes ill-conditioned. Closed forms are used where the antiderivative is elementary (tanh → ln cosh, hard clip → piecewise); otherwise a 2048-point cubic-interpolated table is built at prepare time, with `F1` obtained by Simpson integration of the sampled curve so the pair stays mutually consistent.

Two costs are inherent and accepted: a half-sample group delay (identical across Mid and High, so the bands stay aligned, and absorbed inside the oversampled region), and a mild `cos(pi*f/fs)` droop — which is why this is used *on top of* oversampling rather than instead of it.

## Latency across the two engines

The engines can have different intrinsic latencies, because Circuit's oversampling factor varies with the host rate while Classic is always 4x. Rather than re-report latency when `driveEngine` changes — which hosts handle poorly mid-transport, and which would make an automated engine switch shift the plugin's timing — `computeTotalLatencySamples()` returns the maximum across **both** engines and `circuitAlignDelay` pads the Circuit path up to it. Reported latency is therefore a function of the sample rate alone.

Note that the reported figure is the oversampling delay, not a full group-delay description: the Circuit high band contains IIR filters Classic does not (the 10 Hz DC blocker, the drive-tracked pole), so its reconstructed impulse peaks up to ~25 samples later. No single number can describe a frequency-dependent group delay; what is asserted instead is that reported latency matches across engines and rates, and that the three-way sum stays flat.

## The safety clip's delta form

`src/dsp/OutputClipper.h` applies ADAA to the clipper's **residual**, `r(x) = x − c·tanh(x/c)`, and returns `x − ADAA1_r(x)`.

The reason is that naive ADAA-1 of a function that is nearly linear over the segment degenerates to the two-tap average `(x[n] + x[n−1])/2` — a lowpass about −8.3 dB down at 18 kHz at 48 kHz, plus half a sample of delay, applied to the entire mix whenever the clip is merely armed. The delta form is algebraically `ADAA1(clip(x)) + (x[n] − x[n−1])/2`, i.e. the antialiased clipper plus an exact compensator for that droop and delay, so sub-ceiling material passes through transparently by construction.

That compensator is a first difference, and on fast material it can push a sample back over the ceiling (measured at 1.15 against a ceiling of 1.0). A final hard bound at the ceiling is therefore applied: it never engages below the ceiling, so transparency is untouched, and above it the ADAA has already done the antialiasing. At extreme overdrive the bound does cost the antialiasing advantage — the right priority ordering for a *safety* clip, with heavy clipping belonging to the drive stages.

## Metering

`src/dsp/MeterTaps.h` is a plain struct of `std::atomic<float>` — no FIFO, no queue, no allocation. The audio thread stores at block rate; the UI loads at 30 Hz. A meter is a most-recent-value display, so block-rate decimation is both sufficient and free, and a `static_assert` on `is_always_lock_free` keeps the audio thread from ever blocking on a reader. Both drive engines report their own per-band levels, since the Circuit engine's bands are summed inside its oversampled region and cannot be measured from outside.

## Preset system (M2)

v0.2.0 adds the suite-wide M2 preset system (`.scaffold/specs/preset-system-m2.md`), copied from `basilica-audio/nave`'s pilot implementation (`docs/preset-system-notes.md` in that repo is the replication recipe) - `src/presets/PresetManager.{h,cpp}` and `src/presets/PresetBar.{h,cpp}` are portable, Crypta-agnostic classes; the only Crypta-specific glue is `PluginProcessor.cpp`'s `makePresetManagerConfig()`/`makeFactoryPresetAssets()` helpers and the nine `presets/factory/*.json` files (embedded via `juce_add_binary_data` as `CryptaBinaryData`, see `CMakeLists.txt`). `AudioProcessorValueTreeState` is the single source of truth for parameter values; `PresetManager` reads/writes it only through its public API and owns no parallel copy of state.

`PresetManager`'s only audio-thread-adjacent code is its `AudioProcessorValueTreeState::Listener::parameterChanged()` override (dirty-flag tracking), implemented as a single lock-free `std::atomic<bool>` store. Every other method (file I/O, JSON parsing, `juce::String`/`juce::var` allocation) is message-thread-only, called from the processor's constructor or from `PresetBar`'s UI callbacks - never from `processBlock()`.

The editor (`PluginEditor.cpp`) installs a German localisation frame (`resources/i18n/de.txt`, selected via `SystemStats::getUserLanguage()`) before constructing `PresetBar`, using the same `initLocalisationThenGetPresetManager()` helper-function pattern nave established (member initialisers run in declaration order regardless of the order they're written in the initialiser list, so the helper must be invoked from `presetBar`'s own initialiser expression, not the constructor body).

## State migration (schema v2, v0.3.0)

v0.3.0 adds three engine selectors whose APVTS defaults name the NEW engines, so a genuinely fresh instance boots into them. Saved work must not inherit that, and there are **two independent entry points** that both need covering:

1. **Sessions.** `getStateInformation()` stamps a `stateVersion` attribute on the APVTS root element; `setStateInformation()` injects the legacy value for any of the three IDs a state without that attribute fails to mention. It runs after the v0.1 crossover migration, so a v0.1 session gets both in schema order.
2. **Presets.** Presets never pass through `setStateInformation()` at all. And because `applyParsedPreset()` calls `resetAllParametersToDefault()` before applying a preset's values, a legacy preset — which cannot name the new IDs — would otherwise adopt their new defaults. `PresetManagerConfig` gains a generic, version-gated legacy back-fill (`legacyParameterCutoffVersion` + `legacyParameterDefaults`), empty by default so the rest of the suite is unaffected. This is a read-side default-fill: the preset JSON schema, format tag and `parseAndValidate()` contract are all unchanged.

Both paths refuse to override a value that is explicitly present, and the preset path is gated on version rather than key presence — so a v0.3.0 preset that names one engine and omits the others keeps the new defaults for those, rather than being dragged back to legacy values.

`presets/factory/default.json` is the mechanism by which a fresh instance actually reaches the new engines (the constructor calls `applyStartupDefault()`, which loads it), so it pins all three explicitly and declares `pluginVersion` 0.3.0. The eight presets voiced against the v0.2.0 DSP pin Classic.

## State migration (v0.1.x → v0.2.0)

`CryptaAudioProcessor::setStateInformation()` runs a one-way, best-effort migration (`migrateLegacySingleCrossover()`, `PluginProcessor.cpp`) before handing state to `apvts.replaceState()`: a v0.1.x session's single `crossoverFreq` `PARAM` XML element (that parameter ID no longer exists in v0.2.0's `ParameterLayout`) is read directly out of the raw XML, clamped into `splitHighHz`'s new 300-2000 Hz range, and injected as a new `splitHighHz` `PARAM` element - unless one is already present (defensive, not expected from a genuine v1 or v2 session). Every other new v0.2.0 parameter (`splitLowHz`, `midDrive`, `midLevel`, `highTightHz`) simply falls back to its own `ParameterLayout` default via `AudioProcessorValueTreeState::replaceState()`'s existing "unmentioned parameter ID keeps its current/default value" behaviour - no special-case code needed for those. See `tests/StateMigrationTests.cpp` for the full test coverage, including the dedicated regression test asserting an untouched v0.1.x session (shipped default `crossoverFreq` = 250 Hz, below the new 300 Hz floor) lands exactly at 300 Hz.
