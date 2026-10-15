// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_psola firmware: panel mapping.
//
// TIME     live: delay behind the input; frozen: position in the buffer
// DENSITY  read speed: noon = hold, CW 0..+2x (1x at ~3 o'clock), CCW reverse
// PITCH    F0 transposition, formants preserved (stock law + 1V/oct)
// SIZE     formant shift +/-12 st (red: stutter count)
// SHAPE    intonation: CW -> monotone at the PITCH note (C3 at 0),
//          CCW -> exaggerated; yellow: scale-correction strength
// FEEDBACK / DRY-WET / REVERB as stock
// FREEZE   stop recording and analysis
// SEED     live: hold the current mark while held; frozen: back to TIME
// SEED + FREEZE    latch loop between TIME and the latch point
// MEDIA    green PSOLA, yellow scale-quantized F0, red period stutter,
//          blue narrow F0 range (70-400 Hz)
// MEDIA + SEED     scale: chromatic / major / minor / pentatonic
// ASSIGN + SEED    gate output: OUT R = voiced gate (material at the read head)

#include "vox/common/app/vox_platform.h"
#include "vox/common/app/algorithms.h"

#include "stmlib/system/system_clock.h"

#include "vox/common/hw/settings.h"

#include "vox/psola/dsp/psola_engine.h"

using namespace beads;
using namespace stmlib;

namespace vox {
namespace psola {

PitchMark mark_storage[VoxMarkTable::kCapacity] VOX_SECTION(".ramd2");

PsolaEngine* engine = NULL;  // constructed in the shared arena

volatile bool latch = false;
bool previous_seed = false;

enum {
  FLAG_SCALE_MASK = 3
};

const VoxFirmwareConfig kConfig = {
  VOX_FIRMWARE_PSOLA,
  true,  // PITCH: stock interval law, semitones
  {
    4,  // ASSIGN: stock destinations
    2,  // gate output: off / voiced gate
    {
      { 0, 255, 32 },    // green: PSOLA
      { 255, 160, 0 },   // yellow: scale-quantized F0
      { 255, 0, 0 },     // red: period stutter
      { 0, 64, 255 },    // blue: narrow F0 range
    }
  }
};

void AlgorithmInit() {
  engine = CreateEngine<PsolaEngine>();
  engine->Init(float(kVoxSampleRate), speech_storage, mark_storage, reverb_buffer);
}

void AlgorithmProcess(
    const VoxBlock& b,
    const FloatFrame* in,
    FloatFrame* out,
    bool* gate_out,
    size_t size) {
  PsolaParams p;
  p.time = b.time.Value();
  p.size = b.size.Value();
  p.shape = b.shape.Value();
  p.density = b.density;
  p.pitch = b.pitch_semitones;
  p.feedback = b.mix[VoxUi::MIX_PARAMETER_FEEDBACK];
  p.dry_wet = b.mix[VoxUi::MIX_PARAMETER_DRY_WET];
  p.reverb = b.mix[VoxUi::MIX_PARAMETER_REVERB];
  p.freeze = b.freeze_button || b.freeze_gate[size - 1];

  bool seed = b.seed_button;
  if (b.seed_gate) seed = seed || (b.seed_gate[size - 1] & GATE_FLAG_HIGH);
  p.hold = seed;
  p.trigger = seed && !previous_seed;
  previous_seed = seed;
  p.latch = latch;
  p.submode = b.submode;
  p.scale = settings.state().vox_flags & FLAG_SCALE_MASK;

  engine->Process(p, in, out, gate_out, size);

  // LEDs. VU: input level, blinking while the pitch tracker has lost the
  // voice for more than 200 ms during sound.
  const float level = engine->input_level() * 1.5f;
  uint8_t vu = uint8_t(255.0f * (level > 1.0f ? 1.0f : level * level));
  if (engine->marker().tracking_lost() && level > 0.05f && !p.freeze) {
    vu = (system_clock.milliseconds() & 128) ? vu : 0;
  }
  ui.set_vu_led(vu);
  ui.set_freeze_led(p.freeze ? 255 : 0);
  ui.set_seed_led(latch ? ((system_clock.milliseconds() & 256) ? 255 : 32)
                        : (seed ? 255 : 0));
}

void AlgorithmIdle() {
  if (ui.TakeComboCount(VoxUi::COMBO_SEED_FREEZE) & 1) {
    latch = !latch;
  }
  if (ui.TakeComboCount(VoxUi::COMBO_MEDIA_SEED)) {
    State* state = settings.mutable_state();
    const uint8_t scale = ((state->vox_flags & FLAG_SCALE_MASK) + 1) % PSOLA_SCALE_LAST;
    state->vox_flags = (state->vox_flags & ~FLAG_SCALE_MASK) | scale;
    settings.SaveState();
    const uint8_t masks[PSOLA_SCALE_LAST] = { 0, 1, 3, 7 };
    ui.FlashAssignLeds(masks[scale], 800);
  }
  ui.TakeComboCount(VoxUi::COMBO_MEDIA_FREEZE);
  ui.TakeComboCount(VoxUi::COMBO_SEED_PRESS);
}

}  // namespace psola

const VoxAlgorithm kPsolaAlgorithm = {
  &psola::kConfig,
  { 0, 255, 32 },
  &psola::AlgorithmInit,
  &psola::AlgorithmProcess,
  &psola::AlgorithmIdle,
};

#ifndef VOX_MULTI
const VoxAlgorithm* const kAlgorithms[] = { &kPsolaAlgorithm };
const int kNumAlgorithms = 1;
#endif

}  // namespace vox
