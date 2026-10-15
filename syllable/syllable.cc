// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_syllable firmware: panel mapping.
//
// TIME     unit address (live: back from the newest unit; frozen: from the
//          oldest), quantized to units
// SIZE     grain length: CCW = fraction of a unit, noon = 1 unit, CW = 8 units
// SHAPE    scatter (+/-16 units), envelope softness, reverse probability
// DENSITY  noon = off, CW = regular 0.5-20 Hz, CCW = random 0.5-20 Hz
//          (when the SEED jack is patched, triggers only come from it)
// PITCH    transposition (stock interval law, 1V/oct CV)
// FEEDBACK / DRY-WET / REVERB as stock
// FREEZE   stop recording (stable unit addresses)
// SEED     fire a grain at the TIME unit
// SEED + FREEZE    latch: loop the last unit seamlessly
// MEDIA    unit level: green syllable, yellow word, red onset slice,
//          blue syllable + pause removal
// MEDIA + FREEZE   toggle pause removal (any sub-mode)
// MEDIA + SEED     cycle segmentation sensitivity (low/medium/high)
// ASSIGN + SEED    gate out: OUT R = trigger on every detected onset

#include "vox/common/app/vox_platform.h"
#include "vox/common/app/algorithms.h"

#include "stmlib/system/system_clock.h"

#include "vox/common/hw/gain_control.h"
#include "vox/common/hw/settings.h"

#include "vox/syllable/dsp/syllable_engine.h"

using namespace beads;
using namespace stmlib;

namespace vox {
namespace syllable {

static_assert(VoxSpeechBuffer::kCapacity == kVoxSpeechStorageSize,
              "speech buffer and shared storage sizes differ");

SyllableEngine* engine = NULL;  // constructed in the shared arena

volatile bool latch = false;
bool previous_seed_button = false;
uint32_t seen_finalized = 0;
int32_t vu_flash = 0;
int32_t seed_flash = 0;
uint32_t seen_triggers = 0;

enum {
  FLAG_PAUSE_REMOVAL = 1,
  FLAG_SENSITIVITY_SHIFT = 1,
  FLAG_SENSITIVITY_MASK = 6
};

const VoxFirmwareConfig kConfig = {
  VOX_FIRMWARE_SYLLABLE,
  true,  // PITCH: stock interval law, semitones
  {
    4,  // ASSIGN: stock destinations
    2,  // gate output: off / onset triggers
    {
      { 0, 255, 32 },    // green: syllable
      { 255, 160, 0 },   // yellow: word
      { 255, 0, 0 },     // red: onset slice
      { 0, 64, 255 },    // blue: syllable + pause removal
    }
  }
};

static inline uint8_t Sensitivity() {
  uint8_t s = (settings.state().vox_flags & FLAG_SENSITIVITY_MASK) >> \
      FLAG_SENSITIVITY_SHIFT;
  return s > 2 ? 1 : s;
}

void AlgorithmInit() {
  engine = CreateEngine<SyllableEngine>();
  // Fresh settings start at medium sensitivity.
  State* state = settings.mutable_state();
  if ((state->vox_flags & FLAG_SENSITIVITY_MASK) == 0 && \
      !(state->vox_flags & 0x80)) {
    state->vox_flags |= (1 << FLAG_SENSITIVITY_SHIFT) | 0x80;
  }
  engine->Init(float(kVoxSampleRate), speech_storage, reverb_buffer);
}

void AlgorithmProcess(
    const VoxBlock& b,
    const FloatFrame* in,
    FloatFrame* out,
    bool* gate_out,
    size_t size) {
  SyllableParams p;
  p.time = b.time.Value();
  p.size = b.size.Value();
  p.shape = b.shape.Value();
  p.density = b.density;
  p.pitch = b.pitch_semitones;
  p.feedback = b.mix[VoxUi::MIX_PARAMETER_FEEDBACK];
  p.dry_wet = b.mix[VoxUi::MIX_PARAMETER_DRY_WET];
  p.reverb = b.mix[VoxUi::MIX_PARAMETER_REVERB];
  p.freeze = b.freeze_button || b.freeze_gate[size - 1];

  bool trigger = b.seed_button && !previous_seed_button;
  previous_seed_button = b.seed_button;
  if (b.seed_gate) {
    for (size_t i = 0; i < size; ++i) {
      trigger = trigger || (b.seed_gate[i] & GATE_FLAG_RISING);
    }
  }
  p.trigger = trigger;
  p.seed_patched = b.seed_gate != NULL;
  p.latch = latch;
  p.submode = b.submode;
  p.pause_removal = settings.state().vox_flags & FLAG_PAUSE_REMOVAL;
  p.sensitivity = Sensitivity();

  engine->Process(p, in, out, gate_out, size);

  // LEDs.
  const uint32_t finalized = engine->segmenter().finalized(UNIT_LEVEL_SYLLABLE) + \
      engine->segmenter().finalized(UNIT_LEVEL_WORD) + \
      engine->segmenter().finalized(UNIT_LEVEL_ONSET_SLICE);
  if (finalized != seen_finalized) {
    seen_finalized = finalized;
    vu_flash = 120;  // 30 ms at the 4 kHz block rate
  }
  if (engine->triggers() != seen_triggers) {
    seen_triggers = engine->triggers();
    seed_flash = 80;
  }
  const float level = engine->input_level() * 1.5f;
  uint8_t vu = uint8_t(255.0f * (level > 1.0f ? 1.0f : level * level));
  if (vu_flash > 0) {
    --vu_flash;
    vu = 255;
  }
  ui.set_vu_led(vu);
  ui.set_freeze_led(p.freeze ? 255 : 0);
  uint8_t seed_led = seed_flash > 0 ? 255 : 0;
  if (seed_flash > 0) --seed_flash;
  if (latch) {
    seed_led = (system_clock.milliseconds() & 256) ? 255 : 32;
  }
  ui.set_seed_led(seed_led);
  ui.set_media_brightness(p.pause_removal && \
      p.submode != SYLLABLE_SUBMODE_SYLLABLE_PAUSE_REMOVAL ? 0.4f : 1.0f);
}

void AlgorithmIdle() {
  if (ui.TakeComboCount(VoxUi::COMBO_SEED_FREEZE) & 1) {
    latch = !latch;
  }
  if (ui.TakeComboCount(VoxUi::COMBO_MEDIA_FREEZE) & 1) {
    State* state = settings.mutable_state();
    state->vox_flags ^= FLAG_PAUSE_REMOVAL;
    settings.SaveState();
    ui.FlashAssignLeds(state->vox_flags & FLAG_PAUSE_REMOVAL ? 7 : 0, 800);
  }
  if (ui.TakeComboCount(VoxUi::COMBO_MEDIA_SEED)) {
    State* state = settings.mutable_state();
    const uint8_t s = (Sensitivity() + 1) % 3;
    state->vox_flags = (state->vox_flags & ~FLAG_SENSITIVITY_MASK) | \
        (s << FLAG_SENSITIVITY_SHIFT) | 0x80;
    settings.SaveState();
    const uint8_t masks[3] = { 1, 3, 7 };  // 1, 2 or 3 LEDs
    ui.FlashAssignLeds(masks[s], 800);
  }
  ui.TakeComboCount(VoxUi::COMBO_SEED_PRESS);
}

}  // namespace syllable

const VoxAlgorithm kSyllableAlgorithm = {
  &syllable::kConfig,
  { 255, 160, 0 },
  &syllable::AlgorithmInit,
  &syllable::AlgorithmProcess,
  &syllable::AlgorithmIdle,
};

#ifndef VOX_MULTI
const VoxAlgorithm* const kAlgorithms[] = { &kSyllableAlgorithm };
const int kNumAlgorithms = 1;
#endif

}  // namespace vox
