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
| State round-trip | Every parameter of the full set survives save → load; v2 state round-trips with all 51 parameters | `tests/StateTests.cpp`, `tests/StateMigrationTests.cpp` |
| State migration | v0.1 and v0.2 sessions migrate deterministically; explicit user choices are never overwritten | `tests/StateMigrationTests.cpp` |
| Preset round-trip and rejection | Save → load restores every parameter; foreign-plugin and wrong-format presets are refused; every factory preset parses, loads and is in range | `tests/PresetManagerTests.cpp` |
| `reset()` clears stage state | Silence right after a loud signal is actually silent | `tests/ResetTests.cpp` |
| No zipper noise on fast automation | Automation ramp assertions | `tests/RobustnessTests.cpp` (`[automation]`) |
| Aliasing budget | Alias-to-signal measurements per voicing and engine | `tests/AliasingTests.cpp` |
| Latency reporting | Reported latency matches the actual path, and is stable across engine switches | `tests/LatencyTests.cpp` |
| Cross-thread IR loading | Concurrent `prepare()` / `loadImpulseResponse()` stress | `tests/CrossThreadReprepareTests.cpp` |
| Release build is signed, notarized, stapled | Developer ID Application signing → `notarytool` → `stapler` on tag push | [`.github/workflows/release.yml`](../.github/workflows/release.yml) |

### Automation gaps worth closing before v1.0.0

Each is a test to write, not a manual step to perform. They are listed here so
the manual pass is not silently asked to cover them:

- [ ] **pluginval on the AU as well as the VST3.** CI validates the VST3 at
      strictness 10 and the AU only via `auval`. pluginval can validate the
      `.component` too.
- [ ] **A parameter-sweep null test.** Render at each parameter's extremes and
      assert the output is finite, bounded and free of discontinuities —
      currently covered per-stage, not end-to-end across the whole set.
- [ ] **An offscreen editor snapshot test**, once the M3 GUI exists (#45, #25,
      #26, #27, #28). It doubles as the source of `docs/gui-preview.png` —
      see [`docs/branding.md`](branding.md#gui-preview).
- [ ] **Automated accessibility assertions on the editor** (#46): every control
      exposes an accessible name and role, and the tab order is complete.
      Machine-checkable once the editor exists; the *feel* of keyboard and
      screen-reader use stays manual, below.

---

## Part 2 — manual, needs a person

This is the actual gate. It is Yves' call, and no CI result substitutes for it.

Run each DAW protocol on a release build of the tagged candidate — the
signed, notarized artefact from the release workflow, installed the way a user
would install it, not a local `build/` output.

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

- [ ] **Gnaw** approved by ear.
- [ ] **Wool** approved by ear.
- [ ] **Razor** approved by ear.
- [ ] **Circuit** vs. **Classic** drive engine: Circuit is the default for new
      instances — confirmed as the better default.
- [ ] **Smooth RMS** low-band detector: no audible tremolo on sustained low
      notes.
- [ ] **Modern** gate: chugs cut cleanly, no chatter, no swallowed attacks.
- [ ] Every factory preset is musically usable on a real bass DI, not just
      in range.

### GUI and accessibility (once M3 lands)

- [ ] Every control reachable and operable by keyboard alone.
- [ ] VoiceOver announces a meaningful name and value for every control.
- [ ] The editor is legible at the smallest and largest supported scale.
- [ ] Metering matches what the ear and the DAW's own meters report.

---

## Sign-off

`v1.0.0` is tagged only after both halves are complete. Part 1 signs itself off
by being green on `main`; Part 2 is signed off by Yves, and by nobody and
nothing else.

| | |
|---|---|
| Candidate build | |
| Part 1 green on `main` (commit) | |
| Part 2 signed off by | |
| Date | |
