# vox_syllable: onset / syllable-aware granular for Beads

# 14-10-2026 Sounds good but ended up with silence somehow.

A granular engine whose grains start and end on **phonetic boundaries** (onset slices, syllables, words) found in real time in the incoming speech. Plan: [`../plans/03-vox-syllable.md`](../plans/03-vox-syllable.md).

```
IN (L+R)/2 ─► 48→24 kHz ─► [pause gate] ─► speech buffer (10.9 s, +feedback)
                                       └─► segmenter ─► onset / syllable / word tables
DENSITY or SEED triggers ─► unit choice (TIME, SIZE, SHAPE) ─► 6 grains ─► 24→48 kHz ─► dry/wet, reverb, limiter ─► OUT
```

## Panel

| Control | Function |
|---|---|
| **TIME** (+att/CV) | Which unit to play, quantized to units. Live: 0 = the newest finished unit, CW = further back. Frozen: 0 = the oldest unit in the buffer. CV can address words directly (e.g. from a sequencer) |
| **SIZE** (+att/CV) | Grain length. CCW = a fraction of the unit, from its start ("crumbs"); noon = exactly one unit; CW = up to 8 consecutive units |
| **SHAPE** (+att/CV) | Scatter and envelope. CCW = the exact unit with sharp fades. Noon = ±2 units of random spread, softer fades. CW = ±16 units, with up to 50% reverse probability |
| **DENSITY** (+CV) | Trigger generator. Noon = off. CW = regular, 0.5–20 Hz. CCW = random (Poisson), 0.5–20 Hz. When the SEED jack is patched, grains are triggered only by it |
| **PITCH** (+att/CV) | Transposition, using the stock interval law and 1V/oct CV (resampling) |
| **FEEDBACK** | Output re-recorded into the buffer and re-segmented (fragments of fragments) |
| **DRY/WET**, **REVERB** | As stock |
| **FREEZE** button / gate | Stop recording; unit addresses stay stable |
| **SEED** button / gate | Play a grain at the TIME unit. Morphagene EOSG → SEED re-triggers at every splice |
| **SEED + FREEZE** | Latch: loop the last unit seamlessly (the SEED LED blinks) |
| **MEDIA** | Unit level: 🟢 syllable, 🟡 word, 🔴 onset slice, 🔵 syllable + pause removal |
| **MEDIA + FREEZE** | Toggle pause removal in any sub-mode. The ASSIGN LEDs flash: all = on, none = off. While it is on outside the blue sub-mode, the MEDIA LED is dimmed |
| **MEDIA + SEED** | Cycle segmentation sensitivity: low / medium / high (1, 2 or 3 ASSIGN LEDs flash) |
| **ASSIGN + SEED** | Gate output: OUT R gives a 5 ms trigger on every onset detected in the live input (on every word start in the word sub-mode) |
| **VU LED** | Input level; flashes when a unit is finalized |

**Pause removal:** silences longer than 150 ms are not recorded. The buffer then holds speech rather than wall-clock time, and the 150 ms that are kept preserve word boundaries. Recording resumes with a 5 ms fade-in.

**Latency:** a unit becomes playable once it is final, about 50–150 ms after it ends.

## Testing on a computer

```sh
make -f vox/test/makefile check
build/vox_test/syllable_test process speech.wav out.wav --submode 1 --density 0.8 --shape 0.6 --freeze-at 8
```

Self tests (17 checks), run on synthetic speech with ground truth:

| Area | Checks |
|---|---|
| Segmentation (through the engine) | 96% of syllable starts found, 96% of detections correct (±40 ms) |
| TIME addressing | monotonic sweep; every unit reachable; live TIME = 0 is the newest unit |
| SIZE | fraction < 1 unit < 8 units |
| Grain playback | correlation 0.997 with the source unit |
| Trigger generator | off at noon; 19.8 Hz regular and 17 Hz random at the extremes (target 20 Hz) |
| Latch | loops with no gap longer than 0.2 ms |
| Pause removal | 12 s of input becomes 8.4 s of buffer; no steps at the seams; words still segmented |
| Gate output | a trigger within 60 ms of 96% of onsets |
| Stability | 40 s of random controls: finite and bounded |

`vox/test/analysis_test` scores the shared segmenter directly: 97% of syllable starts and 90% of word starts found at medium sensitivity.

## Not verified yet (needs the module)

- Hardware behaviour: panel, LEDs and CV scaling.
- **CPU load.** About 15.5k x86 instructions per 12-frame block on the host (callgrind). The budget on the module is about 100k cycles per block; the measured worst case is in `vox::profile_max_cycles`, readable with a debugger.
- Segmentation on real speech. The thresholds were tuned on synthetic speech; the sensitivity setting is the first thing to try on real recordings.
