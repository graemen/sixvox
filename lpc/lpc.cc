// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_lpc firmware: panel mapping.
//
// TIME     live: delay behind the input; frozen: position in the buffer
// DENSITY  frame speed: noon = hold (frozen vowel shape), CW 0..+2x, CCW reverse
// PITCH    excitation pitch (stock law + 1V/oct): follow = transposition,
//          fixed = note (C3 at 0)
// SIZE     formant warp (warped LPC analysis), +/-6 semitones;
//          red: chip clock (pitch and formants together)
// SHAPE    excitation: CCW noise (whisper) -> auto voiced/unvoiced ->
//          noon residual (original) -> pulse following the voice ->
//          CW fixed-pitch pulse (robot / monotone)
//          blue: same without the residual; magenta: residual -> IN R carrier
// FEEDBACK "SLEW": spectral envelope smoothing, 0..500 ms
// DRY/WET, REVERB as stock
// FREEZE   stop recording
// SEED     live: hold the current frame while held; frozen: back to TIME
// SEED + FREEZE    latch: sustained frame (a playable formant drone)
// MEDIA    green resynthesis, red speaking chip, blue synth excitation,
//          magenta cross-synthesis (IN R = carrier; LED blinks if unpatched)
// MEDIA + SEED     follow pitch: detected F0 <-> smoothed (200 ms) F0
// ASSIGN + SEED    gate output: OUT R = voiced gate

#include "vox/common/app/vox_platform.h"
#include "vox/common/app/algorithms.h"

#include "stmlib/system/system_clock.h"

#include <algorithm>

#include "vox/common/hw/settings.h"

#include "vox/lpc/dsp/lpc_engine.h"

using namespace beads;
using namespace stmlib;

namespace vox {
namespace lpc {

VoiceTrackEntry track_storage[LpcEngine::kTrackSize] VOX_SECTION(".ramd2");

LpcEngine* engine = NULL;  // constructed in the shared arena

volatile bool latch = false;
bool previous_seed = false;
float level = 0.0f;

enum {
  FLAG_SMOOTH_F0 = 1
};

const VoxFirmwareConfig kConfig = {
  VOX_FIRMWARE_LPC,
  true,  // PITCH: stock interval law, semitones
  {
    4,  // ASSIGN: stock destinations
    2,  // gate output: off / voiced gate
    {
      { 0, 255, 32 },    // green: resynthesis
      { 255, 0, 0 },     // red: speaking chip
      { 0, 64, 255 },    // blue: synth excitation
      { 255, 0, 255 },   // magenta: cross-synthesis
    }
  }
};

void AlgorithmInit() {
  engine = CreateEngine<LpcEngine>();
  engine->Init(float(kVoxSampleRate), speech_storage, track_storage, reverb_buffer);
}

void AlgorithmProcess(
    const VoxBlock& b,
    const FloatFrame* in,
    FloatFrame* out,
    bool* gate_out,
    size_t size) {
  LpcParams p;
  p.time = b.time.Value();
  p.size = b.size.Value();
  p.shape = b.shape.Value();
  p.density = b.density;
  p.pitch = b.pitch_semitones;
  p.slew = b.mix[VoxUi::MIX_PARAMETER_FEEDBACK];
  p.dry_wet = b.mix[VoxUi::MIX_PARAMETER_DRY_WET];
  p.reverb = b.mix[VoxUi::MIX_PARAMETER_REVERB];
  p.freeze = b.freeze_button || b.freeze_gate[size - 1];
  bool seed = b.seed_button;
  if (b.seed_gate) seed = seed || (b.seed_gate[size - 1] & GATE_FLAG_HIGH);
  p.hold = seed;
  p.trigger = seed && !previous_seed;
  previous_seed = seed;
  p.latch = latch;
  p.smooth_f0 = settings.state().vox_flags & FLAG_SMOOTH_F0;
  p.carrier_present = b.right_patched && b.left_patched;
  p.submode = b.submode;

  engine->Process(p, in, out, gate_out, size);

  // VU: input level (the carrier level in cross-synthesis).
  float peak = 0.0f;
  for (size_t i = 0; i < size; ++i) {
    const float x = p.submode == LPC_SUBMODE_CROSS ? in[i].r : in[i].l;
    peak = std::max(peak, x > 0.0f ? x : -x);
  }
  level += (peak > level ? 0.1f : 0.002f) * (peak - level);
  const float l = level * 1.5f;
  ui.set_vu_led(uint8_t(255.0f * (l > 1.0f ? 1.0f : l * l)));
  ui.set_freeze_led(p.freeze ? 255 : 0);
  ui.set_seed_led(latch ? ((system_clock.milliseconds() & 256) ? 255 : 32)
                        : (seed ? 255 : 0));
  const bool missing_carrier = p.submode == LPC_SUBMODE_CROSS && !p.carrier_present;
  ui.set_media_brightness(missing_carrier && (system_clock.milliseconds() & 256)
                          ? 0.2f : 1.0f);
}

void AlgorithmIdle() {
  if (ui.TakeComboCount(VoxUi::COMBO_SEED_FREEZE) & 1) {
    latch = !latch;
  }
  if (ui.TakeComboCount(VoxUi::COMBO_MEDIA_SEED) & 1) {
    State* state = settings.mutable_state();
    state->vox_flags ^= FLAG_SMOOTH_F0;
    settings.SaveState();
    ui.FlashAssignLeds(state->vox_flags & FLAG_SMOOTH_F0 ? 7 : 0, 800);
  }
  ui.TakeComboCount(VoxUi::COMBO_MEDIA_FREEZE);
  ui.TakeComboCount(VoxUi::COMBO_SEED_PRESS);
}

}  // namespace lpc

const VoxAlgorithm kLpcAlgorithm = {
  &lpc::kConfig,
  { 255, 0, 0 },
  &lpc::AlgorithmInit,
  &lpc::AlgorithmProcess,
  &lpc::AlgorithmIdle,
};

#ifndef VOX_MULTI
const VoxAlgorithm* const kAlgorithms[] = { &kLpcAlgorithm };
const int kNumAlgorithms = 1;
#endif

}  // namespace vox
