# GUI parameter mapping

Which physical control on the editor drives which `AudioProcessorValueTreeState` parameter, panel by panel, in the order the editor creates them — which is the signal-flow order and therefore also the keyboard focus (Tab) order.

This table is the human-readable companion to the machine-checked one: `tests/gui/EditorLayoutTests.cpp` asserts that the control count equals the APVTS parameter count exactly, so no parameter can be missing from the surface and no control can exist without a parameter. For the component architecture behind these controls, see [`architecture.md`](architecture.md#gui-vector-editor).

| Section | Control | Type | Parameter ID |
|---|---|---|---|
| Input | Input Gain | Knob | `inputGain` |
| Input | Bypass | Lamp toggle | `bypass` |
| Input | IN | Needle meter | — (reads `cryp::MeterTaps`) |
| Noise Gate | Gate | Lamp toggle | `gateEnabled` |
| Noise Gate | Mode | Knob | `gateMode` |
| Noise Gate | Threshold | Knob | `gateThreshold` |
| Noise Gate | Ratio | Knob | `gateRatio` |
| Noise Gate | Attack | Knob | `gateAttack` |
| Noise Gate | Release | Knob | `gateRelease` |
| Noise Gate | Hysteresis | Knob | `gateHysteresis` |
| Noise Gate | Hold | Knob | `gateHold` |
| Noise Gate | SC Highpass | Knob | `gateScHpf` |
| Noise Gate | Range | Knob | `gateRange` |
| Noise Gate | GATE | Needle meter | — (reads `cryp::MeterTaps`) |
| Crossover | Split Low | Knob | `splitLowHz` |
| Crossover | Split High | Knob | `splitHighHz` |
| Low Band | Detector | Knob | `lowCompDetector` |
| Low Band | Threshold | Knob | `lowCompThreshold` |
| Low Band | Ratio | Knob | `lowCompRatio` |
| Low Band | Knee | Knob | `lowCompKnee` |
| Low Band | Attack | Knob | `lowCompAttack` |
| Low Band | Release | Knob | `lowCompRelease` |
| Low Band | Auto Rel | Lamp toggle | `lowCompAutoRelease` |
| Low Band | Auto Mkup | Lamp toggle | `lowCompAutoMakeup` |
| Low Band | Makeup | Knob | `lowCompMakeup` |
| Low Band | Mix | Knob | `lowCompMix` |
| Low Band | Low Level | Knob | `lowLevel` |
| Low Band | COMP | Needle meter | — (reads `cryp::MeterTaps`) |
| Drive Engine | Engine | Knob | `driveEngine` |
| Mid Band | Mid Drive | Knob | `midDrive` |
| Mid Band | Mid Level | Knob | `midLevel` |
| High Band | Tight | Knob | `highTightHz` |
| High Band | Voicing | Knob | `highVoicing` |
| High Band | High Drive | Knob | `highDrive` |
| High Band | Bias | Knob | `highBias` |
| High Band | Tone | Knob | `highTone` |
| High Band | Blend | Knob | `highBlend` |
| High Band | High Level | Knob | `highLevel` |
| Cabinet | Cab | Lamp toggle | `irEnabled` |
| Cabinet | Cab Mix | Knob | `irMix` |
| EQ | EQ | Lamp toggle | `eqEnabled` |
| EQ | Low Freq | Knob | `eqLowShelfFreq` |
| EQ | Low Gain | Knob | `eqLowShelfGain` |
| EQ | Peak 1 Freq | Knob | `eqPeak1Freq` |
| EQ | Peak 1 Gain | Knob | `eqPeak1Gain` |
| EQ | Peak 1 Q | Knob | `eqPeak1Q` |
| EQ | Peak 2 Freq | Knob | `eqPeak2Freq` |
| EQ | Peak 2 Gain | Knob | `eqPeak2Gain` |
| EQ | Peak 2 Q | Knob | `eqPeak2Q` |
| EQ | High Freq | Knob | `eqHighShelfFreq` |
| EQ | High Gain | Knob | `eqHighShelfGain` |
| Output | Clip | Lamp toggle | `outputClip` |
| Output | Ceiling | Knob | `clipCeiling` |
| Output | Output Gain | Knob | `outputGain` |
| Output | OUT | Needle meter | — (reads `cryp::MeterTaps`) |

**Totals:** 44 knobs + 7 lamp toggles = 51 controls, one per automatable parameter, plus 4 display-only needle meters.

Meters are not parameters: they read the lock-free metering taps (`src/dsp/MeterTaps.h`) on the editor's 30 Hz timer. `IN`/`OUT` show block peak in dBFS, `GATE`/`COMP` show gain reduction in positive dB.
