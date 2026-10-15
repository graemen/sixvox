# vox_lpc: linear-prediction voice processor for Beads

# 14-10-2026 Sounds great

Splits speech into a **spectral envelope** (an order-24 LPC lattice filter: formants and articulation) and an **excitation** (the original residual, a pulse train, noise, or an external carrier on IN R). Each part can then be swapped, warped, frozen and re-timed separately: whisper, robot or monotone voices, a playable "talking synth" from a held vowel, talk-box style cross-synthesis, formant warp, and a lo-fi "speaking chip" mode. Plan: [`../plans/02-vox-lpc.md`](../plans/02-vox-lpc.md).

```
IN L ─► 48→24 kHz ─► speech buffer (10.9 s) ─► 12 kHz YIN ─► voicing / F0 track
read head (TIME, DENSITY) ─► source reader (30 ms Hann grains, 50 % overlap) ─► pre-emphasis
   ─► lattice inverse filter (analysed at the read head every 10 ms) ─► residual
   ─► excitation mix (SHAPE: residual / pulse / noise / whitened IN R)
   ─► lattice synthesis (warped analysis for SIZE, slewed by FEEDBACK) ─► de-emphasis
   ─► 24→48 kHz ─► dry/wet, reverb, limiter ─► OUT
```

With SIZE at noon and SHAPE on "residual", the synthesis filter is the exact inverse of the analysis filter, so the voice passes through unchanged (only delayed by 62.5 ms).

## Panel

| Control | Function |
|---|---|
| **TIME** (+att/CV) | Live: delay behind the input. Frozen: position in the buffer |
| **DENSITY** (+CV) | Frame speed. Noon = hold (a frozen vowel shape). CW = 0 to +2× (with a flat zone at 1×). CCW = reverse |
| **PITCH** (+att/CV) | Excitation pitch (stock interval law, 1V/oct CV). With a "follow" excitation it transposes the detected F0. With a fixed excitation it sets the note (C3 at 0) |
| **SIZE** (+att/CV) | Formant warp ±6 semitones via warped LPC analysis; noon = natural. In 🔴: chip clock (pitch and formants move together) |
| **SHAPE** (+att/CV) | Excitation, from CCW to CW: noise (whisper) → automatic voiced/unvoiced (pulse or noise) → noon: residual (the original voice) → pulse following the voice → fixed-pitch pulse (robot or monotone). In 🔵: the same range without the residual. In 🟣: residual → IN R carrier |
| **FEEDBACK** | "Slew": spectral envelope smoothing, 0–500 ms (slurred, legato articulation) |
| **DRY/WET**, **REVERB** | As stock |
| **FREEZE** button / gate | Stop recording |
| **SEED** button / gate | Live: hold the current frame while held. Frozen: jump back to the TIME position |
| **SEED + FREEZE** | Latch: a sustained frame (a playable formant drone; play it with PITCH CV) |
| **MEDIA** | 🟢 resynthesis. 🔴 speaking chip. 🔵 synth excitation. 🟣 cross-synthesis (IN R = carrier; the LED blinks when IN R is not patched) |
| **MEDIA + SEED** | Follow pitch: detected F0 ↔ smoothed F0 (200 ms, less jitter on a pulse excitation). ASSIGN LEDs show the setting |
| **ASSIGN + SEED** | Gate output: OUT R = voiced gate |
| **VU LED** | Input level (the carrier level in 🟣) |

### Speaking chip (🔴)

A lo-fi LPC vocoder in the style of early-1980s speech-synthesis chips. It is built from original parameters; no ROM data was copied.

- **Analysis:** 8 kHz internal rate, order 10, 25 ms frames, parameters interpolated in 8 steps per frame.
- **Quantization:** reflection coefficients as log-area ratios (5, 5, 4, 4, 3, 3, 3, 3, 3, 2 bits), energy 4 bits, pitch 6 bits (50–400 Hz) and 1 voicing bit.
- **Excitation:** a decaying chirp for voiced frames and a 1-bit noise sequence for unvoiced frames.
- **Output:** 8-bit output with a zero-order hold at the chip clock, which SIZE detunes.

## Testing on a computer

```sh
make -f vox/test/makefile check
build/vox_test/lpc_test process speech.wav whisper.wav --shape 0
build/vox_test/lpc_test process speech.wav robot.wav --shape 1 --pitch -5
build/vox_test/lpc_test process speech.wav small.wav --size 0.8
build/vox_test/lpc_test process speech.wav chip.wav --submode 1
build/vox_test/lpc_test process speech.wav talkbox.wav --submode 3 --shape 1 --carrier synth.wav
```

Options: `--submode --shape --size --pitch --density --slew --carrier`.

Self tests (15 checks):

| Area | Checks |
|---|---|
| Lattice | synthesis(analysis(x)) = x at order 24 (max error 8e-6) |
| Transparency | at the neutral settings the output equals the delayed input (residual −76.6 dB) |
| Formant warp | SIZE CW/CCW moves the spectral envelope by +5.5 / −6.75 st and keeps F0 at 150 Hz |
| Excitation | CW gives a fixed pulse at C3 while the input glides; +7 st gives G3; "follow" +12 st tracks the input F0 (298.6 Hz for 149.7 Hz); CCW whisper has periodicity 0.17 with envelope correlation 0.99 |
| Cross-synthesis | output at the carrier pitch (110 Hz) with the voice envelope (correlation 1.00, against 0.56 for the carrier) |
| Hold | the held frame sings at the PITCH note at constant level |
| Speaking chip | level within 3.2 dB, envelope correlation 0.99, pitch kept; level on speech −0.8 dB |
| Stability | no NaN or Inf, bounded output, reflection coefficients inside the unit circle |

## CPU

The analysis of each 10 ms hop is spread over 40 audio blocks:

1. Snapshot the windowed frame.
2. Five blocks of plain autocorrelation.
3. Levinson-Durbin for the inverse filter.
4. Twenty-four blocks with one warped all-pass stage each (skipped when SIZE is at noon).
5. Levinson-Durbin for the synthesis filter.

YIN runs at a 10 ms hop with its lags spread over the hop. On the host (x86, callgrind) the engine averages about 27k instructions per 12-frame block with SHAPE and SIZE far from noon. The budget on the module is about 100k cycles per block.

## Not verified yet (needs the module)

- Hardware behaviour: panel and LEDs, and the actual CPU load (`vox::profile_max_cycles`, readable with a debugger).
- Envelope quality on real voices and reverberant recordings, the voicing decision on speech over music, and the carrier whitening on real synth patches.
