# Factory impulse responses — provenance and licences

**Status: no impulse response is bundled with Crypta.** This file exists as the
record that must be complete *before* one ever is, and as the human-readable
half of the guard `src/dsp/FactoryIRs.h` enforces in code.

## The bar

Bundling a cabinet IR means redistributing someone else's recording inside every
copy of an AGPLv3 binary. Crypta therefore accepts only three licence
categories, and the code rejects everything else:

| Licence string | Meaning | What must be recorded |
|---|---|---|
| `CC0-1.0` | Creative Commons Zero — dedicated to the public domain | The source URL, and the date it was retrieved |
| `Public Domain` | An explicit public-domain dedication that is not CC0 | The wording of the dedication and where it is published |
| `Self-recorded` | Captured by this project on gear we own | Cab, speaker, mic, position, date, engineer |

Explicitly **not** accepted:

- "free download", "free for commercial use", "royalty free" — these are not
  licences, and none of them grants redistribution inside a binary.
- CC-BY / CC-BY-SA — usable, but they carry attribution (and, for SA, share-alike)
  obligations into every downstream copy. Taking that on is a deliberate decision
  for the project owner, not a default.
- Anything whose origin cannot be traced to a named licensor. "It was on a forum"
  is not provenance.

## How to add one

1. Verify the licence at the source, and archive the evidence (the licence page
   itself, not a third party's description of it).
2. Add the WAV to `resources/irs/` and to `juce_add_binary_data()` in
   `CMakeLists.txt`.
3. Add the entry to `CryptaAudioProcessor::getFactoryIRAssetTable()` in
   `src/PluginProcessor.cpp`, with its licence string and a source field that a
   reviewer can check without asking anyone.
4. Add a row to the table below.
5. `tests/FactoryIRTests.cpp` fails if the entry does not clear the bar, so the
   build is the last line of defence rather than the review.

## Bundled impulse responses

_None._

| Name | Licence | Source / capture details | Added |
|---|---|---|---|
| — | — | — | — |
