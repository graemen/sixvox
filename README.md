# SIXVOX — speech-processing firmwares for Mutable Instruments Beads

Alternative Beads firmwares for spoken-word material (speeches, interviews, ads), designed around a Make Noise Morphagene as the source.

| Firmware | Status | Directory |
|---|---|---|
| `vox_chain`: voice processing chain (gate, de-esser, compressor, tilt EQ, character, ducker) | implemented | [`chain/`](chain/README.md) |
| `vox_syllable`: granular engine whose grains snap to onsets, syllables and words; pause removal | implemented | [`syllable/`](syllable/README.md) |
| `vox_sequencer`: clocked re-sequencer of syllables and words, with WSOLA stretch-to-step | implemented | [`sequencer/`](sequencer/README.md) |
| `vox_split`: voiced / unvoiced splitter (vowel tails, consonant delay, separate outputs) | implemented | [`split/`](split/README.md) |
| `vox_psola`: pitch-synchronous pitch shift, time stretch and intonation control | implemented | [`psola/`](psola/README.md) |
| `vox_lpc`: LPC analysis and resynthesis (whisper, robot, formant warp, cross-synthesis, speaking chip) | implemented | [`lpc/`](lpc/README.md) |
| `vox_multi`: all six algorithms in one firmware; hold MEDIA at power-on and choose with PITCH | implemented | [`multi/`](multi/README.md) |

All seven firmwares are host-tested (`make -f vox/test/makefile check`) and have been run on the hardware and verified working by a human. 
Each directory has a `prebuilt/` folder with the `.bin`, the audio-update `.wav` (decoded and checked against the `.bin` by `qpsk_decode_test`), and `SHA256SUMS`. Plans: [`plans/`](plans/README.md).
All firmwares have been test installed using the wav file. Make sure the source gain is high when using this method. 

| Firmware | Flash (.text) | Host CPU (x86 instructions per 12-frame block, callgrind) |
|---|---|---|
| chain | 65 KB | 18k |
| syllable | 64 KB | 15.5k |
| sequencer | 72 KB | 15k |
| split | 67 KB | 22k |
| psola | 65 KB | 21k |
| lpc | 82 KB | 27k |
| multi | 168 KB | as the active algorithm |

To try a firmware without flashing the module, run it in VCV Rack: [`rack/`](rack/README.md) builds a Rack module that runs the firmware's own code (user interface, settings and engines), compiled for the computer.

The audio interrupt budget on the module is about 100k cycles per block. The measured worst case is in `vox::profile_max_cycles`, readable with a debugger.

## Layout

```
vox/
  plans/         design plans for all firmwares
  common/
    app/         vox_platform: boot, interrupts, control reading, audio callback
    hw/          copies of beads/{cv_reader,normalization_detector,gain_control,
                 calibrator,pot_controller,settings} (namespace beads, VOX changes noted)
    ui/          vox_ui: stock gestures + MEDIA as sub-mode selector
    dsp/         fast log2/exp2 and dB conversions, envelope follower, peak limiter,
                 48<->24 kHz bridge, output stage (dry/wet, reverb, limiter)
    buffer/      SpeechBuffer: 2^18 int16 samples (10.9 s at 24 kHz) in .ramd1
    analysis/    VAD, band energies, segmenter (onsets, syllables, words), YIN,
                 voiced/unvoiced classifier, pitch marker, LPC (plain and warped)
    synth/       grains with fades, WSOLA
    vox.mk       shared build rules (gnu++11, python3 wav encoder, SWD upload)
  chain/ syllable/ sequencer/ split/ psola/ lpc/
                 one algorithm each: makefile, panel mapping (.cc), dsp/, prebuilt/
  multi/         vox_multi: the six algorithms in one firmware (makefile, list)
  host/          VOX cores: any firmware compiled for a computer (shims for the
                 hardware, C interface, loader) - see host/README.md
  rack/          VCV Rack 2 module running the cores - see rack/README.md
  test/          host test harnesses (make -f vox/test/makefile check)
  tools/         qpsk_encoder.py: Python 3 port of the stock audio-update encoder
```

The stock `beads/` directory is not modified. VOX firmwares reuse `beads/drivers` and a few `beads/dsp` headers (frame, reverb) directly.

## Writing a new VOX algorithm

Each algorithm has a `makefile` (copy `chain/makefile`, change `TARGET` and `PACKAGES`) and one `.cc` whose code sits in its own namespace (`vox::<name>`) and defines:

- `const VoxFirmwareConfig kConfig`: firmware id, PITCH knob law, ASSIGN destinations, gate output modes, sub-mode LED colours
- `void AlgorithmInit()`: builds the engine with `CreateEngine<Engine>()`, which places it in the shared 64 KB arena in DTCM
- `void AlgorithmProcess(const VoxBlock&, const FloatFrame* in, FloatFrame* out, bool* gate_out, size_t size)`: audio interrupt, 48 kHz, 12 frames
- `void AlgorithmIdle()`: main loop, which polls UI gestures with `ui.TakeComboCount(...)`

It then exports `const VoxAlgorithm k<Name>Algorithm` (declared in `common/app/algorithms.h`: the config, the colour shown during the boot-time selection, and the three hooks). Outside `VOX_MULTI` it also defines `kAlgorithms` / `kNumAlgorithms` with itself as the only entry; `multi/multi.cc` lists all of them instead.

Shared memory: `speech_storage` (the 10.9 s speech buffer in `.ramd1`) and `reverb_buffer` (`.ramd3`) are defined once by the platform. Tables in `.ramd2` stay per algorithm (the current ones fit side by side). Settings: `State::vox_submode` / `vox_flags` belong to the active algorithm; vox_multi swaps them with `State::vox_slots` when the algorithm changes (`common/app/algorithm_slots.h`).

`stmlib/makefile.inc` compiles every `.cc` in every listed package and names objects by base name, so `.cc` file names must be unique across all packages of a firmware, vox_multi included.

The user manual for all algorithms is the "VOX for Beads — User Manual" doc.

## License

The VOX code is MIT licensed ([`LICENSE`](LICENSE)), like the rest of the Mutable Instruments eurorack code it builds on.
# sixvox
