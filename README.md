<p align="center"><img src="docs/assets/icon.png" alt="Crypta icon" width="160"/></p>

# Crypta

*Split your bass. Compress the lows. Twist the guts out of the highs.*

> Formerly **Twist Your Guts**, renamed to Crypta as part of the suite's move to Basilica Audio naming (the crypt: the basilica's low-end foundation). If you have a v0.1.0-era session referencing the old plugin identity (`com.yvesvogl.twistyourguts`, plugin code `Tygt`), see the [Unreleased] entry in [`CHANGELOG.md`](CHANGELOG.md) — the new bundle ID and plugin code (`com.yvesvogl.crypta`, `Cryp`) mean DAWs treat this as a new plugin, so existing sessions will need to be re-pointed at the new plugin.

[![CI](https://github.com/basilica-audio/Crypta/actions/workflows/ci.yml/badge.svg)](https://github.com/basilica-audio/Crypta/actions/workflows/ci.yml)
[![License: AGPL v3](https://img.shields.io/badge/License-AGPL%20v3-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)

> **Work in progress.** Crypta is pre-1.0 and under active development (v0.2.0). Binaries for macOS and Windows are available from the [Releases](../../releases) page (macOS builds are signed & notarized); building from source works too. Expect breaking changes until v1.0.0 ships (see [Roadmap](#roadmap)).

## What it is

Crypta is a Parallax-style bass plugin built on JUCE 8. As of v0.2.0 it splits your bass signal into **three** bands — low, mid, and high — with two cascaded 4th-order Linkwitz-Riley crossovers, compresses the low band in parallel, drives the mid band with staged saturation, and runs the high band through a choice of three distortion voicings before summing everything back together through a 4-band EQ and an impulse-response (cab sim) loader. v0.2.0 also adds a full preset system (factory + user presets, import/export, German localisation). See [`docs/manual.md`](docs/manual.md) for the full parameter reference and usage tips, and [`docs/design-brief.md`](docs/design-brief.md)/[`docs/research-notes.md`](docs/research-notes.md) for the research behind the v0.2.0 topology rebuild.

## Features

- **Noise gate** — full-band, ahead of both crossover splits
- **Two cascaded LR4 crossover splits** — Split Low (60–400 Hz, default 120 Hz) and Split High (300–2000 Hz, default 600 Hz), building a genuine 3-band (low/mid/high) topology, replacing v0.1.x's 2-band split
- **Low band**: parallel "glue" compressor (re-sourced fast/gentle ballistics, ratio 2:1 / attack 3 ms / release 6 ms) with makeup gain, wet/dry mix, and output level
- **Mid band** (NEW): staged/cascaded drive-only saturation, no filter/tone/blend — a distinct "throatier" character separate from the high band
- **High band**: three distortion voicings, oversampled to keep aliasing under control, with a shared, voicing-independent **Tight** pre-drive highpass
- **Circuit drive engine (v0.3.0)**: the mid and high bands rebuilt from circuit models with antiderivative antialiasing, sharing one oversampling region — measured 25–30 dB less aliasing than the previous engine, which ships on as the bit-identical `Classic` fallback
- **Smooth RMS low-band detector and a Modern gate (v0.3.0)**: a log-domain RMS detector that stops the low band tremoloing on sustained notes, and a gate with hysteresis, hold, a sidechain highpass and a straight-line release
  - **Gnaw** — op-amp-style hard clip
  - **Wool** — cascaded soft-clip fuzz with a mid scoop
  - **Razor** — tight overdrive: soft clip, mid hump
  - Clean/distorted blend control, plus drive/tone/output level
- **4-band EQ** post-sum (LowShelf / Peak / Peak / HighShelf), re-anchored default frequencies (80/500/2800/5000 Hz, sourced from the same design lineage's hardware tone stack)
- **IR loader** (cabinet simulation), relocated in v0.2.0 to process only the Mid+High post-sum signal — the low band never passes through it, matching the reference class's own architecture. Convolution engine is live; bundled factory IRs and a GUI file browser land in a later milestone
- **Delay-compensated, phase-aligned signal path** — the Mid+High branch's shared oversampling latency is reported to the host, the low band is time-aligned to match, and a phase-alignment allpass filter keeps the cascaded three-way sum flat-magnitude
- **Presets** — factory + user presets, save/save-as/delete, import/export (single files and zip banks), German localisation of the preset UI frame
- **State migration** — a v0.1.x session's single crossover frequency is migrated to the new Split High parameter on load
- **Metering** throughout the signal chain *(planned, alongside the custom GUI)*

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
        │                                          Mid+High sum → IR loader (cab sim)
        │                                                │
        └──────────── Phase-align + delay ───────────────┘
                                 │
                       Sum (delay-compensated)
                                 │
                            4-band EQ
                                 │
                       Safety Clip (optional)
                                 │
                            Output Trim
```

The Mid and High bands each run 4x oversampled (identically configured, so their latencies match exactly); the low band is delay-compensated and phase-aligned to stay both time- and magnitude-flat with them before the sum. See [`docs/architecture.md`](docs/architecture.md) for the full breakdown, including the latency-compensation strategy and the phase-alignment proof, and [`docs/manual.md`](docs/manual.md) for the full parameter reference.

## Parameters

See [`docs/manual.md`](docs/manual.md) for the complete, musically-annotated parameter reference. Summary:

| Section | Parameters |
|---|---|
| IO / Global | Input Gain, Output Gain, Bypass, Safety Clip |
| Noise Gate | Enable, Threshold, Ratio, Attack, Release |
| Crossover | Split Low (60–400 Hz), Split High (300–2000 Hz) |
| Low band | Comp Threshold/Ratio/Attack/Release/Makeup/Mix, Level |
| Mid band | Drive, Level |
| High band | Tight, Voicing (Gnaw/Wool/Razor), Drive, Tone, Blend, Level |
| EQ | Enable, Low Shelf Freq/Gain, Peak 1 Freq/Gain/Q, Peak 2 Freq/Gain/Q, High Shelf Freq/Gain |
| IR loader | Enable, Mix |

## Interface & brand

The plugin icon — a gold serpent coiled around a bass clef, antique-gold
bas-relief on a flat near-black squircle — is the header image above, and is
committed at [`docs/assets/icon.png`](docs/assets/icon.png) (1024×1024) and
[`docs/assets/icon-256.png`](docs/assets/icon-256.png) (256×256). Motif,
palette, master locations and the archived logo drafts are documented in
[`docs/branding.md`](docs/branding.md). All artwork is self-made and therefore
license-clean under this repo's AGPLv3.

**Editor.** Crypta currently ships the M2 preset bar and the v0.3.0 metering
readout above JUCE's generic parameter editor — deliberately plain, so every
APVTS parameter has a working control while the DSP is still moving. The
custom LookAndFeel, band-section layout, metering UI and resizable editor are
the M3 milestone (#45, #25, #26, #27, #28).

**Screenshot.** There is no GUI screenshot in this README yet, and no
placeholder image standing in for one. Once the M3 editor lands, the rendered
preview is committed at `docs/gui-preview.png` and linked here, following the
suite convention: the image is produced by an offscreen snapshot of the real
editor taken in the GUI test suite, not drawn by hand. See
[`docs/branding.md`](docs/branding.md#gui-preview) for the exact route.

## Installation

No pre-built binaries are published yet (see the work-in-progress notice above). Once releases begin, installation will follow the standard plugin locations:

**macOS**

| Format | Path |
|---|---|
| AU (Component) | `~/Library/Audio/Plug-Ins/Components/` |
| VST3 | `~/Library/Audio/Plug-Ins/VST3/` |

If Logic Pro doesn't pick up the plugin after installing, force a rescan by resetting the AU cache:

```sh
killall -9 AudioComponentRegistrar
auval -a
```

**Windows**

| Format | Path |
|---|---|
| VST3 | `C:\Program Files\Common Files\VST3\` |

## Building from source

Requires JUCE 8.0.14, C++20, and CMake ≥ 3.24. See [`docs/building.md`](docs/building.md) for full prerequisites and step-by-step build/test commands for macOS and Windows.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Roadmap

| Milestone | Description | Status |
|---|---|---|
| M0 | Bootstrap — project skeleton, CI, docs | Done |
| M1 | DSP completion & test coverage — gate, crossover, parallel compressor, 3 voicings (oversampled), 4-band EQ, IR loader, latency compensation, broadened test suite | Done (v0.1.0) |
| M2 | Deep-dive topology rebuild (2-band → 3-band) + presets & state recall — preset manager, 9 factory presets, state migration, German localisation | Done (v0.2.0) |
| M3 | GUI & accessibility — custom LookAndFeel, metering UI, accessibility pass | Planned |
| M4 | Release: signing, notarization, v1.0.0 — installers, tagged release | Planned |

## License

Crypta is licensed under the [GNU Affero General Public License v3.0](LICENSE) (AGPLv3).

This project uses [JUCE](https://juce.com) 8, whose open-source tier is licensed under AGPLv3 (as of JUCE 8; JUCE 7 and earlier used GPLv3), which is why this project is AGPLv3 rather than GPLv3. See [`docs/adr/0002-agplv3-licensing.md`](docs/adr/0002-agplv3-licensing.md) for the full reasoning.

VST is a registered trademark of Steinberg Media Technologies GmbH.

Crypta is an independent open-source project. It is not affiliated with, endorsed by, or sponsored by Neural DSP or the makers of any Parallax-branded product; any naming similarity refers only to the general "parallel bass processing" concept, not to any specific commercial product.

## Releases & installation

Tagged releases (`v*`) are built and published automatically by [`.github/workflows/release.yml`](.github/workflows/release.yml):

- **macOS** — AU (`.component`), VST3 (`.vst3`), and Standalone, Universal Binary (arm64 + x86_64), signed with a Developer ID Application certificate (org-level secrets, shared across the Basilica Audio suite), notarized, and stapled. Installs and opens without a Gatekeeper warning.
- **Windows** — VST3 and Standalone, **unsigned**. On first run, Windows SmartScreen may show a "Windows protected your PC" warning; choose **More info → Run anyway** to proceed. A signed Windows build is a documented future improvement, not yet available.

See [`v0.1.1`](https://github.com/basilica-audio/Crypta/releases/tag/v0.1.1) for the most recent published release; `v0.2.0` is the next tagged release, shipping the 2-band → 3-band topology rebuild and the M2 preset system.
