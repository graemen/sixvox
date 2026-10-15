// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_split firmware: panel mapping.
//
// SHAPE    stream balance: CW = vowels only, CCW = consonants only
//          (blue: CW attenuates consonants, CCW the noise floor)
// SIZE     vowel-tail decay: off .. 4 s
// PITCH    tail transposition (stock interval law, 1V/oct)
// TIME     consonant delay 10 ms .. 1.2 s, or clock ratio when SEED is clocked
// FEEDBACK delay feedback (up to 105%)
// DENSITY  classifier bias: CW = more "voiced", CCW = more "unvoiced"
// DRY/WET, REVERB (reverb on the stream chosen by the sub-mode)
// FREEZE   hold all tails (drone)
// SEED     capture a tail now; a clock on the SEED jack syncs the delay
// SEED + FREEZE    latch: mute the input vowels (tails + consonants only)
// MEDIA    green process, yellow swap, red splitter (OUT L = vowels,
//          OUT R = consonants), blue clean-up
// MEDIA + SEED     classifier preset: speech / speech over music / phone
// ASSIGN + SEED    gate output: OUT R = voiced gate (not in red)

#include "vox/common/app/vox_platform.h"
#include "vox/common/app/algorithms.h"

#include "stmlib/system/system_clock.h"

#include "vox/common/hw/settings.h"

#include "vox/split/dsp/split_engine.h"

using namespace beads;
using namespace stmlib;

namespace vox {
namespace split {

float delay_buffer[ConsonantDelay::kMaxDelay] VOX_SECTION(".ramd2");

SplitEngine* engine = NULL;  // constructed in the shared arena

volatile bool mute_voiced = false;
bool previous_seed_button = false;
int32_t burst_flash = 0;

enum {
  FLAG_PRESET_MASK = 3
};

const VoxFirmwareConfig kConfig = {
  VOX_FIRMWARE_SPLIT,
  true,  // PITCH: stock interval law, semitones
  {
    4,  // ASSIGN: stock destinations
    2,  // gate output: off / voiced gate
    {
      { 0, 255, 32 },    // green: process
      { 255, 160, 0 },   // yellow: swap
      { 255, 0, 0 },     // red: splitter
      { 0, 64, 255 },    // blue: clean-up
    }
  }
};

static inline uint8_t Preset() {
  const uint8_t p = settings.state().vox_flags & FLAG_PRESET_MASK;
  return p > 2 ? 0 : p;
}

void AlgorithmInit() {
  engine = CreateEngine<SplitEngine>();
  engine->Init(float(kVoxSampleRate), delay_buffer, reverb_buffer);
}

void AlgorithmProcess(
    const VoxBlock& b,
    const FloatFrame* in,
    FloatFrame* out,
    bool* gate_out,
    size_t size) {
  SplitParams p;
  p.shape = b.shape.Value();
  p.size = b.size.Value();
  p.pitch = b.pitch_semitones;
  p.time = b.time.Value();
  p.feedback = b.mix[VoxUi::MIX_PARAMETER_FEEDBACK];
  p.density = b.density;
  p.dry_wet = b.mix[VoxUi::MIX_PARAMETER_DRY_WET];
  p.reverb = b.mix[VoxUi::MIX_PARAMETER_REVERB];
  p.freeze = b.freeze_button || b.freeze_gate[size - 1];
  p.clock_patched = b.seed_gate != NULL;
  p.clock_edge = -1;
  bool capture = b.seed_button && !previous_seed_button;
  previous_seed_button = b.seed_button;
  if (b.seed_gate) {
    for (size_t i = 0; i < size; ++i) {
      if (b.seed_gate[i] & GATE_FLAG_RISING) {
        p.clock_edge = int(i);
        capture = true;
        break;
      }
    }
  }
  p.capture = capture;
  p.mute_voiced = mute_voiced;
  p.submode = b.submode;
  p.preset = Preset();

  engine->Process(p, in, out, gate_out, size);

  // VU LED: live classifier display.
  const VuvClass c = engine->current_class();
  uint8_t vu = 0;
  if (c == VUV_VOICED) vu = 255;
  else if (c == VUV_UNVOICED) vu = 40;
  if (c == VUV_BURST) burst_flash = 80;
  if (burst_flash > 0) {
    --burst_flash;
    vu = (burst_flash & 16) ? 255 : 0;
  }
  ui.set_vu_led(vu);
  ui.set_freeze_led(p.freeze ? 255 : 0);
  ui.set_seed_led(mute_voiced ? ((system_clock.milliseconds() & 256) ? 255 : 32)
                              : (capture ? 255 : 0));
}

void AlgorithmIdle() {
  if (ui.TakeComboCount(VoxUi::COMBO_SEED_FREEZE) & 1) {
    mute_voiced = !mute_voiced;
  }
  if (ui.TakeComboCount(VoxUi::COMBO_MEDIA_SEED)) {
    State* state = settings.mutable_state();
    const uint8_t preset = (Preset() + 1) % 3;
    state->vox_flags = (state->vox_flags & ~FLAG_PRESET_MASK) | preset;
    settings.SaveState();
    const uint8_t masks[3] = { 1, 3, 7 };
    ui.FlashAssignLeds(masks[preset], 800);
  }
  ui.TakeComboCount(VoxUi::COMBO_MEDIA_FREEZE);
  ui.TakeComboCount(VoxUi::COMBO_SEED_PRESS);
}

}  // namespace split

const VoxAlgorithm kSplitAlgorithm = {
  &split::kConfig,
  { 0, 255, 255 },
  &split::AlgorithmInit,
  &split::AlgorithmProcess,
  &split::AlgorithmIdle,
};

#ifndef VOX_MULTI
const VoxAlgorithm* const kAlgorithms[] = { &kSplitAlgorithm };
const int kNumAlgorithms = 1;
#endif

}  // namespace vox
