# VOX — speech-processing firmwares for Mutable Instruments Beads

Alternative Beads firmwares for processing spoken-word material (speeches, interviews, ads) coming from a Make Noise Morphagene.

| Plan | Firmware | One-line description |
|---|---|---|
| [00-common.md](00-common.md) | `vox/common` | Shared foundation: build layout, memory map, speech buffer, analysis library, UI/settings fork, test harness |
| [01-vox-psola.md](01-vox-psola.md) | `vox_psola` | Pitch-synchronous granular: formant-preserving pitch shift, time stretch, intonation control |
| [02-vox-lpc.md](02-vox-lpc.md) | `vox_lpc` | LPC analysis/resynthesis: whisper, robot, cross-synthesis, formant warp, "talking chip" mode |
| [03-vox-syllable.md](03-vox-syllable.md) | `vox_syllable` | Onset/syllable-aware granular: grains snap to syllables and words; pause removal |
| [04-vox-split.md](04-vox-split.md) | `vox_split` | Voiced/unvoiced splitter: separate processing (or separate outputs) for vowels and consonants |
| [06-vox-sequencer.md](06-vox-sequencer.md) | `vox_sequencer` | Clocked word/syllable re-sequencer with optional WSOLA stretch-to-step |
| [07-vox-chain.md](07-vox-chain.md) | `vox_chain` | Voice processing chain: gate, de-esser, compressor, EQ, character, ducker, limiter |

Numbering follows the original idea list (5, the FFT/phase-vocoder idea, is not planned).

All plans are implemented (see each plan's status note and `vox/README.md`); none has been run on the module yet. `vox_multi` (`vox/multi/`) combines the six algorithms in one firmware, chosen at power-on.

## Recommended build order

1. **00 common**: everything else depends on it.
2. **07 chain**: simplest DSP; proves the build, UI fork, settings, calibration reuse, CPU profiling and the bootloader/WAV update path.
3. **03 syllable**: builds the onset detector and segment table that 06 reuses.
4. **06 sequencer**: adds clocking and WSOLA on top of 03.
5. **04 split**: adds the V/UV classifier and YIN usage.
6. **01 PSOLA**: needs a reliable pitch tracker and pitch marks (from 04/common).
7. **02 LPC**: shares framing and voicing/pitch with 01; the most tuning-heavy.

## Hardware facts used throughout (from the `beads/` source in this repo)

- MCU **STM32H743VIT6** (`beads_v51.sch`), 400 MHz Cortex-M7 (`F_CPU = 400000000L`), codec **PCM3060**.
- Audio callback at `kBlockRate = 4000` Hz → 12 frames/block at 48 kHz (250 µs, ≈100k CPU cycles per block); `kMaxCodecBlockSize = 24`.
- Audio RAM used by stock firmware: `delay_buffer_1[512 KB]` in `.ramd1`, `delay_buffer_2[256 KB]` in `.ramd2`, `reverb_buffer[32768 × u16]` in `.ramd3`.
- Controls:
  - Knobs: TIME / SIZE / SHAPE / PITCH, each with an attenuverter and a CV input. DENSITY has a knob and CV but no attenuverter. FEEDBACK / DRY-WET / REVERB pots are multiplexed (slower scan), with one ASSIGN CV input that can be routed to them.
  - Buttons: FREEZE, SEED, MEDIA (quality), ASSIGN.
  - Gate inputs: FREEZE and SEED. Both are sampled per audio frame via `Codec::EVENT_*`; SEED is delayed 2.5 ms.
  - LEDs: MEDIA (RGB), FREEZE, SEED, VU, ASSIGN 1–3.
- Output: stereo audio. The stock **gate-output mode** (ASSIGN+SEED) replaces the right output with a gate (`out.r = gate ? -0.8 : 0`). Every VOX firmware uses this as its "analysis gate out".
- Updates: audio WAV bootloader (`BOOTLOADER = beads_bootloader`, `make … wav`). The updater is entered by holding FREEZE at power-on (`bootloader.cc`).
