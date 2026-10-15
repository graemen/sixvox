// Copyright 2026 VOX contributors.
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
// vox_chain firmware: panel mapping for the voice processing chain.
//
// DENSITY   compression amount         TIME      attack/release macro
// SIZE      tone tilt (body <-> air)   SHAPE     character (lo-fi <- clean ->
//                                                radio/telephone/megaphone)
// PITCH     de-esser depth (linear knob, CV 1.0 per 5V)
// FEEDBACK  gate/expander              DRY/WET   parallel compression blend
// REVERB    room ambience
// FREEZE    button: bypass             FREEZE gate: mute voice
// SEED      button: cycle VU display   SEED gate: push (+6 dB into compressor)
// SEED+FREEZE  toggle input auto-gain (auto <-> unity)
// MEDIA     preset: green Podcast, yellow Broadcast, red Announcer, blue Raw
// ASSIGN    stock destinations + 5th: bed ducking depth (all LEDs blink)
// IN R      music bed, ducked by the voice
// ASSIGN+SEED  gate output mode: OUT R = voice activity gate

#include "vox/common/app/vox_platform.h"
#include "vox/common/app/algorithms.h"

#include "vox/common/hw/gain_control.h"
#include "vox/common/hw/settings.h"

#include "vox/chain/dsp/chain_engine.h"

using namespace beads;
using namespace stmlib;

namespace vox {
namespace chain {

ChainEngine* engine = NULL;  // constructed in the shared arena

// VU LED display: input level, gain reduction, output level, bed level.
volatile uint8_t vu_display = ChainEngine::METER_INPUT;

const VoxFirmwareConfig kConfig = {
  VOX_FIRMWARE_CHAIN,
  false,  // PITCH is a plain knob (de-esser depth)
  {
    5,  // ASSIGN destinations: stock 4 + bed ducking depth
    2,  // gate output: off / voice activity
    {
      { 0, 255, 32 },    // green: Podcast
      { 255, 160, 0 },   // yellow: Broadcast
      { 255, 0, 0 },     // red: Announcer
      { 0, 64, 255 },    // blue: Raw / character
    }
  }
};

void AlgorithmInit() {
  engine = CreateEngine<ChainEngine>();
  engine->Init(float(kVoxSampleRate), reverb_buffer);
}

void AlgorithmProcess(
    const VoxBlock& b,
    const FloatFrame* in,
    FloatFrame* out,
    bool* gate_out,
    size_t size) {
  ChainParams p;
  p.amount = b.density;
  p.time = b.time.Value();
  p.tilt = b.size.Value();
  p.character = b.shape.Value();
  p.deess = b.pitch.Value();
  p.gate = b.mix[VoxUi::MIX_PARAMETER_FEEDBACK];
  p.dry_wet = b.mix[VoxUi::MIX_PARAMETER_DRY_WET];
  p.reverb = b.mix[VoxUi::MIX_PARAMETER_REVERB];
  p.duck_cv = b.assign_extra ? b.assign_cv : 0.0f;
  p.bypass = b.freeze_button;
  p.push = b.seed_gate && (b.seed_gate[size - 1] & GATE_FLAG_HIGH);
  p.mute = b.freeze_gate[size - 1];
  p.bed_present = b.right_patched && b.left_patched;
  p.preset = b.submode;

  engine->Process(p, in, out, gate_out, size);

  // LEDs.
  ui.set_freeze_led(p.bypass ? 255 : 0);
  ui.set_seed_led(p.push ? 255 : (p.mute ? 64 : 0));
  const float m = engine->meter(ChainEngine::Meter(vu_display));
  ui.set_vu_led(uint8_t(255.0f * m * m));
}

void AlgorithmIdle() {
  if (ui.TakeComboCount(VoxUi::COMBO_SEED_PRESS)) {
    vu_display = (vu_display + 1) % ChainEngine::METER_LAST;
    // Show which meter is displayed: ASSIGN LED 1, 2, 3, or all three (bed).
    const uint8_t masks[ChainEngine::METER_LAST] = { 1, 2, 4, 7 };
    ui.FlashAssignLeds(masks[vu_display], 800);
  }

  if (ui.TakeComboCount(VoxUi::COMBO_SEED_FREEZE)) {
    // Toggle input gain between automatic (0) and fixed unity (1).
    State* state = settings.mutable_state();
    state->gain = state->gain == 0 ? 1 : 0;
    settings.SaveState();
    if (state->gain == 0) {
      gain_control.Restart();
    }
    ui.FlashAssignLeds(state->gain == 0 ? 7 : 0, 800);
  }

  // Not used by this firmware.
  ui.TakeComboCount(VoxUi::COMBO_MEDIA_SEED);
  ui.TakeComboCount(VoxUi::COMBO_MEDIA_FREEZE);
}

}  // namespace chain

const VoxAlgorithm kChainAlgorithm = {
  &chain::kConfig,
  { 255, 0, 255 },
  &chain::AlgorithmInit,
  &chain::AlgorithmProcess,
  &chain::AlgorithmIdle,
};

#ifndef VOX_MULTI
const VoxAlgorithm* const kAlgorithms[] = { &kChainAlgorithm };
const int kNumAlgorithms = 1;
#endif

}  // namespace vox
