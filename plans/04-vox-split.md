# 04 — `vox_split`: voiced / unvoiced splitter

> **Status: implemented** in `vox/split/`. It builds and the host self tests pass (16/16); it has not yet been run on the module. The final panel mapping and the test instructions are in `vox/split/README.md`.
>
> Deviations from this plan:
> - The lookahead and consonant delay lines live in `.ramd2` (28,800 floats); there is no speech buffer.
> - YIN runs at a 10 ms hop, with the difference function spread over the audio blocks of the hop.

Classifies speech in real time into **silence, voiced (vowels, nasals, voiced consonants), unvoiced (fricatives, sibilants) and bursts (plosive releases)**. It then processes those streams separately, or sends them to separate outputs so the rest of the system can process them.

Depends on: [00-common](00-common.md) (BandEnergy, Yin, Lpc (k1 only), Vad, VuvClassifier, EnvelopeFollower). Reuses `beads/dsp/multitap_delay.h` ideas and the stock reverb.

---

## 1. Signal flow

```
IN L ─► AGC ─► 48→24k ─┬─► lookahead delay 10 ms ─► mask split ─┬─► V stream ─► Vowel-tail engine ─┐
                       │                                        ├─► U stream ─► Consonant delay ──┤
                       │                                        └─► B stream (bursts) ────────────┤
                       └─► features ─► VuvClassifier ─► smoothed masks m_v, m_u, m_b ─┘         ▼
                                                                                    stream mixer (SHAPE)
                                                                                         ▼
                                                     24→48k ─► dry/wet ─► reverb (send per sub-mode) ─► limiter ─► OUT L/R
```

The dry path is also delayed 10 ms, so dry and wet stay aligned.

---

## 2. Classifier (`VuvClassifier`)

### Features (per 5 ms hop, 10 ms window)

| Feature | Source | Voiced | Unvoiced |
|---|---|---|---|
| Zero-crossing rate | 24 kHz signal | low (< 0.1) | high (> 0.25) |
| Spectral tilt | LPC k1 (order-1 r1/r0) | strongly positive (> 0.6) | low or negative |
| YIN aperiodicity | shared YIN at 12 kHz | < 0.2 | > 0.35 |
| HF/LF energy ratio | 2.5–10 kHz vs 80–900 Hz bands | < −6 dB | > +3 dB |
| Log energy over floor | Vad | > 6 dB | > 3 dB |
| Onset strength (HF) | OnsetDetector high bands | — | burst if > threshold and duration < 30 ms |

### Decision
- A weighted score per class: a linear combination with hand-tuned weights, refined in the Python prototype by logistic regression on labelled frames.
- Hysteresis, plus minimum durations: voiced ≥ 30 ms, unvoiced ≥ 15 ms; a burst is a 5–30 ms event.
- **Bias** from DENSITY (see controls) shifts the V/U decision boundary.

### Masks
- Class → target mask values, smoothed with a 3 ms raised-cosine crossfade.
- Thanks to the 10 ms lookahead, a transition's crossfade completes *before* the acoustic transition, so plosive bursts aren't clipped.
- Masks always satisfy `m_v + m_u + m_b + m_silence = 1`.

### Mixed excitation (voiced fricatives /z/, /v/)
- When both the voiced and the HF scores are high, split by frequency within the frame: a 4 kHz Linkwitz-Riley crossover (2× `stmlib::Svf`) feeds below 4 kHz to V and above to U.
- The frequency split is used **only** while in the "mixed" state.

---

## 3. Stream processors

### V stream: vowel-tail engine
- **Concept:** at the end of each voiced run, keep looping its final pitch periods as a decaying tail ("vowel freeze tails") underneath the ongoing speech. This lengthens vowels without the output drifting behind real time.
- **Capture:** a 2048-sample ring (85 ms) of the V stream. On the voiced→other transition, take the last `2 × T0` (T0 from YIN), find the zero-crossing-aligned period boundaries, and spawn a tail voice.
- **Tail voice:**
  - pitch-synchronous loop of one or two periods (crossfaded loop points)
  - resampled by PITCH
  - exponential decay with time constant from SIZE
  - optional slow random period selection between the two captured periods, avoiding a static buzz
- **Polyphony:** max 4 tails; the oldest is stolen with a fade.
- FREEZE holds all current tails indefinitely (decay = ∞), giving a drone of the last vowels.

### U stream: consonant delay
- Mono delay up to 1.2 s (24 kHz × 1.2 × 4 B = 115 KB float, or int16 = 58 KB) in D2. Feedback, a high-pass in the loop (300 Hz), and an optional ping-pong across L/R in stereo sub-modes.
- TIME = delay time. SEED gate clock-syncs it: the measured period between gates sets the delay time, and TIME selects divisions/multiples (÷4…×2).

### B stream: bursts
- Routed with U in green/yellow; separately in the red output split (see sub-modes).

### Reverb send
- Per sub-mode (see table). By default only the U stream feeds the reverb, so consonants become airy washes while vowels stay dry.

---

## 4. Controls

| Control | Function |
|---|---|
| **SHAPE** (+att, CV) | Stream balance. Noon = all streams equal. CW → voiced only (consonants removed: a mumbling hum of vowels plus tails). CCW → unvoiced + bursts only (vowels removed: a whispered skeleton) |
| **SIZE** (+att, CV) | Vowel-tail length: 0 (off) … 4 s decay time |
| **PITCH** (+att, CV 1V/oct) | Vowel-tail transposition ±24 st; tails become harmonised drones |
| **TIME** (+att, CV) | Consonant delay time, 10 ms…1.2 s, or the clock division when SEED is clocked |
| **FEEDBACK** | Consonant delay feedback (0…105 % with soft-clip in the loop) |
| **DENSITY** (+CV) | Classifier bias. Noon = neutral. CW = more material counted as voiced. CCW = more counted as unvoiced. CV gives rhythmic or dynamic reclassification effects |
| **DRY/WET**, **REVERB** | Dry/wet as stock; reverb = send amount for the streams chosen by the sub-mode |
| **FREEZE** button / gate | Hold all tails (infinite sustain) |
| **SEED** button / gate | Force-capture a tail from the current V buffer now (even mid-vowel). If SEED is patched with a regular clock, it also sets the delay clock |
| **SEED + FREEZE** | Latch "mute input V": only tails and consonants pass, a sustained vowel bed under live consonants |
| **MEDIA** sub-modes | 🟢 Process: V→tails, U→delay, reverb on U. 🟡 Swap: V→delay+reverb, U→tails (noise tails: sibilant "hiss pads"). 🔴 **Splitter:** no processing; OUT L = V stream, OUT R = U+B stream (DRY/WET = split vs original mix; patch each into different modules). 🔵 Clean-up: U attenuated by SHAPE (a de-esser by class); silence gated (noise-floor expander); V passthrough |
| **MEDIA + SEED** | Classifier preset: speech / speech-over-music (heavier YIN weighting) / lo-fi phone |
| **Gate out** (ASSIGN+SEED) | OUT R = voiced gate (green/yellow/blue). Not available in 🔴 because OUT R is carrying U audio |
| **VU LED** | Off = silence, dim = unvoiced, bright = voiced, flash = burst. A live classifier display |

---

## 5. Implementation steps

1. **Python prototype:** features + classifier. Hand-label about 3 minutes of corpus at frame level (praat). Train logistic weights; report a confusion matrix. Prototype tails and masks; listen.
2. **Host:** `vox/common/analysis/vuv_classifier.h` (shared), `vox/split/dsp/{mask_splitter,vowel_tails,consonant_delay}.h`.
3. **Firmware v0:** 🔴 splitter sub-mode only. Lowest DSP, and it proves the classifier on hardware (VU LED display + two outputs).
4. 🔵 clean-up sub-mode.
5. Vowel tails with PITCH/SIZE/FREEZE.
6. Consonant delay with clock sync.
7. 🟢/🟡 routing, SHAPE balance, reverb send, DENSITY bias.
8. Classifier presets, settings, regression (including Morphagene varispeed: at 2× all ZCRs double. The preset or an auto-normalisation of ZCR by the long-term median handles this; decide in the prototype).

## 6. Tests and acceptance

- Frame classification accuracy (V vs U vs silence) ≥ 90 % on clean speech, ≥ 80 % on Morphagene-processed corpus.
- Splitter null test: OUT L + OUT R (summed in the host harness) equals the 10 ms-delayed input within −40 dB.
- No clicks at class transitions with SHAPE at either extreme.
- Tails: no DC build-up or clicks at loop points; 4 simultaneous tails at +24 st within the CPU budget.

## 7. Risks / open questions

- **Music beds:** cymbals and hats classify as unvoiced. The speech-over-music preset weights aperiodicity plus VAD more; accept some leakage.
- **Voiced fricative frequency split:** may sound phasey. The prototype decides whether to keep it or classify mixed frames simply as V.
- **Lookahead:** 10 ms added latency on the whole module. Acceptable for this material; document it.
