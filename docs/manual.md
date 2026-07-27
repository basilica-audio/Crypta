<p align="center"><img src="assets/icon.png" alt="Crypta icon" width="120"/></p>

# Crypta — User Manual

*Split your bass. Compress the lows. Twist the guts out of the highs.*

## What it is

Crypta is a Parallax-style **parallel bass processor** built for metal production. As of v0.2.0 it splits your bass signal into **three** bands — low, mid, and high — with two cascaded 4th-order Linkwitz-Riley ("LR4") crossovers, keeps the low band tight with a parallel compressor, drives the mid band with staged saturation, and runs the high band through a choice of three distortion voicings before summing everything back together through a 4-band EQ and a cabinet-simulation IR loader.

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
  Parallel Comp → Level                                  │
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

## Presets

Crypta ships with a preset system: a horizontal bar at the top of the plugin window lets you step through factory and user presets (`<` / preset name / `>`), save/save-as/delete your own, and import/export single presets or preset banks (zip files of multiple presets). Nine factory presets ship in v0.2.0 — see `docs/presets.md` for what each one demonstrates. User presets are stored per-plugin under:

- **macOS**: `~/Library/Audio/Presets/Yves Vogl/Crypta/`
- **Windows**: `%APPDATA%\Yves Vogl\Crypta\Presets\`

A fresh instance loads a user "Default" preset if you've saved one ("Set current as default" in the preset menu), otherwise the factory "Default" preset (matching the plain parameter defaults documented below).

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
| Output Gain | −24 … +24 | 0 | dB | Final output trim, applied after everything else (including the safety clip). |
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
| High Bias | 0 … 100 | 0 | % | *Circuit only.* Offsets the clipper so it saturates asymmetrically, which adds even-order harmonics — a warmer, more "tube-like" character than the symmetric default. 0 % is exactly the v0.2.0 character. The offset is removed again downstream, so this never puts DC on your output. |

**Voicings:**

- **Gnaw** — an op-amp-style hard clip. Symmetric, unforgiving, the most aggressive of the three; pushes hard into a square-ish waveform at high drive. Good for a raw, buzzy attack.
- **Wool** — cascaded soft-clip fuzz with a mid scoop and a touch of asymmetry for a grittier, more fuzz-pedal-like harmonic character. Good for a woolier, less "digital" grind that still cuts.
- **Razor** — a tighter overdrive: a comparatively mild clipper, with a mid-hump filter afterwards that keeps the low end from ever getting mushy (the pre-clip highpass duty is now handled band-wide by Tight, above, rather than being Razor's own quirk as in v0.1.x).

*Starting points, not final voicing:* the drive-gain ranges and mid-filter hump/scoop settings for all three voicings are engineering defaults, tuned for musical usefulness and mathematically bounded (no runaway output at any drive setting), not yet finalized by ear against reference material. Expect these to be refined in a future release.

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

*Loading impulse responses:* v0.2.0 still does not ship an in-plugin file browser or factory cabinet IRs (both remain on the roadmap for a later milestone alongside the custom GUI). The IR-loading DSP engine itself is fully implemented and real-time safe.

## State migration

### v0.2.x → v0.3.0

Opening a session or loading a preset saved before v0.3.0 sets **Drive Engine**, **Low Comp Detector** and **Gate Mode** to their Classic values, so your saved work sounds exactly as it did. This applies to host sessions and to your own saved presets alike, including a user preset named "Default" — so if you had saved your own default, that is still what you get on a new instance.

The one deliberate exception is the **safety clip**: if you had it switched on, v0.3.0's clipper is not bit-identical to v0.2.0's. It aliases far less and is transparent below the ceiling; the difference is confined to material that was actually being clipped.

New parameters (High Bias, the knee and auto controls, the Modern gate's controls, Clip Ceiling) are either neutral by default or are only read when their engine is selected, which legacy state never selects.

### v0.1.x → v0.2.0

If you open a Crypta v0.1.x session, the old single `Crossover Frequency` value is migrated to the new **Split High** parameter, clamped into its new 300–2000 Hz range (v0.1.x's own shipped default, 250 Hz, is below that floor, so an untouched v0.1.x session lands exactly at 300 Hz on reopen). Split Low and every new Mid-band/Tight parameter fall back to their v0.2.0 defaults. Any low-band compressor settings you had explicitly changed away from v0.1.x's old defaults are preserved as-is — only the *shipped default* changed, not your own deliberate settings. This is a best-effort, lossy, one-directional migration; re-check your low/mid/high balance after reopening an old session.

## Tips

- **Start with the low band tight, then dial in the mid band's throat, then the high band's grind.** Set Low Comp Mix and Makeup first so the fundamental feels locked in, then dial in Mid Drive for the "throatier" cutting character, and only then pick a High voicing and drive amount.
- **Split Low and Split High are tone decisions, not just technical ones.** Pushing Split Low up moves more note body out of the (compressed-only) Low band; pushing Split High up widens the Mid band's own passband, giving the "throatier" character more room before the High band's own fuzz/presence character takes over.
- **High Tight is your main "fuzz vs. tightness" control**, independent of which voicing you've picked - pull it toward its 20 Hz floor for maximum fuzz, push it up toward 500 Hz for a tighter, more controlled top end. It also tames harshness on hot Drive settings.
- **High Blend is your "how much" knob, High Drive is your "how hard" knob.** If a voicing feels too extreme, try lowering Blend before lowering Drive - you'll often keep more of the character that way, just at a lower overall intensity, rather than flattening the nonlinearity itself.
- **Leave the safety clip off during tracking/mixing**, and only reach for it as insurance against unexpected automation or a hot input on a specific pass - it's a safety net, not part of the intended tone-shaping signal path.
