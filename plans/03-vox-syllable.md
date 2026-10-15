# 03 — `vox_syllable`: onset / syllable-aware granular

> **Status: implemented** in `vox/syllable/`. It builds and the host self tests pass (17/17); it has not yet been run on the module. The final panel mapping and the test instructions are in `vox/syllable/README.md`.
>
> Deviations from this plan:
> - The speech buffer is 2^18 int16 samples (10.9 s at 24 kHz) in `.ramd1` only, not the 16.4 s planned in [00-common](00-common.md).
> - Syllable boundaries found late inside a word are snapped back to the nearest onset (`Segmenter::SnapBackToOnset`), which raised boundary recall on the test signals to 97 %.

A granular engine whose grains **start and end on phonetic boundaries**: onsets, syllables, words. Speech stays intelligible (or is fragmented deliberately, by units) instead of being sliced at arbitrary points. Optional **pause removal** keeps only speech in the buffer.

Depends on: [00-common](00-common.md) (SpeechBuffer, MarkerTable, BandEnergy, Vad, OnsetDetector, SyllableNucleus, OlaGrain). Reuses Beads' grain envelope ideas (`beads/dsp/grain_envelope.h`).

The segment-table code from this firmware is reused by [06-vox-sequencer](06-vox-sequencer.md).

---

## 1. Signal flow

```
IN L ─► AGC ─► 48→24k ─┬─► [pause gate] ─► SpeechBuffer
                       └─► BandEnergy(5) ─► OnsetDetector ─┐
                                         ├─► SyllableNucleus├─► Segmenter ─► SegmentTable
                                         └─► Vad ──────────┘

Trigger source (DENSITY / SEED) ─► segment selector (TIME, SHAPE, unit) ─► grain voices (≤ 6)
     ─► 24→48k ─► dry/wet ─► reverb ─► limiter ─► OUT
```

---

## 2. Segmentation

### 2.1 Features (per sample → decimated to 1 kHz feature rate)
- 5 bands from `BandEnergy` (80–300, 300–900, 900–2.5k, 2.5–5k, 5–10 kHz), log-energy, smoothed 5 ms.
- **Onset function:** sum over bands of the rectified first difference of log-energy (spectral-flux-like). The 2.5–10 kHz bands are weighted ×1.5 for consonant onsets.
- **Syllable nucleus function:** 300–900 Hz band energy, low-passed at 10 Hz (the syllable-rate envelope, 3–8 Hz).
- **VAD:** energy over a tracked noise floor, hangover 150 ms.

### 2.2 Markers

| Type | Rule |
|---|---|
| **Onset** | Onset function exceeds adaptive threshold (`median_200ms + δ`, δ set by sensitivity); refractory 50 ms |
| **Nucleus** | Local max of the nucleus function with prominence ≥ 3 dB and spacing ≥ 100 ms |
| **Syllable boundary** | Minimum of the nucleus function between consecutive nuclei, snapped to the nearest onset within ±30 ms |
| **Word/phrase boundary** | VAD speech→silence transition with silence ≥ 120 ms (word gap) or ≥ 400 ms (phrase) |

### 2.3 Segment table

- **Record:**
  - `start:u32`, `end:u32`, `nucleus:u32`
  - `level:u8` (0 = onset-slice, 1 = syllable, 2 = word, 3 = phrase)
  - `energy:u8`, `flags:u8`
  - 16 B per record
- **Capacity:** 1024 entries (16 KB) → DTCM.
- A segment is **finalised** once its end boundary plus 30 ms lookahead has passed.
- **Index addressing:** segments are numbered from the newest finalised one backwards (live) or from the oldest forwards (frozen); see the TIME control.

### 2.4 Pause removal (optional, sub-mode 🔵 or MEDIA+FREEZE toggle)

- While VAD = silence for > `keep_ms` (default 80 ms), stop writing to SpeechBuffer. On resume, write a 5 ms crossfade.
- **Result:** the buffer holds about 16 s of *speech* rather than 16 s of wall-clock time. Segments still line up because positions are buffer-absolute.

---

## 3. Grain engine

- Up to **6 voices** (`OlaGrain`). Each voice plays one or more consecutive segments `[seg_i … seg_{i+n−1}]`.
- **Envelope:** attack 2 ms at the segment start. Release 2–20 ms ending exactly at the segment end, or truncated by SIZE. Hann/trapezoid shape from SHAPE's lower half.
- **Pitch:** resampling (Hermite), ±24 st. A PSOLA build-flag option reuses 01's engine for formant-preserving transposition later.
- **Direction:** forward; reverse probability from SHAPE's upper extreme.
- **Voice stealing:** oldest first, with a 3 ms fade (`beads/dsp/voice_stealing_envelope.h` is the reference).

### CPU estimate
6 voices × 6 samples × ~40 cycles ≈ 1.5k, plus analysis ~2k, plus SRC and reverb. Low.

---

## 4. Controls

| Control | Function |
|---|---|
| **TIME** (+att, CV) | Segment address. Live: 0 = newest finalised segment, CW = further back. Frozen: 0 = first segment in the buffer. **Quantised to segment indices** at the current unit level. CV can address words directly, e.g. from a sequencer |
| **SIZE** (+att, CV) | Grain length in units. CCW half: fraction of a segment (onset→nucleus only, then shorter: "phoneme crumbs"). Noon: exactly 1 unit. CW half: 2…8 consecutive units |
| **DENSITY** (+CV) | Trigger generator as in Beads: noon = off; CW = regular rate 0.5–20 Hz; CCW = random (Poisson) rate. When the SEED gate is patched, triggers come only from SEED (as stock) |
| **PITCH** (+att, CV 1V/oct) | Transposition ±24 st |
| **SHAPE** (+att, CV) | Scatter. CCW = exact segment at TIME, sharp envelopes. Noon = ±2 units random spread, soft envelopes. CW = ±16 units spread + up to 50 % reverse probability |
| **FEEDBACK** | Stock: output re-recorded into the buffer, then re-segmented (fragments of fragments) |
| **DRY/WET**, **REVERB** | Stock |
| **FREEZE** button / gate | Stop recording; segment table frozen (stable addressing) |
| **SEED** button / gate | Fire a grain at the TIME segment. Gate in = rhythmic syllable triggering. Morphagene EOSG → SEED |
| **SEED + FREEZE** | Latch: auto-retrigger the same unit at its end (seamless loop of a word or syllable) |
| **MEDIA** sub-modes (unit level) | 🟢 Syllable. 🟡 Word. 🔴 Onset slices (sub-syllable). 🔵 Syllable + pause removal |
| **MEDIA + SEED** | Cycle detection sensitivity: low / medium / high (LED flash count) |
| **MEDIA + FREEZE** | Toggle pause removal in any sub-mode |
| **Gate out** | OUT R = trigger (5 ms) on every detected onset in the **live input**, at the current unit level. Speech rhythm becomes a clock for the rest of the case |
| **VU LED** | Input level; blinks on each segment finalisation |

---

## 5. Implementation steps

1. **Python prototype:** features → markers → segments. Plot against hand-labelled syllable/word boundaries on 5 corpus clips (praat TextGrids). Tune thresholds.
2. **Host:** `vox/common/analysis/{onset_detector,syllable_nucleus,segmenter}.h` and `vox/common/buffer/segment_table.h` (shared with 06). Compare with the prototype.
3. **Firmware v0:** segmentation running live; gate out fires on onsets (verify against a scope or another module). Nothing else.
4. Grain engine with TIME addressing and SEED triggering.
5. SIZE unit lengths, SHAPE scatter/reverse, DENSITY trigger generator.
6. Freeze, latch, pause removal (check buffer write/skip seams for clicks).
7. Sub-modes, sensitivity, settings; regression on corpus including Morphagene output at 0.5×/2×. At those speeds, syllable rates scale: verify that the thresholds adapt or the sensitivity menu covers them.

## 6. Tests and acceptance

- Syllable boundary detection: ≥ 80 % within ±40 ms of hand labels on clean speech; word boundaries ≥ 90 % for gaps ≥ 120 ms.
- TIME CV sweep across the full table: no clicks, monotonic addressing.
- Pause removal: no audible seams at 1× playback of the buffer (dry listen with DRY/WET full wet, SIZE noon, regular triggers).

## 7. Risks / open questions

- **Fast ad reads** with little pause between words: word-level segmentation degrades to syllables. Document it; the sensitivity menu helps.
- **Music under speech** produces false onsets on drum hits. Mitigation: weight the 300–900 Hz band; optionally require VAD = speech.
- **Index instability while live:** as new segments arrive, "segment N back" shifts. This is by design (it follows the speech). Freeze gives stable indices.
