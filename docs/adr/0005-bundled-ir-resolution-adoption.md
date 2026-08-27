# 5. Adopt Nave's bundled-IR resolution model for the four shared bass IRs

Date: 2026-08-27

## Status

Accepted. Implements issue #111.

## Context

basilica-audio/Nave#45 settled how a preset's impulse-response reference
resolves (the full reasoning is the ADR comment on that issue, implemented in
Nave PR #46 and hardened by #48): resolution is by a SHA-256 of the file's
bytes, the user's library is consulted first and the embedded bundle second,
the embedded bytes become engine samples only by being materialised to a real
file and loaded through the one existing decode path, and the lookup is total
with a not-found branch that performs no audio operation at all.

Crypta ships four of Nave's bass IRs byte-identically (same generator, same
model ids, same bytes) but had none of that: factory IRs were loadable by
index only, presets could not reference a cabinet at all, and the preset
"Cab-Colored Grind" — whose entire point is the IR loader — ran the loader as
an identity passthrough until the user loaded an IR by hand.

## Decision

Port the model as Crypta's own change, not a dependency:

1. **Same identifier.** Presets carry an optional `"ir"` object with the
   SHA-256 of the referenced file's bytes (`src/presets/IrReference.h`,
   suite-format-compatible with Nave; Crypta uses slot `"a"` only — it has
   one IR slot).
2. **Same precedence.** User library (`~/Music/Crypta/Impulse Responses`,
   overridable via the non-parameter state property `irLibraryFolder`)
   first, embedded bundle second. The ordering picks which *file* the slot
   points at, never which sound comes out — a digest can only match bytes
   equal to it.
3. **Same single decode path.** `src/ir/BundledIrSource.h` materialises
   embedded bytes to a cache file (never a library folder, never scanned)
   and `loadImpulseResponseFromFile()` funnels every file into
   `cryp::FactoryIRLibrary::decodeFromMemory()` — the decoder the embedded
   factory slots already used.
4. **Same totality and not-found safety.** notReferenced / alreadyLoaded /
   library / bundled / notFound; a miss loads the preset, leaves the slot
   untouched, substitutes nothing, and raises a user-facing notice.
5. **Same stable-id policy.** Each cabinet carries its manifest `id`
   (`bass-810-cone`, …) as identity for documentation — never a resolution
   key. Same release rules: rename freely, never retune in place, removal is
   a major-version break (`resources/irs/LICENSES.md`, "Release rules").
6. **Same curation ratchets.** `tests/BundledIrCurationTests.cpp` pins the
   shipped set, the manifest match, the embedded provenance and the
   compiled-in footprint; `.gitattributes` marks embedded assets `-text`
   (Nave#48's Windows line-ending lesson).

## Consequences

- First-run users hear "Cab-Colored Grind" as made, with nothing installed.
- New modules `src/ir/{IrLibrary,IrContentIndex,FactoryIrLibrary,
  BundledIrSource}` and `src/presets/IrReference.{h,cpp}`; PresetManager
  gains the suite's generic extra-field hooks. No new decode paths.
- `<user app data>/Basilica Audio/Crypta/Bundled Impulse Responses` is a
  new, reconstructible cache location. Deleting it costs nothing.
- Binary grows by the three embedded provenance files (LICENSES.md,
  CC0-1.0.txt, manifest.json): provenance now travels with the audio, as the
  licensing bar requires. Total embedded IR footprint: 73,624 bytes.
- The GUI does not yet display `getPresetIrNotice()` or offer file loading;
  both are deliberate follow-ups (this change stays out of `src/gui`).
