# 06 — `vox_sequencer`: clocked word / syllable re-sequencer

> **Status: implemented** in `vox/sequencer/`. It builds and the host self tests pass (25/25); it has not yet been run on the module. The final panel mapping and the test instructions are in `vox/sequencer/README.md`.
>
> Deviations from this plan:
> - The speech buffer is 2^18 int16 samples (10.9 s at 24 kHz) in `.ramd1` only, not the 16.4 s planned in [00-common](00-common.md).
> - The segmenter, unit table and WSOLA are shared modules in `vox/common/analysis/segmenter.h` and `vox/common/synth/wsola.h`.

Locks speech to a clock. Detected syllables or words become **steps of a pattern**: played in order, shuffled, or randomised, either truncated to the step or **time-stretched to fit it** (WSOLA, pitch-preserving). Stutters/ratchets add repeats. A live speech can be re-cut into rhythm as it plays in from Morphagene.

Depends on: [00-common](00-common.md), and the segmentation stack from [03-vox-syllable](03-vox-syllable.md) (`OnsetDetector`, `SyllableNucleus`, `Segmenter`, `SegmentTable`, pause removal). Adds `Clock`, `PatternGenerator` and `Wsola`.

---

## 1. Signal flow

```
IN L ─► AGC ─► 48→24k ─┬─► [pause gate] ─► SpeechBuffer
                       └─► segmentation (from 03) ─► SegmentTable

SEED gate ─► Clock (period measure, PLL-ish smoothing) ─┐   (or internal clock: DENSITY / tap)
ASSIGN CV (reset mode) ─────────────────────────────────┤
                                                        ▼
                         PatternGenerator (TIME, SIZE, SHAPE, FEEDBACK) ─► step events
                                                        ▼
            Step player ×2 voices (truncate | WSOLA stretch) ─► 24→48k ─► dry/wet ─► reverb ─► limiter ─► OUT
```

---

## 2. Clock

- **External:** when the SEED jack is patched (`normalization_detector`), each rising edge is a clock pulse. The period is measured in samples, smoothed (median of 3, then a one-pole on the period with τ = 2 beats), with fast re-lock on a > 20 % tempo change.
- **Internal:** when SEED is unpatched, DENSITY sets the tempo, 40–240 BPM 16ths. The SEED button is tap tempo (average of the last 3 taps).
- **Division / multiplication:**
  - DENSITY with an external clock: ÷4, ÷3, ÷2, ×1, ×2, ×3, ×4 (detented zones). Multiplication uses the predicted period.
  - DENSITY with the internal clock: tempo.
- **Reset** (a new ASSIGN CV destination). ASSIGN cycles through 5 destinations in this firmware, with the 5th = reset (all three ASSIGN LEDs blink). A rising edge > 1 V resets the pattern to step 0 on the next clock. This works around Beads having no dedicated reset jack.
- **Morphagene:** feed the same master clock to Morphagene CLK and Beads SEED. Alternatively, Morphagene EOSG → SEED clocks the sequence by splice ends.

---

## 3. Pattern generator

- **Pool:** the segment table at the current unit level (syllable or word, per sub-mode). Live, the pool is the newest N segments, so the pattern keeps re-cutting the latest speech. Frozen, the pool is a fixed set.
- **Parameters:**
  - `offset` (TIME): where in the pool the pattern starts (in segments back from newest, or from the start when frozen)
  - `length` (SIZE): 1–16 steps
  - `order` (SHAPE):
    - CCW: in order
    - 10 o'clock: reverse order
    - noon: fixed shuffle (seeded permutation; regenerated on SEED+FREEZE)
    - 2 o'clock: random each cycle
    - CW: random each step
  - `repeat` (FEEDBACK): probability a step **repeats the previous step's segment** (stutter). At > 75 % it also ratchets: 2–4 sub-repeats within the step.
- **Step event:** `{segment_index, step_length_samples, ratchet_count, accent}`. Accent is set when the segment's energy is in the top 25 % of the pool, which drives gate out.
- **Pattern length vs pool size:** if the pool has fewer segments than `length`, indices wrap.

---

## 4. Step player

Two voices; consecutive steps alternate voices so tails can overlap by up to 10 ms.

### Truncate mode
- Play from segment start with a 2 ms attack.
- When the next step arrives, fade out over 3 ms. If the segment ends before the step, it simply ends (silence).
- PITCH = resampling transposition.

### Stretch-to-fit mode (WSOLA)
- **Target:** the segment's duration is time-scaled so it ends exactly at the next step boundary. The scaling ratio is limited to 0.25×–4×; outside that, truncate or leave a gap.
- **WSOLA parameters:** 20 ms Hann frames, 50 % overlap synthesis hop (10 ms = 240 samples). Search tolerance ±8 ms (±192 samples) via normalised cross-correlation. To save CPU, the search runs on 4× decimated (6 kHz) envelopes, then refines ±4 samples at 24 kHz.
- **CPU:** a coarse search of 96 lags × 60 samples ≈ 6k MAC per hop, plus the refine at 9 × 240 ≈ 2k MAC. Per 10 ms hop that is ≈ 8k MAC, amortized at ≈ 200 MAC/block. Negligible.
- PITCH in stretch mode = resample after WSOLA (pitch shifts formants too). Optional build flag: use 01's PSOLA for formant-preserving shift.

### Ratchets
- The step is subdivided into `ratchet_count` equal parts. Each re-triggers the same segment (truncate mode) or the first 1/n of it (stretch mode).

---

## 5. Controls

| Control | Function |
|---|---|
| **TIME** (+att, CV) | Pattern offset in the segment pool |
| **SIZE** (+att, CV) | Pattern length 1–16 steps (quantised) |
| **SHAPE** (+att, CV) | Order: in-order → reverse → fixed shuffle → random/cycle → random/step |
| **DENSITY** (+CV) | External clock: ÷/× ratio. Internal: tempo |
| **PITCH** (+att, CV 1V/oct) | Transposition ±24 st (per-step CV sampling at step start, so a CV sequencer can play the words melodically) |
| **FEEDBACK** (renamed "REPEAT") | Stutter / ratchet probability |
| **DRY/WET**, **REVERB** | Stock |
| **FREEZE** button / gate | Freeze buffer + segment pool (pattern draws from fixed material) |
| **SEED** jack | Clock in |
| **SEED** button | Tap tempo (internal clock); with an external clock, a manual step advance |
| **SEED + FREEZE** | Lock the pattern: regenerate the fixed shuffle, then hold it (random modes stop changing) |
| **MEDIA** sub-modes | 🟢 Syllable / truncate. 🟡 Word / truncate. 🩵 Syllable / stretch-to-fit. 🟣 Word / stretch-to-fit |
| **MEDIA + FREEZE** | Toggle pause removal in the recorder |
| **MEDIA + SEED** | Toggle "swing" (even steps delayed 0–33 %, amount from the ASSIGN hold-pot editing of REPEAT) |
| **ASSIGN** | Destinations: Repeat / Dry-Wet / Reverb / all / **Reset** (5th) |
| **Gate out** | ASSIGN+SEED cycles the gate-output state through off → **step trigger** (5 ms per step and ratchet) → **accent gate** (high for the duration of high-energy steps) → off. Stock firmware has a boolean toggle; here `gate_output_mode` is extended to 3 values |
| **VU LED** | Blinks on each step; brighter on accented steps |

---

## 6. Implementation steps

1. **Prereq:** 03 segmentation complete and stable (shared code).
2. **Python prototype:** the pattern generator plus WSOLA stretch-to-step on corpus clips with a fixed BPM. Check listening quality at ratios 0.5×, 1.5×, 2×, 3×.
3. **Host:** `vox/sequencer/dsp/{clock,pattern_generator,step_player}.h`, `vox/common/synth/wsola.h`.
4. **Firmware v0:** internal clock, truncate mode, in-order pattern. Verify timing jitter on a scope (step start vs clock ≤ 1 block = 250 µs).
5. External clock in on SEED, division/multiplication, tap tempo, reset via the ASSIGN 5th destination.
6. SHAPE orders, REPEAT stutter/ratchets, pattern lock.
7. WSOLA stretch mode.
8. Sub-modes, swing, gate out, settings; regression with a Morphagene clocked from the same source.

## 7. Tests and acceptance

- Clock following: lock within 3 pulses; step jitter ≤ 250 µs (block-quantised; if needed, sample-accurate start using the `GateFlags` per-sample positions already provided by the stock event path).
- WSOLA at 2× stretch: no audible periodic artefacts on vowels (listening), no clicks at step boundaries.
- Pattern lock is deterministic: the same pattern after power-cycle, if the seed is stored in settings.

## 8. Risks / open questions

- **Live pool churn:** in live mode a long pattern can reference segments that fall out of the buffer. Clamp the pool to segments newer than `buffer.oldest() + 1 s`.
- **Word segmentation on fast reads** (see 03) affects word modes. Syllable modes are the robust default.
