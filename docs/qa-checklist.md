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

| Check | How it is enforced | Where |
|---|---|---|
| Builds on both platforms | `cmake --build`, macOS built as a Universal Binary (arm64 + x86_64) | `ci.yml` matrix |
| Unit + integration suite | `ctest --output-on-failure`, Catch2 — 187 test cases across 35 files | `tests/` |
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
| Aliasing budget | Alias-to-signal measurements per voicing and engine | `tests/AliasingTests.cpp` |
| Latency reporting | Reported latency matches the actual path, and is stable across engine switches | `tests/LatencyTests.cpp` |
| Cross-thread IR loading | Concurrent `prepare()` / `loadImpulseResponse()` stress | `tests/CrossThreadReprepareTests.cpp` |
| End-to-end parameter-extreme sweep | Every parameter, at both endpoints: finite, bounded by its own declared range, and no transition step beyond 4x its steady-state slew | `tests/ParameterSweepTests.cpp` (`[sweep]`) |
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

Three parameters step rather than ramp. Each is recorded in
`knownStepAllowance()` in `tests/ParameterSweepTests.cpp` with the value measured
when the test was written, so none of them can get *worse* without failing the
build:

- **`bypass` — 0.772** (steady-state slew: 0.015). Bypass is an unsmoothed early
  return, so engaging it steps by whatever the wet and dry signals differ by at
  that instant, and the dry path is returned undelayed while the plugin reports
  61 samples of latency. **Filed as #87.** Not fixed here: the bit-exact
  passthrough is pinned deliberately by `tests/GainProcessingTests.cpp`, and
  changing what bypass means is a product decision.
- **`gateEnabled` — 0.216.** Engaging the gate is a mode switch, not a continuous
  control, and its attack starts from closed.
- **`splitLowHz` — 0.085.** `juce::dsp::LinkwitzRileyFilter` recomputes
  coefficients immediately, so snapping a crossover across its entire range in a
  single block steps the filter state. Standard for un-smoothed coefficient
  updates.

The other 48 parameters transition within 4x their own steady-state slew at both
endpoints.

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
| Machine-verified pass (date, commit) | 2026-08-22 — full suite green, `pluginval --strictness-level 10` SUCCESS on VST3 and AU, `auval -strict` SUCCEEDED |
| Part 2 signed off by | *(not signed — requires listening, a DAW, and a signed artefact from #31)* |
| Date | |
