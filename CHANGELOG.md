# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **A fleet-comparable factory-preset headroom gate** (`tests/PresetHeadroomTests.cpp`),
  alongside the existing bass-DI gate in `tests/ListeningProxyTests.cpp` rather than replacing
  it. The DI is the right reference for what Crypta is *for*; the suite reference programme
  (four plucked notes spanning E1 41.203 Hz to A5 880.000 Hz, twelve harmonics each,
  peak-normalised to −12 dBFS) is the one every other plugin in the suite is measured on, and
  it is deliberately broader-band — a harsher test for anything with high-band drive. Both are
  now gated at 0 dBFS.

  It also measures **both** ways a user arrives at a preset — a restored session and a
  mid-session click in the preset browser — the latter held to "below full scale **or** below
  where you already were", so a transition is blamed only for clipping it introduced.

### Fixed

- **Six factory presets cleared the bass DI while pushing the suite reference programme past
  0 dBFS** — including `Default`, which is also the fresh-instance state. The `outputGain`
  trims from the issue #34 item 1 fix were derived against the DI alone, and the DI does not
  see what the high-band drive does to material an octave and more above it:

  | Preset | On the suite reference | Trim | Now |
  |---|---:|---:|---:|
  | Throat | +2.44 dBFS | −2.74 dB (−1.63 → −4.37) | −0.30 dBFS |
  | Cab-Colored Grind | +1.81 dBFS | −2.12 dB (−1.07 → −3.19) | −0.31 dBFS |
  | Cut Through | +0.82 dBFS | −1.13 dB (0 → −1.13) | −0.31 dBFS |
  | Fuzz Wall | +0.45 dBFS | −0.75 dB (−3.16 → −3.91) | −0.30 dBFS |
  | Glue & Grind | +0.40 dBFS | −0.71 dB (−2.25 → −2.96) | −0.31 dBFS |
  | Default | +0.16 dBFS | −0.46 dB (−2.80 → −3.26) | −0.30 dBFS |

  Three more sat inside the 0.3 dB drift margin and were brought back onto it (`Circuit Knife`
  −0.14 → −0.31, `Clean Low, Loud Top` −0.24 → −0.31, `Definition Only` −0.10 → −0.31 dBFS), so
  the whole shipped set holds the full margin the gate reserves. The three already at or below
  the target (`Circuit Foundation` −2.29, `Circuit Grind` −0.47, `Sub Lock` −1.18 dBFS) are
  **not raised** — that would be level-matching, which stays a taste item in #34.

  Every preset still clears the bass DI, with more room than before: −0.31 to −6.25 dBFS.

  **A fresh instance is 0.46 dB quieter, on purpose** — the constructor resolves the factory
  `Default` preset as the startup state, so `Default`'s trim is also the fresh-instance gain
  staging, and a fresh instance rendering the suite reference at +0.16 dBFS was the same defect.
  The `outputGain` *parameter default* stays 0 dB.
### Fixed

- **The x86_64 AU validation gate was testing a transport the plugin never ships into**, and
  reporting that transport's flakiness as a plugin defect (basilica-audio/Crypta#123). The
  v0.4.1 release run died with `Trace/BPT trap: 5` inside pluginval's "Parameter thread
  safety" test against the shipped Universal Binary's x86_64 AU slice, while the same slice's
  VST3 passed the identical strictness-10 sweep 63 s earlier and arm64 passed everything.
  - **Root cause, measured rather than inferred.** `.github/scripts/validate-macos-x86_64-slice.sh`
    pins the slice by installing an `lipo -thin x86_64` copy of the shipped component, which
    is correct and stays. But it then ran the validator *natively* (arm64). An arm64 host
    cannot load an x86_64-only AU into its own address space, so AudioToolbox hosted it
    out-of-process in `AUHostingServiceXPC` and bridged every parameter set, property set and
    render call across XPC. Sampled mid-run, that configuration shows a live
    `AUHostingServiceXPC` process and no mapping of the component in the validator; running
    the validator under `arch -x86_64` instead shows the component's `__TEXT`/`__DATA_CONST`/
    `__LINKEDIT` mapped into the validator itself and no hosting service at all.
  - **Neither an Intel Mac nor an Apple Silicon Mac is ever in that state.** Intel runs an
    x86_64 host against the x86_64 slice; Apple Silicon runs an arm64 host against the arm64
    slice. Both are arch-matched, both in-process. The XPC configuration existed only as a
    side effect of thinning the bundle to pin the slice.
  - **And it is not a neutral side effect.** On the same byte-identical x86_64 bundle: 5/5
    SUCCESS in-process, against 4 failures in 14 runs out-of-process, the failures landing in
    the tests that read a parameter back after writing it across the bridge.
  - **The fix keeps the gate exactly as strict.** Strictness stays at 10, the AU stays in the
    sweep, and the thinned bundle is still the only thing that pins the slice — the AU
    pluginval pass and `auval -strict` now run under `arch -x86_64` so the hosting mode
    matches what ships. The step additionally watches for a live `AUHostingServiceXPC`
    process throughout the AU pass and fails if one appears, so the gate cannot silently
    drift back onto the bridge.

### Added

- **`tests/ConcurrentParameterTests.cpp`** — standing in-repo coverage for the property
  pluginval's "Parameter thread safety" test probes: every non-bypass automatable parameter
  written from two threads at once (`setValueNotifyingHost` and `setValue`) while
  `processBlock()` renders, asserting no non-finite output and no runaway magnitude, plus an
  exact APVTS state round-trip after a concurrent write storm (basilica-audio/Crypta#123).
  The repository had cross-thread coverage for the IR loader (`CrossThreadReprepareTests.cpp`)
  and none for the parameter path.

## [0.4.1] — 2026-08-27

### Added

- **Releases now ship a macOS `.pkg` installer alongside the `.zip`**, with its own `.sha256`
  like every other asset (basilica-audio/Crypta#31). It offers three independently selectable
  choices — AU into `/Library/Audio/Plug-Ins/Components`, VST3 into
  `/Library/Audio/Plug-Ins/VST3`, standalone into `/Applications` — with relocation switched
  off on each, so an install cannot follow an older copy of `com.yvesvogl.crypta` somewhere
  else on disk. Assembly moved out of `release.yml` into
  `.github/scripts/build-macos-pkg.sh` so the shipping code path can be run locally against
  real artefacts; the script self-checks every build, signed or not, asserting each
  component's `install-location`, an empty `<relocate>`, the expected payload bundle and three
  install choices.
  - **The installer ships unsigned for now, and its filename says so**
    (`crypta-vX.Y.Z-macos-unsigned.pkg`). Signing a `.pkg` needs a *Developer ID Installer*
    certificate, which is a different Apple certificate type from the *Developer ID
    Application* certificate the bundles are signed with — `productsign` rejects the
    application identity outright. This is **not** the same as shipping unsigned plugins:
    every bundle inside the payload keeps its own Developer ID signature and stapled
    notarization ticket, so once installed the plugins are Gatekeeper-clean and load without a
    warning; only the wrapper costs the user one right-click → **Open**. The workflow switches
    to signing, notarizing and stapling the `.pkg` automatically once
    `APPLE_INSTALLER_CERT_P12` exists at org level — no code change needed.
  - **Signature survival through packaging is measured, not assumed.** Against the shipped,
    notarized, stapled `v0.4.0` artefacts, every regular file in all three bundles hashes
    identically before packaging and after `pkgutil --expand-full` of the resulting `.pkg`,
    and `verify-macos-signing.sh` reports 9/9 `[PASS]` on the bundles extracted back out of
    it. A real `installer -pkg … -target /` system install remains unverified (it needs root);
    `docs/building.md` says so explicitly rather than implying otherwise.

### Fixed

- **`verify-macos-signing.sh` no longer fails the release on correctly notarized plugin
  bundles.** The `spctl` gate assessed everything with `--type execute`, which asks "may this
  be launched as an application?" — a question a `.component` or `.vst3` can never answer yes
  to, so Gatekeeper returned `rejected (the code is valid but does not seem to be an app)`
  regardless of how impeccably the bundle was signed and notarized. The gate had landed after
  `v0.4.0` was cut and so had never run in a release; the next tag would have gone red on
  artefacts that were perfectly fine. Plugin bundles and `.pkg` files are now assessed with
  `--type install` (the shipped `v0.4.0` AU and VST3 report
  `accepted source=Notarized Developer ID` under it), `--type execute` is kept for the
  standalone `.app`.

### Changed

- **The suite now presents itself as Basilica Audio in every host.** `COMPANY_NAME` moves from
  `Yves Vogl` to `Basilica Audio`, so Crypta files under the brand in Logic's plugin manager,
  Cubase's vendor column and Reaper's FX browser instead of under a person's name. **Plugin
  identity is untouched** and no session is affected: the VST3 class ID derives from
  `PLUGIN_MANUFACTURER_CODE` + `PLUGIN_CODE` alone (JUCE 8.0.14, `juce_VST3ModuleInfo.h`'s
  `VST3Interface::jucePluginId`) and the Audio Unit triple stays `(aufx, <PLUGIN_CODE>, Yvsv)` -
  both diffed on a real build before and after the change. The bundle ID stays
  `com.yvesvogl.crypta` on purpose, because changing it is what would break existing projects, and
  `COMPANY_COPYRIGHT` still names the copyright holder rather than the trading name. See
  [`docs/branding.md`](docs/branding.md) and basilica-audio/.github ADR 0001.
- **User presets now live under `Basilica Audio`, and the ones you already saved come with them.**
  The folder moves to `~/Library/Audio/Presets/Basilica Audio/Crypta/` (macOS) and
  `%APPDATA%\Basilica Audio\Crypta\Presets\` (Windows). On first launch `PresetManager` copies
  every preset out of the old `Yves Vogl` folder into the new one. It **copies rather than moves**,
  so an older build - or a downgrade - still finds its presets where it left them, and it never
  overwrites a file already present under the new name. Nothing is deleted, ever.
- **Plugin metadata now carries the vendor URL, the copyright string, a real description and
  the VST3 sub-category.** `COMPANY_WEBSITE`, `COMPANY_COPYRIGHT` and `DESCRIPTION` were never
  set, so a shipped bundle carried an empty `NSHumanReadableCopyright`, an empty VST3 vendor
  URL, and an AU `description` that was just the plugin name again; `VST3_CATEGORIES` fell back
  to JUCE's bare `Fx` default, which filed every plugin in the suite under the same
  undifferentiated heading in a VST3 host's browser. Crypta now declares
  `Fx Distortion Dynamics` (JUCE 8.0.14, `juce_add_plugin`). **Plugin identity is unchanged** — the VST3 class
  ID is derived from `PLUGIN_MANUFACTURER_CODE` + `PLUGIN_CODE` alone
  (`juce_VST3ModuleInfo.h`'s `VST3Interface::jucePluginId`) and the AU type/subtype/manufacturer
  triple is untouched, so existing sessions keep resolving to the same plugin.

### Fixed

- **Release notes are the changelog again, not a list of PR titles.** `release.yml` now builds the
  release body from this file's section for the tag being released, via the suite-wide
  `basilica-audio/.github/release-notes` action, and appends what a downloader actually needs: what
  each archive contains, the signing status per platform stated accurately (macOS signed, notarised
  and stapled; Windows **not** code-signed, so SmartScreen will warn), the install paths, the AU
  rescan hint, and links to the manual and the product page. A tag whose version has no section in
  this file now fails the release job rather than publishing an empty page.
- **`docs/presets.md` states the true factory-preset total (12)** and names the three
  presets it does not describe yet, instead of silently claiming a smaller number
  (basilica-audio/Crypta#117).

### Added

- **A `Documentation` section in the README** pointing at the user manual, the factory-preset
  reference, the changelog and the product page — the manual was only reachable from a
  sentence in the middle of the Signal flow section.

### Added (presets recall their cabinet: bundled-IR resolution, issue #111)

- **Presets may now carry an optional IR reference — the SHA-256 of the impulse-response file they were voiced with — and loading one puts that cabinet back in the slot.** The model is basilica-audio/Nave#45's, adopted as Crypta's own change: resolution is **by content hash only** (never by name or id, so a retuned model misses loudly instead of silently recalling a different sound), the user's IR library folder (`~/Music/Crypta/Impulse Responses`) is consulted **first** and the embedded bundle **second**, and the lookup is **total** — every digest lands on exactly one of notReferenced / alreadyLoaded / library / bundled / notFound. A miss performs **no audio operation at all**: the preset's parameters load, the slot keeps what it had, nothing is substituted, and a notice names the missing cabinet (`CryptaAudioProcessor::getPresetIrNotice()`; GUI wiring is a follow-up).
  - **There is still exactly one decode path.** A bundled reference materialises the embedded bytes to a real file (`src/ir/BundledIrSource.h`, via the same idempotent install/repair writer `basilica::ir::FactoryIrLibrary` provides) and loads it through `CryptaAudioProcessor::loadImpulseResponseFromFile()`, which funnels into the same `cryp::FactoryIRLibrary::decodeFromMemory()` every embedded factory slot uses — so the two resolution sources are sample-identical by construction, and `tests/BundledIrResolutionTests.cpp` pins it anyway.
  - **The four bundled cabinets now carry their manifest ids as stable identities** (`bass-810-cone`, `bass-810-edge`, `bass-115-vintage`, `bass-410-horn`) — identity for documentation and release notes, **never** a resolution key. Release rules adopted with them (`resources/irs/LICENSES.md`): rename freely, never retune in place, removal is a major-version break; `tests/BundledIrCurationTests.cpp` pins the shipped set, the manifest match, the provenance embedding and the compiled-in footprint (73,624 bytes).
  - **`Cab-Colored Grind` now references the Modelled 8x10 Cone** it was voiced around, so it colors the Mid+High path out of the box instead of running the loader as an identity passthrough until the user loads an IR by hand.
  - **`.gitattributes` marks all embedded assets `-text`** (Nave#48's lesson): Git's default line-ending conversion on Windows would otherwise ship different provenance/preset bytes than the committed ones, which the footprint assertion turns into a hard failure instead of a silent platform difference.
  - **Saving records what is up.** A preset saved with a factory or file-loaded IR in the slot records its digest (factory loads hash the embedded file bytes; file loads hash the very bytes that were decoded); saving from the identity-passthrough default writes no reference, byte-identical to the pre-#111 saver. Renaming a user preset copies every property of the original document instead of re-stamping live state, so a rename can no longer silently rewrite a preset's parameters or drop its reference.

### Fixed (factory presets no longer clip a nominally tracked DI, issue #34 item 1)

- **Eight of the twelve factory presets pushed a −12 dBFS bass DI past 0 dBFS — `Default` among them at +2.49 dBFS, worst `Clean Low, Loud Top` at +3.52 dBFS.** A −12 dBFS peak is the level a bass DI is conventionally tracked at, i.e. the level a factory preset's author must be assumed to have voiced for; a preset that clips at that level asks the user to pull the fader before they can audition it, which is a defect and not gain-staging taste.
  - **Each offending preset gets a derived `outputGain` trim in `presets/factory/*.json` — nothing else in any preset changes.** The trim is exactly the measured overshoot plus a −0.3 dBFS headroom target, rounded to the parameter's 0.01 dB step: Cab-Colored Grind −1.07, Circuit Grind −1.07, Circuit Knife −1.87, Clean Low, Loud Top −3.82, Default −2.80, Fuzz Wall −3.16, Glue & Grind −2.25, Throat −1.63 dB. `outputGain` is the last stage in the chain, so the trim scales the render exactly linearly and every preset's internal voicing is untouched; the four presets already under the target (Circuit Foundation, Cut Through, Definition Only, Sub Lock) are not raised, because that would be level-matching — which stays a recorded taste item in issue #34, deliberately not decided here.
  - **The measurement is now a gate.** `tests/ListeningProxyTests.cpp`'s factory-preset case asserts every factory preset renders the −12 dBFS reference DI to a peak **below 0 dBFS** (the shipped trims target −0.3 dBFS, leaving 0.3 dB between "the voicing drifted" and "the gate goes red"), so a future preset cannot reintroduce clipping. All twelve now measure −0.30 to −3.79 dBFS on that fixture.
  - **A fresh instance is 2.8 dB quieter than before, on purpose.** The constructor resolves the factory `Default` preset as the startup state (`PresetManager::applyStartupDefault()`), so `Default`'s trim is also the fresh-instance gain staging — which is the point: a fresh instance on a nominally tracked DI clipped at +2.49 dBFS, and that was the same defect. The `outputGain` *parameter default* stays 0 dB (it is what resetting the control gives); the neutral-chain identity tests (`GainStagingTests`, `GainProcessingTests`, `LatencyTests`, `MeterTapsTests`) now pin the trim to 0 dB explicitly instead of inheriting it from the startup preset, the same way they already pin the voicing stages dry.

### Fixed (High Bias no longer thumps on state restore, issue #34 item 4)

- **A fresh instance whose saved state already had `High Bias` at 100 % emitted a 0.137-peak (−17 dBFS) DC transient into silence while the 10 Hz blocker settled — a thump on every session load or preset recall of such a state.** The blocker's steady state was already proven clean (4.2e-14 of DC); the settling was the defect.
  - **The bias offset's own image through the shaper is now subtracted at the point of creation** — `f(offset)` for Gnaw and Razor in `src/dsp/CircuitDrive.cpp`, the same construction Wool and `cryp::LowGrowl` (`tanh(g·x + b) − tanh(b)`) already used — so the DC blocker's operating point is zero at every bias setting and there is nothing to settle, on prepare, on transport reset, on engine switch, on mid-playback restore and on bias automation alike. What the blocker still removes is the programme-dependent DC that asymmetric clipping of real signal produces, which no static subtraction can know. Steady-state audio is unchanged: the blocker is linear, so removing a constant upstream of it differs from letting it settle by exactly the decaying transient being fixed.
  - **`CircuitDrive::reset()` additionally primes each high-band shaper's one-sample ADAA history at the bias operating point** (`ADAAState::prime()`, the same prime-before-first-block idea as issue #98's gain stages) — without it the first sample after a reset is the ADAA quotient averaged over [0, offset], half the offset as an impulse into the chain.
  - **`tests/SilenceFloorTests.cpp` gates both regimes.** Session-load order (restore, then prepare, then silence) is held to the file's 2⁻²⁴ silence floor *including* the settling transient, on all three voicings — measured **exactly 0.0**. A mid-playback restore ramps the offset across one block, so what is left is the ADAA midpoint-vs-endpoint mismatch, bounded by `Lmax·Δ/2` per sample scaled by the worst downstream gain with a 4x margin — **1.042e-3 derived, 1.1e-4–1.9e-4 measured**, two orders of magnitude under the 0.137 defect. The existing knob-gesture case (`High Bias` turned up after prepare) is gated by the same derived ramp bound at a measured 1.46e-4.

### Added (the v1.0.0 listening gate becomes an executable measurement gate, issue #34)

- **`docs/qa-checklist.md` Part 2 said seven of its items had "no pass/fail criterion" and could only be approved by ear. Four of them now have one.** The distinction that unlocked it is between the *preference* a listening pass expresses and the *defect* it is listening for: whether Gnaw is good is taste, but whether the Smooth RMS detector breathes, whether the Modern gate chatters, whether the drive control doubles as a fader and whether the bundled cabinets are shaped like bass cabinets are all measurable — and every bound below comes from published perceptual data, from loudspeaker physics, or from the arithmetic of the signal, never from what this plugin happens to measure.
  - **`tests/ListeningProxyTests.cpp` (`[listening]`).** Smooth RMS tremolo: **0.00047 dB peak-to-peak in the 2–15 Hz band at 41.2 Hz** and 0.00018 dB at 55 Hz, against a 0.5 dB bound derived from the AM modulation-depth threshold, which is at its minimum near 4 Hz at a modulation index of 0.03–0.05 on a complex tone (Zwicker & Fastl, *Psychoacoustics*, ch. 10) — 0.87 dB peak-to-peak at the very best case. Modern gate on an eight-note chug train whose amplitude between onsets only ever decreases: **8 openings for 8 onsets**, which is not a tolerance but the only correct answer, and **0.174 dB of peak loss** on notes 2–8 (0.230 dB on the first, from a fully closed gate) against a 0.5 dB bound. Voicings: every one of the six engine/voicing pairs raises level with drive rather than lowering it. Cabinets: **−10 dB bands of 32–4699 Hz, 34–2172 Hz, 34–1804 Hz and 37–3079 Hz**, all four low edges inside the [25, 70] Hz window a cabinet that has to reproduce a 41 Hz low E must sit in, the three hornless models inside the [1.2 kHz, 6 kHz] window voice-coil inductance puts them in, and the horn model **12.2 dB above every hornless model at 8 kHz**.
  - **Three findings are recorded rather than gated**, because gating them would be the test suite deciding a voicing question: eight of the twelve factory presets push a −12 dBFS bass DI past 0 dBFS (`Default` at +2.49 dBFS, worst `Clean Low, Loud Top` at +3.52 dBFS); the three voicings are not level-matched (**3.97 dB spread on Classic, 3.68 dB on Circuit** at 70 % drive); and the 4x10 Horn's −10 dB band edge is 3079 Hz because its model runs the horn path 6 dB down. Each is a one-line change if wanted.

- **Five further measurement gates, covering the parts of the v1.0.0 signal-path contract that had a robustness test but not a *measurement*.**
  - **`tests/SilenceFloorTests.cpp` (`[silence]`) — silence in, silence out.** The bound is **2⁻²⁴ = 5.96e-08**, half a 24-bit LSB: the largest value that always quantises to zero, so anything below it is *provably* absent from a 24-bit render rather than merely inaudible. A fresh instance on either engine measures **exactly 0.0** for peak, DC and RMS; every stage engaged measures **1.4e-37 peak / 6.8e-38 DC**; and with `highBias` at 100 % — the one stage documented to add DC on purpose — the steady-state DC is **4.2e-14** after the 10 Hz blocker, seven orders of magnitude under the bound. Also asserted on the ring-down after a loud passage, with a factory IR loaded and the gate deliberately off so a closing gate cannot mask the tail. Measured on the way past, and reported rather than asserted: a fresh instance whose state already has `High Bias` at 100 % emits a **0.137-peak (−17 dBFS)** DC transient into silence while that blocker settles.
  - **`tests/ContinuousSweepTests.cpp` (`[continuous-sweep]`) — a knob gesture, not a knob jump.** `ParameterSweepTests.cpp` snaps a parameter to an endpoint once, which is the right test for a preset recall and the wrong one for a hand on a knob: an unsmoothed parameter produces *one* step under a snap and one per block boundary under a gesture, which at a 64-sample block is a 750 Hz tone rather than a click. Each of the 42 continuous parameters is swept across its full range in 267 ms and compared against nine static renders of itself, the bound being 1.5x the worst static step — 1.0 for grid coarseness plus room for the legitimate per-block increment. Worst measured: **1.016x**, on `splitHighHz`. Nothing in the parameter set is missing its smoothing.
  - **`tests/StateRecallRenderTests.cpp` (`[recall-render]`) — the state round-trip measured on the audio.** Comparing parameter values across a save/load cannot see a cached coefficient that did not follow, and is usually asserted with a tolerance a float round-trip does not need. This renders instead and compares with `==`: **0 differing samples across 132 configurations** — every parameter at both edges, all-minimum, all-maximum and 20 seeded random whole-set configurations. The two restore scenarios are separated on purpose, because only one of them can be bit-exact: reopening a project (a fresh instance, nothing in flight) is held to bit-identity, while a mid-session restore into a running instance legitimately crossfades — #87/PR #90 made that a feature — and is held instead to landing on the right destination, measured at **0.000 dB of RMS deviation** on all four configurations tried. The transient of that crossfade is printed rather than gated (ratios of 1.87x–2.22x against the loudest steady state involved for realistic states, **21.6x for the all-maximum one**, which is the already-documented unsmoothed `LinkwitzRileyFilter` coefficient update scaled by the +60 dB of gain that same state also restores) because no bound on 54 simultaneous parameter moves follows from anything.
  - **`tests/SampleRateInvarianceTests.cpp` (`[invariance]`) — the same musical result at 44.1/48/88.2/96/192 kHz.** Everything measured is a ratio of output to input through the same transform, so the window scale and transform length cancel and rates are directly comparable; the transform length is picked per rate to hold the analysis bin width at 1.35–1.47 Hz. Magnitude response 55 Hz – 1760 Hz: worst deviation from 48 kHz **0.052 dB**, against a 0.5 dB bound derived from bilinear frequency warping (`tan(πf/fs)/(πf/fs)` = 1.00524 at 1760 Hz / 44.1 kHz, through the chain's steepest 24 dB/octave slope = 0.18 dB, with ~2.6x in hand). Harmonic profile of both engines and all three voicings: worst **0.078 dB** against 1.5 dB. Low-band compressor gain reduction sampled 30 ms after a step, both detectors: worst **0.030 dB** against 0.5 dB — the ballistics are milliseconds, not sample counts.
  - **`tests/AdversarialInputTests.cpp` (`[adversarial]`) — a hostile signal against a fixed configuration**, which is the axis a parameter fuzz cannot reach. Full-scale sine, ±1.0 DC, a full-scale impulse, digital silence, alternating ±1 at Nyquist, denormal-range noise and a full-scale 20 Hz – 20 kHz chirp, on both engines, each for two seconds so the gate, the compressor release and every oversampling and convolution history is made entirely of that signal; the block size changing every callback across `{0, 1, 7, 64, 33, 512, 1024, 2048, 3, 128, 0, 4096}`; the sample rate changing mid-stream thirty times with no `reset()` and the reported latency re-checked after each; and a NaN/Inf-poisoned block, after which the plugin must return to within **0.1 dB** of the level it had before — a chain still holding a NaN in a filter state would emit NaN or silence forever, and both are distinguishable from a correct render.

- **`scripts/qa-gates.tsv` gains six rows**, so `scripts/qa-gate.sh` runs all of the above and records what each measured. The manifest's own guard (`tests/QaGateManifestTests.cpp`) covers them like every other gate: a filter that stops matching a real tag fails on every pull request rather than on release day.

- **Verified on both architectures.** The whole set was run on arm64 and on the macOS x86_64 slice under Rosetta (the same mechanism `.github/scripts/run-macos-x86_64-slice.sh` uses, issue #104): **279 test cases and 161733 assertions green on arm64**, and `[silence]`, `[denormals]`, `[continuous-sweep]`, `[recall-render]`, `[invariance]`, `[adversarial]`, `[offline-realtime]` and `[chunking]` green on x86_64. The #99 denormal story specifically: **0 denormal samples** in a six-second decay to silence across three block sizes and both engines, on the x86_64 slice — which is the slice where `JUCE_SNAP_TO_ZERO` would have been active and where `juce::ScopedNoDenormals` has to carry the load alone.


### Added (CI runs the macOS Universal Binary's x86_64 slice, issue #104)

- **The x86_64 half of every macOS build was compiled, linked, uploaded inside the artefact — and never executed.** `ci.yml` configures macOS with `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`, so `Tests` is a Universal Binary; `ctest` then runs it and macOS selects the slice matching the host, which on GitHub's Apple Silicon runners is always arm64. The suite had therefore never run against the macOS x86_64 code path on any commit. It now does, as a step in the existing macOS job: `.github/scripts/run-macos-x86_64-slice.sh` runs `arch -x86_64 build/Tests` under Rosetta.
  - **The whole suite, on every pull request, for 2.95 min.** 257 test cases and 159051 assertions on the x86_64 slice, measured on the runner at 169 s of suite time inside a 2.95 min step, against 3.07 min for the same suite on the arm64 slice through `ctest`. A narrower trigger — `main` only, nightly, or paths under `src/dsp/` — was considered and rejected on that number rather than on preference: it is +24 % of a 12.4 min job, and a selection narrow enough to matter would have had to guess which tests are architecture-sensitive. Both defects this closes the gap on were found in `GoldenRenderTests` and `OfflineRealtimeNullTests`, neither of which a "core DSP tests only" selection would obviously have contained. One process rather than `catch_discover_tests`' one-per-case, because ~250 Rosetta process starts cost more than the tests do.
  - **A step in the existing macOS job rather than a job of its own, and not a `macos-*-intel` runner.** The object code already exists in that job, so the added cost is the test runtime alone; a separate Intel runner would add a second checkout, CPM restore and ~8 min Universal build to buy the same assertions, on the most expensive minute in the fleet. Genuine Intel *silicon* remains covered by `windows-latest`, which runs the whole suite on real x86_64 hardware and is what proves #99's fix; what Rosetta adds is the Apple-clang-on-Intel combination that the shipped Universal Binary actually contains, and which nothing ran.
  - **It asserts that it is running what it claims to be running.** The binary must really contain an x86_64 slice (naming `CMAKE_OSX_ARCHITECTURES` if not); Rosetta must be present, and is installed if a future runner image stops shipping it — it is present on `macos-latest` today; the child process must report `uname -m=x86_64` and `sysctl.proc_translated=1`, so this can never quietly degrade into a second arm64 run wearing an x86_64 label; and both slices must register the identical set of test cases before either runs, because a `TEST_CASE` behind an `#if` on the architecture simply would not exist in this run and "All tests passed" would be printed anyway. Assertion counts are deliberately *not* compared — they legitimately differ where a case takes a different branch per architecture, which is exactly what the pre-#102 tree does (158737 on arm64 against 158765 on x86_64).
  - **Demonstrated against the code the gap hid, on CI, rather than asserted.** Three runs, same workflow:
    - **The tree as it was before PR #102** (35a6e37, whose CI run was green at the time) with only this step added: `Test (macOS, arm64 slice)` **passes** — 253 cases, 158737 assertions, which is precisely what CI reported on the day — and `Test (macOS, x86_64 slice under Rosetta)` **fails** with 3 assertions in `GoldenRenderTests`, at −80.75 / −73.07 / −80.39 dB relative to the golden fixtures. That is issue #100, caught by the job instead of by someone thinking to build under Rosetta by hand.
    - **Current `main` with `JUCE_DSP_ENABLE_SNAP_TO_ZERO` put back to `1`** — #99's defect under today's test contract — over `[offline-realtime]`: the arm64 slice fails only the flag's own sentinel case, because `JUCE_SNAP_TO_ZERO` is `#if JUCE_INTEL` and the guard is a no-op there, while the x86_64 slice fails **17 assertions across 16 block-schedule configurations**, worst case −77.89 dB null at 8.9967e-04 peak, on both engines, at 44.1 and 48 kHz, for 512-vs-32, 512-vs-441 and the ragged 1…512 schedule. That is the block-size dependence, caught.
    - **Current `main`, unmodified:** 257 cases and 159051 assertions green on both slices.

### Added (pluginval and auval now validate the x86_64 slice too, issue #108)

- **The Intel slice had tests but no plugin-format validation.** PR #106 put the whole suite under Rosetta, so `ctest` covers both slices; `pluginval --strictness-level 10` and `auval` still exercised the arm64 slice only. Those catch a different class — `ctest` asserts DSP behaviour, `pluginval` asserts format conformance under hostile host behaviour: parameter thrashing, out-of-order calls, odd block sizes, editor open/close cycles. `.github/scripts/validate-macos-x86_64-slice.sh` closes that half.
  - **The mechanism was established empirically, and it is not the one #104 anticipated.** #104 expected that neither tool could be wrapped in `arch -x86_64`, because both go through host plugin-scanning and the host picks the architecture. That is half right, and the wrong half matters more than the right one.
    - **`pluginval`'s CLI validates in-process, so `arch -x86_64` does steer the VST3.** A run shows exactly one `pluginval` pid and no child — `Validator.cpp` does contain a `ChildProcessValidator`, but `CommandLine.cpp` takes `ValidationType::inProcess`, so the child-process path is the library API rather than the CLI one. The process that `dlopen()`s the VST3 is therefore the process the step launches. Against an x86_64-only bundle: native arm64 gives `Unable to load VST-3 plug-in file` and `*** FAILED`; `arch -x86_64` gives `SUCCESS`.
    - **`auval` cannot be steered with `arch` at all, and neither can `pluginval`'s AU path — for a structural reason.** An AUv2 `.component` is never loaded into the host's address space; macOS hosts it out-of-process in an XPC service, and ships one per architecture — `AUHostingServiceXPC.xpc` is **x86_64**-only and `AUHostingServiceXPC_arrow.xpc` is **arm64e**-only (`/System/Library/Frameworks/AudioToolbox.framework/XPCServices/`). The system selects the one matching the *component's* slices, and the caller's own architecture is irrelevant. Measured against an x86_64-only component, **all three** of native `auval`, `arch -x86_64 auval` and native `pluginval` report success. So `arch -x86_64 auval` is not a control — it looks like one, which is worse than having none, and is precisely the "appears to validate Intel while silently validating arm64" outcome #108 asked not to ship.
  - **The artefact is the steering wheel, not the tool.** What both formats do respect is which slices the bundle contains, so the step validates an x86_64-*thinned* copy: `lipo -thin x86_64` of the same Mach-O the Universal Binary ships, ad-hoc re-signed because thinning invalidates the linker's signature. `lipo` extracts rather than recompiles, so the object code under test is byte-for-byte the code that ships — and there is no second build to pay for. A separate x86_64-only *build*, the third option #108 raised, would buy the same bytes for another ~9 min on the most expensive minute in the fleet.
  - **That choice is what makes the gate self-verifying, which asserting on `arch` never could.** A bundle containing only x86_64 machine code cannot be validated as arm64 by `pluginval`, by `auval`, or by the XPC hosting service. If a future macOS turned `arch -x86_64` into a no-op, the VST3 pass would fail loudly with `Unable to load VST-3 plug-in file` instead of going quietly green on the wrong slice. The step additionally asserts Rosetta is present (installing it if an image stops shipping it), that a translated child really reports `uname -m=x86_64` and `sysctl.proc_translated=1`, that each shipped bundle contained an x86_64 slice to begin with, and that each thinned bundle reports **exactly** `x86_64` and nothing else — refusing to run rather than validating an ambiguous artefact. `auval`'s own verdict line is required in addition to its exit code, so a truncated run cannot read as a pass.
  - **3 min 26 s on the runner, and it runs on `push` and `workflow_dispatch` rather than on every pull request.** Measured in [run 33026731255](https://github.com/basilica-audio/Crypta/actions/runs/33026731255): `pluginval` VST3 **63 s**, `pluginval` AU **139 s**, `auval` **3 s**, 205 s of work inside a 206 s step. The AU dominates because it runs translated inside the x86_64 hosting service; the same validation costs 25 s natively. The trigger follows from that number rather than from preference, and the arithmetic runs the opposite way to intuition: over the last 30 days this repository saw 60 `pull_request` and 28 `push` CI runs, so at a 10× multiplier every-PR would cost ~3020 runner-minutes a month against ~960 for push-only — while a *nightly workflow of its own* would cost ~3870, because it would have to rebuild the Universal Binary it validates. **A separate scheduled workflow is both the most expensive option and the latest-reporting one.** Keeping the step inside the existing macOS job, where the Universal Binary already exists, is what makes it affordable at all. The every-PR gates are unchanged: `pluginval` at strictness 10 on the arm64 slice, `auval`, and the full suite on both slices.
  - **Demonstrated failing as a job on CI, not asserted.** A throwaway branch adds one Intel-only defect to `processBlock()` — a `#if JUCE_INTEL` "denormal squash" that snaps anything below the subnormal threshold to zero by dividing out its own magnitude, and forgets that a genuinely silent sample is below that threshold too, so `0.0f / 0.0f` is `NaN`. That is the shape #108 names: a divergence living behind `JUCE_INTEL`, invisible to every arm64 job in the matrix. Against that build:

    | gate | result |
    |---|---|
    | `pluginval` s10, arm64 VST3 — *today's gate* | ✅ SUCCESS |
    | `pluginval` s10, arm64 AU — *today's gate* | ✅ SUCCESS |
    | `auval -strict`, arm64 — *today's gate* | ✅ AU VALIDATION SUCCEEDED |
    | **`Validate the x86_64 slice (macOS)` — new** | ❌ **`NaNs found in buffer`**, 5 failed tests of 450 across three groups, `FAILURE`, `*** FAILED`, exit 1 |

    Measured on the runner in [run 33029504881](https://github.com/basilica-audio/Crypta/actions/runs/33029504881): every gate that exists today green, the new step red, the job red with it, and the failure-path log upload exercised. That demo branch also skips the two `ctest` steps, and the first attempt ([run 33028141210](https://github.com/basilica-audio/Crypta/actions/runs/33028141210)) is why — on the unmodified workflow #106's x86_64 suite step catches this defect *first*, so the job never reached the step under test. Defence in depth working, and useless as evidence for this step; skipping the suite steps on a throwaway branch isolates it. The claim being proven is #108's — an Intel-only defect the **arm64 format validation** misses — not that `pluginval` catches what `ctest` cannot. The transfer from a local measurement is not assumed either: the thinned bundles hash identically in both places, `3e1133b6…` / `b9b9d5d1…` clean and `c101bf17…` / `d30f4bd0…` broken, on this machine and on the runner.
  - **Two honest limits on what this buys, both worth stating rather than leaving implied.**
    - **`auval` does not catch that defect**, even against the x86_64 slice: it reports `AU VALIDATION SUCCEEDED` on the same broken build that `pluginval` fails. The `auval` half of the step is genuinely steerable now, and it still checks AU-specific conformance that `pluginval` does not, but it is not what makes this gate bite. Only `pluginval` caught it.
    - **This widens coverage; it tightens no contract.** The same distinction PR #106 had to draw about its own demonstration applies here unchanged — on the pre-#102 tree the block-schedule nulls did not fail, because the test carried an Intel-only −60 dB allowance and the defect measured −75.9 dB, inside its own bar. Every assertion `pluginval` and `auval` make is the assertion they already made; what changes is the architecture they make it against.

### Fixed (a fresh instance no longer ramps up from silence, issue #98)

- **Five gain stages plus the low-band compressor's makeup gain started at silence on a freshly constructed instance and ramped up over the first ~20 ms, where a re-prepared instance started at level.** `juce::dsp::Gain::prepare()` (JUCE 8.0.14) calls `reset()`, which is `SmoothedValue::reset (sampleRate, rampDuration)`, and that snaps the smoother's *current* value to whatever *target* the object holds at that instant — and a default-constructed `juce::dsp::Gain` holds a target of `0.0` **linear**, i.e. silence, not unity. `prepareToPlay()` prepared the five cascaded gain stages (input trim, the three per-band level trims, output trim) and the compressor's makeup gain **before** telling them the session's values, so they ramped from nothing; a re-prepare already held the real values and snapped to them. Measured at a **−13.5 dB null with a 0.485 peak difference** between the two, and **−13.50 dB (Classic) / −13.05 dB (Circuit)** of level on the opening 20 ms of a steady tone.
  - **`prepareToPlay()` now sets every target before `prepare()`** — the same ordering `cryp::LowGrowl`, `cryp::CircuitDrive` and every `DryWetMixer` stage in the same function already required and documented, simply never applied to the plain gain stages. `ParallelCompressor::prepare()` takes the initial makeup gain as an explicit third parameter for the same reason it already takes the mix proportion, defaulting to 0 dB so a caller that says nothing gets unity rather than silence. The compressor's static controls are pushed in first so the makeup figure passed is the one the first `processBlock()` will compute, auto-makeup included, without restating that formula outside the class that owns it.
  - **`reset()` needed no equivalent change and gets none**, which was checked rather than assumed: it also snaps current to target, and by the time a host can call it the target is already the session's value, so a mid-session transport stop or loop re-arms at level. That is now asserted rather than reasoned about.
  - **Three bring-up paths, one contract.** `tests/OfflineRealtimeNullTests.cpp` asserts that prepared-once, prepared-twice and `reset()`-before-the-first-block render **bit-identically**, at 44.1 and 48 kHz on both drive engines. Reverting the fix turns that red at −12.5 dB with a 0.575 peak difference.
  - **And the audible property, separately.** A second case measures the opening 20 ms of a steady tone against the same processor's settled level on a deliberately neutral chain — gate off, compressor fully dry, no drive, no EQ, no clip, with a non-zero value on every gain stage, because it was the smoother's *starting* point that was wrong and not its target. Measured **−1.32 dB** after the fix against **−13.50 / −13.05 dB** before it. The residual 1.32 dB is not a gain ramp — with the fix there is no gain trajectory left to observe — but the chain's own startup: the 61 samples of reported latency the render opens with, plus the LR4 splits and phase-align allpass settling on a 110 Hz tone against a 120 Hz split. Both drive engines agree to four decimal places, which places the residue in the shared front end rather than in either engine. That case is deliberately not run on the engine parameter sets the rest of the file uses: a compressor pulls the settled level down towards the ramped one and turned the same defect into a 1.6 dB reading, leaving no room between "broken" and "fixed".
  - **`docs/architecture.md`'s "`DryWetMixer` priming gotcha" section is generalised** into a `prepare()` priming gotcha, because it is not a `DryWetMixer` property — it is a property of every `juce::dsp` stage whose `prepare()` snaps a `SmoothedValue`, and stating it narrowly is why this one was missed.

### Changed (the v0.2.0 golden fixtures are deliberately NOT regenerated, issue #98)

- **The priming fix changes the opening 40 ms of every render, including a legacy v0.2.0 session's, and the committed fixtures are kept as they are.** This was a decision, so it is recorded as one rather than left implied by a fixture refresh.
  - **Is a legacy v0.2.0 session supposed to render identically after this? No — not in its opening 40 ms, and yes everywhere else.** What v0.2.0 did there was ramp every gain stage up from silence; that is a defect v0.2.0 also had, not a voicing choice, and preserving it would mean preserving a bug for the sake of fidelity to a bug.
  - **The fixtures are not regenerated**, for four reasons: they are the only artefact in the repository produced by code that no longer exists, and regenerating turns a cross-version lock into a snapshot of `HEAD` that several other tests already provide better; a regenerated fixture absorbs this change silently, where keeping it makes the change visible in the source diff permanently with its measured size attached; the file already has precedent — the clip-ON fixture is a deliberate, documented, non-bit-identical change given its own bounded contract rather than a refresh; and the documented regeneration path would not work anyway, because `[.generate-goldens]` run against current code emits a state XML carrying a `stateVersion` attribute that the well-formedness test rejects by design.
  - **`tests/GoldenRenderTests.cpp` now measures two regions instead of one.** The opening `[0, 1920)` — 40 ms at 48 kHz, the 20 ms ramp plus roughly another burst of the programme's 120 ms cycle for the gate and compressor ballistics to converge back — is held to ≤ −25 dB relative and ≤ 0.12 peak, against measured **−31.66 dB (gnaw), −29.94 dB (wool), −31.68 dB (razor)** and a peak of **0.0951** in all three. That bound is not configuration-dependent: the priming difference is 30 dB larger than any toolchain drift, so it dominates that window everywhere.
  - **The settled region `[1920, end)` is where the migration contract still lives**, and it is held as tightly as the build allows: **≤ −85 dB on macOS arm64**, against measured **−90.78 / −91.28 / −92.41 dB** with peak differences of 1.14e-04, 8.3e-05 and 1.20e-04, bit-identical across repeated runs; ≤ −60 dB elsewhere, unchanged. Region by region the null against the fixtures reads −31.7 dB from sample 0, −64.6 dB from 960 (the end of the ramp) and −90.8 dB from 1920, where it settles and stays (−90.4 dB from 2880, −93.7 dB from 9600).
  - **This is a loss and the file says so.** An unintended arm64 change quieter than −85 dB would now pass where the previous `memcmp` would have caught it. It remains 25 dB tighter than the bar the same test applies everywhere else and 12 dB tighter than the ordinary cross-toolchain drift measured on the Windows runner, and the migration property the fixtures exist for — a legacy session landing on Circuit / Smooth RMS / Modern instead of the legacy engines — is a tens-of-dB failure that is additionally pinned by the engine-index `REQUIRE`s in the same test, which do not depend on audio at all.

### Fixed (the render no longer depends on the host's buffer size, issue #99)

- **On Intel, the plugin rendered different audio at different buffer sizes — so an offline bounce did not match what the user monitored.** This was a property of the shipped binary, not of a test. `juce::dsp`'s denormal guard, `util::snapToZero()` (JUCE 8.0.14, `juce_dsp/juce_dsp.h`), zeroes a filter's internal state at the **end of every `process()` call** once its magnitude falls below 1e-8, and it is called once per call by `LinkwitzRileyFilter`, `IIR::Filter`, `Oversampling` and `BallisticsFilter` — in this plugin that is both crossover splits, the phase-align allpass, the post-sum EQ and every oversampled drive stage. The snap events therefore landed on the host's block boundaries: change the buffer size, change where state is zeroed, change the output. `JUCE_SNAP_TO_ZERO` is `#if JUCE_INTEL` (`juce_audio_basics/buffers/juce_FloatVectorOperations.h`), so an Apple Silicon build of the same source was already bit-exact and an Intel one was not. A mixing plugin whose export differs from its monitoring is wrong in a way that no listening pass reliably catches, which is why this is a fix rather than a documented quirk.
  - **`CMakeLists.txt` sets `JUCE_DSP_ENABLE_SNAP_TO_ZERO=0`.** One flag, and the mechanism is gone rather than bounded.
  - **Denormal protection is not removed with it — it moves from `juce::dsp` to the FPU, where it is both stronger and cheaper.** `processBlock()` opens with `juce::ScopedNoDenormals`, which sets MXCSR `FTZ|DAZ` (mask `0x8040`) on Intel and FPCR `FZ` on ARM for the whole callback and restores the host's own mode on the way out, so *every* arithmetic result is flushed rather than only the filter state `juce::dsp` chose to snap. It is a per-thread mode, RAII-scoped, and the host's thread is left exactly as it was found. No DSP in this plugin runs outside that scope: `processBlockBypassed()` is JUCE's default, which performs no arithmetic. JUCE's own documentation for the flag says as much — "if your audio app already disables denormals altogether (for example, by using the `ScopedNoDenormals` class) … you can safely disable this flag to shave off a few cpu cycles".
  - **The interlock is compile-time.** `src/PluginProcessor.cpp` fails the build with an explanation if the flag is ever off on a target where `ScopedNoDenormals` compiles to nothing, so the trade cannot silently become "no denormal protection at all".
  - **The replacement is proven to work, not assumed to.** `tests/RobustnessTests.cpp` gains a deterministic, non-timing assertion: drive the chain hard, feed it six seconds of digital silence, and require that not one sample of the decaying tail is a denormal — across block sizes 32/128/512 and both drive engines. It measures zero. Commenting out the `ScopedNoDenormals` and changing nothing else turns all six configurations red at 453,022–554,216 denormal samples, so the zero is the FPU mode working rather than the signal never reaching the denormal range. The existing timing-based guard ("a long silence after a loud burst does not inflate block time") continues to pass, with silence *cheaper* than programme on both architectures.
  - **Measured before and after**, same machine, same compiler, macOS x86_64 under Rosetta: before, block-schedule nulls worst-cased at **−75.90 dB / 1.3437e-03 peak** with the first differing sample landing on a block boundary (sample 32 exactly for the 512-versus-32 Classic case); after, **all 8017 assertions in `tests/OfflineRealtimeNullTests.cpp` are bit-exact**. Apple Silicon is unchanged — the macro was already a no-op there, so no arm64 render moves and the committed v0.2.0 golden fixtures remain valid.
  - **What changes for users: Intel renders are fractionally different from this build on**, and identical to what an Apple Silicon machine has been producing all along. The full suite is 255 test cases / 158,750 assertions green on arm64 and on x86_64, with the two architectures now agreeing on the assertion count as well as the result.
  - **`tests/OfflineRealtimeNullTests.cpp` tightens accordingly.** The Intel-only −60 dB / 1.0e-2 bound is deleted and the bit-exact contract applies unconditionally on every architecture. The mechanism and the measurements stay in the file as the justification for the flag, and a new test case asserts the flag itself, so a merge or a JUCE bump that loses it fails with a sentence naming the reason instead of several dozen unexplained null failures.

### Fixed (a test gated on the operating system where it meant the architecture, issue #100)

- **`tests/GoldenRenderTests.cpp` switched its strictness on `#if JUCE_MAC` when the property that decides it is the architecture.** The committed fixtures are an arm64 Apple-clang artefact, so bit-exactness is only attainable in the configuration that produced them; `JUCE_MAC` stood in for "arm64" and happened to be right only because every macOS build so far has been arm64. CI already builds a Universal Binary and simply never runs the x86_64 slice, so the wrong branch was never selected. Building this repository for x86_64 and running it under Rosetta takes the `memcmp` branch and fails — while measuring **−80.75 dB (gnaw), −73.07 dB (wool), −80.39 dB (razor)** relative, all comfortably inside the file's own −60 dB bar. The output was fine; the branch selection was wrong. The gate is now `#if JUCE_MAC && JUCE_ARM`.
  - **This is deliberately *not* gated on `JUCE_DSP_ENABLE_SNAP_TO_ZERO`**, which the issue first suggested. Those three x86_64 figures are unchanged to four significant figures with the guard on and with it off, which attributes the whole of the divergence to cross-architecture codegen — FMA contraction and libm — rather than to state snapping. Gating on the snap-to-zero flag would select `memcmp` on macOS x86_64 and on Windows once #99 is closed, and fail both.
  - **The fixtures are not regenerated.** Regenerating on x86_64 would swap which architecture is exact and move the problem rather than remove it; arm64 is the right one to pin, because per #99 it is the deterministic one.

### Fixed (offline bounce now nulls against realtime, issue #34)

- **The Circuit engine rendered a different first 10–20 ms depending on the host's buffer size, so an offline bounce did not null against realtime playback.** `CircuitDrive`'s per-block control ramps (`RampedScalar`) interpolate from the previous block's value to the new target across exactly one block — whatever that block's length — which is the right shape for automation and the wrong shape for a value that has never been set. `prepareToPlay()` was pushing only `splitHighHz` and `highTightHz` into the engine and pushing them *after* `prepare()`, so drive, blend, bias, tone and both level trims were ramped up from `CircuitDrive`'s constructed defaults across the render's first block. The length of that ramp was therefore the host's buffer size: 0.7 ms at 32 samples, 10.7 ms at 512, 21.3 ms at 1024. The same passage rendered at two block sizes nulled at **−36.2 dB** with a **0.083** peak difference, and at **−20.2 dB** over the first 1000 samples alone. Two changes, both priming rather than tolerating:
  - **Every Circuit control is now pushed in before `circuitDrive.prepare()`**, the same order the low-band compressor's `DryWetMixer`, `cryp::Voicing`'s blend and `cryp::LowGrowl` already required and documented. `prepare()` snaps the ramps to what it finds, so the first block starts at the session's values instead of sliding up to them.
  - **`CircuitDrive::prepare()` re-arms its one-shot ramp snap.** The snap was a once-per-object guard, so a *re*-prepare — which is exactly what a host does when it switches from playback to rendering, usually at a different buffer size — did not snap, and a control moved between stopping the transport and starting the render would ramp in across the render's first block. `prepare()` has just rebuilt the oversampler and is about to reset every filter, shaper and delay in the engine; there is no previous block left to ramp from.
  - Measured after the fix: **−inf dB, bit-exact**, at 44.1 and 48 kHz, across block sizes 32/441/480/1024, a ragged 1…512 schedule, both engines, and the host's full stop-render sequence with a knob moved in the middle.

### Added

- **`tests/OfflineRealtimeNullTests.cpp` — the acceptance bullet "Offline bounce == realtime processing verified" (#34), as a test instead of a listening pass.** Offline-vs-realtime equivalence is a determinism property, not a matter of taste, and the defects that break it — state that depends on wall-clock time, on the buffer size, on a `prepareToPlay()` value that is not re-applied, or on a smoother that advances per callback rather than per sample — are the ones an ear is worst at catching and a subtraction is best at. Four cases, 8017 assertions, separating the three things a host changes at once when it bounces:
  - **`setNonRealtime()` on its own changes nothing** — the flag a host actually toggles (JUCE 8.0.14, `juce::AudioProcessor::setNonRealtime`), with the block schedule held identical so the flag is the only variable. Bit-exact.
  - **Re-buffering on its own changes nothing** — a realtime reference at 512 nulled against offline renders at 32, 441/480, 1024 and a ragged schedule whose block length changes every call down to single samples, at 44.1 and 48 kHz, on both drive engines, with a render length that is not a multiple of any of them so every configuration ends on a short remainder. Bit-exact. This is the case that failed, and the fix above is why it now passes.
  - **The host's real bounce sequence changes nothing** — the instance that was already playing, knobs moved, `setNonRealtime (true)`, re-prepared at the render's block size. Bit-exact.
  - **The tolerance is bit-exact everywhere, with exactly one named exception.** `juce::dsp::Convolution` builds its partitioned FFT engine from `ProcessSpec::maximumBlockSize`, so preparing at a different block size partitions the same convolution differently and rounds differently in the last bit. That stage is switched off for the assertions above rather than having an epsilon widened around it, and gets its own case which measures it in a linear chain: **1.1921e-07, which is 2^−23, one ULP** — the smallest difference float32 can represent there — for a null depth of −151 dB. In the full chain the post-sum EQ's biquads integrate that last bit into **−85.4 dB**; the same chain with the IR stage off is bit-exact, which is what attributes the figure entirely to the convolution.
  - **No assertion in the file is wall-clock sensitive.** Nothing sleeps or waits, and the one quantity that does vary from run to run — which samples the convolution rounds differently, since JUCE installs its engines from a background thread — is measured and reported but deliberately not asserted on.

- **A third divergence was found by the Windows CI job, traced to JUCE rather than to Crypta, and bounded on Intel only.** The re-buffering cases failed on `windows-latest` at −75.7 dB with a 1.3427e-03 peak, on both engines, across the whole render rather than at its head. The first suspicion — MSVC floating-point contraction — is wrong, and two experiments say so rather than an argument:
  - **Building this repository for `x86_64` with the same Apple clang, on the same machine, and running it under Rosetta reproduces the Windows failures configuration for configuration**: worst case −75.90 dB / 1.3437e-03 peak locally against −75.73 dB / 1.3427e-03 on the Windows runner. Two different compilers on two different operating systems agreeing to 0.2 dB is not a codegen difference — it is the architecture.
  - **Rebuilding that same `x86_64` binary with `JUCE_DSP_ENABLE_SNAP_TO_ZERO=0`, changing nothing else, restores bit-exactness across all 8017 assertions.** One flag, one mechanism.

  The mechanism is `juce::dsp`'s denormal guard: `util::snapToZero()` zeroes a filter's internal state at the **end of every `process()` call** whenever its magnitude has fallen below 1e-8, and it is called once per call by `LinkwitzRileyFilter`, `IIR::Filter`, `Oversampling` and `BallisticsFilter` — i.e. by both crossover splits, the phase-align allpass, the post-sum EQ and every oversampled drive stage, which is why both engines are affected and not only the Circuit one. `JUCE_SNAP_TO_ZERO` is defined as a no-op on non-Intel targets, so on Apple Silicon nothing snaps and the state trajectory is a pure function of the input; on Intel the snap events land exactly on the host's block boundaries. **The render therefore genuinely depends on the buffer size on Intel** — it is a real block-size dependence, not a rounding artefact. It shows the mechanism's fingerprint too: the schedules with the most block boundaries diverge (32-sample, ragged 1…512), the ones with fewest are bit-exact even on Intel (512 vs 1024, both engines, both rates), and the first differing sample for the 512-versus-32 Classic case is sample 32 exactly.

  **The bit-exact contract is not weakened on Apple Silicon**, where it is proven and holds. On Intel the same cases assert ≤ −60 dB and ≤ 1.0e-2 peak: −60 dB is the bar `tests/GoldenRenderTests.cpp` already sets for the same class of platform split, it sits 15.7 dB below the worst figure measured on either Intel toolchain, and 24 dB below the smallest genuine defect this file has actually caught. The condition tracks `JUCE_DSP_ENABLE_SNAP_TO_ZERO`, so the day the guard is switched off the contract tightens back to bit-exact by itself instead of quietly staying loose.

  Switching that guard off is a **recommended follow-up, not taken here**: it is safe in this plugin's audio path (`processBlock()` installs `juce::ScopedNoDenormals`, so the CPU already flushes denormals and the guard is redundant there) and it is proven above to work, but it changes the audio the shipped Intel binary produces, which is a product decision rather than a test's to take.

- **A second, smaller divergence is characterised in the file rather than fixed, and says so.** A *freshly constructed* instance ramps up from silence across its first 20 ms where a re-prepared one starts at level: `juce::dsp::Gain::prepare()` snaps its smoother to whatever target the object is holding, and `prepareToPlay()` prepares the five cascaded gain stages (and the low-band compressor's makeup gain) before telling them the session's value. Measured at a **−13.5 dB** null with a **0.485** peak difference on an otherwise neutral chain, and **0.062** for the makeup gain alone. It is left alone on purpose: no host renders into a never-prepared instance, so both sides of a real bounce-versus-playback comparison are re-prepared and agree — and correcting the priming order changes the first 20 ms of every render, which the committed v0.2.0 golden fixtures encode. Regenerating those is documented as a deliberate, reviewed act and is not this change's to take.

### Added (the v1.0.0 QA gate is now a command, issue #34)

- **`scripts/qa-gate.sh` — Part 1 of `docs/qa-checklist.md`, executable.** The checklist's automated half was a prose table pointing at CI, which meant that "run the QA gate" was still a human reading rows and reproducing workflow steps by hand. It is now one command: it builds every format `juce_add_plugin()` declares, runs 20 gates, records what each one measured, writes `build/qa-gate/report.{md,json}`, and exits non-zero if any gate failed. The report is what gets pasted into the checklist's sign-off table, so a recorded pass is a run that happened rather than a claim that it would.
  - **A gate that stops testing anything fails.** Each Catch2 slice is a test-spec filter; Catch2 exits non-zero when a filter matches nothing, so a gate that silently fell out of the suite can never read as a pass. This is the failure mode the previous prose table had no defence against.
  - **The shipped formats are read out of `CMakeLists.txt`'s `FORMATS` line**, not restated in the script. Adding a format cannot leave the gate validating a subset of what ships. AU, VST3 and Standalone are each located, and each shipped Mach-O's architectures are recorded — a release claiming a Universal Binary and shipping arm64-only is now something the report can disprove.
  - **pluginval's verdict is not the runner's to decide.** It is delegated to `.github/scripts/assert-pluginval-passed.sh`, the same script `ci.yml` uses, at the same pinned v1.0.4 and the same `--strictness-level 10`, so a local run and a CI run cannot disagree about what "passed" means.
  - **The measured figures come out of the tests themselves**, not out of a second implementation of the measurement: gates declared with a pattern are re-run with `-s` and their `INFO` text is harvested, with Catch2's own scaffolding and test-case headings filtered out by asking the binary for its test names rather than guessing at them.
  - **The gate was run twice and agreed with itself exactly.** Once locally on a loaded machine, once on an idle GitHub macOS runner: every measured figure is bit-identical between the two — the same transition steps, the same −inf dB null, the same alias-to-signal numbers. Only the durations differ, and durations are the one thing in the report that is not a measurement.
  - **Every run records the machine's load average.** No gate is a wall-clock assertion, so no verdict is affected by load — but the report says so itself when the machine was busy, and marks its own durations as noise. `tests/CpuLoadTests.cpp` learned that lesson the expensive way.

- **`scripts/qa-gates.tsv` — the gate manifest**, one row per Catch2 gate (id, filter, measurement pattern, what it proves). The single place a gate is declared, and the file to keep in step with the checklist's Part 1 table.

- **`tests/QaGateManifestTests.cpp` — the manifest's rot guard**, which runs on every pull request like any other test. The runner failing on a filter that matches nothing only helps when somebody runs it, and by design that is on demand and on a tag. The rot that actually bites is the other direction: a pull request renames a tag in `tests/`, every test still passes, CI is green, and the manifest silently stops covering it — surfacing on release day, in the one run nobody wants to be debugging. This asserts every tag in the manifest against Catch2's own test registry, plus that gate ids are unique and selectable, so the regression surfaces in the pull request that caused it. Verified by breaking it deliberately: renaming `[realtime]` to `[realtimee]` in the manifest fails the guard.

- **`.github/workflows/qa-gate.yml`** — the same script in CI, on macOS, publishing its report to the job summary and uploading it with every gate log. It runs **on demand, on a `v*` tag, and on any pull request that touches the gate itself** — deliberately not on every pull request, because that would duplicate `ci.yml`'s build for no new signal, and deliberately not never, because a gate script nobody exercises until release day is a gate that fails on release day.

### Changed

- **`docs/qa-checklist.md` records a measured pass instead of an assertion of one.** The sign-off table now carries the full gate-by-gate result of a real run — commit, host, load average, and the figure each gate printed. Part 1 gains the three bypass rows that PR #90 made assertable, and the stale note claiming #87 was "not fixed here" is replaced by what the fix measures: a transition step of 0.0106 engaging and 0.0124 disengaging against steady-state slews of 0.0145 and 0.0152, a dirac arriving at exactly the reported 61 samples, and a null against a latency-shifted dry copy at −inf dB. The parameter-sweep section is corrected to the 54 parameters actually swept, of which two — not three — still need a step allowance.
- **The document now ends with the list of what is left**, which is four items long and is entirely ears, hosts and Apple secrets: the installation smoke test (blocked on #31), anything requiring a running DAW, the voicing approvals, and whether the accessibility surface is *useful* rather than merely present. Nothing on that list can be automated without changing what it means.

### Fixed

- **Bypass ist jetzt klickfrei und latenzkompensiert (#87).** `processBlock()` behandelte Bypass bisher als harten Early-Return: die Wet-Kette lief nicht mehr mit, der Signalpfad schaltete diskontinuierlich um, und der trockene Pfad wurde unverzögert zurückgegeben, obwohl das Plugin 61 Samples Latenz meldet — ein Bypass-Instanz-Nulltest gegen eine trockene Referenzspur unter Host-PDC nullte deshalb nicht. Beide Defekte hatten dieselbe Ursache (der Early-Return) und sind jetzt zusammen behoben:
  - **Klickfrei**: Bypass ist nun ein Crossfade zwischen der (durchgehend weiterlaufenden) Wet-Kette und einer verzögerten Kopie des trockenen Eingangssignals, gesteuert über eine `juce::SmoothedValue`-Rampe mit fester, samplerate-unabhängiger Zeitkonstante (20 ms, `bypassCrossfadeDurationSeconds`) statt eines festen Sample-Counts — siehe `PluginProcessor.h`s `bypassWetMix`-Dokumentation für die Begründung. Gemessener Sample-zu-Sample-Sprung beim Umschalten: vorher 0,772 (Ein) / 0,496 (Aus) gegen eine Steady-State-Slew von 0,015 — jetzt 0,0106 / 0,0124, also innerhalb der eigenen Steady-State-Slew beider Endpunkte.
  - **Latenzkompensiert**: Ein neuer `bypassDryDelay` (`juce::dsp::DelayLine`, None-Interpolation) verzögert den trockenen Pfad exakt um `getLatencySamples()`, armiert in `updateLatencyCompensation()` genau wie die bestehende `lowBandLatencyDelay`. Ein Dirac-Impuls im Bypass trifft jetzt exakt bei Sample 61 (dem gemeldeten Wert) statt bei Sample 0; ein Nulltest gegen eine um die Latenz verschobene Trockenkopie nullt jetzt bis auf Float-Präzision (vorher −1,3 dB, jetzt −∞ dB).
  - **Kein eingefrorener Zustand mehr**: die Wet-Kette (`processChunk()`) läuft jetzt unbedingt weiter, auch vollständig bypassed — vorher fror der Early-Return jeden Stage-internen Zustand (Filter-Memory, Envelope-Follower, Oversampling-FIR-History) ein, was beim Zurückschalten aus dem Bypass einen zweiten, unabhängigen Klick erzeugte. Ein Zustands-Kontinuitätstest (Gate+Kompressor mit bewegtem Envelope, 30 Blöcke bypassed in der Mitte) zeigt nach dem Einschwingen des Crossfades eine Abweichung von −∞ dB gegenüber einem nie-bypassten Referenzlauf.
  - Neue Tests: `tests/BypassTests.cpp` (7 Testfälle: Klick-Grenzen beim Ein-/Ausschalten, gemeldete Latenz bleibt beim Umschalten konstant, Dirac-Latenz, Nulltest, Zustandskontinuität). `tests/GainProcessingTests.cpp`s namentlich fixierter Bit-exact-Passthrough-Test ist auf "settled, latenzkompensiert" umgestellt — die alte Fixierung auf sofortige Bit-Exaktheit war exakt das in #87 gemeldete Fehlverhalten. `tests/ParameterSweepTests.cpp`s `knownStepAllowance("bypass")`-Ausnahme (0,85) ist entfernt, da der allgemeine 4×-Steady-State-Slew-Test jetzt ohne Sonderfall besteht.

### Added (v1.0.0 QA gate, issue #34)

- **`tests/ParameterSweepTests.cpp` — the end-to-end parameter-extreme sweep** the QA checklist listed as an open automation gap. Walks every `RangedAudioParameter` to both endpoints and asserts the render is finite, bounded by the gain that parameter actually advertises (derived from its own declared range, so `Output Gain` reaching +24 dB is not mistaken for a runaway), and free of transition steps beyond 4x its own steady-state slew. 866 assertions.
  - **It found three parameters that step rather than ramp**, each recorded in `knownStepAllowance()` with the measured value so none can get worse without failing the build: `bypass` (0.772 against a 0.015 steady-state slew), `gateEnabled` (0.216), `splitLowHz` (0.085). The other 48 parameters transition within 4x their own slew at both endpoints. (`bypass`'s entry was later removed once #87's click-free crossfade landed — see the Fixed section above; the sweep now holds it to the same 4x bound as everything else, unexempted.)
  - Comparing a transition against its own two endpoints, rather than against an absolute slew limit, is what makes the assertion meaningful on a plugin whose steady-state waveform legitimately contains near-vertical clipping edges.
- **`tests/CpuLoadTests.cpp` — a CPU cost benchmark** (`[.cpu]`, hidden from the default run, since a wall-clock measurement has no business failing CI on a shared runner). Reports a realtime factor across sample rates 44.1–192 kHz, block sizes 16–2048, and per optional stage.
  - **It asserts nothing about time**, only that the output stayed finite. The first version asserted a 5x-realtime floor and failed on a machine whose load average happened to be 40 — with 192 kHz measuring *faster* than 88.2 kHz, which is how you know the measurement, not the plugin, was wrong. Each line now prints the machine's load average beside the figure, and if it is not near zero the numbers are worthless. **No uncontended measurement of Crypta's CPU cost exists yet**; this instruments the checklist item rather than answering it.

### Changed

- **`docs/qa-checklist.md` reworked against what is actually enforced.** All four automation gaps it listed are closed — pluginval now validates the AU as well as the VST3, the parameter sweep exists, the offscreen editor snapshot test exists, and the accessibility assertions exist. Every subjective item in Part 2 now carries the closest honest measurement beside it, **explicitly labelled "measured, not heard"**, so the person doing the listening starts from data — not so the listening can be skipped. The two items that cannot be discharged by measurement at all (the installation smoke test, which needs the signed artefact blocked behind #31, and anything requiring a running DAW) are named as such.

### Added (headline: four bundled cabinet IRs, generated rather than sourced)

- **Four bass cabinet impulse responses, bundled via `BinaryData`** — `Modelled 8x10 Cone`, `Modelled 8x10 Edge`, `Modelled 1x15 Vintage`, `Modelled 4x10 Horn` (`resources/irs/`, issue #81). `CryptaAudioProcessor::getFactoryIRAssetTable()` is no longer empty; the slot mechanics shipped in v0.4.0 now have content behind them.

  **They are models, not captures, and the naming says so.** Nothing bundled is a recording of any cabinet, speaker or microphone, and none is named after one. Each is computed by `tools/ir-synth/cabsynth.py` from a documented analytical model — driver/box alignment, cone-breakup modes, voice-coil-inductance roll-off, baffle and floor reflections at physical path lengths, microphone proximity and directivity — and every display name begins with "Modelled". `tests/FactoryIRTests.cpp` asserts that prefix rather than leaving it to review.

  **Generating them is the licensing answer.** The content half of #21 stalled because bundling a cab IR means redistributing someone else's recording inside every copy of an AGPLv3 binary, a capture carries rights from the cabinet, the microphone and whoever pressed record, and "free download" is not a licence. Sourcing one that is beyond doubt is a research project with an uncertain outcome. A generated IR removes the question instead of answering it: there is no third-party recording in the binary, so there is nothing to trace and no licensor to find. The generator ships with the audio — re-run it, get byte-identical output, compare the SHA-256 in `resources/irs/manifest.json`. Reproducibility is the provenance. Filed under `CC0-1.0` rather than `Self-recorded`, because `Self-recorded` means a capture and these are not captures; the CC0 1.0 legal code is committed at `resources/irs/CC0-1.0.txt`. No attribution is required of anybody.

- **`tools/ir-synth/` — the generator and the verifier** (`cabsynth.py`, `verify_irs.py`, `README.md`). Standard-library Python 3 only: no numpy, no network access, deliberately, because a generator that needs a pinned scientific stack to reproduce its output is a weaker reproducibility claim than one that needs nothing. `verify_irs.py` measures the shipped `.wav` files as signals and now gates CI.

- **Measured verification of every bundled asset, in the test suite and in CI.** `tests/FactoryIRTests.cpp` gains six cases that re-measure the bytes `BinaryData` actually embedded — the copy that ships — rather than trusting the files that produced them: documented format (mono, 48 kHz, 4096 taps), no clipped or non-finite sample, DC offset below 1e-4 and `|H(0)|` at least 60 dB under the response peak (measured −69 dB to −114 dB), `max |H(f)|` at 0 dBFS within 0.1 dB, a tail that has faded to digital silence rather than been cut off, under 0.01 % of energy in the final tenth, and — the check the issue turns on — **each of the four slots loading into the convolution engine and audibly changing the signal, with "None" restoring the bit-exact passthrough.** A seventh case pins that the four voicings are measurably different from one another, which every other test would happily pass if they were not.

### Changed

- `resources/irs/LICENSES.md` rewritten from "no impulse response is bundled" to the full provenance record: per-file model parameters, licence, checksums and measured response (peak, RMS, DC, −10 dB band, decay time, octave-band table).
- `resources/irs/manifest.json` is the machine-readable half of that record, and `verify_irs.py --expect-manifest` cross-checks every file's SHA-256 against it in CI, so the documentation cannot drift away from the audio it describes.
- The v0.4.0 documentation test asserting the asset table is *empty* is replaced by its counterpart: the table is populated, with these four names in this order, and out-of-range indices still refuse cleanly.

### Notes

- **The bundled IRs have not been listened to.** They are verified by measurement — response curves, DC, normalisation, decay, engine load — and labelled as such. Whether they are musically right is issue #81's remaining open question and needs ears.
- **No GUI IR slot list exists yet.** The four IRs are reachable through the processor API (`getNumFactoryImpulseResponses()`, `getFactoryImpulseResponseName()`, `loadFactoryImpulseResponse()`, `clearImpulseResponse()`) and are exercised end-to-end by the test suite, but the v0.4.0 vector editor exposes only the `Cab` toggle and `Cab Mix` knob — there is no control that lets a user pick one. That is a GUI gap, not a content gap.

## [0.4.0] - 2026-08-20

### Added (headline: the custom vector GUI)

- **A fully vector-drawn editor** (`src/gui/`, `src/PluginEditor.{h,cpp}`), replacing the v0.1–v0.3 preset bar + `GenericAudioProcessorEditor` stack. Ported from the suite's proven M3 component family (`basilica-audio/Miserere` PR #31): `BasilicaLookAndFeel` (pointer knobs with engraved scale rings, detented choice knobs, lamp toggles, gold-on-black suite palette), `PointerKnob`, `BusPanel`, `KeyboardSteps` and `NeedleMeter`. No bitmaps: every knob, lamp, panel, needle and glyph is drawn at runtime with `juce::Graphics`/`juce::Path`. Typography is EB Garamond, embedded via `BinaryData` (OFL, `resources/fonts/OFL-EBGaramond.txt`), so the editor renders identically on macOS and Windows. Closes #45, #25.
- **The complete parameter surface, laid out in signal-flow order** across ten section panels — Input, Noise Gate, Crossover, Low Band, Drive Engine, Mid Band, High Band, Cabinet, EQ, Output. All 51 parameters are on the front panel: 44 knobs (40 float + 4 choice; choice knobs are detented and announce the choice *name*) and 7 lamp toggles. Closes #26.
- **Four needle meters reading the v0.3.0 metering backend** (`src/dsp/MeterTaps.h`): input and output peak (dBFS, fast-attack/slow-release ballistics, warning zone from 0 dBFS) plus gate and low-band-compressor gain reduction (positive dB, symmetric ballistics). Driven by a single 30 Hz GUI-thread timer over relaxed atomic loads — the audio thread is never touched. Closes #27.
- **A resizable, aspect-ratio-locked editor with the scale persisted in plugin state.** The surface is laid out once at its design size and the window is a uniform scale transform on it (0.6×–1.8×), so no window size can re-flow or clip the layout. The chosen scale is stored as a root property on the APVTS state tree and therefore round-trips through the existing `get`/`setStateInformation()` pair; a pre-existing session without it opens at unity. Closes #28.
- **Accessibility as a shipped feature, not a follow-up** (closes #46): every control is keyboard-focusable with WAI-ARIA-style stepping (Arrow = 1 % of range, Shift+Arrow = fine, PageUp/Down = 10 %, Home/End = extremes; one detent per press on choice knobs), a gold focus ring with a dark halo on every focusable control, accessible name/role/value on every control (units included — `-12.00 dB`, not `-12.00`), each section exposed to screen readers as a named group without trapping Tab, focus order equal to the signal flow, and meters exposed as read-only value text. All of it is asserted headlessly against real `AccessibilityHandler`s.

### Added (DSP: Graaawl, measured voicing character, telemetry, IR slots)

- **"Graaawl" — a Warwick Thumb low-band growl mode** (`src/dsp/LowGrowl.{h,cpp}`, issue #36). Off by default. New controls: `Graaawl` (off/on), `Graaawl Amount` (0–100 %, default 0), `Graaawl Tone` (0–100 %, default 50). The growl is generated in a **parallel, band-limited branch** — an asymmetric saturator (`tanh(g·x + b) − tanh(b)`, g = 6, b = 0.35) followed by a 700 Hz – 2.2 kHz formant band-pass with a resonant peak — and blended on top of a low band that is never itself shaped. Inserted after the low-band compressor, before `Low Level`.
  - **The sub stays clean, measured:** with a 50 Hz probe at 100 % Amount the 20–150 Hz region moves by **under 0.01 dB**, and the fundamental by under 0.05 dB.
  - **Zero latency.** The branch is antialiased with ADAA-1 at base rate rather than by oversampling, so enabling Graaawl never re-reports latency and never needs a compensating delay. Measured alias-to-signal on a hot 50 Hz probe at full growl: **−86 dB at Tone 0, −105 dB at Tone 100**.
  - **Off is a bit-exact bypass**, structurally: `process()` returns without touching the block once the 20 ms gain ramp has reached zero. Toggling it on and off returns the low band to sample-identical output, and no state migration is needed — the defaults *are* the legacy behaviour.
  - Correction to the issue's own sketch, recorded in the code: a 300–400 Hz pre-highpass on the branch cannot work in this topology (the stage sits below a 120 Hz LR4 split, where 350 Hz is 37 dB of stopband leakage). The formant energy is generated from the fundamental and band-passed afterwards; that band-pass, not a pre-filter, is what protects the sub.
- **Measured character evidence for the three voicings** (`tests/VoicingCharacterTests.cpp`, issues #15/#16/#17). "Hard clip", "mid scoop", "tight" were adjectives in code comments and nowhere in the test suite; they are now numbers with tolerances. Gnaw's shaper is verified symmetric (even harmonics 69 dB below their odd neighbours) and its character filter verified neutral (±0.5 dB); Wool's asymmetry is verified as real even-harmonic content 49 dB above the symmetric reference, and its scoop measured at **−6.2 dB at 500 Hz** with recovered shoulders; Razor's hump measured at **+5.0 dB at 900 Hz**, its soft clip 20 dB milder than Gnaw's at a realistic playing level, and the Tight pre-highpass verified at its requested corner (−3 dB) with a 12 dB/octave slope. A hidden `[.character-table]` test prints the full harmonic table.
- **Gain-reduction telemetry on both low-band detector engines** (issue #12). `Classic Peak` previously reported a flat zero, leaving the GR meter dead for every migrated pre-v0.3.0 session; it now reports a block-peak-derived estimate, measured to track its static curve within 1.5 dB on steady material. New public accessor `CryptaAudioProcessor::getLowBandGainReductionDb()` — the same lock-free relaxed atomic the meters publish, for UI components to poll. The parallel mix is now proven to be an equal-latency blend: a 50 % render equals the sample-by-sample average of the 0 % and 100 % renders.
- **Factory-IR slot mechanics and a licence guard** (`src/dsp/FactoryIRs.{h,cpp}`, `resources/irs/LICENSES.md`, issue #21): an asset table, an in-memory WAV/AIFF decoder, `IRLoader::clearImpulseResponse()` for a slot list's "None" entry, and the processor API a GUI IR list needs (`getNumFactoryImpulseResponses()`, `getFactoryImpulseResponseName()`, `loadFactoryImpulseResponse()`, `clearImpulseResponse()`).

### Changed

- `docs/architecture.md`'s module map now describes `src/gui` (the vector component family) instead of the placeholder `src/ui` row.

- **A generated editor preview** at `docs/gui-preview.png`, rendered by the GUI test suite from the real editor (`tests/gui/GuiPreviewSnapshotTests.cpp`) rather than drawn by hand, plus `docs/gui-mapping.md` listing which control drives which parameter.

### Notes

- WCAG contrast is enforced by tests against the same colour accessors the LookAndFeel paints with, so a palette tweak that drops a text pair below AA 4.5:1 fails the build rather than shipping.

### Not shipped, deliberately

- **No cabinet IRs are bundled.** Bundling one means redistributing someone else's recording inside every copy of an AGPLv3 binary, and nothing has been sourced with a licence worth staking that on — "free download" and "royalty free" are not licences. The bar (CC0 / explicit public domain / self-recorded, each with a checkable source) is enforced by `FactoryIRLibrary`'s constructor and asserted by `tests/FactoryIRTests.cpp`, so an unverified IR fails the build rather than shipping quietly. Content curation is tracked separately.
- **Graaawl's voicing constants are engineering-derived starting points.** Every claim about them here is a measurement, but the final voicing is an ear decision against a real Thumb 5 DI (issue #36's own gate), as are the three high-band voicings (#15/#16/#17, #34).

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
