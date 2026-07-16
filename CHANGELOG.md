# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.1] - 2026-07-16

### Changed

- Renamed plugin from Twist Your Guts to Crypta (new plugin code `Cryp`, new bundle id `com.yvesvogl.crypta`). Old identity: plugin code `Tygt`, bundle id `com.yvesvogl.twistyourguts` — DAWs treat this as a new plugin; v0.1.0-era sessions will need to be re-pointed at the new plugin identity. Part of the suite's move to Basilica Audio naming (the crypt: the basilica's low-end foundation).
- `.github/workflows/release.yml` reconciled with the suite-wide release template: org-level Apple signing secrets (`APPLE_CERT_P12`, `APPLE_CERT_PASSWORD`, `APPLE_API_KEY_P8`, `APPLE_API_KEY_ID`, `APPLE_API_ISSUER_ID`) instead of the prior per-repo secret set, `find`-based artefact discovery, tag-only (`v*`) trigger with no `workflow_dispatch` dry-run path.
- Removed `docs/releasing.md` and `docs/adr/0006-macos-signing-notarization.md`, which documented the prior per-repo signing pipeline; the org-level signing setup is now documented centrally at `.scaffold/SIGNING-SETUP.md`, matching sibling suite repos (none of which carry a per-repo releasing runbook or signing ADR).
- Reworded the stale "symphonic-metal" suite framing in `CLAUDE.md` and `docs/manual.md` to "heavy-music", matching the suite bible.
- `juce_add_plugin` now sets `ICON_BIG` from `docs/assets/icon.png`, so the plugin bundle carries its own icon instead of JUCE's default.

### Fixed

- `CryptaAudioProcessor` now overrides `reset()`, flushing every per-stage DSP class's own state (LR4 crossover filter memory, gate/compressor envelopes, the high-band voicing's oversampling/mid/tone filter state, the low-band latency-compensation delay line, EQ biquad history, IR convolution engine) on a host transport stop/loop/rewind, instead of leaving stale state ringing into whatever plays next.
- `Voicing::setVoicing()` now resets `midFilter`'s state on every voicing change, matching `preHighPass`'s existing Razor-switch handling, so a coefficient jump between voicings (e.g. Wool's -6dB scoop to Razor's +5dB hump) no longer rings a stale-state transient.
- `getTailLengthSeconds()` now reports `IRLoader`'s actually-loaded impulse response length instead of a hardcoded `0.0`, so hosts making bounce/freeze/render-tail decisions don't truncate a loaded cab IR's convolution tail.
- Added regression coverage for `processBlock()`'s chunking path when a host block exceeds `prepareToPlay()`'s `samplesPerBlock` (no functional bug found - the existing chunking logic was already correct, just untested).

## [0.1.0] - 2026-07-14

### Added

- Project bootstrap: README, license, contributing guide, architecture and build docs, ADRs, and CI workflow.
- Full v1.0 `AudioProcessorValueTreeState` parameter layout (frozen parameter IDs) covering IO/global, noise gate, crossover, low band, high band, EQ, and IR loader.
- LR4 (Linkwitz-Riley 4th order) crossover band-split (`src/dsp/Crossover`), flat-sum tested, with a latency-compensation framework/seam in the processor.
- **DSP completion (M1):** the full v1.0 signal path is wired and live:
  - Full-band input noise gate (`src/dsp/NoiseGateStage`), off by default.
  - Low-band parallel ("New York style") compressor with makeup gain and wet/dry mix (`src/dsp/ParallelCompressor`).
  - High-band distortion engine (`src/dsp/Voicing`) with three selectable voicings — **Gnaw** (op-amp hard clip), **Wool** (cascaded soft-clip fuzz with mid scoop), **Razor** (tight overdrive: pre-clip highpass, soft clip, mid hump) — each running its nonlinear shaping stage 4x oversampled (FIR half-band equiripple) to control aliasing, with drive, tone, and clean/distorted blend controls.
  - Post-sum 4-band EQ (`src/dsp/BandEQ`: LowShelf / Peak / Peak / HighShelf), off by default.
  - Cab-sim IR loader (`src/dsp/IRLoader`, `juce::dsp::Convolution`-based), off by default, safe-by-default (bit-exact passthrough with no IR loaded, at every session sample rate); `loadImpulseResponse()` is the DSP-side seam a future GUI/preset system will call to load user or factory IRs.
  - Latency compensation extended to cover the high band's oversampling latency: reported to the host via `setLatencySamples`, low band delay-compensated to match, high band's own clean/distorted `DryWetMixer` blend delay-compensated too.
  - `src/dsp/RealtimeCoefficients.h`: shared real-time-safe (zero-allocation) `juce::dsp::IIR` coefficient update helper, used by `BandEQ` and `Voicing`'s mid/tone filters.
- Broadened Catch2 test suite (issue #43): dedicated test files for every new DSP stage (`NoiseGateTests`, `ParallelCompressorTests`, `VoicingTests`, `BandEQTests`, `IRLoaderTests`), plus sample-rate sweeps (44.1–192 kHz), mono/stereo bus-configuration tests, extreme-parameter-automation and long-run NaN/Inf stability soak tests (`SampleRateAndRobustnessTests`). Existing gain-staging/latency/passthrough tests updated to account for the now-live (non-transparent-by-default) compressor and voicing stages.
- `docs/manual.md`: full user manual — what the plugin is, where it sits in a symphonic-metal chain, signal-flow description, complete parameter reference, and usage tips.

### Changed

- `docs/architecture.md`: signal-flow diagram and module map updated to match the new full signal path; new sections documenting the real-time-safe filter-coefficient pattern, the IR loader's safe-by-default behaviour, and the extended latency-compensation design (including the `DryWetMixer` priming gotcha).
- `README.md`: feature list, signal-flow diagram, and roadmap table updated to match the live DSP and the project's actual milestone scheme (M1 DSP completion & test coverage → M2 presets & state → M3 GUI & accessibility → M4 release).
