/* Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
 *
 * VOX core: a VOX firmware (one algorithm, or vox_multi) compiled for a
 * computer as a shared library. It contains the same panel glue, engines,
 * user interface (vox_ui) and settings code as the module, with the hardware
 * replaced by the shims in vox/host/shim.
 *
 * One loaded copy of the library = one emulated module: the firmware code
 * keeps its state in globals, exactly as on the module. A host that wants
 * several modules loads a private copy of the file for each one (VCV Rack:
 * vox/rack/src/CoreLoader.hpp).
 *
 * Timing: vox_core_process() is the codec interrupt (48 kHz, 12 frames). The
 * 1 kHz UI interrupt and the main loop are run from it at their usual rates,
 * so the host only calls vox_core_process().
 */

#ifndef VOX_HOST_VOX_CORE_API_H_
#define VOX_HOST_VOX_CORE_API_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define VOX_CORE_EXPORT __declspec(dllexport)
#else
#define VOX_CORE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define VOX_CORE_API_VERSION 1
#define VOX_CORE_SAMPLE_RATE 48000
#define VOX_CORE_BLOCK_SIZE 12

/* Pots, in the order of beads::PotsAdcChannel. 0 = fully CCW, 1 = fully CW
 * (attenuverters: 0.5 = noon). */
enum VoxCorePot {
  VOX_POT_FEEDBACK,
  VOX_POT_DRY_WET,
  VOX_POT_REVERB,
  VOX_POT_TIME_AMOUNT,
  VOX_POT_SIZE_AMOUNT,
  VOX_POT_SHAPE_AMOUNT,
  VOX_POT_PITCH_AMOUNT,
  VOX_POT_TIME,
  VOX_POT_DENSITY,
  VOX_POT_PITCH,
  VOX_POT_SIZE,
  VOX_POT_SHAPE,
  VOX_POT_LAST
};

/* CV jacks, in the order of beads::CvAdcChannel. */
enum VoxCoreCv {
  VOX_CV_ASSIGN,
  VOX_CV_DENSITY,
  VOX_CV_PITCH,
  VOX_CV_TIME,
  VOX_CV_SIZE,
  VOX_CV_SHAPE,
  VOX_CV_LAST
};

/* Jacks whose patched state the firmware sees, in the order of
 * beads::NormalizationDetectorChannel. */
enum VoxCoreJack {
  VOX_JACK_SEED,
  VOX_JACK_TIME_CV,
  VOX_JACK_SIZE_CV,
  VOX_JACK_SHAPE_CV,
  VOX_JACK_PITCH_CV,
  VOX_JACK_DENSITY_CV,
  VOX_JACK_IN_L,
  VOX_JACK_IN_R,
  VOX_JACK_LAST
};

enum VoxCoreButton {
  VOX_BUTTON_FREEZE,
  VOX_BUTTON_SEED,
  VOX_BUTTON_MEDIA,
  VOX_BUTTON_ASSIGN,
  VOX_BUTTON_LAST
};

/* LEDs, in the order of beads::Led. */
enum VoxCoreLed {
  VOX_LED_MEDIA_R,
  VOX_LED_MEDIA_G,
  VOX_LED_MEDIA_B,
  VOX_LED_FREEZE,
  VOX_LED_SEED,
  VOX_LED_VU,
  VOX_LED_ASSIGN_1,
  VOX_LED_ASSIGN_2,
  VOX_LED_ASSIGN_3,
  VOX_LED_LAST
};

/* Per-frame gate inputs (bit field). */
enum {
  VOX_GATE_SEED = 1,
  VOX_GATE_FREEZE = 2
};

typedef struct {
  float pot[VOX_POT_LAST];          /* 0..1 */
  float cv_volts[VOX_CV_LAST];      /* volts at the CV jacks (PITCH: 1 V/oct) */
  uint8_t patched[VOX_JACK_LAST];   /* 1 = a cable is plugged in */
  uint8_t button[VOX_BUTTON_LAST];  /* 1 = held */
} VoxCoreControls;

enum VoxCorePhase {
  VOX_PHASE_OFF,          /* before vox_core_power_on */
  VOX_PHASE_SELECTING,    /* vox_multi boot-time algorithm selection */
  VOX_PHASE_RUNNING
};

typedef struct {
  float led[VOX_LED_LAST];      /* 0..1 */
  int phase;                    /* VoxCorePhase */
  int algorithm;                /* index of the running (or selected) algorithm */
  float cpu_load;               /* host time per block / block period */
  uint32_t settings_saves;      /* incremented whenever the firmware saves */
} VoxCoreStatus;

/* Identification. */
VOX_CORE_EXPORT int vox_core_api_version(void);
VOX_CORE_EXPORT const char* vox_core_firmware_name(void);          /* e.g. "vox_multi" */
VOX_CORE_EXPORT int vox_core_num_algorithms(void);
VOX_CORE_EXPORT const char* vox_core_algorithm_name(int index);    /* e.g. "psola" */

/* Settings flash (calibration and state chunks), saved by the host with the
 * patch. Set before vox_core_power_on (sizes of 0 = blank flash). */
VOX_CORE_EXPORT void vox_core_set_storage(
    const uint8_t* persistent, size_t persistent_size,
    const uint8_t* state, size_t state_size);
VOX_CORE_EXPORT size_t vox_core_get_persistent(uint8_t* buffer, size_t capacity);
VOX_CORE_EXPORT size_t vox_core_get_state(uint8_t* buffer, size_t capacity);

/* Controls: call before vox_core_power_on (buttons held at power-on matter:
 * SEED = recover settings, MEDIA = algorithm selection in vox_multi) and
 * then as often as wanted (at least once per block for smooth knobs). */
VOX_CORE_EXPORT void vox_core_set_controls(const VoxCoreControls* controls);

/* Boots the firmware, as at power-on. */
VOX_CORE_EXPORT void vox_core_power_on(void);

/* One codec block: VOX_CORE_BLOCK_SIZE frames at 48 kHz. Audio in volts
 * (+-5 V = codec full scale), gates as VOX_GATE_* bits per frame. */
VOX_CORE_EXPORT void vox_core_process(
    const float* in_l,
    const float* in_r,
    const uint8_t* gates,
    float* out_l,
    float* out_r);

VOX_CORE_EXPORT void vox_core_get_status(VoxCoreStatus* status);

#ifdef __cplusplus
}
#endif

#endif  /* VOX_HOST_VOX_CORE_API_H_ */
