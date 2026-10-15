# vox_sequencer: clocked word / syllable re-sequencer for Beads

Locks speech to a clock. The syllables or words that the segmenter finds become the **steps of a pattern**: played in order, reversed, shuffled or randomized; either truncated to the step or **time-stretched to fit it** (WSOLA, pitch preserved); with stutters and ratchets. Plan: [`../plans/06-vox-sequencer.md`](../plans/06-vox-sequencer.md).

```
IN ─► 48→24 kHz ─► [pause gate] ─► speech buffer ─► segmenter ─► unit pool
SEED jack / internal clock ─► clock (÷/×, swing) ─► pattern (TIME, SIZE, SHAPE, REPEAT)
                           ─► 2 step voices (truncate or WSOLA) ─► 24→48 kHz ─► dry/wet, reverb, limiter ─► OUT
```

## Panel

| Control | Function |
|---|---|
| **TIME** (+att/CV) | Pattern offset in the unit pool. Live: counted back from the newest unit. Frozen: from the oldest |
| **SIZE** (+att/CV) | Pattern length, 1–16 steps |
| **SHAPE** (+att/CV) | Order, in five zones: in order → reverse → fixed shuffle → random per cycle → random per step |
| **DENSITY** (+CV) | With a clock on SEED: ÷4, ÷3, ÷2, ×1, ×2, ×3, ×4 (seven zones). With no clock: tempo, 40–240 BPM (sixteenth-note steps) |
| **PITCH** (+att/CV) | Transposition (stock interval law, 1V/oct), sampled at each step, so a CV sequencer can play the words melodically |
| **FEEDBACK** = REPEAT | Probability that a step repeats the previous unit (stutter). Above ¾, steps can also ratchet (2–4 sub-repeats) |
| **DRY/WET**, **REVERB** | As stock |
| **FREEZE** button / gate | Freeze the buffer and the pool, so the pattern draws from fixed material |
| **SEED** jack | Clock input. The period is smoothed (median of 3); a tempo change of more than 20% re-locks after 2 pulses |
| **SEED** button | Tap tempo with no clock (quarter notes, average of 3 taps; moving DENSITY returns to the knob tempo). Manual step advance with a clock |
| **SEED + FREEZE** | Pattern lock: regenerates the fixed shuffle and holds the order, so the random modes stop changing |
| **MEDIA** | 🟢 syllable/truncate, 🟡 word/truncate, 🩵 syllable/stretch, 🟣 word/stretch |
| **MEDIA + FREEZE** | Pause removal on/off (the MEDIA LED is dimmed when on) |
| **MEDIA + SEED** | Swing: off / 16% / 33% on even steps (0, 1 or 2 ASSIGN LEDs flash) |
| **ASSIGN** | Stock destinations, plus a 5th, **reset** (all three LEDs blink): a rising edge above 1 V restarts the pattern on the next step |
| **ASSIGN + SEED** | Gate output: off → step triggers (5 ms per step and per ratchet) → accent gate (high during steps whose unit is in the loudest quarter of the pool) |
| **VU LED** | Blinks on each step, brighter on accents |

**Stretch mode:** a unit is stretched or compressed to the step length when the ratio is between 0.25× and 4×; outside that range it is truncated. With an external clock the step length is the measured period × ratio.

**Morphagene:** feed the same master clock to the Morphagene's CLK input and to SEED. Alternatively, patch Morphagene EOSG → SEED to step on splice ends.

## Testing on a computer

```sh
make -f vox/test/makefile check
build/vox_test/sequencer_test process speech.wav out.wav --submode 2 --clock-hz 8 --density 0.5 --shape 0.5 --freeze-at 10
```

Self tests (25 checks):

| Area | Checks |
|---|---|
| Internal clock | 40, 98 and 240 BPM exact |
| External clock | steps sample-accurate on the edges; ÷4, ÷2, ×2 and ×4 within 2%; re-lock after a tempo change within 3 pulses |
| Order | in order (chronological, repeating); reverse; fixed shuffle (a repeating permutation); random per step; lock freezes a random pattern |
| Reset | the next step is step 1 |
| Swing | 33% gives 2 : 1 interval alternation |
| REPEAT | ratchets at maximum |
| WSOLA | ×0.5, ×2 and ×3 stretches: exact duration, pitch within 0.5%, level 1.00 |
| Stretch mode | fills the steps: 11% of 10 ms windows silent, against 23% when truncating |
| Stability | 40 s of random controls including clock, tap and reset |

## Not verified yet (needs the module)

- Hardware behaviour: panel and LEDs.
- **CPU load.** About 15k x86 instructions per 12-frame block on the host (callgrind), with WSOLA frame searches peaking at about 10k per block per voice. The budget on the module is about 100k cycles per block; the measured worst case is in `vox::profile_max_cycles`, readable with a debugger.
- Timing jitter against a real clock source.
