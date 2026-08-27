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
| Offline == realtime, bit-exact | The same passage rendered with `setNonRealtime()` and re-buffered at 32/441/480/1024/ragged blocks nulls to −inf dB against the realtime pass, at 44.1 and 48 kHz, including the short final block. One stage is excluded and bounded rather than tolerated: `juce::dsp::Convolution` builds its FFT engine from `maximumBlockSize`, so it rounds within one ULP (measured 1.19e-07 = 2⁻²³) | `tests/OfflineRealtimeNullTests.cpp` (`[offline-realtime]`) |
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
| Silence in, silence out | A prepared instance fed digital silence emits exactly 0.0 (fresh, both engines) and 1.4e-37 peak / 6.8e-38 DC with every stage engaged, against a bound of 2^-24 = 5.96e-08 — half a 24-bit LSB, i.e. provably absent from any delivery render. Also asserted after a loud passage has been left to ring down, and with `highBias` at 100 % where the shaper's DC offset is deliberate | `tests/SilenceFloorTests.cpp` (`[silence]`) |
| A knob gesture, not just a knob jump | Every one of the 42 continuous parameters swept across its full range in 267 ms, one new value per 64-sample block, against nine static renders of the same parameter. Worst ratio measured: **1.016x** (`splitHighHz`), bound 1.5x. This is the test a missing `SmoothedValue` fails; the endpoint-snap test above is not | `tests/ContinuousSweepTests.cpp` (`[continuous-sweep]`) |
| A reopened session renders bit-identically | Save, then hand the state to a fresh instance the way a host does when a project opens, and compare the render `==`, not against an epsilon. **0 differing samples across 132 configurations** — every parameter at both edges, all-minimum, all-maximum, and 20 seeded random whole-set configurations. A mid-session restore is measured separately, because a running instance legitimately crossfades | `tests/StateRecallRenderTests.cpp` (`[recall-render]`) |
| The same musical result at every sample rate | Magnitude response 55 Hz - 1760 Hz at 44.1/48/88.2/96/192 kHz: worst deviation from the 48 kHz reference **0.052 dB** against a 0.5 dB bound derived from bilinear frequency warping. Harmonic profile of both drive engines and all three voicings: worst **0.078 dB** against 1.5 dB. Compressor gain reduction 30 ms after a step, both detectors: worst **0.030 dB** against 0.5 dB — the ballistics are times, not sample counts | `tests/SampleRateInvarianceTests.cpp` (`[invariance]`) |
| Adversarial input | Full-scale sine, +1.0 and -1.0 DC, a full-scale impulse, digital silence, alternating +-1 at Nyquist, denormal-range noise and a full-scale 20 Hz - 20 kHz chirp, on both engines; the block size changing every callback including 0 and 4096; the sample rate changing mid-stream 30 times without a `reset()`; and a NaN/Inf-poisoned block, after which the plugin must return to within **0.1 dB** of the level it had before | `tests/AdversarialInputTests.cpp` (`[adversarial]`) |
| The listening gate, measured | Detector tremolo, gate chatter, swallowed attacks, drive-versus-level, factory preset gain staging and cabinet plausibility — each held to a bound taken from psychoacoustics or loudspeaker physics rather than from what the plugin happens to measure. See Part 2 | `tests/ListeningProxyTests.cpp` (`[listening]`) |
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

> **Superseded in part on 2026-08-27.** The section below records the first
> pass, which stopped at "here is the closest number, but it is not a listening
> sign-off". The second pass did not stop there: see
> [The listening gate, measured](#the-listening-gate-measured--2026-08-27)
> further down, where four of the seven taste items are now held to bounds taken
> from psychoacoustics and loudspeaker physics, and the three findings that are
> genuinely matters of taste are named as fine-tune items rather than left as
> unticked boxes.


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

Offline-vs-realtime equivalence **is** proven in CI now — see
`tests/OfflineRealtimeNullTests.cpp` (`[offline-realtime]`), bit-exact across
44.1/48 kHz and every block size including ragged remainders. What is left for
a human here is the *host's* offline path: whether Logic and Reaper actually
drive the plugin the way the test assumes.

That sentence used to read "block-size independence is already proven in CI,
so what is being checked here is the host's offline path, not the plugin's
arithmetic." **That was wrong, and it was wrong in the direction that costs
you a bug.** `ChunkingTests.cpp` proves an oversized block equals the same
signal in sub-blocks — but not that an engine *re-prepared* at a different
block size renders identically. It does not, or rather it did not:
`CircuitDrive`'s ramps interpolate across exactly one block whatever its
length, so the first render block depended on the host's buffer size (0.7 ms
at 32 samples, 21.3 ms at 1024). Measured −58.7 dB null before the fix in
PR #97. A checklist that tells the tester "the arithmetic is fine, only check
the host" is how that survives a listening pass.

- [ ] Bounce the same passage offline and in realtime in Logic; null the two
      against each other — the difference is at the noise floor.
- [ ] Repeat in Reaper (render vs. realtime record).
- [ ] Repeat once with the IR loader engaged and once with the safety clip
      engaged, since both change the tail behaviour.

### The listening gate, measured — 2026-08-27

**The taste items below were worked a second time, and this pass did not stop at
"measured, not heard".** Each one names the defect a listener would actually be
listening *for*, and holds it to a bound taken from published perceptual data,
from loudspeaker physics, or from the arithmetic of the signal — never from what
this plugin happens to measure. `tests/ListeningProxyTests.cpp` (`[listening]`)
is where they live, and `./Tests "[.listening-table]"` prints every figure.

What that can and cannot do is worth stating plainly. It cannot decide whether
Gnaw is *good*. It can decide whether the Smooth RMS detector breathes, whether
the Modern gate chatters, whether the drive control doubles as a fader, and
whether the bundled cabinets are shaped like bass cabinets — which is what those
checklist lines were written to catch.

| Question | Bound, and where it comes from | Measured |
|---|---|---|
| Smooth RMS: audible tremolo on a sustained low note? | **0.5 dB peak-to-peak in the 2–15 Hz band.** The just-noticeable modulation depth for amplitude modulation is at its minimum near 4 Hz, at a modulation index of roughly 0.03–0.05 on a complex tone (Zwicker & Fastl, *Psychoacoustics*, ch. 10) — 0.87 dB peak-to-peak at the very best case. The bound is a little over half of that, so the modulation has to be inaudible with margin rather than merely at threshold | **0.00047 dB p-p at 41.2 Hz, 0.00018 dB at 55 Hz** — three orders of magnitude below the most sensitive published threshold, against a settled reduction of several dB. **Answered: it does not breathe.** |
| Modern gate: chatter on chugs? | **Exactly 8 openings, and it is not a tolerance.** The test signal is an eight-note chug train whose amplitude, between onsets, only ever decreases. A ninth opening is a re-trigger on a signal that was getting quieter, which is what chatter *is* | **8 openings for 8 notes. Answered: it does not chatter.** |
| Modern gate: swallowed attacks? | **0.5 dB of peak loss** against the same note with the gate off — below the ~1 dB level difference reliably heard on a transient | **0.174 dB on notes 2–8, 0.230 dB on the first note from a fully closed gate. Answered: it does not swallow attacks.** |
| Voicings: does the drive control still behave like one? | **No attenuation** relative to the same voicing at 0 % drive (no tolerance to choose), and **no more than +12 dB** over it | Classic: Gnaw −17.9 → −12.5 dB, Wool −17.9 → −14.5 dB, Razor −17.9 → −16.5 dB. Circuit: −16.1 → −12.0 / −16.1 → −15.7 / −16.0 → −15.1 dB. All monotone, all inside +12 dB |
| Factory presets: broken, silent, or clipping on a real DI? | **Bounded by +12 dBFS** (the safety clip's ceiling plus transient headroom — `tests/ParameterSweepTests.cpp`'s own derivation), **not more than 20 dB below the input**, and — since the issue #34 item 1 trims — **below 0 dBFS** on the −12 dBFS DI, with the shipped `outputGain` trims targeting −0.3 dBFS on that fixture | All 12 finite, all in bounds, none silent, all below **−0.30 dBFS** (range −0.31 to −6.25). Level changes −0.3 dB to +10.0 dB on a −12 dBFS bass DI. The `outputGain` trims were re-derived against the broader-band suite reference programme (`tests/PresetHeadroomTests.cpp`), which six of them cleared this DI while failing — so every preset now clears this fixture with more room than it targets there |
| Cabinets: are these shaped like bass cabinets? | **−10 dB low edge in [25, 70] Hz** — a bass cab has to reproduce a 41 Hz low E and none extends below ~25 Hz. **−10 dB high edge in [1.2 kHz, 6 kHz]** for the hornless models — where voice-coil inductance kills a 10"/15" driver. **The horn model at least 10 dB above every hornless model at 8 kHz**, or its HF path is inaudible and the name is wrong | 8x10 Cone **32–4699 Hz**, 8x10 Edge **34–2172 Hz**, 1x15 Vintage **34–1804 Hz**, 4x10 Horn **37–3079 Hz**; horn at 8 kHz **−15.0 dB against −27.3 dB** for the loudest hornless, a **12.2 dB** difference. All four pairwise distinct by more than 3 dB somewhere in 40 Hz – 8 kHz |

#### What the measurements found, and what is a taste call rather than a defect

Three figures came out where a listener would have had something to say. One of
them has since been reclassified as a defect and fixed; the other two are not
asserted, because asserting them would be this file deciding a question that
belongs to the person who voiced the plugin. They are recorded here, and in the
issue, as fine-tune items:

1. **[FIXED — issue #34 item 1] Eight of the twelve factory presets pushed a
   −12 dBFS bass DI past 0 dBFS**, `Default` among them at **+2.49 dBFS**,
   worst `Clean Low, Loud Top` at **+3.52 dBFS**. Reclassified: a preset that
   clips at the canonical tracking level is a defect, not gain-staging taste.
   Each offending preset now carries a derived `outputGain` trim targeting
   **−0.3 dBFS** on the reference DI (nothing else in any preset changed, and
   presets already under the target were not raised — that would be
   level-matching, which is item 2 and still open), and the measurement is a
   gate: `tests/ListeningProxyTests.cpp` asserts every factory preset stays
   below 0 dBFS on this fixture.
2. **The three voicings are not level-matched.** At 70 % drive on a bass DI the
   spread is **3.97 dB on Classic** (Gnaw −12.5, Wool −14.5, Razor −16.5) and
   **3.68 dB on Circuit** (Gnaw −12.0, Wool −15.7, Razor −15.1). Above about
   1 dB the louder option wins an A/B regardless of its tone, so as it stands
   the voicing selector is partly a volume control. Fixable with three
   constants. Some people deliberately want Razor quieter, which is why it is
   not gated.
3. **The 4x10 Horn does not measure like a horn-equipped cabinet at the −10 dB
   bandwidth criterion.** Its band edge is **3079 Hz**, in the same region as
   the hornless models, because the model puts the horn path at 0.5 gain — a
   horn with its attenuator turned down. It is clearly *present* (12.2 dB above
   every hornless model at 8 kHz), so the assertion is on that; whether the
   attenuator is voiced too low is a voicing decision.

One further measured datapoint, from the silence gate rather than the listening
one — **[FIXED — issue #34 item 4]**: a freshly prepared instance whose state
already had `High Bias` at 100 % emitted a **0.137-peak (−17 dBFS) DC transient
into silence** while the 10 Hz blocker settled — a thump on session load and
preset recall. The bias offset's own image through the shaper is now subtracted
at its creation (the construction Wool and `cryp::LowGrowl` already used,
extended to Gnaw and Razor) and `CircuitDrive::reset()` primes the shapers'
ADAA history at the bias operating point, so the blocker has nothing to settle.
Gated in `tests/SilenceFloorTests.cpp`: session-load restore measures **exactly
0.0** against the 2⁻²⁴ floor including the transient; mid-playback restore
measures 1.1e-4–1.9e-4 against a derived 1.042e-3 ramp-residual bound.

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
      and land in range (`tests/PresetManagerTests.cpp`), and every one renders
      the −12 dBFS reference DI below 0 dBFS (`tests/ListeningProxyTests.cpp`,
      the issue #34 item 1 gate — shipped trims target −0.3 dBFS). "Musically
      usable" is untested and untestable here.

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
| Machine-verified pass (date, commit) | 2026-08-27 — the measurement gate below: **279 test cases, 161733 assertions, 0 failures**, arm64 and the x86_64 slice |
| Part 2 signed off by | *(not signed — the DAW protocols and the installation smoke test still require a person and a signed artefact from #31; the listening items are discharged by measurement, see above)* |
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

### What is left, after the 2026-08-27 measurement pass

Point 3 of this list used to read "the voicing approvals — no pass/fail
criterion exists for any of them". Four of the seven items under it now have
one, because the *defect* each was written to catch turned out to be measurable
even though the *preference* behind it is not. See "The listening gate,
measured" above.

1. **The installation smoke test** — blocked on #31 (Developer ID secrets).
   There is no signed, notarized artefact to install. Unchanged.
2. **Anything requiring a running DAW** — the Logic and Reaper protocols, and
   the host-side half of offline-vs-realtime bounce and delay compensation.
   Unchanged, and genuinely unchangeable: the plugin's half of both is proven
   host-independently, the host's half needs the host.
3. **The taste that is actually taste**, which is now three things rather than
   seven:
   - **Circuit vs. Classic as the default.** Both report identical latency at
     every sample rate, both hold the band sum flat, both keep their harmonic
     profile to within 0.078 dB across 44.1–192 kHz, and Circuit's alias floor
     is 33.9 dB below Classic's at 1244 Hz. That last figure is the only
     measurable argument either way, and it points at Circuit — which is the
     shipped default. Nothing further is measurable here.
   - **Whether Gnaw, Wool and Razor are the right three characters.** Their
     harmonic structure, filter corners, alias floors and level behaviour are
     all pinned. Whether the result is musical is not.
   - **Whether the four bundled cabinets are the right four.** All four measure
     as plausible, distinct bass cabinets. Whether the set is the right set is
     a product decision.
4. **The feel of the accessibility surface** — whether VoiceOver's actual
   speech is useful, and whether the editor is legible rather than merely
   unclipped. Unchanged.

Plus the two fine-tune items still recorded above (voicing level matching, the
horn attenuator) — the preset gain staging and the High Bias settling thump
were reclassified as defects and fixed (issue #34 items 1 and 4). Neither
remaining item is a ship blocker; both are one-line changes if Yves wants them.
