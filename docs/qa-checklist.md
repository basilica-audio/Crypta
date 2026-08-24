# v1.0.0 QA checklist

The gate in front of the `v1.0.0` tag (issue #34). It has two halves, and the
split matters: **everything a machine can decide is already decided by a
machine**, so the manual pass is only the part that genuinely needs a person —
ears, hands on a DAW, and taste.

Do not re-run the automated half by hand. If something in it can regress
silently, that is a bug in the test suite, and the fix belongs in `tests/`
rather than in this document.

---

## Part 1 — automated, already enforced

These run on every pull request and every push to `main`
([`.github/workflows/ci.yml`](../.github/workflows/ci.yml)), on macOS **and**
Windows, and block the merge. Nothing here needs a human at release time; the
release is gated on `main` being green.

### Run it

Part 1 is not a list to work through by hand. It is one command:

```
scripts/qa-gate.sh
```

It builds every format `juce_add_plugin()` declares, runs every gate below,
records what each one measured, and exits non-zero if any of them failed. The
result lands in `build/qa-gate/report.md` (and `report.json`), which is what
gets pasted into the sign-off table at the bottom of this document — so the
recorded pass is a run that happened, not a claim that it would.

| | |
|---|---|
| Every gate | `scripts/qa-gate.sh` |
| One gate | `scripts/qa-gate.sh --gates bypass,latency` |
| Against an existing build | `scripts/qa-gate.sh --skip-build` |
| Offline (no pluginval download, no AU install) | `scripts/qa-gate.sh --skip-validators` |
| What the gates are | `scripts/qa-gate.sh --list-gates` |

The Catch2 half of the gate is declared in
[`scripts/qa-gates.tsv`](../scripts/qa-gates.tsv), one row per gate, and that
file is the only place a gate is defined. Three properties of the runner are
worth knowing, because they are what make its green meaningful:

- **A gate whose filter stops matching any test case fails.** Catch2 exits
  non-zero when nothing matched, so a gate that quietly stopped testing
  anything can never read as a pass. The mirror image of that — a tag renamed
  in `tests/` while the manifest is left behind — is caught on *every* pull
  request by `tests/QaGateManifestTests.cpp`, which asserts the manifest
  against the test registry rather than waiting for release day to find out.
- **The shipped formats are read out of `CMakeLists.txt`'s `FORMATS` line**,
  not restated in the script, so adding a format cannot leave the gate
  validating a subset of what ships.
- **pluginval's verdict is not the runner's to decide.** It is delegated to
  [`.github/scripts/assert-pluginval-passed.sh`](../.github/scripts/assert-pluginval-passed.sh),
  the same script the workflow uses, at the same pinned version and the same
  `--strictness-level 10` — so a local run and a CI run cannot disagree about
  what "passed" means.

In CI the same script runs from
[`.github/workflows/qa-gate.yml`](../.github/workflows/qa-gate.yml): on demand,
on a `v*` tag, and on any pull request that touches the gate itself. It is not
run on every pull request, because it would duplicate `ci.yml`'s build for no
new signal.

| Check | How it is enforced | Where |
|---|---|---|
| Builds on both platforms | `cmake --build`, macOS built as a Universal Binary (arm64 + x86_64) | `ci.yml` matrix |
| Unit + integration suite | `ctest --output-on-failure`, Catch2 — 249 test cases across 46 files | `tests/` |
| pluginval, maximum strictness | `pluginval --strictness-level 10 --validate` on the VST3, pinned to v1.0.4 by SHA-256 | `ci.yml` |
| AU validation | `auval -strict -v aufx Cryp Yvsv` after installing the built `.component` | `ci.yml` |
| No allocations on the audio thread | `AllocationGuard` around `processBlock` on both drive engines, across engine switches, oversized blocks and long silences | `tests/RobustnessTests.cpp` (`[realtime]`) |
| Block-size independence | An oversized host block renders sample-identically to the same signal fed in properly-sized sub-blocks | `tests/ChunkingTests.cpp` |
| No NaN/Inf, no crash on degenerate input | Denormal-range input, zero-sample buffers, bypassed and active | `tests/RobustnessTests.cpp` |
| Deterministic render vs. committed goldens | Legacy v0.2.0 sessions still render identically under the current engine; the engaged safety clip stays inside its documented −40 dB null | `tests/GoldenRenderTests.cpp` |
| State round-trip | Every parameter of the full set survives save → load; v2 state round-trips with all 54 parameters | `tests/StateTests.cpp`, `tests/StateMigrationTests.cpp` |
| State migration | v0.1 and v0.2 sessions migrate deterministically; explicit user choices are never overwritten | `tests/StateMigrationTests.cpp` |
| Preset round-trip and rejection | Save → load restores every parameter; foreign-plugin and wrong-format presets are refused; every factory preset parses, loads and is in range | `tests/PresetManagerTests.cpp` |
| `reset()` clears stage state | Silence right after a loud signal is actually silent | `tests/ResetTests.cpp` |
| No zipper noise on fast automation | Automation ramp assertions | `tests/RobustnessTests.cpp` (`[automation]`) |
| Bypass is click-free | Toggling bypass mid-render steps no further than a small multiple of either endpoint's own steady-state slew, in both directions | `tests/BypassTests.cpp` (`[bypass]`) |
| Bypass is latency-compensated | A dirac arrives while bypassed at exactly `getLatencySamples()`, and a settled bypassed render nulls against a dry copy shifted by that figure | `tests/BypassTests.cpp` (`[bypass][latency]`) |
| The wet chain never goes stale while bypassed | A run bypassed for 30 blocks mid-signal converges back to a never-bypassed run to numerical precision | `tests/BypassTests.cpp` (`[bypass]`) |
| Aliasing budget | Alias-to-signal measurements per voicing and engine | `tests/AliasingTests.cpp` |
| Latency reporting | Reported latency matches the actual path, and is stable across engine switches | `tests/LatencyTests.cpp` |
| Cross-thread IR loading | Concurrent `prepare()` / `loadImpulseResponse()` stress | `tests/CrossThreadReprepareTests.cpp` |
| End-to-end parameter-extreme sweep | Every parameter, at both endpoints: finite, bounded by its own declared range, and no transition step beyond 4x its steady-state slew | `tests/ParameterSweepTests.cpp` (`[sweep]`) |
| The gate manifest still describes the suite | Every tag in `scripts/qa-gates.tsv` is still carried by a registered test case; every gate id is unique and selectable | `tests/QaGateManifestTests.cpp` (`[qa-gate][meta]`) |
| Bundled IRs are sane signals | Every factory IR re-measured from the embedded bytes: format, no clipping, DC 60 dB down, unity peak response, faded tail, engine load, voicing spread | `tests/FactoryIRTests.cpp` (`[factory][content]`) |
| Bundled IRs match their provenance record | SHA-256 of every shipped `.wav` cross-checked against `resources/irs/manifest.json` | `tools/ir-synth/verify_irs.py`, run in `ci.yml` |
| Release build is signed, notarized, stapled | Developer ID Application signing → `notarytool` → `stapler` on tag push | [`.github/workflows/release.yml`](../.github/workflows/release.yml) |

### Automation gaps — closed

All four gaps listed in the original version of this document have been closed.
They are recorded here rather than deleted, because the point of the list was
that the manual pass should not silently be asked to cover them.

- [x] **pluginval on the AU as well as the VST3.** `ci.yml` now runs
      `--strictness-level 10` against the built `.component` as well as the
      `.vst3` (the `validate macos-au` call). Verified locally on the current
      `main`: both `SUCCESS`.
- [x] **A parameter-sweep null test.** `tests/ParameterSweepTests.cpp` walks
      every `RangedAudioParameter` to both endpoints and asserts the render is
      finite, bounded by the gain that parameter actually advertises, and free
      of transition steps beyond 4x its own steady-state slew. 866 assertions.
      It found three parameters that step — see below.
- [x] **An offscreen editor snapshot test.** `tests/gui/GuiPreviewSnapshotTests.cpp`
      (`[preview]`), which also generates `docs/gui-preview.png`.
- [x] **Automated accessibility assertions on the editor** (#46). 19 cases under
      `[a11y]`: accessible name, role and value on every control, focus order,
      keyboard stepping, and WCAG contrast against the LookAndFeel's own colour
      accessors.

### What the parameter sweep found

Three parameters stepped rather than ramped when the sweep was written; two
still do. Each is recorded in
`knownStepAllowance()` in `tests/ParameterSweepTests.cpp` with the value measured
when the test was written, so none of them can get *worse* without failing the
build:

- **`bypass` — 0.772** (steady-state slew: 0.015) when this was written. Bypass
  was an unsmoothed early return, so engaging it stepped by whatever the wet and
  dry signals differed by at that instant, and the dry path was returned
  undelayed while the plugin reported 61 samples of latency. **Filed as #87 and
  since fixed in PR #90** — bypass is now a crossfade between the
  continuously-running wet chain and a latency-delayed dry copy. Its
  `knownStepAllowance()` entry is gone: the sweep holds it to the same 4x bound
  as everything else, unexempted, and it passes. Measured after the fix:
  **0.0106 engaging and 0.0124 disengaging, against steady-state slews of 0.0145
  and 0.0152** — the transition is quieter than the steady state it moves
  between.
- **`gateEnabled` — 0.216.** Engaging the gate is a mode switch, not a continuous
  control, and its attack starts from closed.
- **`splitLowHz` — 0.085.** `juce::dsp::LinkwitzRileyFilter` recomputes
  coefficients immediately, so snapping a crossover across its entire range in a
  single block steps the filter state. Standard for un-smoothed coefficient
  updates.

The suite sweeps **54 parameters** at both endpoints. Two carry an allowance;
the other 52 — `bypass` now among them — transition within 4x their own
steady-state slew, unexempted.

### CPU cost

`tests/CpuLoadTests.cpp` (`[.cpu]`, hidden — a wall-clock measurement has no
business failing CI on a shared runner). Run it deliberately:

```
./Tests "[.cpu]"
```

It reports a realtime factor per configuration across sample rates 44.1–192 kHz,
block sizes 16–2048, and per optional stage. **It asserts nothing about time** —
only that the output stayed finite. The first version did assert a 5x-realtime
floor and promptly failed on an otherwise-healthy machine that happened to have
a load average of 40; the giveaway was 192 kHz measuring *faster* than 88.2 kHz.
A wall-clock assertion on shared hardware measures the hardware's mood.

Each line therefore prints the machine's one-minute load average next to the
figure. **If it is not near zero, throw the numbers away.** As of 2026-08-22 no
uncontended measurement of this plugin's CPU cost exists; the box available was
never idle enough to produce one, so the checklist item is *instrumented* rather
than *answered*.

**2026-08-24 update — a CI runner is not a quiet box either.**
[`.github/workflows/cpu-load.yml`](../.github/workflows/cpu-load.yml) runs
`./Tests "[.cpu]" -s` on demand on a fresh GitHub-hosted `macos-latest` runner,
on the theory that a freshly booted VM is the closest thing to an idle machine
available here. It is not: the run reported a one-minute load average of
**16.5–18.0** eight minutes after boot, on a runner doing nothing else — by
this document's own "near zero" bar, that is still not a clean measurement,
and the number is recorded as a datapoint rather than an answer.

The figures themselves (44.1–192 kHz: 12.4x–21.3x realtime, 4.7–8.1% of one
core; block sizes 16–2048 @ 48 kHz: 17.6x–23.4x; per-stage cost at 48 kHz /
512: 23.6x–29.7x depending on what is engaged) land inside the same range two
separate contended local runs measured — 5.9–28.4 load average on 2026-08-22,
and 22.8–24.0 load average on 2026-08-24 (see `git log` on this file) — despite
those three environments disagreeing on load average by more than an order of
magnitude. That convergence is circumstantial, not a substitute for a clean
run: it is offered as one further reason not to expect the ranking of one
configuration against another to be an artefact of contention, not as a
resolution of this item. **This checklist item stays *instrumented, not
answered*.** If a genuinely idle machine becomes available, `workflow_dispatch`
the CPU load report or run `./Tests "[.cpu]"` locally and replace this note.

---

## Part 2 — manual, needs a person

This is the actual gate. It is Yves' call, and no CI result substitutes for it.

Run each DAW protocol on a release build of the tagged candidate — the
signed, notarized artefact from the release workflow, installed the way a user
would install it, not a local `build/` output.

### Discharged by measurement, 2026-08-22 — not by ear

Issue #34 was worked through mechanically. Everything a machine can decide was
decided, and each subjective item below carries the closest honest measurement
in its place, **labelled as measured, not heard**. That is not the same thing as
a listening sign-off and does not claim to be. If listening disagrees with any
of it, the measurement is the thing that was wrong, and the issue should be
reopened.

Two items in this part **cannot** be discharged by measurement at all, and are
not marked below:

- **The installation smoke test**, in full. There is no signed, notarized
  artefact to test: #31 (Developer ID signing + notarization) is blocked on the
  Apple Developer ID secrets, so `spctl`, Gatekeeper and the `.sha256` sidecar
  have nothing to run against. Everything below was verified on a local
  `build/` output instead, which is explicitly not what this section asks for.
- **Anything requiring a running DAW.** No Logic Pro or Reaper session was
  opened. Where a checklist item has a host-independent equivalent — block-size
  independence, state round-trip, latency reporting, offline-vs-realtime
  arithmetic — that equivalent is in Part 1 and is green; where it does not,
  the item stands.

### Installation smoke test

- [ ] macOS: the downloaded zip opens without a Gatekeeper warning; AU and
      VST3 load after being copied into their folders.
- [ ] macOS: `spctl -a -vv -t install` accepts the Standalone app.
- [ ] Windows: the zip extracts and the VST3 loads; the SmartScreen warning is
      the documented, expected one.
- [ ] The `.sha256` sidecar matches the downloaded archive.

### Logic Pro protocol (AU)

- [ ] Plugin appears after an AU rescan; no validation warning in Logic's
      plugin manager.
- [ ] Insert on a bass track: audio passes, bypass is click-free.
      *Measured, not heard:* the click-free property is asserted, in both
      directions, at a tighter bound than "no audible click" — the transition
      must not step further than 4x either endpoint's own steady-state slew,
      and measures 0.0106 / 0.0124 against slews of 0.0145 / 0.0152
      (`tests/BypassTests.cpp`, #87/PR #90). Whether Logic's own bypass button
      routes through it is the part that needs the DAW.
- [ ] Automate `Split Low`, `Split High`, `High Drive` and `Voicing` during
      playback — no zipper noise, no dropouts, no clicks on voicing changes.
- [ ] Save the project, close it, reopen it: every parameter and the loaded
      preset come back exactly.
- [ ] Load an IR, save, reopen: the IR is still loaded and still audible.
- [ ] Change the session sample rate and buffer size mid-session: no crash, no
      stuck state, latency re-reported correctly.
- [ ] Freeze / unfreeze the track: result matches realtime playback.

### Reaper protocol (VST3)

- [ ] Same insert, bypass, automation, save/reopen and IR sequence as above.
- [ ] Plugin delay compensation reports correctly — a null test against a
      dry-copy track cancels when the plugin is in a neutral state.
      *Measured, not heard:* the plugin's half of this is now proven
      host-independently. A bypassed instance nulls against a dry copy shifted
      by `getLatencySamples()` at **−inf dB** — a bit-exact null, not merely a
      deep one — and a dirac arrives while bypassed at exactly sample 61, the
      figure the plugin reports (`tests/BypassTests.cpp`). What is left for
      Reaper is whether the *host* applies that figure, which no test here can
      answer.
- [ ] Multiple instances on multiple tracks: no cross-instance interference,
      no CPU cliff.

### Offline bounce vs. realtime

Block-size independence is already proven in CI, so what is being checked here
is the *host's* offline path, not the plugin's arithmetic.

- [ ] Bounce the same passage offline and in realtime in Logic; null the two
      against each other — the difference is at the noise floor.
- [ ] Repeat in Reaper (render vs. realtime record).
- [ ] Repeat once with the IR loader engaged and once with the safety clip
      engaged, since both change the tail behaviour.

### Voicing approval — the taste gate

No pass/fail criterion exists for these. They are approved or they are not.

**None of the boxes below is ticked, and none should be ticked by anything other
than listening.** What follows each one is the closest measurement that exists,
so that the person doing the listening starts from data rather than from
nothing — not so that the listening can be skipped.

- [ ] **Gnaw** approved by ear.
      *Measured, not heard:* shaper verified symmetric — even harmonics 69 dB
      below their odd neighbours — and its character filter flat within ±0.5 dB
      (`tests/VoicingCharacterTests.cpp`). Full harmonic table:
      `./Tests "[.character-table]"`.
- [ ] **Wool** approved by ear.
      *Measured, not heard:* asymmetry is real even-harmonic content 49 dB above
      the symmetric reference; scoop measured at −6.2 dB at 500 Hz with
      recovered shoulders.
- [ ] **Razor** approved by ear.
      *Measured, not heard:* hump measured at +5.0 dB at 900 Hz; soft clip 20 dB
      milder than Gnaw's at a realistic playing level; Tight pre-highpass at its
      requested corner (−3 dB) with a 12 dB/octave slope.
- [ ] **Circuit** vs. **Classic** drive engine: Circuit is the default for new
      instances — confirmed as the better default.
      *Measured, not heard:* the two report identical latency at every sample
      rate (61 samples at 44.1 kHz) and both hold the three-way band sum flat,
      so the choice is purely tonal — there is no measurement that can pick the
      better default, and this one genuinely cannot be discharged.
- [ ] **Smooth RMS** low-band detector: no audible tremolo on sustained low
      notes.
      *Measured, not heard:* gain-reduction telemetry tracks the static curve
      within 1.5 dB on steady material, and the parallel mix is a proven
      equal-latency blend (a 50 % render equals the sample-by-sample average of
      the 0 % and 100 % renders). Neither of those measures *tremolo*.
- [ ] **Modern** gate: chugs cut cleanly, no chatter, no swallowed attacks.
      *Measured, not heard:* gate behaviour is pinned by `tests/GateEngineTests.cpp`
      and `tests/NoiseGateTests.cpp` for threshold, ratio and hysteresis. Chatter
      on real palm-mutes is not something the suite reproduces.
- [ ] Every factory preset is musically usable on a real bass DI, not just
      in range.
      *Measured, not heard:* all twelve factory presets parse, load, round-trip
      and land in range (`tests/PresetManagerTests.cpp`). "Musically usable" is
      untested and untestable here.

### Bundled cabinet IRs — the other taste gate (#81)

Four bass cabinet IRs ship as of #81, and they are **models, not captures**
(`resources/irs/LICENSES.md`).

- [ ] The four bundled IRs approved by ear.
      *Measured, not heard:* every one verified for format, absence of clipping
      and DC (60–114 dB below the response peak), unity peak magnitude response
      within 0.1 dB, a tail faded to digital silence, and an end-to-end load
      into the convolution engine with "None" restoring the bit-exact
      passthrough. The voicing *spread* is measured too — the 4x10-plus-horn
      sits 28 dB above the 1x15 at 8 kHz, and each cone position sits ~10 dB
      above its edge counterpart at 4 kHz. Whether any of them sounds like a
      bass cabinet is exactly what has not been established.
- [ ] The set is the right set — four is enough, and these four are the right
      four for the plugin's focus.

### GUI and accessibility

M3 has landed (v0.4.0), so these are live. Three of the four now have machine
coverage; the *feel* of keyboard and screen-reader use still does not.

- [ ] Every control reachable and operable by keyboard alone.
      *Measured, not felt:* asserted headlessly against real
      `AccessibilityHandler`s — every control focusable, focus order equal to
      signal flow, WAI-ARIA-style stepping, no Tab trap
      (`tests/gui/EditorAccessibilityTests.cpp`, `[a11y]`).
- [ ] VoiceOver announces a meaningful name and value for every control.
      *Measured, not heard:* accessible name, role and unit-carrying value text
      asserted for every control. Whether VoiceOver's actual speech is *useful*
      is not something a test can decide.
- [ ] The editor is legible at the smallest and largest supported scale.
      *Measured, not seen:* layout asserted at 0.6x–1.8x with no clipping or
      re-flow (`tests/gui/EditorScaleTests.cpp`, `[scale]`), and WCAG AA 4.5:1
      contrast enforced against the LookAndFeel's own colour accessors.
- [ ] Metering matches what the ear and the DAW's own meters report.
      *Measured, not heard:* ballistics and tap values asserted
      (`tests/gui/EditorMeteringTests.cpp`, `[metering]`). Agreement with a
      host's own meters is untested.

---

## Sign-off

`v1.0.0` is tagged only after both halves are complete. Part 1 signs itself off
by being green on `main`; Part 2 is signed off by Yves, and by nobody and
nothing else.

A **machine-verified** pass is recorded below alongside it. It is a separate
row on purpose: it records that everything mechanically checkable was checked
and passed, and it does not substitute for the listening sign-off, which has not
happened.

| | |
|---|---|
| Candidate build | |
| Part 1 green on `main` (commit) | |
| Machine-verified pass (date, commit) | 2026-08-23 — `scripts/qa-gate.sh` on `372238e` plus the change set that added it, **20 gates run, 0 failed**, locally and again on an idle CI runner with bit-identical figures (see below) |
| Part 2 signed off by | *(not signed — requires listening, a DAW, and a signed artefact from #31)* |
| Date | |

### The recorded machine-verified pass

`scripts/qa-gate.sh`, on `372238e` (`main` at the time) plus the change set
that added this runner, macOS arm64, JUCE 8.0.14, pluginval v1.0.4 at
`--strictness-level 10`. Twenty gates, none failed. The figures below are what
the gates printed while passing — not the thresholds they were held to.

**It was run twice, and that is the interesting part.** Once locally on a
machine carrying other work, and once on a GitHub macOS runner that was idle
(one-minute load average 1.23 rising to 2.27 on 3 CPUs). Every measured figure
below is *bit-identical* between the two runs — the same 0.0105713, the same
−inf dB null, the same alias-to-signal numbers — which is what a deterministic
render suite ought to produce and is worth having demonstrated rather than
assumed. Only the durations differ, and durations are the one thing in the
report that is not a measurement.

| Gate | Result | What it measured |
|---|---|---|
| `build` | PASS | Configured and built `Release` |
| `formats` | PASS | AU, VST3, Standalone all present. Architectures: arm64 (a native build; the Universal Binary is CI's `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`) |
| `standalone-bundle` | PASS | `Info.plist` lints, Mach-O executable present, signature `adhoc` — Developer ID signing is #31 |
| `irs-manifest` | PASS | 4 shipped `.wav` files re-measured and SHA-256-matched against `resources/irs/manifest.json` |
| `suite-full` | PASS | 249/249 ctest cases |
| `bypass` | PASS | 923 assertions. Transition step 0.0106 (engage) / 0.0124 (disengage) against steady-state slews of 0.0145 / 0.0152; dirac at sample 61 against a reported latency of 61; null against a latency-shifted dry copy at **−inf dB**; state-continuity after un-bypass **−inf dB** |
| `latency` | PASS | 121 assertions. Reported latency 61 samples on both engines at 44.1 / 48 / 96 / 192 kHz. Classic peaks at 61–62; Circuit peaks 15–26 samples later, which is IIR group delay, not a reporting error — see the rationale in `tests/LatencyTests.cpp` |
| `realtime-safety` | PASS | 15 assertions, no allocation on the audio thread |
| `param-sweep` | PASS | 1016 assertions over 54 parameters at both endpoints. Largest transition step: 0.231 (`outputGain`), which passes because the bound is 4x *that parameter's own* steady-state slew — a parameter whose endpoint is +24 dB legitimately moves further than one whose endpoint is a filter corner. Only `gateEnabled` (0.216, allowed 0.25) and `splitLowHz` (0.085, allowed 0.10) still need an allowance |
| `state-recall` | PASS | 25239 assertions across 42 cases |
| `rate-and-block` | PASS | 48883 assertions across 40 cases |
| `null-tests` | PASS | 113 assertions. Golden render null at **−200 dB** relative to the golden; engaged safety clip null at −19.1 dB against a 7.4 dB signal, inside its documented budget |
| `aliasing` | PASS | 100 assertions. Alias-to-signal −95.9 dB on the reference tone; Circuit vs. Classic at 1244 Hz: −81.9 dB vs. −48.0 dB; ADAA-1 improves a plain `tanh` by 12.3 dB (−17.1 → −29.3) and the delta-form by 13.6 dB (−55.7 → −69.3) |
| `voicing-measured` | PASS | 86824 assertions across 39 cases — measured, **not heard** (see Part 2) |
| `content` | PASS | 6757 assertions across 25 cases |
| `editor` | PASS | 4189 assertions across 47 cases |
| `manifest` | PASS | 208 assertions — every tag in `scripts/qa-gates.tsv` is still carried by a registered test case, and every gate id is unique |
| `pluginval-vst3` | PASS | `--strictness-level 10`, 1 plugin found, SUCCESS, exit 0 |
| `pluginval-au` | PASS | `--strictness-level 10`, 1 plugin found, SUCCESS, exit 0 |
| `auval` | PASS | `AU VALIDATION SUCCEEDED.` |

**One caveat on the numbers, and it is the same one `tests/CpuLoadTests.cpp`
records.** The machine was carrying other work throughout — a one-minute load
average between 5.9 and 28.4 on 8 CPUs, across the runs. No gate above is a
wall-clock assertion, so no verdict is affected; the *durations* in
`report.md` are noise and should not be read as measurements. `scripts/qa-gate.sh` records the load average
beside every run for exactly this reason, and its report says so itself when
the machine was loaded.

### What is left, and it is only ears

Everything in this document that a machine can decide is decided, and it
passes. What remains is Part 2, and it is short enough to state in full:

1. **The installation smoke test** — blocked on #31 (Developer ID secrets).
   There is no signed, notarized artefact to install.
2. **Anything requiring a running DAW** — the Logic and Reaper protocols, and
   the host-side half of offline-vs-realtime bounce and delay compensation.
3. **The voicing approvals** — Gnaw, Wool, Razor, Circuit-vs-Classic as the
   default, the Smooth RMS detector on sustained low notes, the Modern gate on
   chugs, the twelve factory presets on a real bass DI, and the four bundled
   IRs. No pass/fail criterion exists for any of them.
4. **The feel of the accessibility surface** — whether VoiceOver's actual
   speech is useful, and whether the editor is legible rather than merely
   unclipped.

Nothing on that list can be automated without changing what it means.
