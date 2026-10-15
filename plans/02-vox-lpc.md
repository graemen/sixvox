# 02 — `vox_lpc`: LPC analysis / resynthesis

> **Status: implemented** in `vox/lpc/`. It builds and the host self tests pass (15/15); it has not yet been run on the module. The final panel mapping and the test instructions are in `vox/lpc/README.md`.
>
> Deviations from this plan:
> - The speech buffer is 2^18 int16 samples (10.9 s at 24 kHz) in `.ramd1` only, not the 16.4 s planned in [00-common](00-common.md); there is no audio/frame-table split of D2, so the buffer length is the same as in the other firmwares.
> - There is no stored frame table. The LPC analysis runs at the read head, every 10 ms (not 5 ms), spread over 40 audio blocks (one warped all-pass stage per block), and coefficients are interpolated per sample. Only voicing and F0 are stored (a 4,096-entry track in `.ramd2`).
> - YIN runs at a 10 ms hop.

Separates speech into a **spectral envelope** (LPC filter: formants, articulation) and an **excitation** (residual, pulse, noise, or an external carrier), then lets you swap, warp, freeze and re-time each independently. Covers: whisper, robot/monotone, playable "talking synth", talk-box/vocoder cross-synthesis, formant warp, and a lo-fi "speaking chip" mode.

Depends on: [00-common](00-common.md) (SpeechBuffer, Lpc, Yin, Vad, VuvClassifier, MarkerTable).

---

## 1. Signal flow

```
IN L ─► AGC ─► 48→24k ─┬─► SpeechBuffer (audio)
                       └─► pre-emph ─► frame 20 ms / hop 5 ms ─► Lpc(order p) ─► FrameTable
                                         └─► Yin (12k) ─► f0, voicing ────────────┘

Read head (TIME, DENSITY) ─► interpolate frames (reflection coeffs, gain, f0, voicing)
                                    │
Excitation ─────────────────────────┼─► lattice synthesis (optionally warped) ─► de-emph ─► gain match ─► 24→48k ─► mix/reverb/limiter ─► OUT
  ├─ residual (inverse-filter buffered audio at read head)
  ├─ glottal pulse train @ f0_out
  ├─ noise
  └─ IN R carrier (pre-whitened)
```

---

## 2. Analysis

- **Frame:** 480 samples (20 ms) Hamming, hop 120 samples (5 ms, 200 frames/s). Pre-emphasis `1 − 0.94 z⁻¹`.
- **Order** p = 24 at 24 kHz (rule of thumb fs/1 kHz + 2…4). The "speaking chip" sub-mode uses p = 10 at an effective 8 kHz (see §5).
- **Autocorrelation** with lag window (Gaussian, 60 Hz bandwidth) and white-noise correction (r0 × 1.0001) for stability. Then Levinson-Durbin → reflection coefficients `k[1..p]`, prediction error `E` → gain `g = sqrt(E)`.
- **CPU:** ≈ 12k MAC per hop. Amortized over 20 blocks, or done in one block (≈ 15k cycles), both fine.
- **Per frame also store:** `f0`, `voicing` and `aperiodicity` (from YIN), and `rms` of the input frame (for output gain matching).

### Frame table (for frozen / scrubbed / stretched playback)

- **Record:**
  - `k[24]` as int16 Q15 (48 B)
  - `g` (float16 or log-int16, 2 B)
  - `f0` (u16 centi-Hz/4, 2 B)
  - `voicing/aperiodicity` (1 B)
  - `rms` (log, 1 B)
  - total ≈ 54 B, padded to 56 B
- **At 200 frames/s:** 11.2 KB/s. Use hop 10 ms for the table (100 frames/s, 5.6 KB/s); live synthesis still interpolates per sub-block.
- **Placement:** take **96 KB of D2** for the table (≈ 17 s at 10 ms hop). The audio buffer then = D1 512 KB + D2 160 KB = 672 KB → **14 s**. Table length ≥ audio length, so every audio position has frames.

---

## 3. Synthesis

- **Frame interpolation:** linear interpolation of **reflection coefficients** (keeps |k| < 1, so the lattice remains stable), applied per 6-sample sub-block (one internal block). Log-domain interpolation for gain and f0.
- **Lattice all-pole synthesis filter**, order p: ~2p multiply-adds per sample. At p = 24 this is ~50 MAC × 6 samples ≈ 300 MAC per block, which is trivial.
- **Formant warp (SIZE):** replace each unit delay in the lattice with a first-order all-pass `A(z) = (z⁻¹ − λ)/(1 − λz⁻¹)`, λ ∈ [−0.4, +0.4] (a warped lattice). λ > 0 shifts formants down (bigger vocal tract); λ < 0 shifts them up. About 3× the lattice cost; still small.
  - Implementation note: warped all-pole synthesis has a delay-free loop. Use the standard reformulation (Härmä, "Implementation of frequency-warped recursive filters").
  - **Fallback if unstable at extremes:** pole-angle scaling via a lookup on log-area ratios is not exact. Alternatively, restrict λ to ±0.3.
- **De-emphasis** `1/(1 − 0.94 z⁻¹)`.
- **Gain match:** compute the output RMS over 10 ms and slew the gain to match the stored frame `rms` × level (τ 5 ms), with a ceiling of +12 dB to avoid blowing up on noise excitation.

### Excitation sources

| Source | Implementation |
|---|---|
| **Residual** | Inverse-filter (FIR lattice, same k) the buffered audio at the read head. With DENSITY at 1× and SIZE at noon this reconstructs the input (transparent). With stretch ≠ 1, the residual is read with the PSOLA grain engine from 01, if present, or with plain 30 ms granular |
| **Pulse** | Band-limited glottal pulse: a Rosenberg-style pulse, a 1024-entry wavetable read with PolyBLEP-corrected phase, or a BLIT. Rate `f0_out` |
| **Noise** | White noise (xorshift32) with spectral tilt from k1 |
| **Carrier (IN R)** | IN R → pre-whitening (order-8 LPC inverse filter on the carrier itself, hop 10 ms), which flattens the carrier so the speech envelope dominates. Enabled automatically when IN R is patched (`normalization_detector`) in the magenta sub-mode |

- **Voicing-aware mix:** in "auto" voicing, voiced frames use pulse/carrier and unvoiced frames use noise, crossfaded over 10 ms on V/UV transitions. SHAPE biases this mix.
- **f0_out:**
  - **follow:** `f0_src × pitch_ratio`
  - **fixed:** the PITCH note from 1V/oct, which gives a "singing" robot
  - selected by SHAPE region; see the controls table.

---

## 4. Controls

| Control | Function |
|---|---|
| **TIME** (+att, CV) | Live: delay behind write head (min 25 ms = analysis latency). Frozen: absolute frame position |
| **DENSITY** (+CV) | Frame playback rate. Noon = 0 (hold the current frame: frozen vowel shape); CW 0…+2×; CCW 0…−2× (reverse speech). Excitation keeps running, so a held frame "sings" |
| **PITCH** (+att, CV 1V/oct) | Excitation pitch. Follow mode: transposition ±24 st. Fixed mode: absolute note (C2–C6 range over CV) |
| **SIZE** (+att, CV) | Formant warp λ (−0.4…+0.4); noon = natural |
| **SHAPE** (+att, CV) | Excitation morph. Sub-modes green/blue: CCW = noise (whisper) → 10 o'clock = auto V/UV (pulse+noise) → noon = original residual → 2 o'clock = pulse follow-pitch → CW = pulse fixed-pitch (monotone/robot). Magenta: CCW = residual → CW = carrier (cross-synthesis amount) |
| **FEEDBACK** (renamed "SLEW") | Envelope smoothing: one-pole across frames on k[] (τ 0…500 ms). High settings smear articulation into slowly morphing vowels. Audio feedback is not used in this firmware |
| **DRY/WET**, **REVERB** | Stock |
| **FREEZE** button / gate | Stop recording + analysis. Buffer and frame table frozen; TIME scrubs |
| **SEED** button / gate | Frozen: restart the read head at TIME ("speak this word again"). Live: hold the current frame while high (gate-controlled vowel freeze). Morphagene EOSG → SEED |
| **SEED + FREEZE** | Latch frame hold (sustained formant drone, playable via PITCH) |
| **MEDIA** sub-modes | 🟢 Resynth (residual-centred SHAPE). 🔵 Synth excitation (pulse/noise only, SHAPE as above, no residual). 🟣 Cross-synthesis (IN R carrier; LED blinks if IN R unpatched). 🔴 "Speaking chip" (§5) |
| **MEDIA + SEED** | Toggle f0 source for "follow": detected f0 ↔ smoothed (τ 200 ms) detected f0 (smoother robot) |
| **Gate out** | OUT R = voiced gate from the frame at the read head |
| **VU LED** | Input level; in cross-synth mode shows carrier level |

---

## 5. "Speaking chip" sub-mode (🔴)

A lo-fi aesthetic suited to ads and speeches: early-1980s LPC speech-synthesis character, built from **original** quantisation tables. No copying of TI ROM data.

- Internal 8 kHz path (an extra ÷3 decimation from 24 kHz, via a polyphase FIR designed offline). Order 10, 40 frames/s (25 ms).
- **Quantisation:**
  - k1–k2: 5 bits; k3–k4: 4 bits; k5–k8: 3 bits; k9–k10: 2–3 bits
  - non-uniform tables designed in the Python prototype by Lloyd-Max on the corpus
  - energy: 4 bits (log)
  - pitch: 6 bits (log); voicing 1 bit
- **Excitation:** a fixed 32-sample chirp-like voiced pulse (designed, not copied) plus a 1-bit noise LFSR for unvoiced.
- **Output:** 8-bit-style quantisation and zero-order hold to 24 kHz before the anti-image filter. SIZE controls a playback rate offset (the chip's clock), giving pitch and formant shift together, like detuning the chip's clock.

---

## 6. Implementation steps

1. **Python prototype:** LPC analysis/resynthesis with residual (verify reconstruction SNR > 30 dB at 1×). Then pulse/noise excitation, cross-synthesis with pre-whitening, warped lattice, and the speaking-chip quantiser design.
2. **Host engine:** `vox/lpc/dsp/{lpc_analyzer,frame_table,lattice,warped_lattice,excitation,carrier_whitener}.h`.
3. **Firmware v0:** live analysis→residual→lattice passthrough (null test on hardware: output ≈ input).
4. Pulse and noise excitation plus the SHAPE morph.
5. Frame table, freeze/scrub/stretch (DENSITY), SEED behaviours.
6. Cross-synthesis sub-mode (IN R).
7. Warped lattice (SIZE) with stability tests at λ extremes and k near ±1.
8. Speaking-chip sub-mode.
9. Slew, gate out, settings; corpus and Morphagene regression.

## 7. Tests and acceptance

- Residual resynthesis at neutral settings: null-test residual ≤ −30 dB vs the 10 ms-delayed input (host). Audibly transparent on hardware.
- 24 h soak with random CV on all inputs: no NaN, no instability (watch k clamp counters).
- Cross-synthesis intelligibility: an informal listening test with saw-wave and noise carriers.
- ISR worst case < 80k cycles (analysis + synthesis + carrier whitening + reverb).

## 8. Risks / open questions

- **Gain blow-ups** with noise/carrier excitation on frames with sharp formant peaks. Mitigation: the gain-match ceiling and k magnitude clamp (|k| ≤ 0.995).
- **Warped-lattice implementation complexity.** If it slips, ship v1 with SIZE = excitation-independent "playback rate of the filter track only", and do the warp in v2.
- **Music beds under speech (ads)** produce LPC envelopes of the mix. Acceptable as an effect; document it.
- **D2 split** between audio and the frame table changes the buffer length vs other firmwares (14 s vs 16.4 s). Acceptable.
