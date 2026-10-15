# vox_multi: all six VOX algorithms in one Beads firmware

`vox_multi` holds the six VOX algorithms. Hold **MEDIA** at power-on to choose one with **PITCH**; otherwise the module starts with the algorithm used last. Each algorithm behaves exactly as its single firmware and keeps its own MEDIA sub-mode and options.

## Selecting an algorithm

1. Hold **MEDIA** and power the module on, then release MEDIA. The FREEZE and SEED LEDs blink alternately; there is no audio.
2. Turn **PITCH**: six equal zones, algorithm 1 fully CCW to 6 fully CW (with hysteresis at the zone edges).
3. Press **MEDIA** to confirm. The algorithm starts and the choice is saved.

| PITCH zone | Algorithm | MEDIA LED | ASSIGN LEDs (binary, LED 1 = FEEDBACK = 1) |
|---|---|---|---|
| 1 (CCW) | [psola](../psola/README.md) | green | 1 |
| 2 | [lpc](../lpc/README.md) | red | 2 |
| 3 | [syllable](../syllable/README.md) | yellow | 1 + 2 |
| 4 | [split](../split/README.md) | cyan | 3 |
| 5 | [sequencer](../sequencer/README.md) | blue | 1 + 3 |
| 6 (CW) | [chain](../chain/README.md) | magenta | 2 + 3 |

The press held at power-on does not confirm: MEDIA must read released for 20 ms first (the debouncer starts in the "released" state, so the boot hold would otherwise look like a fresh press). SEED at power-on still recovers the previous settings and FREEZE still enters the bootloader.

## How it is built

- `multi.cc` lists the six algorithm descriptors (`common/app/algorithms.h`); the makefile builds every algorithm directory with `-DVOX_MULTI`, which drops each one's single-firmware `kAlgorithms` list.
- Only one engine exists at a time: it is constructed in a shared 64 KB arena in DTCM (`CreateEngine<T>()`; the largest, split, is about 61 KB). The speech buffer (`.ramd1`) and reverb memory (`.ramd3`) are shared; the `.ramd2` tables of split, psola and lpc sit side by side (208 KB of 256 KB).
- Settings: `State::vox_firmware` = `VOX_FIRMWARE_MULTI | index`. The active algorithm uses `vox_submode` / `vox_flags` as in its single firmware; the other five keep theirs in `State::vox_slots` (10 bytes), swapped by `SwitchAlgorithmSlots` when the algorithm changes. The State chunk keeps the stock 24-byte layout.

| | vox_multi |
|---|---|
| Flash (.text) | 168 KB |
| DTCM (.bss) | 70 KB (64 KB arena + platform) |
| `.ramd1` / `.ramd2` / `.ramd3` | 512 KB / 208 KB / 64 KB |
| Audio update | `prebuilt/vox_multi.wav`, about 2 min 35 s |

## Building and testing

```sh
make -f vox/multi/makefile TOOLCHAIN_PATH=/path/to/toolchain/        # build/vox_multi/vox_multi.bin
make -f vox/multi/makefile TOOLCHAIN_PATH=/path/to/toolchain/ wav    # build/vox_multi/vox_multi.wav
make -f vox/test/makefile check                                      # includes multi_test
```

`multi_test` (13 checks) covers the settings slots (2,000 random switches with edits, fresh settings, no-op switch), the PITCH zones (order, edges, ends, no flicker with ±3 % noise at a boundary, out-of-range stored index), and the MEDIA confirmation driven through a model of the stock debouncer (boot hold then press, hold without release, 10 ms gap, contact bounce, short tap at power-on).

## Not verified yet (needs the module)

- The selection on hardware: LED colours and brightness, the PITCH zones under real pot noise, and the transition from the selection into audio.
- Everything listed under "Not verified yet" in each algorithm's README.
