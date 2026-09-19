# Factory presets

Twelve factory presets ship with Crypta. All settings are starting
points designed against the research-derived v0.2.0 defaults (`docs/design-brief.md`'s Factory
Presets section), not exact renders against any reference material.

Since issue #111 a preset may carry an **optional IR reference** — the SHA-256
of the impulse-response file it was voiced with (see `src/presets/IrReference.h`).
It resolves against the user's IR library folder first
(`~/Music/Crypta/Impulse Responses`) and Crypta's own embedded bundle second,
so a factory preset that names a bundled cabinet sounds as made on a fresh
install with nothing on disk. A reference that cannot be resolved degrades
loudly and safely: the preset's settings load, the IR slot keeps whatever it
had, nothing is substituted, and a notice names the missing cabinet
(`CryptaAudioProcessor::getPresetIrNotice()`). Presets without a reference
(all pre-#111 presets) behave exactly as before.

| Preset | Category | Intent |
|---|---|---|
| **Default** | Init | The plain `ParameterLayout` defaults, loaded on a fresh instance - identical settings to "Glue & Grind" below, filed separately under the technical `Init`/"Default" name the preset system's default-resolution order looks for. |
| **Glue & Grind** | Bass | The shipped default character: fast/gentle low-band glue compression, moderate mid saturation, tight high-band fuzz. |
| **Sub Lock** | Bass | Maximum low-end control for a dense mix; the low band does almost all the work, mids/highs kept modest. |
| **Throat** | Bass | Emphasizes the mid band's documented "throatier" character; the mid band carries most of the grind. |
| **Fuzz Wall** | Bass | Maximum documented "fuzz" pull: Tight at its sourced floor (100 Hz), aggressive Wool voicing at high Drive. |
| **Cut Through** | Bass | Drop-tuned rhythm use case: both splits pushed up so more note body reaches the distorted bands. |
| **Definition Only** | Bass | Showcases the high band's harshness-control role: Tight pulled up, Drive kept moderate, EQ presence bump engaged. |
| **Clean Low, Loud Top** | Bass | Low band audibly present but mostly uncompressed (Mix pulled down), mid/high pushed harder. |
| **Cab-Colored Grind** | Bass | Demonstrates the v0.2.0-relocated IR loader coloring only the Mid+High path while the low end stays uncolored. References the bundled **Modelled 8x10 Cone** cabinet (issue #111), so it sounds as made out of the box. |
| **Circuit Foundation** | Bass | The Circuit engine's own foundation tone: Drive Engine, Low Comp Detector and Gate Mode all left at their new v0.3.0 defaults (Circuit / Smooth RMS / Modern), with a widened mid band (Split Low 110 Hz, Split High 700 Hz), a soft-kneed, auto-makeup low-band glue at a parallel 70% Mix, restrained mid/high drive (25%/35%) with Razor voicing and a touch of even-harmonic High Bias, and a gentle Modern gate (45 dB range) for definition without obvious chatter. |
| **Circuit Grind** | Bass | Circuit engine throughout (Smooth RMS detector, Modern gate), tuned aggressive: a firm, fast low-band glue (-24 dB threshold, 4:1 ratio, hard 4 dB knee, auto makeup) under a driven mid band (55% Mid Drive) and a heavily driven Wool high band (75% Drive, 45% High Bias for extra even-harmonic grind), a tighter Modern gate (-52 dB threshold, 60 dB range) and a -1.07 dB output trim to compensate for the added level. |
| **Circuit Knife** | Bass | Circuit engine throughout (Smooth RMS detector, Modern gate), with a light touch on the low band (60% Mix, gentle -16 dB threshold) but the most extreme high band of the three: Gnaw voicing (symmetric, no High Bias) with High Tight pushed to 200 Hz and Drive to 90% for a narrow, cutting edge rather than a warm one, a tight, high-threshold Modern gate (-50 dB, 70 dB range) for percussive definition, and the largest output trim of the three (-2.04 dB) to tame the resulting brightness. |
