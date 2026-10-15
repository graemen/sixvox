// Copyright 2026 VOX contributors. Derived from beads/beads.cc,
// Copyright 2019 Emilie Gillet.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// VOX platform: hardware bring-up, control reading and the audio callback
// shared by every VOX firmware. A firmware provides one or more algorithms
// (descriptors at the bottom of this file) and links against vox/common/app.
// With more than one algorithm (vox_multi), holding MEDIA at power-on enters
// the algorithm selection: PITCH chooses, MEDIA confirms.
//
// The codec always runs at 48 kHz with 12-frame blocks (4 kHz block rate).

#ifndef VOX_COMMON_APP_VOX_PLATFORM_H_
#define VOX_COMMON_APP_VOX_PLATFORM_H_

#include <new>

#include "stmlib/stmlib.h"
#include "stmlib/utils/gate_flags.h"

#include "beads/dsp/frame.h"

#include "vox/common/ui/vox_ui.h"

namespace beads {
class GainControl;
class Settings;
}

namespace vox {

const int kVoxSampleRate = 48000;
const int kVoxBlockRate = 4000;
const size_t kVoxBlockSize = kVoxSampleRate / kVoxBlockRate;  // 12

// A knob with attenuverter and CV input (TIME, SIZE, SHAPE, and PITCH when
// read with the linear law).
struct Knob {
  float base;          // 0..1
  float attenuverter;  // -1..1 (stock attenuverter law, with center dead zone)
  float cv;            // calibrated CV, 1.0 per 5V
  bool has_cv;         // jack patched

  inline float Value() const {
    float v = base + (has_cv ? attenuverter * cv : 0.0f);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  }
};

// Everything a firmware needs to know about the controls for one block.
struct VoxBlock {
  Knob time;
  Knob size;
  Knob shape;
  Knob pitch;              // PITCH as 0..1 knob, cv normalized to 1.0 per 5V
  float pitch_semitones;   // PITCH with stock detune law + 1V/oct CV
  float density;           // 0..1 (knob + CV)
  bool density_has_cv;
  // FEEDBACK, DRY/WET, REVERB pots after ASSIGN CV routing (destinations
  // 0..3, as stock). 0..1.
  float mix[VoxUi::MIX_PARAMETER_LAST];
  float assign_cv;         // -1..1, calibrated
  bool assign_extra;       // ASSIGN destination 4 (firmware-specific) active

  const bool* freeze_gate;            // per frame, FREEZE jack only
  const stmlib::GateFlags* seed_gate; // per frame, NULL if SEED unpatched

  bool freeze_button;      // FREEZE button toggle state
  bool seed_button;        // SEED button held
  bool left_patched;
  bool right_patched;
  uint8_t submode;         // 0..3 (MEDIA button)
  uint8_t gate_output_mode;
};

// Per-firmware configuration.
struct VoxFirmwareConfig {
  uint8_t firmware_id;     // beads::VoxFirmwareId
  bool pitch_detune_law;   // read PITCH knob with the stock interval law
  VoxUiConfig ui;
};

// Platform objects available to the firmware (UI LEDs, settings, AGC).
extern VoxUi ui;
extern beads::Settings settings;
extern beads::GainControl gain_control;

// Profiling (readable over SWD): cycles spent in the audio callback.
extern volatile uint32_t profile_last_cycles;
extern volatile uint32_t profile_max_cycles;

// --- Memory shared by the algorithms ---------------------------------------
//
// Only one algorithm runs at a time, so the big buffers are shared:
// - speech_storage: the 10.9 s speech buffer (int16 at 24 kHz) in SRAM D1;
// - reverb_buffer: the reverb delay memory in SRAM D3;
// - the engine object lives in an arena in DTCM (CreateEngine).
// Algorithm-specific tables in SRAM D2 stay separate (they fit together).

// Places a buffer in one of the STM32H7 RAM regions (".ramd1", ".ramd2",
// ".ramd3"). Host builds (VOX_HOST: VCV Rack, tests) ignore it.
#ifdef VOX_HOST
#define VOX_SECTION(name)
#else
#define VOX_SECTION(name) __attribute__ ((section (name)))
#endif

const size_t kVoxSpeechStorageSize = 1 << 18;
const size_t kVoxReverbBufferSize = 32768;
const size_t kVoxArenaSize = 64 * 1024;

extern int16_t speech_storage[kVoxSpeechStorageSize];
extern uint16_t reverb_buffer[kVoxReverbBufferSize];
extern uint64_t engine_arena[kVoxArenaSize / sizeof(uint64_t)];

// Constructs the algorithm's engine in the arena. Called once, from the
// algorithm's init hook.
template<typename T>
inline T* CreateEngine() {
  static_assert(sizeof(T) <= kVoxArenaSize, "engine does not fit the arena");
  return new (static_cast<void*>(engine_arena)) T;
}

// --- Algorithms --------------------------------------------------------------

struct VoxAlgorithm {
  const VoxFirmwareConfig* config;
  // LED colour shown during the boot-time algorithm selection.
  beads::Leds::Color selection_color;

  // Called once at boot, after settings are loaded, before the codec starts.
  void (*init)();

  // Called from the audio interrupt. in/out are stereo float frames at
  // 48 kHz (the input has already been through the stock auto-gain / manual
  // gain stage). gate_out[i] is used for OUT R when gate_output_mode != 0.
  void (*process)(
      const VoxBlock& block,
      const beads::FloatFrame* in,
      beads::FloatFrame* out,
      bool* gate_out,
      size_t size);

  // Called repeatedly from the main loop.
  void (*idle)();
};

// Provided by the firmware: the algorithms it contains, in selection order
// (PITCH fully CCW = first). Single firmwares list exactly one.
extern const VoxAlgorithm* const kAlgorithms[];
extern const int kNumAlgorithms;

// Index of the running algorithm in kAlgorithms.
extern int active_algorithm;

}  // namespace vox

#endif  // VOX_COMMON_APP_VOX_PLATFORM_H_
