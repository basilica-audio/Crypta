# Factory presets

Twelve factory presets ship with Crypta. All settings are starting
points designed against the research-derived v0.2.0 defaults (`docs/design-brief.md`'s Factory
Presets section), not exact renders against any reference material.

**Three of them are not described below yet** — *Circuit Foundation*, *Circuit Grind* and
*Circuit Knife*, added in v0.3.0 (basilica-audio/Crypta#117).

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
