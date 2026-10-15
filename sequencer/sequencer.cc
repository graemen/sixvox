// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_sequencer firmware: panel mapping.
//
// TIME     pattern offset in the unit pool
// SIZE     pattern length, 1-16 steps
// SHAPE    order: in order / reverse / fixed shuffle / random per cycle /
//          random per step
// DENSITY  external clock: /4 /3 /2 x1 x2 x3 x4; internal: 40-240 BPM
// PITCH    transposition (stock interval law, 1V/oct), sampled per step
// FEEDBACK "REPEAT": stutter probability, ratchets above 3/4
// DRY/WET, REVERB as stock
// FREEZE   freeze the buffer and the pool
// SEED jack    clock input
// SEED button  tap tempo (internal clock) / manual step (external clock)
// SEED + FREEZE    pattern lock
// MEDIA    green syllable/truncate, yellow word/truncate,
//          cyan syllable/stretch, magenta word/stretch
// MEDIA + FREEZE   pause removal on/off
// MEDIA + SEED     swing: off / 16% / 33%
// ASSIGN   stock destinations + 5th: reset (rising edge above 1 V)
// ASSIGN + SEED    gate output: off / step triggers / accent gate

#include "vox/common/app/vox_platform.h"
#include "vox/common/app/algorithms.h"

#include "stmlib/system/system_clock.h"

#include "vox/common/hw/settings.h"

#include "vox/sequencer/dsp/sequencer_engine.h"

using namespace beads;
using namespace stmlib;

namespace vox {
namespace sequencer {

static_assert(VoxSpeechBuffer::kCapacity == kVoxSpeechStorageSize,
              "speech buffer and shared storage sizes differ");

SequencerEngine* engine = NULL;  // constructed in the shared arena

volatile bool lock = false;
bool previous_seed_button = false;
bool reset_high = false;
uint32_t seen_steps = 0;
int32_t vu_flash = 0;
uint8_t vu_flash_level = 0;
int32_t seed_flash = 0;

enum {
  FLAG_PAUSE_REMOVAL = 1,
  FLAG_SWING_SHIFT = 1,
  FLAG_SWING_MASK = 6
};

const VoxFirmwareConfig kConfig = {
  VOX_FIRMWARE_SEQUENCER,
  true,  // PITCH: stock interval law, semitones
  {
    5,  // ASSIGN: stock destinations + reset
    3,  // gate output: off / step triggers / accent gate
    {
      { 0, 255, 32 },    // green: syllable / truncate
      { 255, 160, 0 },   // yellow: word / truncate
      { 0, 255, 255 },   // cyan: syllable / stretch
      { 255, 0, 255 },   // magenta: word / stretch
    }
  }
};

static inline uint8_t Swing() {
  const uint8_t s = (settings.state().vox_flags & FLAG_SWING_MASK) >> FLAG_SWING_SHIFT;
  return s > 2 ? 0 : s;
}

void AlgorithmInit() {
  engine = CreateEngine<SequencerEngine>();
  engine->Init(float(kVoxSampleRate), speech_storage, reverb_buffer);
}

void AlgorithmProcess(
    const VoxBlock& b,
    const FloatFrame* in,
    FloatFrame* out,
    bool* gate_out,
    size_t size) {
  SequencerParams p;
  p.time = b.time.Value();
  p.size = b.size.Value();
  p.shape = b.shape.Value();
  p.density = b.density;
  p.pitch = b.pitch_semitones;
  p.repeat = b.mix[VoxUi::MIX_PARAMETER_FEEDBACK];
  p.dry_wet = b.mix[VoxUi::MIX_PARAMETER_DRY_WET];
  p.reverb = b.mix[VoxUi::MIX_PARAMETER_REVERB];
  p.freeze = b.freeze_button || b.freeze_gate[size - 1];

  p.clock_patched = b.seed_gate != NULL;
  p.clock_edge = -1;
  if (b.seed_gate) {
    for (size_t i = 0; i < size; ++i) {
      if (b.seed_gate[i] & GATE_FLAG_RISING) {
        p.clock_edge = int(i);
        seed_flash = 60;
        break;
      }
    }
  }
  p.tap = b.seed_button && !previous_seed_button;
  previous_seed_button = b.seed_button;
  if (p.tap) seed_flash = 60;

  // Reset: ASSIGN CV routed to the 5th destination, rising above 1 V.
  p.reset = false;
  if (b.assign_extra) {
    if (!reset_high && b.assign_cv > 0.2f) {
      reset_high = true;
      p.reset = true;
    } else if (reset_high && b.assign_cv < 0.1f) {
      reset_high = false;
    }
  }
  p.lock = lock;
  p.submode = b.submode;
  p.pause_removal = settings.state().vox_flags & FLAG_PAUSE_REMOVAL;
  p.swing = Swing();
  p.gate_mode = b.gate_output_mode;

  engine->Process(p, in, out, gate_out, size);

  // LEDs.
  if (engine->steps() != seen_steps) {
    seen_steps = engine->steps();
    vu_flash = 120;
    vu_flash_level = engine->accent() ? 255 : 96;
  }
  if (vu_flash > 0) {
    --vu_flash;
    ui.set_vu_led(vu_flash_level);
  } else {
    ui.set_vu_led(0);
  }
  ui.set_freeze_led(p.freeze ? 255 : 0);
  uint8_t seed_led = seed_flash > 0 ? 255 : 0;
  if (seed_flash > 0) --seed_flash;
  if (lock) seed_led = seed_led ? 255 : 48;
  ui.set_seed_led(seed_led);
  ui.set_media_brightness(p.pause_removal ? 0.4f : 1.0f);
}

void AlgorithmIdle() {
  if (ui.TakeComboCount(VoxUi::COMBO_SEED_FREEZE) & 1) {
    lock = !lock;
  }
  if (ui.TakeComboCount(VoxUi::COMBO_MEDIA_FREEZE) & 1) {
    State* state = settings.mutable_state();
    state->vox_flags ^= FLAG_PAUSE_REMOVAL;
    settings.SaveState();
    ui.FlashAssignLeds(state->vox_flags & FLAG_PAUSE_REMOVAL ? 7 : 0, 800);
  }
  if (ui.TakeComboCount(VoxUi::COMBO_MEDIA_SEED)) {
    State* state = settings.mutable_state();
    const uint8_t s = (Swing() + 1) % 3;
    state->vox_flags = (state->vox_flags & ~FLAG_SWING_MASK) | (s << FLAG_SWING_SHIFT);
    settings.SaveState();
    const uint8_t masks[3] = { 0, 1, 3 };
    ui.FlashAssignLeds(masks[s], 800);
  }
  ui.TakeComboCount(VoxUi::COMBO_SEED_PRESS);
}

}  // namespace sequencer

const VoxAlgorithm kSequencerAlgorithm = {
  &sequencer::kConfig,
  { 0, 64, 255 },
  &sequencer::AlgorithmInit,
  &sequencer::AlgorithmProcess,
  &sequencer::AlgorithmIdle,
};

#ifndef VOX_MULTI
const VoxAlgorithm* const kAlgorithms[] = { &kSequencerAlgorithm };
const int kNumAlgorithms = 1;
#endif

}  // namespace vox
