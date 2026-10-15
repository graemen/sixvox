# VOX cores: the firmwares on a computer

A **VOX core** is a VOX firmware (one algorithm, or `vox_multi`) compiled for a computer as a shared library. It contains the module's own code, compiled unchanged:
- the panel glue of each algorithm (`vox/<name>/<name>.cc`) and its engine (`vox/<name>/dsp`);
- the user interface (`vox/common/ui/vox_ui.cc`), with every gesture, LED pattern and the boot-time algorithm selection;
- the settings (`vox/common/hw/settings.cc`), the CV and pot reading with its laws and calibration (`cv_reader.cc`), and the input gain (`gain_control.cc`).

Only the hardware is replaced:

| Module | Core |
|---|---|
| `vox/common/app/vox_platform.cc` | `vox_host_platform.cc`: the same codec callback; the 1 kHz UI interrupt and the main loop run from it; the selection loop is a state machine |
| ADCs, switches, LEDs, codec types (`beads/drivers`) | `shim/beads/drivers`: same interfaces, values from the host |
| Normalization probe | `shim/vox/common/hw/normalization_detector.h`: the host says which jacks are patched |
| Settings flash (`stmlib/system/storage.h`) | `shim/stmlib/system/storage.h`: kept in memory, saved by the host |

Knob positions become the codes the module's ADCs would read, and jack voltages are converted through the module's default CV calibration. The firmware then applies its own pot laws, filtering and calibration. If the stock calibration procedure (ASSIGN + MEDIA) is run in the emulation, it calibrates the emulation.

## Interface

`vox_core_api.h` is a C interface: set the controls, power on, run one 12-frame block at 48 kHz, and read the LEDs. One loaded copy of the library is one emulated module, because the firmware keeps its state in globals as on the module. `core_loader.h` loads a private copy of the file for each module.

## Building

```sh
make -f vox/host/core.mk FIRMWARE=multi     # build/vox_core/vox_multi.so (.dylib on macOS)
make -f vox/host/core.mk all                # all seven
make -f vox/test/makefile core-check        # build them and run core_test (30 checks)
```

A new VOX firmware gets a core with no extra work: `make -f vox/host/core.mk FIRMWARE=<name>` compiles `vox/<name>/<name>.cc` and `vox/<name>/dsp/*.cc` against the host platform. The only rule is to place RAM buffers with `VOX_SECTION(".ramd2")` (from `vox_platform.h`) rather than a raw `__attribute__((section))`, which does not compile on macOS. Everything in the repository already does this.

`core_test` (30 checks) drives the cores like the Rack module does, with button presses, knob positions and audio:
- **Selection:** the boot sequence with MEDIA held, the PITCH zones and the LED codes.
- **Settings:** settings saved and restored, and each algorithm's sub-mode kept across switches.
- **Instances:** two loaded copies stay independent.
- **Audio:** speech through every algorithm, the ASSIGN + SEED gate output, and two minutes of Syllable without a silent second.
- **Single firmwares:** each of the six loads and runs.
