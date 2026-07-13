# Twist Your Guts

*Split your bass. Compress the lows. Twist the guts out of the highs.*

[![CI](https://github.com/yves-vogl/twist-your-guts/actions/workflows/ci.yml/badge.svg)](https://github.com/yves-vogl/twist-your-guts/actions/workflows/ci.yml)
[![License: AGPL v3](https://img.shields.io/badge/License-AGPL%20v3-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)

> **Work in progress.** Twist Your Guts is pre-1.0 and under active development. There are no built binaries or releases yet — building from source is currently the only way to run it. Expect breaking changes until v1.0.0 ships (see [Roadmap](#roadmap)).

## What it is

Twist Your Guts is a Parallax-style bass plugin built on JUCE 8. It splits your bass signal into low and high bands with a linear-phase-adjacent Linkwitz-Riley crossover, compresses the low band in parallel, and runs the high band through a choice of three distortion voicings before summing everything back together through a 4-band EQ and an impulse-response (cab sim) loader.

## Features (v1.0 scope)

- **LR4 crossover band-split** — 4th-order Linkwitz-Riley split, adjustable 60 Hz – 1000 Hz (default 250 Hz)
- **Low band**: parallel compressor with wet/dry mix and output level
- **High band**: three distortion voicings, each with independent drive/tone
  - **Gnaw** — hard clip
  - **Wool** — fuzz
  - **Razor** — tight overdrive
  - Clean/distorted blend control per voicing, plus output level
- **Noise gate** on the input stage
- **4-band EQ** post-sum
- **IR loader** (cabinet simulation) on the output stage
- **Presets** with full state save/recall
- **Metering** throughout the signal chain

## Signal flow

```
Input Trim → Gate → LR4 Split (60–1000 Hz, default 250 Hz)
                      │
        ┌─────────────┴─────────────┐
        │                           │
     Low band                   High band
  Comp → Mix → Level    Voicing → Drive → Tone → Blend → Level
        │                           │
        └─────────────┬─────────────┘
                       │
              Sum (delay-compensated)
                       │
                  4-band EQ
                       │
                  IR loader
                       │
                     Output
```

The high band runs through oversampling for the distortion stage; the low band is delay-compensated to stay time-aligned with it before the sum. See [`docs/architecture.md`](docs/architecture.md) for the full breakdown, including the latency-compensation strategy.

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
| M0 | Bootstrap — project skeleton, CI, docs | In progress |
| M1 | DSP core — crossover + latency framework | Planned |
| M2 | Dynamics — gate + compressor | Planned |
| M3 | Distortion engine — oversampling, 3 voicings | Planned |
| M4 | EQ + IR loader | Planned |
| M5 | State & presets | Planned |
| M6 | Custom GUI | Planned |
| M7 | Release engineering — signing, notarization, installers, v1.0.0 | Planned |

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
