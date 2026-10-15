# 07 — `vox_chain`: voice processing chain

> **Status: implemented** in `vox/chain/`. It builds and the host self tests pass (29/29); it has not yet been run on the module. The panel reference and the build, install and test instructions are in `vox/chain/README.md`. Implementation notes and deviations from this plan are in §7.

A broadcast / podcast-style **voice channel strip**: expander/gate → high-pass → de-esser → compressor → EQ → character → limiter, plus a **music-bed ducker** on IN R (radio-ad style). Intended as a front end for the other VOX firmwares' material, a tool for evening out wildly varying Morphagene material, and a character processor (telephone, radio, megaphone, lo-fi).

Depends on: [00-common](00-common.md) (app skeleton, UI, EnvelopeFollower, Vad, BandEnergy). Reuses `beads/dsp/limiter.h` and the stock reverb. **First firmware to build** (see README), because it exercises the whole common platform with simple DSP.

---

## 1. Signal flow

The whole chain runs at **48 kHz** (no SRC, keeps the "air" band).

```
IN L ─► AGC (stock, optional) ─► Expander/Gate ─► HPF ─► De-esser ─► Compressor ─► EQ ─► Character ─┐
                                    ▲ sidechain = pre-chain level                                   │
IN R (music bed) ─► Ducker (key = processed voice level / VAD) ─────────────────────────────────────┤
                                                                                                    ▼
                                  parallel-mix (DRY/WET) ─► reverb ─► output limiter ─► OUT L/R (+ gate out)
```

The voice is mono, output to both channels. The music bed is stereo if IN R is patched (bed duplicated to both sides) and summed after the voice chain.

---

## 2. Stages

| Stage | Design | Key parameters |
|---|---|---|
| **Expander / gate** | Downward expander, peak detector (attack 1 ms, release 100 ms), with hold 50 ms plus VAD-assisted hysteresis | threshold −60…−20 dBFS, ratio 1:2 → 1:∞ (gate) at CW extreme, range −40 dB |
| **HPF** | `stmlib::Svf` 2-pole (12 dB/oct). Frequency fixed per sub-mode preset (80 / 100 / 150 Hz), or 300 Hz in character modes | — |
| **De-esser** | Split-band: sidechain band-pass 5–9 kHz (Svf BP, Q 1.5) vs full-band RMS. When the ratio exceeds the threshold, reduce a **high-shelf** (dynamic, 6 kHz) by up to −12 dB. Attack 0.5 ms, release 40 ms | depth 0…−12 dB |
| **Compressor** | Feed-forward, log-domain, RMS/peak blend detector (τ_RMS 5 ms), soft knee 6 dB, auto make-up, program-dependent release (two-stage: fast 50 ms + slow 500 ms, larger on sustained GR) | amount macro → threshold −10…−40 dB and ratio 1.5…8:1. Time macro → attack 1…30 ms, release 40…800 ms |
| **EQ** | 3 biquads (RBJ cookbook): low shelf 150 Hz, presence peak 3 kHz (Q 1), high shelf 10 kHz | tilt macro: body ↔ presence/air, ±6 dB |
| **Character** | Morph across models (crossfaded): clean → **radio** (BP 200 Hz–5 kHz, mild tanh drive, 1 dB ripple EQ) → **telephone** (BP 300–3400 Hz, μ-law 8 kHz emulation with ZOH and alias) → **megaphone** (BP 500–4 kHz, hard-ish clip, small comb resonance at 1.2 kHz). CCW side: **lo-fi** (sample-rate reduction 24k → 4k plus bit depth 16 → 6 bits, with an optional pre-LPF to keep it clean vs aliasing) | morph 0…1 each side |
| **Ducker** | Key = processed voice envelope (attack 10 ms, release 300 ms, hold 150 ms gated by VAD). The bed gain follows `1 − depth × key_norm` with smoothing | depth 0…−24 dB; bed level |
| **Output limiter** | `beads/dsp/limiter.h` (stock), ceiling −0.5 dBFS | — |

**CPU:** about 10 biquad/SVF sections plus 4 detectors plus μ-law and decimation at 48 kHz is roughly 1.5k cycles per sample, × 12 samples ≈ 18k cycles per block, plus reverb. Within budget.

---

## 3. Controls

| Control | Function |
|---|---|
| **DENSITY** (+CV) | **Compression amount** macro (threshold + ratio + make-up). CV = dynamic squash (e.g. an envelope from Morphagene CV OUT) |
| **TIME** (+att, CV) | Compressor **attack/release** macro: CCW punchy/fast → CW smooth/slow |
| **SIZE** (+att, CV) | **Tone tilt** EQ: CCW = body (low shelf +, presence −) → noon flat → CW = presence + air |
| **SHAPE** (+att, CV) | **Character** morph: noon clean; CW → radio → telephone → megaphone; CCW → lo-fi crush |
| **PITCH** (+att, CV) | **De-esser** depth (0 at CCW of the attenuated range … −12 dB). The 1V/oct calibration is irrelevant here; the knob is used as a plain control |
| **FEEDBACK** (renamed "GATE") | Expander threshold / ratio (CCW off) |
| **DRY/WET** | **Parallel compression** blend (dry = uncompressed but EQ'd and HPF'd; wet = full chain) |
| **REVERB** | Booth/room ambience (stock reverb, pre-delay 10 ms) |
| **FREEZE** button | **Bypass** (true A/B against the AGC-only signal); LED on = bypassed |
| **FREEZE** gate | **Duck/mute voice** while high (20 ms fades). Useful for gating speech from a sequencer |
| **SEED** button | Cycle the VU display: input level → gain reduction → output level → bed level |
| **SEED** gate | Momentary "push": +6 dB of extra compression drive while high (accent words rhythmically) |
| **SEED + FREEZE** | Toggle input AGC (stock `GainControl` auto-gain) on/off for the chain (off = fixed manual gain) |
| **MEDIA** sub-modes (voicing presets) | 🟢 Podcast (gentle, HPF 80, de-ess light). 🟡 Broadcast (heavier compression, presence, HPF 100). 🔴 Announcer/maximised (fast heavy compression, limiter driven, ducker default −15 dB). 🔵 Raw/character (HPF 150, minimal compression, character morph range doubled). Knobs act as offsets around each preset |
| **ASSIGN CV** | As stock across Gate/Dry-Wet/Reverb, plus a 5th destination **Duck depth** (useful for automating the bed level per ad section) |
| **IN R** | Music bed for the ducker when patched; otherwise ignored |
| **Gate out** (ASSIGN+SEED) | OUT R = VAD gate (voice present), for ducking or triggering elsewhere. In gate-out mode the voice is mono on OUT L and the bed is mixed into OUT L |
| **VU LED** | Per SEED display selection |

---

## 4. Implementation steps

1. **Common platform first:** `vox_null` passthrough working (00 milestones 1–2).
2. **Python prototype:** the chain on corpus clips, with presets tuned by ear. Measure loudness consistency (integrated LUFS spread across clips with `pyloudnorm`) as an objective target.
3. **Host:** `vox/chain/dsp/{expander,deesser,compressor,tilt_eq,character,ducker}.h`. Unit tests: compressor static curve, de-esser GR on synthetic sibilants, ducker timing.
4. **Firmware v1:** HPF + compressor + limiter + bypass. Hardware check for noise floor and clipping behaviour with hot Morphagene levels.
5. Expander, de-esser, EQ tilt.
6. Character models (radio/telephone/megaphone/lo-fi) with crossfaded morph.
7. Ducker + IN R bed + VAD gate out.
8. Presets, VU display modes, settings; regression and A/B listening against the Python reference.

## 5. Tests and acceptance

- Loudness: integrated LUFS spread across the corpus reduced by ≥ 60 % in Broadcast preset vs bypass.
- Compressor static curve within ±0.5 dB of design (host sine sweeps).
- Bypass toggle is click-free (10 ms crossfade).
- Ducker: bed reaches −12 dB within 30 ms of speech onset and recovers within 500 ms after speech ends (hold 150 ms).
- Self-noise in clean preset at full make-up gain: document a measured value. Expect the PCM3060 floor to dominate.

## 6. Risks / open questions

- **Make-up gain vs noise:** heavy compression raises Morphagene and codec noise in pauses. The expander and VAD-gated hold mitigate this; the presets set sensible defaults.
- **Knob semantics (PITCH as de-ess depth):** the panel legend won't match. A printed overlay or a cheat-sheet card is needed for all VOX firmwares; consider producing one per firmware from these tables.
- **Stock AGC interaction:** it already normalises input level. Decide in the prototype whether to keep it before the compressor (default on, togglable via SEED+FREEZE).

## 7. Implementation notes (deviations from the plan above)

- **Output limiter:** a new stereo-linked peak limiter (instant attack, 60 ms release, −0.5 dBFS ceiling) replaces `beads/dsp/limiter.h`. The stock limiter recovers linearly over about 6 s, which audibly lowered loudness after every peak.
- **De-esser:** a dynamic 5 kHz high shelf (RBJ, coefficients refreshed every 12 samples; exact pass-through at 0 dB). The SVF split-band version only removed 1.2 dB on sibilants, because the band split overlaps heavily around 5–8 kHz.
- **Compressor detector:** RMS (5 ms) plus a peak path at −10 dB. With the planned "RMS/peak blend" (peak −3 dB), the peak path dominated on speech and caused about 9 dB too much gain reduction. Make-up restores the GR of a −18 dBFS signal.
- **Ducker:** key release 40 ms and recovery 100 ms, so the bed comes back within 500 ms after speech (measured 477 ms, including the 150 ms hold). Onset: −12 dB in 13.5 ms.
- **SEED+FREEZE:** toggles the stock `gain` setting between 0 (auto) and 1 (unity manual gain). No new setting is needed.
- **Manual gain** is MEDIA held + FEEDBACK pot (stock), not the TIME pot.
- **Reverb:** the stock reverb (mix r²·0.7, time 0.25 + 0.55·r³), without the planned 10 ms pre-delay.
- **Character models** all run continuously (no state jumps when morphing). They are level-matched within 4 LU on speech-like material.
- **Measured on synthetic speech** (±9 dB phrase levels): phrase loudness spread 10.4 → 3.2 LU in Broadcast at DENSITY 0.75 (−69 %), which meets the ≥ 60 % target.
- **CPU:** host ≈ 55× real time; the M7 load is to be measured with `vox::profile_max_cycles` (budget 100k cycles per block).
