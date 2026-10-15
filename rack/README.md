# VOX for VCV Rack

A VCV Rack 2 module that runs the VOX firmwares, for testing them without flashing the module. It has the Beads panel (controls and jacks at the positions of the hardware panel) and runs a **VOX core**: the firmware's own code compiled for the computer (`vox/host`). The gestures, LEDs, settings and the boot-time algorithm selection are the module's.

## Building and installing

Requirements: the [Rack SDK](https://vcvrack.com/manual/PluginDevelopmentTutorial) for your system, and the tools listed there (on macOS: Xcode command line tools, plus `jq` and `zstd` from Homebrew).

```sh
cd vox/rack
make RACK_DIR=/path/to/Rack-SDK              # plugin + cores/vox_*.dylib|so|dll
make RACK_DIR=/path/to/Rack-SDK install      # into the Rack user folder
```

The plugin's makefile builds the seven cores from the firmware sources in this repository. Rebuild after changing a firmware, then restart Rack (or load the new core from the menu).

## Using it

The module starts with `vox_multi` and the algorithm used last. The top line of the panel shows the firmware and the running algorithm, and the bottom line shows the core's processing load.

Context menu:

| Item | Does |
|---|---|
| Load firmware ▸ | One of the cores built into `cores/` (`vox_multi` or a single algorithm), or **Load firmware file…** for any core built elsewhere |
| Restart | Power cycle |
| Restart holding MEDIA | Power on with MEDIA held for 0.6 s. The algorithm selection starts: turn PITCH, then click MEDIA to confirm (vox_multi only) |
| Restart holding SEED | Power on with SEED held (recover settings) |
| Erase settings and restart | Blank settings flash. **Initialize** does the same |

**Settings:** the firmware's settings (calibration, state chunk) are saved with the patch, so each module instance keeps its algorithm, sub-modes and options. Loading another firmware keeps them, as installing a firmware on the module does. Duplicating a module copies them.

**Levels:** ±5 V at the audio inputs and outputs is the codec's full scale. The gate output gives +4 V. CV inputs use the module's scaling: +5 V sweeps a knob's full range with its attenuverter fully clockwise, and PITCH follows 1 V/oct.

**Several modules:** each module loads a private copy of its core, so instances are independent and can run different firmwares.

## How it works

- `src/Vox.cpp`: the module.
  - **Controls:** knobs, CVs, jack states and buttons become a `VoxCoreControls` once per block.
  - **Gates:** FREEZE and SEED go through Schmitt triggers at 0.5 / 1 V.
  - **Lights:** LED brightness comes from the core.
- `src/AudioBridge.hpp`: the engine-rate ↔ 48 kHz bridge. It uses Rack's resampler and runs the core in 12-frame blocks. The output starts once 64 frames are queued, about 1.3 ms at 48 kHz.
- `vox/host/core_loader.h`: copies the core file to the temporary folder and loads that copy.

The panel is `res/Vox.svg` (shapes only); the labels are drawn by the module.

## Tests

```sh
make -f vox/test/makefile core-check                            # the cores (30 checks)
make -C vox/rack/test RACK_DIR=... SPEEXDSP=... check           # the audio bridge (25 checks)
```

The bridge test runs at 44.1, 48, 88.2, 96 and 192 kHz. It checks for underruns, a stable queue, exactly 4,000 core blocks per second, and a transparent round trip at unity gain.

## Not verified yet

The plugin has been compiled and linked against the Rack 2.6.6 headers, on Linux with gcc and with clang. Every Rack function it calls exists in Rack 2.6.6. It has not yet been run inside Rack: the panel rendering, the context menu and the file dialog have to be checked there.
