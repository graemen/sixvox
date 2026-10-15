# 01 — `vox_psola`: pitch-synchronous granular voice

> **Status: implemented** in `vox/psola/`. It builds and the host self tests pass (19/19); it has not yet been run on the module. The final panel mapping and the test instructions are in `vox/psola/README.md`.
>
> Deviations from this plan:
> - The speech buffer is 2^18 int16 samples (10.9 s at 24 kHz) in `.ramd1` only, not the 16.4 s planned in [00-common](00-common.md).
> - The pitch-mark table has 8,192 entries (64 KB) in `.ramd2` instead of a 10k-entry ring in DTCM; that covers 600 marks/s over the whole buffer.
> - YIN runs at a 5 ms hop, with the difference function spread over the audio blocks of the hop.

Formant-preserving pitch shift, pitch-independent time stretch/scrub, and **intonation control** (flatten, exaggerate or quantise speech melody) for spoken word, using TD-PSOLA driven by a live pitch tracker.

Depends on: [00-common](00-common.md) (SpeechBuffer, MarkerTable, Yin, Vad, OlaGrain/OlaAccumulator).

---

## 1. Signal flow

```
IN L ─► AGC ─► 48→24k ─┬─► SpeechBuffer (int16, ≤16.4 s) ◄── feedback
                       │
                       └─► ÷2 → 12k ─► YIN (hop 5 ms) ─► F0 track ─► PitchMarker ─► MarkTable
                                                                    (refine on 24k)

Read head (TIME, DENSITY) ─► nearest analysis mark ─► grain(2·T0, Hann, formant resample)
Synthesis clock (F0_out from PITCH/SHAPE) ─► OLA accumulator ─► 24→48k ─► dry/wet ─► reverb ─► limiter ─► OUT
```

Live and frozen operation work the same way. Live, the read head trails the write head by `TIME`. Frozen, `TIME` is an absolute position in the buffer.

---

## 2. Analysis

### 2.1 F0 tracking (YIN, shared module)
- 12 kHz copy, window 25 ms, hop 5 ms (200 frames/s).
- Search range 50–600 Hz by default (wider than natural speech, to tolerate Morphagene varispeed). Sub-mode menu option: "narrow" 70–400 Hz, which gives fewer octave errors.
- Outputs per hop: `f0`, `aperiodicity` (YIN d′ minimum), `rms`.
- **Voicing decision:** `aperiodicity < 0.15 && rms > vad_floor + 6 dB`, with 2-hop hysteresis.
- **Octave-error guard:** median-of-5 over voiced hops. Reject a jump > 0.8 octave unless it persists for 3 hops.

### 2.2 Pitch marks (`PitchMarker`)
- **Voiced:** predict the next mark at `last_mark + T0` (T0 in 24 kHz samples, interpolated between hops). Refine by searching ±T0/4 for the maximum of the **negative peak** of a 1 kHz low-passed copy (an approximate glottal closure instant). Store `{pos:u32, period:u16, voiced:1, conf:7}` (8 bytes).
- **Unvoiced:** synthetic marks every 8 ms with ±1 ms jitter (prevents buzzy periodicity when unvoiced grains are repeated). Period field = 192 samples.
- **Table size:** worst case 600 marks/s × 16.4 s ≈ 9,840 marks × 8 B ≈ 79 KB → DTCM. A 10 k-entry ring (80 KB) is allocated; if DTCM is tight, cap F0 at 400 Hz (52 KB).
- **Latency:** a mark is final about 15 ms after its position (YIN window + refine). The live read head is clamped to ≥ 30 ms behind the write head.

---

## 3. Synthesis (TD-PSOLA)

- **Synthesis mark clock:** advance a fractional phase by `F0_out / 24000` per sample. On wrap, emit a new grain.
- **F0_out** = `f0_source × pitch_ratio × intonation(shape)` for voiced material. For unvoiced, grains are emitted at the analysis mark spacing scaled by the time-stretch (no pitch meaning).
- **Choosing the analysis mark:** the read head `r` (float, absolute sample position) advances by `stretch × 1` per output sample. At each synthesis mark, the grain source is `marks.Nearest(r)`.
- **Grain:**
  - length `2 × T0_src` (two source periods), Hann window, centred on the analysis mark
  - read with Hermite interpolation at rate `formant_ratio` (SIZE)
  - written into the OLA accumulator centred on the synthesis mark
  - amplitude normalised by the window overlap sum (prevents level change at high pitch ratios)
- **Polyphony:** at most 8 simultaneous grains (pitch +2 octaves with formant −1 octave is the worst case). If the cap is reached, the oldest grain is dropped with a 1 ms fade.
- **Accumulator ring:** 4096 floats (16 KB) in DTCM.
- **Unvoiced passages:** grains from synthetic marks, with random ±2 ms read offset, giving noise-like overlap-add without phasiness.
- **Low-confidence fallback:** when a region is marked low confidence (overdubbed or polyphonic Morphagene material), the engine crossfades over 20 ms to plain asynchronous granular (fixed 30 ms grains, 50 % overlap, Beads-like). This avoids PSOLA artifacts on music beds.

### CPU estimate (24 kHz)

| Item | Cost |
|---|---|
| YIN | ~2.3k MAC/block (amortized) |
| Mark refine | < 1k per mark |
| 8 grains × 6 samples × ~40 cycles | ~2k cycles/block |
| SRC down/up | ~5k |
| Reverb + limiter | stock cost (~15–20k, profile) |

Comfortably below the 60k target.

---

## 4. Controls

| Control | Function | Range / notes |
|---|---|---|
| **TIME** (+att, CV) | Live: delay behind write head. Frozen: absolute position | 30 ms – full buffer; CV adds |
| **DENSITY** (+CV) | Time-stretch rate of the read head | Noon = 0 (hold the current mark: sustained vowel); CW → 0…+2×; CCW → 0…−2× (reverse). Detent zone ±3 %. Live mode clamps forward > 1× once the head reaches the write head |
| **PITCH** (+att, CV 1V/oct) | F0 transposition, formants preserved | ±24 st; calibrated 1V/oct via `cv_reader` |
| **SIZE** (+att, CV) | Formant shift (vocal-tract size) | −12…+12 st grain resample ratio; noon = natural |
| **SHAPE** (+att, CV) | Intonation | Noon = natural. CW → flatten toward monotone at the PITCH-defined note (full CW = fixed pitch; with PITCH CV this makes "sung" speech). CCW → exaggerate deviations from the running mean F0, up to 3× in log-F0 |
| **FEEDBACK** | Output re-recorded into the buffer | Stock behaviour; enables cumulative re-pitching |
| **DRY/WET**, **REVERB** | Stock | |
| **FREEZE** button / gate | Stop recording + mark analysis; buffer and marks frozen | |
| **SEED** button / gate | Frozen: jump the read head back to TIME (retrigger a word). Live: momentarily hold the current mark (stutter) while high | Morphagene EOSG → SEED re-syncs per splice |
| **SEED + FREEZE** | Latch loop between the TIME position and the position when latched | |
| **MEDIA** sub-modes | 🟢 PSOLA. 🟡 Scale-quantised F0 (SHAPE = correction strength, root = PITCH; chromatic by default, MEDIA+SEED cycles chromatic/major/minor/pentatonic). 🔴 Period stutter: each source period repeated N times (N from SIZE CW half; vocal fry/robot). 🔵 Narrow F0 range (70–400 Hz), otherwise as green | |
| **Gate out** (ASSIGN+SEED) | OUT R = voiced gate of the *read head's* material | Drive envelopes/VCAs from vowels |
| **VU LED** | Input level; flashes on F0 tracking loss | |

---

## 5. Implementation steps

1. **Python prototype** (`proto/psola.py`): YIN + marks + TD-PSOLA on corpus files. Reference listening at pitch ±12, formant ±6, stretch 0.5/2×, and monotone.
2. **Host engine:** `vox/psola/dsp/{pitch_tracker,pitch_marker,psola_engine}.h`. Diff mark positions and output against the prototype.
3. **Firmware v0:** live PSOLA with fixed parameters on hardware; profile the ISR.
4. **Controls:** all knob mappings; DENSITY detent; freeze/scrub; seed behaviours.
5. **Intonation (SHAPE):** running mean of log-F0 (τ 2 s); quantiser for the yellow sub-mode, with glide smoothing (τ 20 ms) to avoid steppy jumps.
6. **Low-confidence crossfade** to the asynchronous granular fallback.
7. Sub-modes red/blue, gate out, settings persistence.
8. Corpus regression test plus Morphagene varispeed and overdub tests.

## 6. Tests and acceptance

- F0 gross error (> 20 % vs praat/pYIN reference) < 3 % of voiced frames on clean speech; < 10 % on Morphagene-processed corpus.
- No audible clicks when sweeping TIME across the full buffer, including across the D1/D2 seam.
- Pitch ±12 st with SIZE at noon: formants measured with praat stay within 10 % of the source.
- Monotone mode tracks a 1V/oct sequence within ±10 cents over 3 octaves.
- ISR worst case < 80k cycles with all features enabled.

## 7. Risks / open questions

- **Glottal closure instant refinement** on band-limited or reverberant material: may need a smoothed linear-prediction residual instead of the low-passed waveform. Prototype both.
- **Morphagene's own grain/varispeed artefacts** at extreme speeds produce periodicities that YIN may lock onto. Mitigation: the narrow-range sub-mode.
- **Speech over a music bed (ads):** expect frequent fallback to async granular; acceptable as designed.
- **DTCM budget:** the mark table plus the accumulator plus stack must fit 128 KB. Verify the linker map early (milestone 3).
