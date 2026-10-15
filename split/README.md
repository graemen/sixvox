# vox_split: voiced / unvoiced splitter for Beads

Classifies speech in real time into **silence, voiced (vowels, nasals, voiced consonants), unvoiced (fricatives, sibilants) and bursts (plosive releases)**. The streams are then processed separately, or sent to separate outputs so the rest of the system can process them. Plan: [`../plans/04-vox-split.md`](../plans/04-vox-split.md).

```
IN ─► 48→24 kHz ─┬─► classifier (zero crossings, tilt, HF/LF, level, YIN aperiodicity)
                 └─► 10 ms look-ahead ─► masks (3 ms ramps) ─► V │ U │ B │ silence
   V ─► vowel tails │ U+B ─► consonant delay   (routing per sub-mode)
   ─► balance (SHAPE) ─► 24→48 kHz ─► reverb send, dry/wet (dry delayed 10 ms), limiter ─► OUT
```

Voiced frames with strong high-frequency content (voiced fricatives such as /z/, /v/) are split at 4 kHz: the low band goes to V, the high band to U. The split is complementary (high = input − low), so the streams always sum back to the input exactly.

## Panel

| Control | Function |
|---|---|
| **SHAPE** (+att/CV) | Stream balance. Noon = all streams. CW = vowels only (a mumbled hum plus tails). CCW = consonants and bursts only (a whispered skeleton). In 🔵: CW attenuates consonants (down to −24 dB, a de-esser by class), CCW attenuates the noise floor between words (down to −40 dB) |
| **SIZE** (+att/CV) | Vowel-tail length. CCW = off; up to a 4 s decay |
| **PITCH** (+att/CV) | Tail transposition (stock interval law, 1V/oct); tails become harmonized drones |
| **TIME** (+att/CV) | Consonant delay, 10 ms – 1.2 s. With a clock on SEED: ¼, ⅓, ½, ¾, 1, 1½ or 2 clock periods |
| **FEEDBACK** | Delay feedback, up to 105% (300 Hz high-pass and soft clipping in the loop) |
| **DENSITY** (+CV) | Classifier bias. Noon = neutral; CW = more material counted as voiced; CCW = more counted as unvoiced. CV gives rhythmic reclassification effects |
| **DRY/WET** | As stock (the dry signal is delayed 10 ms to stay aligned). In 🔴: split vs original |
| **REVERB** | Reverb send on the stream chosen by the sub-mode (consonants in 🟢, vowels in 🟡, everything in 🔵) |
| **FREEZE** button / gate | Hold every tail indefinitely (a drone of the last vowels) |
| **SEED** button / gate | Capture a tail now, even mid-vowel. A regular clock on the SEED jack also sets the delay time |
| **SEED + FREEZE** | Latch "mute input vowels": only tails and consonants pass (the SEED LED blinks) |
| **MEDIA** | 🟢 Process: V → tails, U → delay, reverb on U. 🟡 Swap: V → delay + reverb, U → noise tails (sibilant "hiss pads"). 🔴 **Splitter**: no processing, OUT L = vowels + silence, OUT R = consonants + bursts. 🔵 Clean-up |
| **MEDIA + SEED** | Classifier preset: speech / speech over music (relies more on periodicity) / phone (band-limited) |
| **ASSIGN + SEED** | Gate output: OUT R = voiced gate, aligned with the audio. Leave it off in 🔴, where OUT R carries the consonants |
| **VU LED** | Live classifier: off = silence, dim = unvoiced, bright = voiced, flashing = burst |

**Latency:** the whole module is 10 ms late (the look-ahead lets each mask switch just before the sound it applies to, so plosives are not clipped).

## Testing on a computer

```sh
make -f vox/test/makefile check
build/vox_test/split_test process speech.wav split.wav --submode 2      # L = vowels, R = consonants
build/vox_test/split_test process speech.wav out.wav --size 0.6 --pitch 7 --time 0.5 --feedback 0.6
```

Self tests (16 checks):

| Area | Checks |
|---|---|
| Splitter | OUT L + OUT R equals the delayed input (residual −157 dB); 99.6% of vowel energy on L; 99.0% of consonant energy on R |
| SHAPE | CW removes consonants by 28.5 dB, CCW removes vowels by 21.1 dB; the other stream is unchanged |
| Tails | tail at −5 dB after the vowel at the vowel's pitch (150.5 Hz for 150 Hz) and an octave up at +12; none with SIZE at CCW; FREEZE holds the tail |
| Delay | TIME 0.5 gives 114 ms (110 expected); clock sync gives 205 ms for a 200 ms clock |
| Transitions | no steps larger than the input's at either SHAPE extreme |
| Stability | 40 s of random controls |

`vox/test/analysis_test` scores the classifier: 99.9% frame accuracy on clean synthetic speech, 93.9% with −40 dB noise.

## Not verified yet (needs the module)

- Hardware behaviour: panel and LEDs.
- **CPU load.** About 22k x86 instructions per 12-frame block on the host (callgrind). YIN runs at a 10 ms hop with its lags spread over the hop. The budget on the module is about 100k cycles per block; the measured worst case is in `vox::profile_max_cycles`, readable with a debugger.
- Classifier accuracy on real speech and on speech over music. The thresholds were tuned on synthetic speech, so try the presets and DENSITY.
