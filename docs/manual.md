<p align="center"><img src="assets/icon.png" alt="Crypta icon" width="120"/></p>

# Crypta — User Manual

*Split your bass. Compress the lows. Twist the guts out of the highs.*

## What it is

Crypta is a **parallel bass processor** built for metal production, in the lineage of the reference plugin class for parallel-bus bass processing. As of v0.2.0 it splits your bass signal into **three** bands — low, mid, and high — with two cascaded 4th-order Linkwitz-Riley ("LR4") crossovers, keeps the low band tight with a parallel compressor, drives the mid band with staged saturation, and runs the high band through a choice of three distortion voicings before summing everything back together through a 4-band EQ and a cabinet-simulation IR loader.

### Research-derived rebuild (v0.2.0)

v0.2.0 is a research-driven rewrite of v0.1.x's simpler two-band (low/high) topology, sourced against the reference plugin class's own official user manual, a third-party professional review, the same design lineage's hardware product manual, and general parallel-bus-compression community/vendor consensus — **not measured against any reference plugin's actual audio output, DSP source code, or hardware unit by this project.** See `docs/design-brief.md` and `docs/research-notes.md` for the full sourcing, and the same disclosure v0.1.x already carried: voicing character (drive-gain ranges, mid-filter hump/scoop settings) is engineering-tuned, not yet finalized by ear against reference material.

### Where it sits in a heavy-music chain

Crypta is designed to be the **bass-specific voicing stage** in the Basilica Audio suite:

- Track order: **DI/amp sim → Crypta → bus compression/glue → mix bus**. It expects a reasonably clean, already-amp-sim'd or DI'd bass signal; it is not itself a full amp sim (no built-in preamp gain staging beyond the input trim and drive controls).
- The low band's parallel compressor is meant to keep the fundamental/sub content of the bass locked in place under a wall of distorted guitars. The mid band adds a distinct "throatier" saturation character that sits in the frequency range most likely to clash with a guitar wall — dial it in deliberately, not just as an afterthought. The high band's voicing adds the upper-mid/high "grind" that lets the bass cut through a dense mix.
- Two independent split points (**Split Low**, **Split High**) let you tune both crossover corners across the low-mid register to match the song's tuning (drop-tunings push useful low-end content further up) and to control how wide the mid band's "throat" is.
- The output stage's IR loader — now applied only to the Mid+High path, never the Low band — is meant for quick cabinet-style tone shaping without needing a separate cab-sim plugin later in the chain, though it can also be left off entirely if you're already running a dedicated cab sim elsewhere.

## Signal flow

```
Input Trim → Gate → LR4 Split Low (60–400 Hz, default 120 Hz)
                      │
        ┌─────────────┴───────────────────────────────┐
        │                                              │
     Low band                              Remainder → LR4 Split High (300–2000 Hz, default 600 Hz)
 Parallel Comp → Graaawl → Level                         │
        │                          ┌───────────────────┴───────────────────┐
        │                       Mid band                              High band
        │                    Drive → Level          Tight → Voicing → Drive → Tone → Blend → Level
        │                          └───────────────────┬───────────────────┘
        │                                          Mid+High sum
        │                                                │
        │                                          IR loader (cab sim)
        │                                                │
        └───────────────────────┬────────────────────────┘
                                 │
                       Sum (delay-compensated)
                                 │
                            4-band EQ
                                 │
                       Safety Clip (optional)
                                 │
                               Output
```

The Mid and High bands share the same oversampling anti-aliasing headroom (each 4x oversampled independently, but identically configured, so they report identical latency); the low band carries a matching compensation delay, plus a phase-alignment allpass filter tied to Split High's own cutoff, so all three bands sum flat and stay time-aligned at the final sum. The IR loader (cabinet simulation) sits **after** the Mid+High sum and **before** the final three-way sum — the Low band never passes through it, matching the reference class's "low band bypasses the cabsim" architecture. See [`docs/architecture.md`](architecture.md) for the full technical breakdown, including exactly how the latency and phase-alignment compensation work.

### Reported latency

Crypta is not zero-latency: oversampling is its source. **Reported latency is a function of the sample rate alone, identical across both Drive Engines at every rate** (44.1 / 48 / 96 / 192 kHz), and independent of host block size (`tests/LatencyTests.cpp`, "T14"). This is deliberate: the two engines can have different intrinsic latencies (Circuit's oversampling factor adapts to the host rate — 4x at or below 50 kHz, 2x at or below 100 kHz, 1x above; Classic is always 4x), so rather than re-report latency whenever you automate **Drive Engine** — which hosts handle poorly mid-transport — the plugin reports the maximum across both engines and pads the Circuit path up to it. Switching Drive Engine therefore never shifts your track by a sample.

One consequence worth knowing: the *reported* figure is the oversampling delay, not a full group-delay description. On Classic, a test impulse peaks within 1 sample of the reported latency; on Circuit it peaks within 32 samples, because the two extra IIRs Circuit's High band carries (the 10 Hz DC blocker and the drive-tracked pole) move the impulse's peak by up to roughly 25 samples (about 0.5 ms) without changing what gets reported. No single number can describe a frequency-dependent group delay - what is guaranteed instead is that the reported figure matches across engines and rates, and that the three-way band sum stays flat (≤ 1.0 dB deviation) at every split-frequency combination tested.

No absolute sample count is pinned as a constant in this manual - check your host's own plugin-delay-compensation readout for the exact figure at your session's sample rate.

## Presets

Crypta ships with a preset system: a horizontal bar at the top of the plugin window lets you step through factory and user presets (`<` / preset name / `>`), save/save-as/delete your own, and import/export single presets or preset banks (zip files of multiple presets). Nine factory presets ship in v0.2.0 — see `docs/presets.md` for what each one demonstrates. User presets are stored per-plugin under:

- **macOS**: `~/Library/Audio/Presets/Yves Vogl/Crypta/`
- **Windows**: `%APPDATA%\Yves Vogl\Crypta\Presets\`

A fresh instance loads a user "Default" preset if you've saved one ("Set current as default" in the preset menu), otherwise the factory "Default" preset (matching the plain parameter defaults documented below).

## The interface

The plugin window is laid out **in signal order**, left to right and top to bottom, in ten sections: Input, Noise Gate, Crossover, Low Band, Drive Engine, Mid Band, High Band, Cabinet, EQ, Output. Whatever you see further right or further down happens later in the chain. Every one of the plugin's 54 parameters is on the front panel — nothing is hidden behind a menu.

**Knobs.** Drag up/down or left/right to turn; hold **Shift** while dragging for fine adjustment. Knobs with a fixed set of positions (Gate Mode, Low Comp Detector, Drive Engine, High Voicing) click into their detents and show the option's name. Double-click a value box to type an exact number.

**Keyboard.** Every control is reachable with **Tab**, in signal order — and Tab never gets stuck inside one section. With a knob focused:

| Key | Step |
|---|---|
| ← / → / ↑ / ↓ | 1 % of the knob's range (choice knobs: one position) |
| Shift + arrow | fine — 0.1 % of the range |
| Page Up / Page Down | 10 % of the range |
| Home / End | minimum / maximum |

Toggles (the lamp switches) respond to **Space** and **Return**. Ctrl/Cmd-modified arrows are left alone, so your host's own shortcuts keep working. The focused control is always marked with a gold ring.

**Screen readers.** Every control announces its name, its role and its current value *with units* ("Gate Threshold, slider, −60.00 dB"), and each section is announced as a named group. The four meters can be queried on demand and read out their current value ("Input peak level meter, −8.2 dBFS"); they deliberately do not interrupt you on every update.

**Meters.** Four needle meters read the metering added in v0.3.0:

| Meter | Scale | Reads |
|---|---|---|
| IN | −60…+6 dBFS | Input block peak, before anything else |
| GATE | 0…20 dB | How much the noise gate is currently pulling down |
| COMP | 0…20 dB | How much the low band's parallel compressor is pulling down |
| OUT | −60…+6 dBFS | Output block peak, after everything |

The peak meters rise fast and fall slowly, so a transient is actually visible, and the scale from 0 dBFS up is engraved in red — the needle turns red there too, but the position alone already tells you. Gain-reduction needles rest at the right and swing left as reduction deepens. All metering runs on the interface thread; it costs the audio engine nothing.

**Resizing.** Drag the grip in the bottom-right corner. The window keeps its aspect ratio and scales between 60 % and 180 % — it is a pure zoom, so nothing re-flows or gets clipped at any size. **The size you choose is saved with the session** and with the plugin instance, so a reopened project comes back the way you left it. A project saved before this version opens at 100 %.

## Engines (NEW in v0.3.0)

Three parameters select between the v0.2.0 DSP and its v0.3.0 replacement. A **new** instance boots into the new engines; **any session or preset you saved before v0.3.0 keeps the old ones**, so nothing you have already made changes how it sounds. Switch freely — the change is crossfaded, not stepped.

| Parameter | Options | Default (fresh instance) | What changes |
|---|---|---|---|
| Drive Engine | Classic / Circuit | Circuit | *Classic* is the v0.2.0 Mid and High band exactly. *Circuit* rebuilds both from circuit models, with far less aliasing (25–30 dB better) and per-voicing pre/post-emphasis networks. |
| Low Comp Detector | Classic Peak / Smooth RMS | Smooth RMS | *Classic Peak* is the v0.2.0 detector. *Smooth RMS* measures over a window longer than one bass cycle, so the low band stops tremoloing on sustained low notes. |
| Gate Mode | Classic / Modern | Modern | *Classic* is the v0.2.0 gate. *Modern* adds hysteresis, hold, a detector-only sidechain highpass and a straight-line release. |

**If you preferred the old sound**, set the relevant engine to Classic — it is preserved exactly, not approximated.

## Parameter reference

Unless noted otherwise, all continuous parameters are smoothed to avoid zipper noise when automated.

### IO / Global

| Parameter | Range | Default | Unit | What it does |
|---|---|---|---|---|
| Input Gain | −24 … +24 | 0 | dB | Trims the signal before anything else in the chain. Use this to get a hot but not clipping signal into the gate/compressor/drive/voicing stages - all of their thresholds are calibrated assuming a reasonably "line level" input. |
| Output Gain | −24 … +24 | 0 | dB | Final output trim, applied after everything else (including the safety clip). The control's own default is 0 dB, but a fresh instance starts on the factory *Default* preset, which sets −2.8 dB — the derived trim that keeps a −12 dBFS bass DI under full scale (every factory preset carries such a trim where it needs one). |
| Bypass | off/on | off | — | Forces a bit-exact passthrough of the input signal. Also exposed as the plugin's host-facing bypass parameter, so your DAW's own bypass button/automation lane works too. |
| Safety Clip | off/on | off | — | A soft ceiling clip on the very last stage before the output trim. Off by default; turn it on as a safety net against accidental hard-clipped overs, not as a tone-shaping tool. As of v0.3.0 it is antialiased and is genuinely transparent below the ceiling — arming it no longer colours anything until something actually reaches the ceiling. |
| Clip Ceiling | −12 … 0 | 0 | dBFS | Where the safety clip starts working. Only read while Safety Clip is on. 0 dBFS reproduces the v0.2.0 behaviour. |

### Noise Gate (full-band, before the crossover splits)

Sits ahead of both crossovers, so it gates the input signal as a whole rather than per band.

**Gate Ratio is Classic-only.** Modern is a gate with a range floor rather than a ratio expander, and ignores it.

| Parameter | Range | Default | Unit | What it does |
|---|---|---|---|---|
| Gate Enable | off/on | **off** | — | Enables the gate. Off by default - most already-tracked bass DI/amp signals don't need one, and an incorrectly-set gate can chop off legitimate low-level playing (ghost notes, decays). |
| Gate Threshold | −80 … 0 | −60 | dB | Signal level below which the gate starts attenuating. |
| Gate Ratio | 1 … 20 | 10 | :1 | How aggressively the gate attenuates once below threshold. Higher = closer to a hard mute. |
| Gate Attack | 0.1 … 50 | 1 | ms | How fast the gate opens once the signal crosses back above threshold. |
| Gate Release | 5 … 500 | 100 | ms | How fast the gate closes once the signal drops below threshold. |
| Gate Hysteresis | 0 … 12 | 4 | dB | *Modern only.* How far below the threshold the signal must fall before the gate starts closing. This is what stops a note decaying through the threshold from making the gate stutter. |
| Gate Hold | 0 … 500 | 20 | ms | *Modern only.* Keeps the gate open for this long after the signal drops, and restarts on each new transient — so it does not slam shut between fast notes. |
| Gate SC Highpass | 20 … 400 | 80 | Hz | *Modern only.* Highpasses the gate's **detector** only; the audio is untouched. Raise it if a ringing low string is holding the gate open. |
| Gate Range | 6 … 90 | 60 | dB | *Modern only.* How far a fully closed gate attenuates, and the height the release ramp falls through. A gate that only ducks 12 dB often sounds more natural than one that shuts completely. |

### Split Low / Split High (two cascaded crossovers, NEW topology in v0.2.0)

| Parameter | Range | Default | Unit | What it does |
|---|---|---|---|---|
| Split Low | 60 … 400 | 120 | Hz | The LR4 split point between the Low band and everything above it. Log-scaled control. Lower it to push more of the fundamental into the (compressed-only) Low band; raise it to give the Mid band more low-mid content to work with. |
| Split High | 300 … 2000 | 600 | Hz | The LR4 split point between the Mid band and the High band. Log-scaled control. |

Split High is always kept at least a fraction of an octave above Split Low internally (a reasoned safety margin against a degenerate near-zero-width Mid band) — if you push the two close together, Split High's *effective* value will float slightly above whatever you've set Split Low to, rather than collapsing the Mid band to nothing.

### Low band: parallel compressor + level

The low band is compressed **in parallel**: the compressed signal is blended back with its own uncompressed self via Mix, rather than replacing it outright, which is what keeps the low end feeling tight and controlled without ever sounding squashed or lifeless. **v0.2.0 re-sources the ballistics defaults** to the reference class's own fixed, sourced values — a fast, gentle "glue" bus compressor, not the heavier "New York style" squash v0.1.x's defaults implied (see `docs/research-notes.md` §3–4 for the full sourcing).

| Parameter | Range | Default | Unit | What it does |
|---|---|---|---|---|
| Low Comp Threshold | −60 … 0 | −18 | dB | Level above which the low-band compressor engages. |
| Low Comp Ratio | 1 … 20 | **2** | :1 | Compression ratio above threshold. |
| Low Comp Attack | 0.1 … 100 | **3** | ms | How fast the compressor clamps down once above threshold. |
| Low Comp Release | **5** … 1000 | **6** | ms | How fast the compressor lets go once back under threshold. Range floor lowered from 10 ms in v0.1.x so the sourced 6 ms default is reachable. |
| Low Comp Makeup | −12 … +24 | 0 | dB | Gain applied to the compressed (wet) signal before it's blended back with the dry low band - use this to bring the compressed signal back up to match the dry level, so Mix behaves as a true "how much compression character" control rather than also changing overall loudness. |
| Low Comp Mix | 0 … 100 | 100 | % | Blend between the dry (uncompressed) and wet (compressed + makeup) low band. 0% = compressor has no audible effect; 100% = fully compressed. |
| Low Level | −24 … +12 | 0 | dB | Level trim on the low band, applied after compression and before the bands are summed back together. |
| Low Comp Knee | 0 … 18 | 6 | dB | *Smooth RMS only.* Width of the soft knee around the threshold. 0 dB is a hard knee; wider settings start compressing gradually as the signal approaches the threshold, which is much less obvious on sustained bass. |
| Low Comp Auto Release | off/on | on | — | *Smooth RMS only.* Stretches the release on sustained material while leaving it at the set value for transients, so a held low note is not pumped. |
| Low Comp Auto Makeup | off/on | off | — | Read by **both** detectors. Adds a fixed boost that compensates roughly half the gain the compressor takes away at the threshold, so changing the threshold does not also change your level. Summed with the manual Makeup control. |

The gain reduction both detectors apply is published to the meters (and to any UI that asks for it) in positive dB. On **Smooth RMS** the figure is the detector's own gain; on **Classic Peak** it is estimated from the block's peak either side of the compressor, because `juce::dsp::Compressor` does not expose its internal gain — accurate on the steady material a meter is read against, worst-case-per-block on fast transients.

### Low band: Graaawl (NEW in v0.4.0)

**Graaawl** puts the woody, vocal, aggressive grind of a Warwick Thumb onto your low end without making it flabby. Off by default.

It is not low-frequency distortion, and that distinction is the whole design. The Thumb's growl is an **asymmetric upper-mid character around 700 Hz – 2.2 kHz**, with a formant-like resonance near 1 kHz, sitting on top of a low end that stays tight. So Crypta generates that harmonic content in a **parallel branch** — an asymmetric saturator followed by a band-pass onto the formant window — and blends it on top of the untouched low band. Your fundamental never goes through a shaper. Measured on a 50 Hz probe with Amount at 100 %: the energy in the 20–150 Hz sub region changes by **less than 0.01 dB**, while the 700 Hz – 2.2 kHz window gains real signal.

It sits **after** the low-band compressor and before Low Level, so the compressor has already made the input level predictable and the Amount control means the same thing from note to note.

| Parameter | Range | Default | Unit | What it does |
|---|---|---|---|---|
| Graaawl | off/on | off | — | Enables the growl branch. Switched off it is a bit-exact bypass — not "almost transparent", literally the same samples. |
| Graaawl Amount | 0 … 100 | 0 | % | How much growl is blended on top of the low band. A linear gain on the harmonic branch, so halving it is −6 dB. Level-dependent by design: the growl shows up when you dig in. |
| Graaawl Tone | 0 … 100 | 50 | % | Moves the formant window: 800 Hz at 0 %, ~1130 Hz at 50 %, 1600 Hz at 100 %. Lower is woodier and louder (low harmonics carry more energy), higher is more nasal and more restrained. |

The branch is antialiased arithmetically (ADAA-1) rather than by oversampling, which is why **Graaawl adds no latency at all** — switching it on mid-session can never shift your track. Measured alias-to-signal on a hot 50 Hz probe at 100 % Amount: **−86 dB at Tone 0, −105 dB at Tone 100**.

### Mid band: drive + level (NEW in v0.2.0)

A dedicated mid band with staged/cascaded saturation, structurally similar to the High band's Wool voicing (two cascaded soft-clip stages) but with no filter, tone, or blend control of its own — matching the reference class's own Mid band exactly ("Mid Drive... Mid Level" only). This band's job is a distinct "throatier" grind character, separate from the High band's own presence/fuzz/harshness-control role.

| Parameter | Range | Default | Unit | What it does |
|---|---|---|---|---|
| Mid Drive | 0 … 100 | 30 | % | Staged saturation amount. 0% is an exact passthrough; increasing it blends progressively toward a fully cascaded-tanh-driven signal. |
| Mid Level | −24 … +12 | 0 | dB | Level trim on the mid band, applied after drive and before the bands are summed back together. |

### High band: Tight, voicing, drive, tone, blend, level

Three selectable distortion voicings, each 4x oversampled to keep the nonlinear shaping stage's aliasing out of the audible band.

| Parameter | Range | Default | Unit | What it does |
|---|---|---|---|---|
| High Tight | 20 … 500 | 100 | Hz | **NEW in v0.2.0**: a pre-drive high-pass filter, now applied ahead of *every* voicing (was a Razor-only fixed 200 Hz internal constant in v0.1.x). This is the primary "how much fuzz vs. tightness" control for the whole High band, and also tames harshness on high-drive settings - pull it down toward its floor for maximum fuzz, push it up for a tighter, more controlled top end. |
| High Voicing | Gnaw / Wool / Razor | Gnaw | — | Selects the distortion character. See below. |
| High Drive | 0 … 100 | 50 | % | How hard the signal is pushed into the selected voicing's nonlinearity. |
| High Tone | 0 … 100 | 50 | % | Post-shaper tone control: a low-pass sweeping from dark (0%) to bright (100%), tucking away or opening up fizz/harshness from the distortion stage. |
| High Blend | 0 … 100 | 100 | % | Blend between the clean (pre-voicing) and fully distorted high band. 0% = clean high band (voicing has no audible effect); 100% = fully distorted. |
| High Level | −24 … +12 | 0 | dB | Level trim on the high band, applied after voicing/blend and before the bands are summed back together. |
| High Bias | 0 … 100 | 0 | % | *Circuit only.* Offsets the clipper so it saturates asymmetrically, buying even-order harmonics: on the symmetric voicings, H2/H1 rises by at least 20 dB going from bias 0 to bias 100 (`tests/CircuitDriveTests.cpp`, "T4"). 0 % is exactly the v0.2.0 character. The offset is removed again downstream by a 10 Hz blocker, so this never puts meaningful DC on your output (measured ≤ −80 dBFS at every voicing and bias setting). |

**Voicings:**

All three keep their v0.2.0 placement — Gnaw flat, Wool a −6 dB/Q 0.9 scoop at 500 Hz, Razor a +5 dB/Q 1.0 hump at 900 Hz — but on the *Circuit* engine the nonlinearity and the filtering around it are rebuilt from circuit models rather than being three settings of one curve (`src/dsp/CircuitDrive.cpp`).

- **Gnaw** — *Classic*: an op-amp-style hard clip, symmetric and unforgiving, pushing hard into a square-ish waveform at high drive. *Circuit*: the same character, gained through a pre-emphasis shelf (1200 Hz, +6 dB) ahead of the clipper and its exact algebraic inverse behind it — the pairing collapses to unity at Drive 0 *structurally*, not approximately, and concentrates clipping on the upper harmonics instead of the whole band.
- **Wool** — *Classic*: cascaded soft-clip fuzz with a mid scoop and a touch of fixed asymmetry. *Circuit*: the asymmetric diode shunt clipper's own DC curve — two series diodes one way, one the other, solved from a small-signal-diode SPICE card by damped Newton at every table point, at prepare time, never on the audio thread — plus a dynamic bias side chain that leaves the clipper offset for roughly 20 ms after a loud passage. This makes Wool genuinely history-dependent: the same quiet probe measures at least 3 dB differently depending on what played immediately before it, against under 1.5 dB for the memoryless voicings (`tests/CircuitDriveTests.cpp`, "T5"). **Note:** the shipped behaviour is a decaying *bloom* after a loud passage, not a *sag* — the opposite sign to this feature's original design prediction. See [Known limitations](#known-limitations) below; describe Wool as history-dependent / touch-sensitive, not as "sag."
- **Razor** — *Classic*: a tighter overdrive — a comparatively mild clipper with a mid-hump filter afterwards. *Circuit*: rebuilt as a feedback-clipper factorisation ("unity clean + clipped difference"), with the clipped-path pre-emphasis corner moved from a guitar-pedal original's 720 Hz down to 330 Hz for the bass register, and a softer-kneed tanh-fit clipping curve that better matches the modelled topology.

Gnaw and Razor also share a drive-tracked post-clip pole on Circuit: it opens to 61 kHz at Drive 0 (transparent) and slides down to 5.1–6.9 kHz at full drive, moving continuously in between rather than sitting at a single fixed corner.

*Starting points, not final voicing:* the drive-gain ranges and mid-filter hump/scoop settings for the Classic engine, and the Circuit voicing constants alike, are engineering defaults - tuned for musical usefulness and mathematically bounded (no runaway output at any drive setting), not yet finalized by ear against reference material. The suite's open ear-tuning gates (issues #15/#16/#17, #34) still apply to the Circuit engine specifically.

### Post-sum 4-band EQ

Applied after all three bands are summed back together (and after the IR loader). Off by default; when off, the EQ stage is skipped entirely (guaranteed transparent, not just set to unity gain). **v0.2.0 re-anchors the default corner frequencies** to a sourced bass-tone-stack frequency set from the same design lineage as the reference class (v0.1.x's defaults were unsourced placeholders).

| Parameter | Range | Default | Unit | What it does |
|---|---|---|---|---|
| EQ Enable | off/on | off | — | Enables the EQ stage. |
| EQ Low Shelf Frequency | 40 … 400 | **80** | Hz | Low shelf corner frequency. |
| EQ Low Shelf Gain | −18 … +18 | 0 | dB | Low shelf boost/cut. |
| EQ Peak 1 Frequency | 100 … 2000 | 500 | Hz | First parametric peak band's centre frequency. |
| EQ Peak 1 Gain | −18 … +18 | 0 | dB | First peak band's boost/cut. |
| EQ Peak 1 Q | 0.2 … 5.0 | 0.7 | — | First peak band's bandwidth (higher = narrower). |
| EQ Peak 2 Frequency | 500 … 8000 | **2800** | Hz | Second parametric peak band's centre frequency - a "presence/definition" high-mid anchor. |
| EQ Peak 2 Gain | −18 … +18 | 0 | dB | Second peak band's boost/cut. |
| EQ Peak 2 Q | 0.2 … 5.0 | 0.7 | — | Second peak band's bandwidth. |
| EQ High Shelf Frequency | 2000 … 16000 | **5000** | Hz | High shelf corner frequency. |
| EQ High Shelf Gain | −18 … +18 | 0 | dB | High shelf boost/cut. |

### IR loader (cabinet simulation)

A convolution-based cab-sim stage that now processes **only the Mid+High post-sum signal** (relocated in v0.2.0 - it sat post-everything in v0.1.x). Off by default. With no impulse response loaded, this stage is a guaranteed bit-exact passthrough at every session sample rate, so turning it on before loading an IR never changes your sound. The Low band is structurally never passed through this stage, matching the reference class's "low band bypasses the cabsim" architecture - your fundamental/sub content stays uncolored regardless of what cab IR you load.

| Parameter | Range | Default | Unit | What it does |
|---|---|---|---|---|
| IR Enable | off/on | off | — | Enables the IR loader stage. |
| IR Mix | 0 … 100 | 100 | % | Blend between the dry (pre-convolution) and fully convolved Mid+High signal. |

*Loading impulse responses:* the IR-loading engine and its **factory-IR slot mechanics** are complete — decoding an embedded WAV, installing it, and clearing back to the passthrough identity IR are all implemented and tested (`getNumFactoryImpulseResponses()` / `loadFactoryImpulseResponse()` / `clearImpulseResponse()`). The Cabinet section of the editor currently exposes the two parameters above and nothing else: there is **no in-plugin file browser yet**, and a host or wrapper that calls the plugin's IR-loading entry point works today.

What is **not** shipped is content: **Crypta bundles no cabinet IRs**, and that is a deliberate licensing decision rather than an unfinished feature. Bundling a cab IR means redistributing someone else's recording inside every copy of the plugin, so the bar is a licence that is beyond doubt — CC0, an explicit public-domain dedication, or our own capture. "Free download" and "royalty free" are not licences. Nothing has cleared that bar yet; the bar itself is enforced in code (`src/dsp/FactoryIRs.h`) and asserted by the test suite, so an unverified IR cannot slip into a release build. Until then, load your own IRs — the stage is a guaranteed bit-exact passthrough with nothing loaded, at every session sample rate.

## State migration

### v0.2.x → v0.3.0

Opening a session or loading a preset saved before v0.3.0 sets **Drive Engine**, **Low Comp Detector** and **Gate Mode** to their Classic values, so your saved work sounds exactly as it did. This applies to host sessions and to your own saved presets alike, including a user preset named "Default" — so if you had saved your own default, that is still what you get on a new instance.

The one deliberate exception is the **safety clip**: if you had it switched on, v0.3.0's clipper is not bit-identical to v0.2.0's. It aliases far less and is transparent below the ceiling; the difference is confined to material that was actually being clipped.

New parameters (High Bias, the knee and auto controls, the Modern gate's controls, Clip Ceiling) are either neutral by default or are only read when their engine is selected, which legacy state never selects.

### v0.1.x → v0.2.0

If you open a Crypta v0.1.x session, the old single `Crossover Frequency` value is migrated to the new **Split High** parameter, clamped into its new 300–2000 Hz range (v0.1.x's own shipped default, 250 Hz, is below that floor, so an untouched v0.1.x session lands exactly at 300 Hz on reopen). Split Low and every new Mid-band/Tight parameter fall back to their v0.2.0 defaults. Any low-band compressor settings you had explicitly changed away from v0.1.x's old defaults are preserved as-is — only the *shipped default* changed, not your own deliberate settings. This is a best-effort, lossy, one-directional migration; re-check your low/mid/high balance after reopening an old session.

## Under the hood

Everything below is measured in-tree and runs on every CI push (macOS + Windows, Release, `.github/workflows/ci.yml`, plus `pluginval --strictness-level 10` on both platforms and `auval -strict` on macOS). See `docs/architecture.md` for the full derivations.

### One shared oversampling region, not two

v0.2.0 ran **two** independent 4x oversampling instances — one inside the Mid band, one inside the High band. The Circuit engine collapses them into **one**: the remainder is upsampled once, split by a second Linkwitz-Riley crossover running *at the oversampled rate*, processed there, summed and downsampled once (`src/dsp/CircuitDrive.{h,cpp}`). The saved region pays for the extra per-voicing filtering. The oversampling factor adapts to the host rate — 4x at or below 50 kHz, 2x at or below 100 kHz, 1x (ADAA only) above — and that trade is tested, not assumed: 2x at 96 kHz measures at least as clean as 4x at 48 kHz, tone for tone (`tests/AliasingTests.cpp`, "T1: dropping to 2x at 96 kHz costs nothing measurable"). Both engines stay prepared at all times, so `Drive Engine` is automatable; a switch crossfades over 256 samples (5.3 ms at 48 kHz) and flushes the incoming engine's state first, because the idle engine's oversampling history otherwise releases a stale-audio burst on switch-back (measured: a 1.96 peak before the flush existed, 1.39 after, `tests/CircuitDriveTests.cpp`, "T18").

### Antialiasing done arithmetically, on top of oversampling

`src/dsp/ADAAShaper.h` implements first-order antiderivative antialiasing (Parker, Zavalishin & Le Bivic, DAFx-16): closed forms where the antiderivative is elementary (`tanh` → `ln cosh`, hard clip → piecewise), and a 2048-point cubic-interpolated table with a Simpson-integrated antiderivative where it is not, so the curve and its integral stay mutually consistent — pinned directly by a test, because a mismatch would show up as a DC step under overload rather than as a small error (`tests/AliasingTests.cpp`, "T2"). Measured result: the Circuit engine beats the engine it replaces by **25–30 dB** across a 1.2–10 kHz sweep at full drive (recorded: Circuit −81.9 / −64.0 / −57.1 / −51.9 dB vs. Classic −48.0 / −36.2 / −27.9 / −22.7 dB at 1244 / 2489 / 4978 / 9956 Hz), and clears **−80 dB or better through the whole bass register** (311, 622, 1244 Hz) (`tests/AliasingTests.cpp`, "T1").

### Per-voicing circuit topologies

See the Voicings note above the parameter tables for what changed in Gnaw, Wool and Razor. The drive-tracked post-clip pole Gnaw and Razor share uses a square-law (audio-taper) mapping rather than a linear one, specifically so the pole stays above 12 kHz at half drive instead of collapsing to roughly 9 kHz and making the middle of the drive range audibly duller than the circuit being modelled (`tests/CircuitDriveTests.cpp`, "T3"). Every automatable scalar in the Circuit engine — drive gains, blend, bias offset, band levels, and the two one-pole filter coefficients themselves — is ramped across the block rather than held constant across it; a per-block-constant parameter measured as roughly −27 dBc of broadband non-harmonic spurs on a fast drive sweep before this existed (`src/dsp/CircuitDrive.h`, `RampedScalar`).

### A low-band detector that stops the bass tremoloing

v0.2.0's low band used a peak detector with the sourced 6 ms "glue" release, and a 6 ms release follows the individual half-cycles of a bass fundamental, so the gain reduction rippled at twice the note frequency. `Low Comp Detector = Smooth RMS` (`src/dsp/LevelDetector.h`) is a log-domain RMS detector with a soft knee, program-dependent release and auto-makeup. Measured ripple on an 80 Hz tone 6 dB over threshold: **over 1 dB → under 0.5 dB**, while still genuinely compressing (`tests/LevelDetectorTests.cpp`, "T7"). The auto-makeup sign is worth knowing: `−0.5·T·(1 − 1/R)` with a negative threshold gives a positive gain — the easy slip (`−0.5·T·(1/R − 1)`) produces attenuation instead, which the test suite pins at three anchors to 0.1 dB, plus an assertion that the result is a boost at all ("T8").

### A gate with hysteresis, and the test that proves it

`Gate Mode = Modern` (`src/dsp/GateEngine.h`) runs its control path per sample and adds hysteresis (0–12 dB), retriggering hold (0–500 ms), a detector-only sidechain highpass (20–400 Hz) and a dB-linear release. The cleanest proof point: a 500 Hz tone dithering ±1.5 dB around the threshold at 3 Hz for two seconds — a single-threshold gate chatters continuously through that; with a 4 dB hysteresis window this one opens once and then makes **exactly zero transitions** for the rest of the render (`tests/GateEngineTests.cpp`, "T11"). The release is fit by least squares against the ideal `−range/release` slope at R² > 0.99. `Gate Ratio` is Classic-only; Modern ignores it — it is a gate with a range floor, not a ratio expander.

### A safety clip that is transparent until it isn't

The v0.2.0 safety clip was a raw per-sample `std::tanh` on the full mix, which lowpassed everything whenever it was merely armed. `src/dsp/OutputClipper.h` applies ADAA to the clipper's **residual** instead of the whole signal — algebraically the antialiased clipper plus an exact compensator for the droop and delay that naive ADAA would otherwise apply to material nowhere near the ceiling. Measured deviation across 40 Hz – 20 kHz while armed but not clipping: **0.13 dB** (`tests/OutputClipperTests.cpp`, "T13"). One documented wrinkle: the compensator is a first difference and can push a sample back over the ceiling on fast material (measured 1.15 against a ceiling of 1.0), so a final hard bound is applied - it never engages below the ceiling, so transparency below it is untouched.

### Nothing you already made changes

A fresh instance boots into Circuit / Smooth RMS / Modern; every pre-v0.3.0 session and preset gets the legacy engines injected, through two independent migration paths because presets never pass through `setStateInformation()` at all (see State migration, above). The proof is against real committed output, not just the migration code: `tests/GoldenRenderTests.cpp` renders four committed v0.2.0 state fixtures (Gnaw / Wool / Razor / Gnaw-with-clip) and asserts **sample-exact equality (`memcmp == 0`) on macOS**, the golden platform; on Windows the bar is ≤ −60 dB relative, with the worst of three fixtures measured at −73 dB. The fixtures are asserted to carry no `stateVersion` attribute, so a regenerated fixture cannot silently turn the migration test into a tautology.

### Engineering hygiene

Zero heap allocations on the audio thread, on **both** engines, with every v0.3.0 feature live simultaneously — Circuit drive, Modern gate, Smooth RMS detector with auto-makeup, the safety clip and the EQ (`tests/RobustnessTests.cpp`, `[realtime]`). Reported latency is a function of sample rate alone (see Reported latency, above). `reset()` flushes every stage (crossover memory, gate/compressor envelopes, oversampling state, the latency-compensation delay line, EQ biquad history, the convolution engine), so a host transport stop/loop/rewind never leaves stale state ringing into whatever plays next (`tests/ResetTests.cpp`). The low band is structurally excluded from the cab-sim: its own isolated output is bit-exact identical whether the IR loader is on or off, regardless of which IR is loaded (`tests/LowBandIsolationTests.cpp`).

## Known limitations

- **Wool's dynamic bias has the opposite sign to what the original design predicted.** The design brief expected a loud passage to suppress ("sag") the following quiet probe; what shipped is a decaying **bloom** instead. The mechanism is understood and is faithful analogue behaviour: the bias makes the clipping asymmetric, asymmetric clipping generates real DC, and the 10 Hz blocker downstream then restores it over its own ~16 ms constant, so the probe rides a bloom rather than a dip. History-dependence itself is confirmed as intended (11 dB on Wool against 1 dB on the memoryless voicings). This is flagged for the suite's open ear-tuning gate (issues #15/#16/#17, #34) - describe Wool as history-dependent / touch-sensitive, not as "sag."
- **The Circuit voicing constants remain engineering-derived starting points**, not yet finalized by ear against reference material - the same disclosure v0.1.x and v0.2.0 already carried for the drive-gain ranges and character-filter settings.
- **The alias floor is stated per tone, not as one flat −80 dB bar, and the reason is arithmetic.** Gnaw is a 40x hard clip whose harmonic series falls off as 1/n with no bandwidth limit, so the floor is set by which harmonic order folds back into the band, and that order drops as the fundamental rises. Raising the engine to 8x oversampling was measured and still misses −80 dB at the upper tones, while doubling the stage's cost. Delivered instead: 25–30 dB better than the previous engine on every tone (against a 10 dB brief requirement), −80 dB or better through the bass register, and a −49 dB in-band floor everywhere.
- **The two engines are not identical above 3 kHz at drive 0, and this is not claimed.** Parity holds to ±0.5 dB up to 3 kHz; above it Circuit is up to 2.5 dB brighter (measured +0.5 dB at 8 kHz, +2.5 dB at 14 kHz), because its tone lowpass runs at the oversampled rate and escapes the bilinear frequency warping the base-rate Classic filter has.
- **The drive-tracked pole opens to 61 kHz at drive 0**, not the 24 kHz the original design brief called for - a one-pole at 24 kHz is already about −1.9 dB down at 18 kHz and would not have met this feature's own transparency requirement, while 61 kHz (−0.36 dB at 18 kHz) is also the figure the circuit research gives for the real thing.
- **CPU is a design intent here, not a benchmark.** Collapsing two oversampling regions into one is what funds the extra per-voicing filtering, and the release notes state the trade as landing at roughly the CPU v0.2.0 spent. No CPU measurement ships in the test suite - do not put a percentage or an "x% lighter" figure anywhere.
- **The safety clip trades its antialiasing advantage for its ceiling guarantee at extreme overdrive.** The delta-form compensator can push a sample back over the ceiling on fast material, so a final hard bound is applied; it never engages below the ceiling, but roughly 10 dB or more past it the antialiasing advantage goes away. Deliberate priority ordering - a safety clip that lets material through is not a safety clip, and heavy clipping belongs to the drive stages, which are oversampled and ADAA'd for exactly that.
- **One deliberate departure from v0.2.0's output: the engaged safety clip.** If you had it switched on, v0.3.0 is not bit-identical - it aliases far less and is transparent below the ceiling. The difference is confined to material that was actually being clipped and is bounded (measured −26.5 dB relative on a fixture driven 12 dB past the ceiling). Everything else about a pre-v0.3.0 session or preset is sample-exact.
- **Cross-toolchain bit-exactness is unattainable and is not claimed.** macOS is the golden platform; on Windows the bar is −60 dB relative, with the worst of three fixtures measured at −73 dB. The drift is not last-ulp noise - the gate and the low-band compressor both take decisions off a detector level, so a 1-ulp difference near a threshold can shift a transition by a sample.
- **The GUI is a functional generic editor** plus a plain labelled meter readout row. The photoreal M3 GUI is a later milestone and will consume the same `MeterTaps` struct; the preset bar is a plain functional strip.
- **No bundled factory cabinet IRs, by decision.** The slot mechanics, the decoder and the licence guard all ship and are tested; the asset table is empty because no IR has been sourced with a licence worth staking a redistributed binary on. The in-plugin file browser is the GUI milestone's work. The convolution engine is a guaranteed bit-exact passthrough with nothing loaded, at every session sample rate.
- **Graaawl's voicing constants are engineering-derived starting points.** The shaper drive and asymmetry, the formant window and its resonance are chosen from what the growl demonstrably is, and every claim about them in this manual is a measurement — but the final voicing is an ear decision against a real Thumb 5 DI, and that gate (issue #36) is still open.
- **Graaawl's sub protection is band-limiting, not magic.** The branch's own harmonics leak through the formant highpass's stopband: on a pure 50 Hz sine the third harmonic at 149 Hz sits about 51 dB below the fundamental. In aggregate the 20–150 Hz region moves by under 0.01 dB, and real bass material carries its own third harmonic some 30 dB louder than that, so this is inaudible — but it is not zero, and it is not claimed to be.
- **Deliberately out of scope for v0.3.0, tracked openly:** factory IRs / IR browser / IR trim-align; the stereo strategy (low mono-sum toggle, Mid/High width); full per-sample Newton DK circuit simulation (v0.3.0 ships the calibrated factorised models; a full simulation is a possible later "HQ Circuit" mode); a lookahead gate and time-varying auto-release; linear-phase / HQ-offline oversampling modes and a shared suite-level oversampler module.
- **The voicing is research-derived, never measured against hardware or against any reference product's audio, DSP source, or unit.** Where a model is a deliberate simplification, the docs say so.
- **Pre-1.0 and AGPLv3** - breaking changes possible until v1.0.0. The v0.1.1 rename (plugin code `Cryp`, bundle id `com.yvesvogl.crypta`) means DAWs treat this as a new plugin relative to v0.1.0-era sessions.

## Tips

- **Start with the low band tight, then dial in the mid band's throat, then the high band's grind.** Set Low Comp Mix and Makeup first so the fundamental feels locked in, then dial in Mid Drive for the "throatier" cutting character, and only then pick a High voicing and drive amount.
- **Split Low and Split High are tone decisions, not just technical ones.** Pushing Split Low up moves more note body out of the (compressed-only) Low band; pushing Split High up widens the Mid band's own passband, giving the "throatier" character more room before the High band's own fuzz/presence character takes over.
- **High Tight is your main "fuzz vs. tightness" control**, independent of which voicing you've picked - pull it toward its 20 Hz floor for maximum fuzz, push it up toward 500 Hz for a tighter, more controlled top end. It also tames harshness on hot Drive settings.
- **High Blend is your "how much" knob, High Drive is your "how hard" knob.** If a voicing feels too extreme, try lowering Blend before lowering Drive - you'll often keep more of the character that way, just at a lower overall intensity, rather than flattening the nonlinearity itself.
- **Graaawl is a character layer, not a distortion amount.** Start at 30–40 % with Tone near the middle and listen for the vowel rather than for grit; if it starts sounding like a fuzz on the bottom end, you have gone past what the Thumb actually does. Because it sits after the compressor, changing Low Comp Threshold changes how hard the growl is driven — set the compressor first.
- **Leave the safety clip off during tracking/mixing**, and only reach for it as insurance against unexpected automation or a hot input on a specific pass - it's a safety net, not part of the intended tone-shaping signal path.
