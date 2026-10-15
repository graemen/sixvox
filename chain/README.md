# vox_chain — voice processing chain for Beads

`vox_chain` is a broadcast/podcast-style voice channel strip for spoken word coming from a Morphagene (or any other source). It has a music-bed ducker on IN R for radio-ad style mixes. The plan is in [`../plans/07-vox-chain.md`](../plans/07-vox-chain.md).

```
IN L (voice) ─► auto-gain ─► gate ─► HPF ─► de-esser ─► compressor ─┬─► tilt EQ ─► character ─► mute ─► reverb ─┐
                                                       (parallel) ──┘                                         ├─► limiter ─► bypass ─► OUT L/R
IN R (bed)   ─────────────────────────────── ducker (keyed by the processed voice) ───────────────────────────┘
```

Everything runs at 48 kHz, with zero added latency (no look-ahead).

## Panel

| Control | Function |
|---|---|
| **DENSITY** (+CV) | Compression amount (threshold + ratio, auto make-up). Fully CCW = off |
| **TIME** (+att/CV) | Compressor speed: CCW fast and punchy (1 ms / 40 ms), CW slow and smooth (30 ms / 800 ms). Release is program-dependent (up to 3× slower on sustained gain reduction) |
| **SIZE** (+att/CV) | Tone tilt. Noon = flat. CCW = body (+6 dB at 150 Hz, presence cut). CW = presence (+5 dB at 3 kHz) and air (+6 dB at 10 kHz) |
| **SHAPE** (+att/CV) | Character. Noon = clean. CW goes radio → telephone (300–3400 Hz, 8 kHz µ-law) → megaphone. CCW goes to lo-fi (down to 4 kHz / 6-bit) |
| **PITCH** (+att/CV) | De-esser depth (a plain knob in this firmware, not semitones). Fully CCW = off |
| **FEEDBACK** | Gate/expander. Fully CCW = off; turning CW raises the threshold (−70 → −25 dBFS) and ratio (up to gate) |
| **DRY/WET** | Parallel compression: CCW = uncompressed (still gated, filtered and de-essed), CW = fully compressed |
| **REVERB** | Room/booth ambience |
| **FREEZE** button | Bypass (10 ms crossfade, click-free). FREEZE LED lit = bypassed |
| **FREEZE** gate | Mutes the voice while high (20 ms fades) |
| **SEED** button | Cycles the VU LED display: input level → gain reduction → output level → bed level. ASSIGN LEDs flash 1, 2, 3 or all to show which |
| **SEED** gate | "Push": +6 dB into the compressor detector while high, to squash words on accents. SEED LED lit = pushing |
| **SEED + FREEZE** | Toggles the input gain between automatic (stock AGC) and fixed unity. ASSIGN LEDs flash all = auto, none = unity |
| **MEDIA** | Preset (LED colour): 🟢 Podcast, 🟡 Broadcast, 🔴 Announcer, 🔵 Raw/character. The knobs work within ranges set by the preset |
| **MEDIA** long press | Re-learn the automatic input gain (stock) |
| **MEDIA** held + **FEEDBACK** pot | Manual input gain (stock) |
| **ASSIGN** | ASSIGN CV destination: Feedback(gate) / Dry-Wet / Reverb (as stock). The 5th destination is **bed ducking depth** (all three LEDs blink; ±24 dB around the preset depth at ±5 V) |
| **ASSIGN** held + mix pots | ASSIGN CV amounts per destination (stock) |
| **ASSIGN + SEED** | Gate output mode: OUT R = voice activity gate, OUT L = mix (stock convention) |
| **ASSIGN + MEDIA** | CV calibration (stock procedure, unchanged) |
| **IN R** | Music bed. It is used only when both IN L and IN R are patched. It is ducked by the processed voice and summed into both outputs |

| Preset | HPF | Compression range | Ducking | Notes |
|---|---|---|---|---|
| 🟢 Podcast | 80 Hz | −6…−30 dB threshold, up to 4:1 | −12 dB | slightly slower time constants |
| 🟡 Broadcast | 100 Hz | −8…−38 dB, up to 6:1 | −15 dB | a little presence and de-ess bias |
| 🔴 Announcer | 100 Hz | −10…−44 dB, up to 10:1 | −18 dB | faster, +1 dB output |
| 🔵 Raw | 150 Hz | −6…−20 dB, up to 2.5:1 | −12 dB | SHAPE at double sensitivity |

## Building

From the `eurorack/` root:

```sh
git submodule update --init stmlib stm_audio_bootloader   # stmlib is empty in a fresh clone
make -f vox/chain/makefile TOOLCHAIN_PATH=/path/to/arm-gnu-toolchain/   # → build/vox_chain/vox_chain.bin
make -f vox/chain/makefile TOOLCHAIN_PATH=... wav                       # → build/vox_chain/vox_chain.wav
```

Build requirements:
- Any recent `arm-none-eabi-gcc`. It was tested with xPack GCC 13.3.1; on macOS, `brew install --cask gcc-arm-embedded` works. The makefile pins `-std=gnu++11`, because the STM32H7 HAL headers do not compile as C++17 under `-Werror`.
- `wav` needs `python3` with `numpy`. It uses `vox/tools/qpsk_encoder.py`, a Python 3 port of the stock encoder.

### Prebuilt files

`prebuilt/vox_chain.bin` (checksum in `prebuilt/SHA256SUMS`) and `prebuilt/vox_chain.wav`, its audio update file. The WAV was checked to decode byte for byte with the stock bootloader's demodulator (`qpsk_decode_test`). `make ... wav` regenerates it.

## Installing (audio update, same as stock)

1. Power off. Hold **FREEZE** and power on: the bootloader starts.
2. Play `vox_chain.wav` into **IN L** from a computer or phone at a moderate volume. Disable effects, "sound check" and normalization on the player. The TIME knob sets the bootloader's input gain.
3. When the update finishes, the module restarts into `vox_chain`.

To go back, install the official Beads firmware the same way.

**Settings compatibility:**
- Calibration is shared with stock and is never touched.
- VOX keeps the stock settings layout and stores its own fields in bytes the stock firmware treats as padding. Switching back and forth therefore never makes either firmware reformat the settings or enter the stock factory test.
- The ASSIGN routing, the manual gain and the gate output mode are shared between the two firmwares.

SWD (development): `make -f vox/chain/makefile upload` writes the application at 0x08020000 and leaves the bootloader alone. Set `OPENOCD_BIN_PATH` / `OPENOCD_SCRIPT_PATH` / `PGM_INTERFACE` for your probe.

## Testing on a computer

```sh
make -f vox/test/makefile check          # self tests (synthetic signals)
build/vox_test/chain_test process in.wav out.wav --preset 1 --amount 0.7 --deess 0.4 --normalize
build/vox_test/chain_test process voice.wav out.wav --bed music.wav --preset 2
```

`process` runs the same engine code as the firmware, in 12-frame blocks at 48 kHz (other rates are resampled). It prints integrated loudness in and out (ITU-R BS.1770) and the gain reduction. Knob positions are 0..1 (`--tilt` and `--character`: 0.5 = center). `--normalize` peak-normalizes the input, standing in for the module's auto-gain.

What the self tests cover (29 checks, all passing):

| Area | Checks |
|---|---|
| Fast log/exp | log2 error < 4e-6; exp2 relative error < 3e-7 |
| Bypass | exact null; toggling every 100 ms is click-free |
| Neutral settings | flat within ±0.25 dB from 200 Hz to 12 kHz |
| Compressor | static curve within 0.04 dB of the design; attack timing |
| De-esser | > 6 dB of reduction on sibilant noise; vowels untouched; transparent at depth 0 |
| Gate | −60 dBFS floor down 40 dB; speech level untouched |
| Ducker | bed at −12 dB within 14 ms of speech; recovers within 500 ms; VAD on during speech, off in silence |
| Character models | level-matched within 6 LU |
| Loudness consistency | phrase-to-phrase spread (±9 dB synthetic speech) cut by ~70 % in Broadcast at DENSITY 0.75 |
| Stability | 60 s of random control changes with hot bursts: no NaN/Inf |

`vox/test/qpsk_decode_test` decodes the generated `.wav` with the stock bootloader's QPSK demodulator and packet decoder. It checks the result byte for byte against the `.bin`.

## Not verified yet (needs the module)

- **CPU load.** The host runs the engine at about 55× real time. A rough projection for the M7 is about 20–30 % of the audio interrupt budget. The real figure is in `vox::profile_max_cycles` (budget: 100,000 cycles per 12-frame block), readable with a debugger.
- **Panel behaviour, LED feedback, CV scaling and noise floor** on real hardware.
- **Tuning on real Morphagene speech.** The tests use synthetic speech, because no speech recordings could be downloaded here. `chain_test process` is the quickest way to tune on your own recordings.
