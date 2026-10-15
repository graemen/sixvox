# vox_psola: pitch-synchronous voice processor for Beads

# Sounds GOOD

Formant-preserving pitch shift, time stretch and scrub independent of pitch, and **intonation control** (flatten, exaggerate, or snap the melody of speech to a scale), using TD-PSOLA driven by a live pitch tracker. Plan: [`../plans/01-vox-psola.md`](../plans/01-vox-psola.md).

```
IN ─► 48→24 kHz ─► speech buffer (10.9 s, +feedback) ─► 12 kHz YIN ─► pitch marks (one per period)
read head (TIME, DENSITY) ─► nearest mark ─► 2-period Hann grain (formant resampling, SIZE)
   placed at the synthesis period (PITCH, SHAPE) ─► overlap-add ─► 24→48 kHz ─► dry/wet, reverb, limiter ─► OUT
```

How each kind of material is handled:
- **Voiced:** one grain per output period, built from the nearest analysis mark.
- **Uncertain** (YIN aperiodicity 0.2–0.45, e.g. speech over music): the engine falls back to 30 ms asynchronous grains resampled by the pitch ratio.
- **Unvoiced:** 8 ms grains at random offsets, so there is no buzz.

## Panel

| Control | Function |
|---|---|
| **TIME** (+att/CV) | Live: delay behind the input (at least 100 ms, so the region has been analysed). Frozen: absolute position in the buffer |
| **DENSITY** (+CV) | Read speed. Noon = hold (a sustained vowel). CW = 0 to +2× (with a flat zone around 1×, at about 3 o'clock). CCW = reverse, 0 to −2× |
| **PITCH** (+att/CV) | F0 transposition with formants preserved (stock interval law, 1V/oct CV) |
| **SIZE** (+att/CV) | Formant shift ±12 semitones ("vocal tract size"); noon = natural. In 🔴: stutter count |
| **SHAPE** (+att/CV) | Intonation. Noon = natural. CW = towards a monotone at the PITCH note (C3 at 0 semitones; with PITCH CV this gives "sung" speech). CCW = exaggerated, up to 3× the deviation from the speaker's running mean F0. In 🟡: correction strength |
| **FEEDBACK** | Output re-recorded into the buffer (repeated re-pitching accumulates) |
| **DRY/WET**, **REVERB** | As stock |
| **FREEZE** button / gate | Stop recording and analysis; TIME scrubs the frozen buffer |
| **SEED** button / gate | Live: while held, hold the current mark (stutter or sustain). Frozen: jump back to the TIME position |
| **SEED + FREEZE** | Latch: loop between the TIME position and the read position at the moment of latching |
| **MEDIA** | 🟢 PSOLA. 🟡 F0 quantized to a scale (root = PITCH, 20 ms glide). 🔴 period stutter: each period repeated N times (SIZE CW half, 1–8), for vocal fry or robot voices. 🔵 narrow F0 range (70–400 Hz, fewer octave errors) |
| **MEDIA + SEED** | Scale: chromatic / major / minor / pentatonic (0–3 ASSIGN LEDs) |
| **ASSIGN + SEED** | Gate output: OUT R = voiced gate (of the material at the read head) |
| **VU LED** | Input level; blinks when the pitch tracker has lost the voice for more than 200 ms |

The pitch tracker covers 50–600 Hz by default, which leaves room for Morphagene varispeed. An octave-jump guard holds the previous period unless a new one persists for three frames.

## Testing on a computer

```sh
make -f vox/test/makefile check
build/vox_test/psola_test process speech.wav out.wav --pitch 7 --size 0.4
build/vox_test/psola_test process speech.wav sung.wav --shape 1 --pitch -5
build/vox_test/psola_test process speech.wav major.wav --submode 1 --shape 1 --scale 1
```

Self tests (19 checks):

| Area | Checks |
|---|---|
| Pitch marks | one per period: every spacing within 1 sample on a 150 Hz vowel |
| Pitch shift | −12, +7 and +12 st exact (75.0 / 224.7 / 300.0 Hz). The spectral envelope stays within 1.3 dB of the input; plain 2× resampling moves it by 7.8 dB |
| Formant shift | SIZE ±6 st moves the envelope by +5.5 / −5.75 st, F0 unchanged |
| Intonation | CW gives a monotone at C3 (130.8 Hz); CW + PITCH +7 gives G3 (196.2 Hz); CCW widens a 0.54-octave glide to 1.34 octaves; 🟡 major scale puts 84 of 87 F0 estimates within 25 cents of a scale note |
| Time | read speed 0.500 at DENSITY 0.5×, pitch unchanged; noon holds a vowel at constant level |
| Speech | 100% of marks inside vowels are voiced; 13 of 847 marks outside vowels are voiced |
| Scrub | no clicks when sweeping TIME across a frozen buffer |
| Stability | 40 s of random controls |

## Not verified yet (needs the module)

- Hardware behaviour: panel and LEDs.
- **CPU load.** About 21k x86 instructions per 12-frame block on the host (callgrind) at +24 st with formants −6 st, the case with the most grain overlap. YIN runs at a 5 ms hop with its lags spread over the hop. The budget on the module is about 100k cycles per block; the measured worst case is in `vox::profile_max_cycles`, readable with a debugger.
- Tracking on real voices, reverberant rooms and speech over music. The fallback to asynchronous grains for uncertain regions is the safety net there.
