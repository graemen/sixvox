# 00 — VOX common foundation

Shared code and infrastructure that every VOX firmware links against. Nothing here is musically interesting on its own; it exists so the six firmware plans contain only their DSP and control mapping.

---

## 1. Repository layout

```
eurorack/
  stmlib/                 ← git submodule — EMPTY in current checkout, run:
                            git submodule update --init stmlib stm_audio_bootloader
  beads/                  ← untouched stock firmware (reference + driver source)
  vox/
    common/               ← this plan
      app/                main-loop skeleton, codec start/stop, Process() shell
      hw/                 copies of beads/{cv_reader,normalization_detector,
                          gain_control,calibrator,pot_controller,settings}.*
      ui/                 vox_ui.{h,cc}  (fork of beads/ui.cc)
      buffer/             speech_buffer.h, marker_table.h
      analysis/           envelope, svf banks, yin, lpc, onset, vuv, vad
      synth/              ola_grain.h, hermite resampler, wsola.h
      fx/                 (reuse beads/dsp/fx/reverb.h + limiter.h via include path)
    psola/ lpc/ syllable/ split/ sequencer/ chain/
      makefile, <name>.cc (main), dsp/…
    test/                 host (macOS/Linux) harness — see §8
    proto/                Python reference prototypes — see §9
```

### Why copy rather than include `beads/` as a package

`stmlib/makefile.inc` compiles every `.cc` in each directory listed in `PACKAGES`. Listing `beads` would pull in `beads.cc` (a second `main`) and `ui.cc` (hard-wired to `Processor`). The plan is:

- **Reuse as packages, unmodified:** `beads/drivers` (codec, ADCs, switches, LEDs, gate inputs, system) and `beads/dsp/fx` (header-only reverb), plus `beads/dsp/limiter.h` (header).
- **Copy into `vox/common/hw` and keep the `beads` namespace:** `cv_reader`, `normalization_detector`, `gain_control`, `calibrator`, `pot_controller.h`, `settings`. Copying lets settings and UI diverge without touching stock code.

### Per-firmware makefile template

```make
F_CRYSTAL      = 8000000L
F_CPU          = 400000000L
SYSCLOCK       = SYSCLK_FREQ_400MHz
FAMILY         = h7xx
APPLICATION_LARGE = TRUE
BOOTLOADER     = beads_bootloader
TARGET         = vox_psola
PACKAGES       = vox/psola vox/psola/dsp vox/common/app vox/common/hw \
                 vox/common/ui vox/common/analysis vox/common/synth \
                 beads/drivers stmlib/dsp stmlib/utils stmlib/ui stmlib/system
RESOURCES      = vox/common/resources
include stmlib/makefile.inc
```

Toolchain:
- Builds with current arm-none-eabi-gcc (tested: xPack 13.3.1). `vox/common/vox.mk` forces `-std=gnu++11`, because the H7 HAL headers fail as C++17 under `-Werror`.
- `stmlib/makefile.inc` compiles every `.cc` of every package and names objects by **base name**, so `.cc` names must be unique per firmware.
- Stock `make wav` uses Python 2 tooling. VOX uses `vox/tools/qpsk_encoder.py` (Python 3 port; output verified with the stock demodulator by `vox/test/qpsk_decode_test`).

Flashing:
- **Development:** SWD via OpenOCD (`UPLOAD_COMMAND = upload_combo_jtag_erase_first_sector`). A debug probe gives you `printf` over the debug port and cycle counting.
- **Users:** `make -f vox/<name>/makefile wav`, then play the WAV into the IN L jack with FREEZE held at power-on. The stock Beads WAV always restores factory firmware.

**Status:** `vox/common` is implemented (app/, hw/, ui/, dsp/, buffer/, analysis/, synth/, vox.mk), as described in `vox/README.md`. Deviation: the speech buffer is 2^18 int16 samples (**10.9 s** at 24 kHz) in `.ramd1` only; `.ramd2` is left to each firmware for its own tables and delay lines.

---

## 2. Audio rate strategy

The codec runs at **48 kHz in every VOX firmware**. The stock source says the PCM3060 is "very hissy" at 24 kHz.

The speech DSP runs at **24 kHz internally**, decimating by 2 in and interpolating by 2 out with `stmlib::SampleRateConverter<SRC_DOWN/SRC_UP, 2, 48>`, the same filters `GranularProcessor` uses (`dsp/sample_rate_conversion_filters.h`). Reasons:
- Speech intelligibility needs ≤ 10 kHz bandwidth.
- It halves buffer memory and CPU per second of audio.
- 24 kHz gives 6 internal samples per 250 µs block.

Exceptions:
- **07 chain** runs at 48 kHz without conversion, to keep the "air" band.
- **YIN** runs on a further ÷2 decimated copy at 12 kHz.

Per-block CPU budget: **≈100k cycles**. VOX target: ≤ 60k cycles average and ≤ 80k worst case, which leaves headroom for UI and flash writes. Frame-based analysis that would exceed this in a single block runs either:
- **amortized:** split across the hop's blocks with a small state machine (preferred, deterministic), or
- **in the main loop:** fed by a single-producer/single-consumer ring of frame indices. Latency is one hop, which is acceptable for analysis-driven parameters.

---

## 3. Memory map

| Region | Size | Stock use | VOX use |
|---|---|---|---|
| AXI SRAM D1 `.ramd1` | 512 KB | delay_buffer_1 | `SpeechBuffer` segment A (int16, 24 kHz mono: 10.9 s) |
| SRAM D2 `.ramd2` | 288 KB (256 KB used) | delay_buffer_2 | `SpeechBuffer` segment B (up to 5.4 s) **or** per-firmware tables (LPC frames, …) |
| SRAM D3 `.ramd3` | 64 KB | reverb_buffer | reverb (unchanged) |
| DTCM | 128 KB | stack, statics (verify in linker script) | marker tables, analysis frames, OLA accumulators |
| `.dma` | — | codec/ADC DMA | unchanged |

**Total speech buffer:** up to 393,216 int16 samples, which is **16.4 s mono at 24 kHz**. Morphagene holds the long reel, so 10–16 s of live history is enough.

**Storage format:** int16. Float would halve capacity. 8-bit µ-law (`beads/dsp/mu_law.h`) is available as a "long buffer" option that doubles capacity to about 32 s; it is optional per firmware.

### `SpeechBuffer` (buffer/speech_buffer.h)

- Circular buffer over **two non-contiguous segments** (D1 + D2). `Read(int64 pos)` maps an absolute sample index to (segment, offset).
- Positions are **absolute 32-bit sample counters** (wrap after 49 h at 24 kHz). The valid range is `[write_pos − capacity, write_pos)`. Every marker table uses the same counter, so markers stay valid without rebasing.
- API: `Write(const int16_t*, n)`, `Read(pos)`, `ReadHermite(float pos)`, `ReadBlock(pos, int16_t* dst, n)`, `oldest()`, `newest()`, `frozen` flag.
- Freeze stops `Write`. On unfreeze, recording restarts with a 5 ms fade-in to avoid clicks.

### `MarkerTable<T, N>` (buffer/marker_table.h)

- Ring of POD records keyed by absolute position, with binary search `Find(pos)`, `Nearest(pos)` and `Range(a, b)`. Entries older than `buffer.oldest()` are lazily invalidated.
- Instances: pitch marks (01, 02), segments (03, 06), V/UV runs (04).

---

## 4. Analysis library (analysis/)

Every block is written for a 24 kHz input, steps through per-sample `Process()` or per-hop `ProcessFrame()`, avoids allocation, and is single-precision float.

| Module | Purpose | Key parameters | Used by |
|---|---|---|---|
| `EnvelopeFollower` | Peak/RMS envelope, separate attack/release | τ in ms | all |
| `BandEnergy` | `stmlib::Svf` bank → per-band log energy | bands: 80–300, 300–900, 900–2.5k, 2.5–5k, 5–10k | 03, 04, 06, 07 |
| `Vad` | Speech/silence: energy over adaptive noise floor + hangover | floor tracker τ 2 s, hangover 150 ms | 03, 04, 06, 07 |
| `Yin` | F0 + aperiodicity on 12 kHz copy | window 300 smp (25 ms), τ 30–150 (80–400 Hz), hop 5 ms, threshold 0.12–0.2, parabolic interp. | 01, 02, 04 |
| `PitchMarker` | Glottal-epoch-ish marks from F0 track | see 01 | 01, (02) |
| `Lpc` | Autocorr + Levinson-Durbin → reflection coeffs k[i], gain, k1 tilt | order ≤ 24, frame 20 ms, hop 5–10 ms, pre-emph 0.94, lag window | 02, 04 |
| `OnsetDetector` | Rectified log-energy flux per band, adaptive threshold | refractory 50 ms | 03, 04, 06 |
| `SyllableNucleus` | Peaks in smoothed 300–900 Hz envelope | min spacing 100 ms | 03, 06 |
| `VuvClassifier` | Silence / voiced / unvoiced / burst | see 04 | 04, (01, 02) |

### CPU sanity numbers

- **YIN** at 12 kHz: 150 lags × 300 samples ≈ 45k MAC per 5 ms hop. Spread over the hop's 20 blocks, that is ~2.3k MAC per block.
- **LPC order 24**, 480-sample frame: ≈ 12k MAC autocorrelation plus ~600 for Levinson, per hop.
- **SVF bank**: 5 bands × ~10 ops per sample, which is negligible.

### Shared synthesis helpers (synth/)

- `OlaGrain`: windowed grain with fractional read position and rate. Window from a 256-entry Hann LUT, Hermite interpolation (`stmlib::InterpolateHermite`).
- `OlaAccumulator`: output ring for pitch-synchronous overlap-add.
- `Wsola`: waveform-similarity overlap-add time-scaling (06, optionally 03).
- `stmlib/fft/shy_fft.h` (used by Clouds/Warps) is available if any spectral feature is added later. Verify once the stmlib submodule is populated.

---

## 5. Application skeleton (app/)

Derived from `beads/beads.cc`, with these changes:

- `Process()` keeps: normalization detection, `cv_reader.Read()`, the pot→mix parameter path with the ASSIGN CV, freeze/seed event conversion (including the 2.5 ms seed delay and `GateFlags`), `GainControl` (input AGC), output conversion, and gate-out mode.
- It replaces `processor.Process(...)` / `synth.Render(...)` with `engine.Process(const VoxInput&, FloatFrame* out, size_t n)`.
- **Removed:**
  - Synth mode (the 10 s no-input switch).
  - `MEDIA` → sample-rate reconfiguration. The codec stays fixed at 48 kHz, so there are no codec restarts.
  - `background_buffer_backup` in v1. Optional later: persisting a frozen buffer and its marker tables to flash.
- `VoxInput` bundles: dry and 24 kHz audio, per-frame freeze/seed flags, `Parameters` (time, size, shape, pitch as `ModulatedParameter` with base, attenuverter, cv, has_cv), density, mix params, sub-mode, `mono` / `right_patched` flags, and the clock (§6).
- `normalization_detector.mono()` and the audio-right normalization flag tell each engine whether IN R is a real second signal (carrier, sidechain, music bed).

---

## 6. UI and settings conventions (ui/, hw/settings)

`vox_ui.cc` forks `beads/ui.cc` and keeps every behaviour users rely on, so muscle memory and calibration still work.

| Gesture | Stock | VOX (all firmwares) |
|---|---|---|
| ASSIGN press | cycle ASSIGN CV destination | **same** (Feedback / Dry-Wet / Reverb / all-with-amounts) |
| ASSIGN hold + turn mix pots | per-destination amount | **same** |
| ASSIGN + SEED | toggle gate output mode | **same**; the gate's meaning is per-firmware |
| ASSIGN + FREEZE | toggle autosave | **same** (autosave of settings only in v1) |
| ASSIGN + MEDIA | calibration | **same, must not change** |
| MEDIA short press | cycle quality (4 media) | **cycle sub-mode** (4 per firmware; MEDIA RGB colour = sub-mode) |
| MEDIA long press (1.5 s) | reset auto-gain | **same** |
| MEDIA hold + turn FEEDBACK pot | manual input gain | **same** |
| SEED + FREEZE | seed lock | per-firmware ("latch" behaviour) |
| MEDIA + SEED / MEDIA + FREEZE | advanced features | free for per-firmware secondary toggles |

**Sub-mode colours, in order:** green, yellow, red, blue. Where a firmware uses cyan or magenta, its plan says so.

**VU LED:** input level by default. A firmware may repurpose it, for example as a gain-reduction meter in 07.

### Settings (implemented in `vox/common/hw/settings.*`)

`stmlib::ChunkStorage` behaviour (checked in `stmlib/system/page_storage.h`):
- It reads the `CALI` chunk first, then State chunks while their tag and size match.
- If no State chunk matches, it **reformats the sector**: it rewrites the calibration it has already loaded, plus a default State, and `Init` returns false.
- The stock firmware treats that `false` as "freshly baked" and **starts its factory test**.

A distinct State tag per firmware would therefore throw the stock firmware into factory test every time it was re-flashed after VOX. So:
- `PersistentData` (`CALI`) is byte-identical to stock; calibration is shared and never rewritten by VOX.
- `State` keeps the stock tag (`STAT`) and size (24 bytes, enforced by `static_assert`). VOX fields live in what stock treats as padding: `vox_magic`, `vox_firmware`, `vox_submode`, `vox_flags`.
- When the magic or firmware id doesn't match (state written by stock or by another VOX firmware), only the VOX fields are reset. The ASSIGN routing, gain and gate output mode are shared.
- Stock zeroes the padding when it saves, which is harmless.

---

## 7. Morphagene integration (shared conventions)

| Morphagene | → Beads | Use |
|---|---|---|
| OUT L (and R) | IN L (R) | Speech source. Mono Morphagene material on L leaves IN R free for a carrier, sidechain or music bed |
| EOSG (end-of-splice gate) | SEED gate | Segment-aligned retrigger / re-analysis / sequencer clock |
| CV OUT (envelope follower) | ASSIGN CV or a knob CV | Zero-cost envelope modulation |
| shared master clock | Morphagene CLK + Beads SEED | Lock splice playback and the VOX sequencer (06) |

Notes:
- **Morphagene varispeed** shifts pitch and formants together before VOX sees the signal. Pitch trackers must cover 50–800 Hz, not just the natural speech range, when varispeed is far from 1×. Range is configurable per firmware.
- **Overdubbed or layered Morphagene material** breaks single-voice assumptions (YIN, LPC). Each firmware must degrade gracefully when voicing confidence is low (see the per-plan risks).

---

## 8. Host test harness (vox/test)

- Builds each engine's DSP core for macOS (clang) or Linux with a stub `stmlib` system layer, following `beads/test/beads_test.cc`.
- CLI: `vox_test <engine> in.wav out.wav --params automation.json`. The automation file holds a time-stamped knob/CV/gate timeline, so tests are reproducible.
- Analysis dumps: CSV of F0, voicing, markers, LPC frames and segment tables, for plotting against the Python reference.
- **Corpus** (`vox/test/corpus/`, not committed if licensing is unclear):
  - male, female and child speech; fast ad read; interview with room tone
  - speech over a music bed; telephone-quality speech
  - the same clips **recorded out of Morphagene** at 0.5×, 1× and 2× varispeed and with overdub, to capture its output stage

## 9. Python prototypes (vox/proto)

numpy/scipy, plus librosa / praat-parselmouth for reference F0 and formants. One notebook or script per firmware, prototyping the algorithm at 24 kHz before porting. The host harness output must match the prototype within tolerance (F0 within 1 %, marker positions within ±1 sample, or a null test for deterministic paths).

## 10. Profiling and robustness

- `PROFILE_INTERRUPT` debug pin, kept from stock, for scope measurement of ISR time. Add a DWT `CYCCNT` running max-cycles counter, readable over SWD.
- Watchdog: keep `Watchdog::Start()`. Any main-loop analysis worker must yield regularly.
- Clean all NaN/denormal paths: set flush-to-zero in FPSCR at init, and constrain every filter state.

## 11. Milestones

1. Populate submodules; stock Beads builds and flashes via SWD and WAV.
2. `vox/common` skeleton builds a **passthrough firmware** (`vox_null`): 48k→24k→48k SRC, AGC, mix knobs, reverb, gate-out, sub-mode LED cycling, calibration intact.
3. `SpeechBuffer` + freeze + scrub test engine (TIME = position), checked on hardware for clicks at segment boundaries (D1/D2 seam).
4. Analysis modules individually verified against Python on the corpus (host harness).
5. CPU profile of all analysis running together at worst case: < 40k cycles/block.
