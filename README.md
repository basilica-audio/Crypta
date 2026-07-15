<p align="center"><img src="docs/assets/icon.png" alt="Twist Your Guts icon" width="160"/></p>

# Twist Your Guts

*Split your bass. Compress the lows. Twist the guts out of the highs.*

[![CI](https://github.com/yves-vogl/twist-your-guts/actions/workflows/ci.yml/badge.svg)](https://github.com/yves-vogl/twist-your-guts/actions/workflows/ci.yml)
[![License: AGPL v3](https://img.shields.io/badge/License-AGPL%20v3-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)

> **Work in progress.** Twist Your Guts is pre-1.0 and under active development (v0.1.0). Binaries for macOS and Windows are available from the [Releases](../../releases) page (currently unsigned — see the release notes); building from source works too. Expect breaking changes until v1.0.0 ships (see [Roadmap](#roadmap)).

## What it is

Twist Your Guts is a Parallax-style bass plugin built on JUCE 8. It splits your bass signal into low and high bands with a linear-phase-adjacent Linkwitz-Riley crossover, compresses the low band in parallel, and runs the high band through a choice of three distortion voicings before summing everything back together through a 4-band EQ and an impulse-response (cab sim) loader. See [`docs/manual.md`](docs/manual.md) for the full parameter reference and usage tips.

## Features

- **Noise gate** — full-band, ahead of the crossover split
- **LR4 crossover band-split** — 4th-order Linkwitz-Riley split, adjustable 60 Hz – 1000 Hz (default 250 Hz)
- **Low band**: parallel ("New York style") compressor with makeup gain, wet/dry mix, and output level
- **High band**: three distortion voicings, each 4x oversampled to keep aliasing under control, with independent drive/tone
  - **Gnaw** — op-amp-style hard clip
  - **Wool** — cascaded soft-clip fuzz with a mid scoop
  - **Razor** — tight overdrive: pre-clip highpass, soft clip, mid hump
  - Clean/distorted blend control per voicing, plus output level
- **4-band EQ** post-sum (LowShelf / Peak / Peak / HighShelf)
- **IR loader** (cabinet simulation) on the output stage — convolution engine is live; bundled factory IRs and a GUI file browser land in a later milestone
- **Delay-compensated signal path** — the high band's oversampling latency is reported to the host and the low band is time-aligned to match
- **Presets** with full state save/recall *(planned — a dedicated preset manager/versioning scheme is a later milestone; APVTS state save/load already round-trips today)*
- **Metering** throughout the signal chain *(planned, alongside the custom GUI)*

## Signal flow

```
Input Trim → Gate → LR4 Split (60–1000 Hz, default 250 Hz)
                      │
        ┌─────────────┴─────────────┐
        │                           │
     Low band                   High band
  Parallel Comp        Voicing → Drive → Tone → Blend
  → Makeup → Mix                    │
        │→ Level              → Level
        └─────────────┬─────────────┘
                       │
              Sum (delay-compensated)
                       │
                  4-band EQ
                       │
                  IR loader
                       │
              Safety Clip (optional)
                       │
                 Output Trim
```

The high band runs 4x oversampled for the distortion stage; the low band is delay-compensated to stay time-aligned with it before the sum. See [`docs/architecture.md`](docs/architecture.md) for the full breakdown, including the latency-compensation strategy, and [`docs/manual.md`](docs/manual.md) for the full parameter reference.

## Parameters

See [`docs/manual.md`](docs/manual.md) for the complete, musically-annotated parameter reference. Summary:

| Section | Parameters |
|---|---|
| IO / Global | Input Gain, Output Gain, Bypass, Safety Clip |
| Noise Gate | Enable, Threshold, Ratio, Attack, Release |
| Crossover | Frequency (60–1000 Hz) |
| Low band | Comp Threshold/Ratio/Attack/Release/Makeup/Mix, Level |
| High band | Voicing (Gnaw/Wool/Razor), Drive, Tone, Blend, Level |
| EQ | Enable, Low Shelf Freq/Gain, Peak 1 Freq/Gain/Q, Peak 2 Freq/Gain/Q, High Shelf Freq/Gain |
| IR loader | Enable, Mix |

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
| M2 | Presets & state recall — preset manager, factory presets, versioned state | Planned |
| M3 | GUI & accessibility — custom LookAndFeel, metering UI, accessibility pass | Planned |
| M4 | Release: signing, notarization, v1.0.0 — installers, tagged release | Planned |

## License

Twist Your Guts is licensed under the [GNU Affero General Public License v3.0](LICENSE) (AGPLv3).

This project uses [JUCE](https://juce.com) 8, whose open-source tier is licensed under AGPLv3 (as of JUCE 8; JUCE 7 and earlier used GPLv3), which is why this project is AGPLv3 rather than GPLv3. See [`docs/adr/0002-agplv3-licensing.md`](docs/adr/0002-agplv3-licensing.md) for the full reasoning.

VST is a registered trademark of Steinberg Media Technologies GmbH.

Twist Your Guts is an independent open-source project. It is not affiliated with, endorsed by, or sponsored by Neural DSP or the makers of any Parallax-branded product; any naming similarity refers only to the general "parallel bass processing" concept, not to any specific commercial product.

## Releases & installation

Tagged releases (`v*`) are built and published automatically by [`.github/workflows/release.yml`](.github/workflows/release.yml):

- **macOS** — AU (`.component`) and VST3 (`.vst3`), Universal Binary (arm64 + x86_64), signed with a Developer ID Application certificate, notarized, and stapled. Installs and opens without a Gatekeeper warning.
- **Windows** — VST3, **unsigned**. On first run, Windows SmartScreen may show a "Windows protected your PC" warning; choose **More info → Run anyway** to proceed. A signed Windows build is a documented future improvement, not yet available.

See [`docs/releasing.md`](docs/releasing.md) for the full release runbook and [ADR 0006](docs/adr/0006-macos-signing-notarization.md) for the signing/notarization design rationale. This pipeline is dormant until the first `v*` tag is pushed with the required signing secrets configured — no releases have been published yet (see the work-in-progress notice above).
