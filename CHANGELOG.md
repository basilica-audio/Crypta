# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added (headline: the custom vector GUI)

- **A fully vector-drawn editor** (`src/gui/`, `src/PluginEditor.{h,cpp}`), replacing the v0.1–v0.3 preset bar + `GenericAudioProcessorEditor` stack. Ported from the suite's proven M3 component family (`basilica-audio/Miserere` PR #31): `BasilicaLookAndFeel` (pointer knobs with engraved scale rings, detented choice knobs, lamp toggles, gold-on-black suite palette), `PointerKnob`, `BusPanel`, `KeyboardSteps` and `NeedleMeter`. No bitmaps: every knob, lamp, panel, needle and glyph is drawn at runtime with `juce::Graphics`/`juce::Path`. Typography is EB Garamond, embedded via `BinaryData` (OFL, `resources/fonts/OFL-EBGaramond.txt`), so the editor renders identically on macOS and Windows. Closes #45, #25.
- **The complete parameter surface, laid out in signal-flow order** across ten section panels — Input, Noise Gate, Crossover, Low Band, Drive Engine, Mid Band, High Band, Cabinet, EQ, Output. All 51 parameters are on the front panel: 44 knobs (40 float + 4 choice; choice knobs are detented and announce the choice *name*) and 7 lamp toggles. Closes #26.
- **Four needle meters reading the v0.3.0 metering backend** (`src/dsp/MeterTaps.h`): input and output peak (dBFS, fast-attack/slow-release ballistics, warning zone from 0 dBFS) plus gate and low-band-compressor gain reduction (positive dB, symmetric ballistics). Driven by a single 30 Hz GUI-thread timer over relaxed atomic loads — the audio thread is never touched. Closes #27.
- **A resizable, aspect-ratio-locked editor with the scale persisted in plugin state.** The surface is laid out once at its design size and the window is a uniform scale transform on it (0.6×–1.8×), so no window size can re-flow or clip the layout. The chosen scale is stored as a root property on the APVTS state tree and therefore round-trips through the existing `get`/`setStateInformation()` pair; a pre-existing session without it opens at unity. Closes #28.
- **Accessibility as a shipped feature, not a follow-up** (closes #46): every control is keyboard-focusable with WAI-ARIA-style stepping (Arrow = 1 % of range, Shift+Arrow = fine, PageUp/Down = 10 %, Home/End = extremes; one detent per press on choice knobs), a gold focus ring with a dark halo on every focusable control, accessible name/role/value on every control (units included — `-12.00 dB`, not `-12.00`), each section exposed to screen readers as a named group without trapping Tab, focus order equal to the signal flow, and meters exposed as read-only value text. All of it is asserted headlessly against real `AccessibilityHandler`s.

### Changed

- `docs/architecture.md`'s module map now describes `src/gui` (the vector component family) instead of the placeholder `src/ui` row.

- **A generated editor preview** at `docs/gui-preview.png`, rendered by the GUI test suite from the real editor (`tests/gui/GuiPreviewSnapshotTests.cpp`) rather than drawn by hand, plus `docs/gui-mapping.md` listing which control drives which parameter.

### Notes

- WCAG contrast is enforced by tests against the same colour accessors the LookAndFeel paints with, so a palette tweak that drops a text pair below AA 4.5:1 fails the build rather than shipping.

## [0.3.1] - 2026-07-31

### Fixed

- **Serialised the IR loader against concurrent `prepare()` / `loadImpulseResponse()`** (`src/dsp/IRLoader.{h,cpp}`, #72). Both call into `juce::dsp::Convolution::loadImpulseResponse()`, whose background hand-off is documented safe from only one thread at a time, while `prepare()` runs on the host's `prepareToPlay()` thread and the public `CryptaAudioProcessor::loadImpulseResponse()` is reachable independently from another. A mutex serialising the two closes the race; it is never taken by `process()`, `reset()` or `setWetMixProportion()`, so the audio thread stays lock- and allocation-free. Stress-testing confirmed the race was live: an unfixed build reliably double-freed inside JUCE's convolution command storage on the first aggressive run, the fixed build survived 30/30 repeats.
- Added the missing allocation-guard self-test (`tests/RobustnessTests.cpp`), proving `AllocationCounter` actually detects an allocation — every existing `[realtime]` test only asserted *zero* allocations, so the guard itself was never shown to work.

## [0.3.0] - 2026-07-27

### Added (headline: the Circuit drive engine)

- **A second drive engine, `Drive Engine = Circuit`**, replacing the Mid and High bands' stock waveshapers with circuit-derived, antialiased stages (`src/dsp/CircuitDrive.{h,cpp}`). The two bands now share ONE oversampling region instead of two independent ones — the remainder is upsampled once, split by a Linkwitz-Riley crossover running at the oversampled rate, processed, summed and downsampled once. The saved cost pays for the extra per-voicing filtering at roughly the CPU v0.2.0 spent. The oversampling factor adapts to the host rate (4x below 50 kHz, 2x below 100 kHz, 1x above).
- **First-order antiderivative antialiasing** (`src/dsp/ADAAShaper.h`), after Parker, Zavalishin & Le Bivic (DAFx-16). Closed forms for tanh and hard clip; a cubic-interpolated table with a numerically integrated antiderivative for curves whose antiderivative is not elementary. Measured alias-to-signal improvement over the Classic engine: **25–30 dB** across a 1.2–10 kHz sweep at full drive, and **−80 dB or better through the bass register**.
- **Per-voicing circuit topologies.** *Gnaw* gains a pre-emphasis shelf and its exact algebraic inverse behind the clipper, so drive 0 collapses to unity structurally rather than approximately. *Wool* is now the asymmetric diode clipper's own DC curve, Newton-solved per table point at prepare time, plus a dynamic bias side chain that leaves the clipper offset for ~20 ms after a loud passage — history-dependent behaviour a memoryless waveshaper cannot produce. *Razor* gains the feedback clipper's unity-clean-plus-clipped-difference structure, with the guitar pedal's 720 Hz pre-emphasis corner moved to 330 Hz for the bass register. Gnaw and Razor share the drive-tracked post-clip pole.
- **`High Bias` (0–100 %)**, a continuous even-harmonic control: a DC offset into the High clipper, removed again by a 10 Hz blocker so it never reaches the output. 0 % is exactly the symmetric v0.2.0 character.
- **A log-domain RMS low-band detector, `Low Comp Detector = Smooth RMS`** (`src/dsp/LevelDetector.h`), with a soft knee, program-dependent release and auto-makeup. This fixes the low band's most audible v0.2.0 weakness: a peak detector with the sourced 6 ms release follows the half-cycles of a bass fundamental, so the gain reduction ripples and the low end tremolos. Measured ripple on an 80 Hz tone 6 dB over threshold: **over 1 dB → under 0.5 dB**. New controls: `Low Comp Knee` (0–18 dB), `Low Comp Auto Release`, `Low Comp Auto Makeup`.
- **A `Gate Mode = Modern` gate** (`src/dsp/GateEngine.{h,cpp}`) with hysteresis, retriggering hold, a detector-only sidechain highpass and a dB-linear release, running its control path per sample. New controls: `Gate Hysteresis` (0–12 dB), `Gate Hold` (0–500 ms), `Gate SC Highpass` (20–400 Hz), `Gate Range` (6–90 dB).
- **`Clip Ceiling` (−12–0 dBFS)** for the safety clip.
- **Metering backend** (`src/dsp/MeterTaps.h`, closes #13): lock-free input/output peak, per-band level, and low-comp and gate gain reduction, with a plain labelled readout row in the editor. The photoreal M3 GUI consumes the same struct later.
- Three factory presets showcasing the new engine: **Circuit Foundation**, **Circuit Grind**, **Circuit Knife**.

### Changed

- **Fresh instances boot into Circuit / Smooth RMS / Modern.** Existing work does not: every pre-v0.3.0 session and every pre-v0.3.0 preset has the legacy engines injected on load, through two independent migrations (see below). The eight factory presets voiced against the v0.2.0 DSP pin the Classic engines explicitly, so none of them changes character.
- **State schema v2.** Saved state now carries a `stateVersion` attribute; state without one has `driveEngine`, `lowCompDetector` and `gateMode` set to their legacy values on load.
- **Presets are migrated separately.** Presets never pass through `setStateInformation()`, and `applyParsedPreset()` resets to defaults before applying a preset's values — so a legacy preset would otherwise adopt the new engines. `PresetManager` gains a generic, version-gated legacy back-fill for this, empty by default so the rest of the suite is unaffected. The preset JSON schema is unchanged: this is a read-side default-fill, not a format change.
- The Circuit engine ramps its automatable scalars per sample rather than holding them constant across a block.
- Reported latency is now the maximum across both engines, with the Circuit path padded up to it, so switching drive engines never re-reports latency to the host. Reported latency depends on the sample rate alone.

### Fixed

- **The safety clip no longer aliases freely.** It was a raw per-sample `std::tanh` on the full mix; it is now an ADAA ceiling clip in delta form (`src/dsp/OutputClipper.h`), which is transparent below the ceiling instead of lowpassing everything whenever it is armed. Measured deviation across 40 Hz – 20 kHz: **0.13 dB**. With the clip engaged this is a deliberate, documented departure from v0.2.0's output.

### Known deviations from the v0.3.0 brief

Each is recorded in full at the relevant assertion in the test suite:

- The brief's flat **−80 dB alias floor** is not reachable for Gnaw, a 40x hard clip whose harmonics fall off as 1/n. Raising the engine to 8x oversampling was measured and still misses it. Delivered instead: 25–30 dB better than Classic against a 10 dB requirement, −80 dB or better through the bass register, and a −49 dB floor everywhere.
- The drive-tracked pole opens to **61 kHz** at drive 0, not the brief's 24 kHz — a one-pole at 24 kHz is already −1.9 dB at 18 kHz and could not meet the brief's own transparency requirement. 61 kHz is also the figure the research gives for the real circuit.
- **Engine parity** at drive 0 holds to ±0.5 dB up to 3 kHz. Above that the Circuit engine is up to 2.5 dB brighter, because its tone lowpass runs at the oversampled rate and so escapes the bilinear warping the base-rate Classic filter has.
- **Wool's sag has the opposite sign** to the brief's prediction: the probe blooms rather than dips, because the DC the bias creates dominates the slope change. History-dependence is confirmed and is 11 dB on Wool against 1 dB on the memoryless voicings. Flagged for the ear-tuning gate.
- The Circuit voicing constants remain **engineering-derived starting points**; the listening gates (#15/#16/#17, #34) still apply.

## [0.2.0] - 2026-07-16

### Changed (headline: 2-band → 3-band topology rebuild)

- **Restructured the signal path from a 2-band (low/high) split to a genuine 3-band (low/mid/high) split**, matching the reference class's own defining architecture ("bass, mids, and high frequencies are processed separately" - see `docs/design-brief.md`/`docs/research-notes.md` for the full sourcing). Two cascaded 4th-order Linkwitz-Riley crossovers replace v0.1.x's single split: `Split Low` (60–400 Hz, default 120 Hz, was `Crossover Frequency`) peels off the Low band; `Split High` (300–2000 Hz, default 600 Hz, NEW) further splits the remainder into Mid and High. `Split High` is always clamped at least 1/3 octave above `Split Low` (`src/dsp/SplitGap.h`) to prevent a degenerate near-zero-width Mid band.
- **Added a new Mid band** (`src/dsp/MidBand.h/.cpp`): staged/cascaded drive-only saturation (0-100%, default 30%) plus an independent output Level, no filter/tone/blend - matching the reference class's own Mid band exactly. Runs its own 4x-oversampled shaping stage.
- **Promoted the High band's pre-drive highpass ("Tight") from a Razor-only fixed 200 Hz internal constant to a first-class, voicing-independent control** (20-500 Hz, default 100 Hz), applied ahead of all three voicings (Gnaw/Wool/Razor) - closing the gap identified in the design brief's "Why v1 falls short" analysis.
- **Re-sourced the low-band parallel compressor's ballistics defaults** to the reference class's own fixed, documented "glue" bus-compressor values: ratio 4:1 → **2:1**, attack 10 ms → **3 ms**, release 120 ms → **6 ms** (release range floor lowered from 10 ms to 5 ms, a breaking pre-1.0 change, so the sourced default is reachable). Retires the v0.1.x manual's "New York style" framing, which characterized the wrong sub-genre of parallel compression for what the reference class actually implements - see `docs/research-notes.md` §3-4.
- **Relocated the IR loader (cab-sim convolution)** to process only the Mid+High post-sum signal, never the Low band - matching the reference class's "low band bypasses the cabsim" architecture. Structurally enforced (not just conventional) and directly tested (`tests/LowBandIsolationTests.cpp`): the Low band's own isolated output is bit-exact identical whether the IR loader is on or off, or which IR is loaded.
- **Re-anchored the post-sum 4-band EQ's default corner frequencies** to a sourced bass-tone-stack frequency set from the same design lineage as the reference class: Low Shelf 100 → **80 Hz**, Peak 2 2500 → **2800 Hz**, High Shelf 8000 → **5000 Hz** (Peak 1's existing 500 Hz default already matched the sourced anchor). Dormant-until-engaged (EQ ships off by default), so not an audible v0.1.x → v0.2.0 change unless a user or preset turns the EQ on.
- **Added a phase-alignment allpass filter for the Low band** (`src/dsp/PhaseAlignFilter.h`), required to make the new cascaded (not parallel) crossover topology actually flat-sum - discovered and fixed during this rebuild: a naive cascade of two independent LR4 crossovers does *not* flat-sum on its own (deviations up to −10 dB were measured at close `Split Low`/`Split High` ratios before this fix). The fix is proven algebraically, not just empirically - see `docs/architecture.md`'s "Cascaded 3-band flat-sum and phase alignment" section for the full derivation. This is a genuine engineering necessity this rebuild surfaced, not something anticipated by the original design brief.
- **Lossy state migration for v0.1.x sessions**: the old single `Crossover Frequency` (`crossoverFreq`) parameter is migrated to the new `Split High` parameter on load, clamped into its 300–2000 Hz range. v0.1.x's shipped default (250 Hz) sits below that floor, so the single most common migration path - an untouched v0.1.x session - lands exactly at the 300 Hz floor (dedicated regression test in `tests/StateMigrationTests.cpp`). `Split Low` and every new Mid-band/Tight parameter fall back to their v0.2.0 defaults; any low-band compressor values a user had explicitly changed away from v0.1.x's old defaults are preserved as-is.
- Docs rewritten to match: `docs/manual.md`, `docs/architecture.md`, `README.md` (signal-flow diagrams, parameter tables, feature list). `docs/design-brief.md` and `docs/research-notes.md` added (the binding brief and its sourcing for this rebuild).

### Added (M2 preset system)

- Suite-wide M2 preset system (`.scaffold/specs/preset-system-m2.md`), ported from `basilica-audio/nave`'s pilot implementation: `src/presets/PresetManager.{h,cpp}` (factory + user presets, save/save-as/delete/rename, default resolution, import/export of single presets and zip banks, dirty-state tracking) and `src/presets/PresetBar.{h,cpp}` (a horizontal preset strip docked at the top of the editor).
- Nine factory presets (`presets/factory/*.json`, category `Init`/`Bass`) - see `docs/presets.md` for what each demonstrates: **Default**, **Glue & Grind**, **Sub Lock**, **Throat**, **Fuzz Wall**, **Cut Through**, **Definition Only**, **Clean Low, Loud Top**, **Cab-Colored Grind**.
- German localisation of the preset UI frame (`resources/i18n/de.txt`), auto-selected via `SystemStats::getUserLanguage()`. Core/DSP terminology (parameter names, units) is never translated.
- User presets stored at `~/Library/Audio/Presets/Yves Vogl/Crypta/` (macOS) / `%APPDATA%\Yves Vogl\Crypta\Presets\` (Windows).

### Fixed

- Discovered and fixed during the topology rebuild: a naive cascade of two independent LR4 crossovers does not flat-sum (see the phase-alignment entry above) - this was never an issue in v0.1.x's single-crossover topology, so it is not a regression from any prior release, but a new correctness requirement introduced by the 3-band rebuild itself.

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
