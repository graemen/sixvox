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
// VOX platform: main(), interrupts, control reading.
//
// Differences from beads/beads.cc:
// - No synth mode, no factory test, no buffer backup.
// - The codec is started once at 48 kHz and never reconfigured.
// - The FREEZE button and FREEZE gate are reported separately.
// - Cycle counting of the audio callback (profile_*_cycles).

#include "vox/common/app/vox_platform.h"

#include <algorithm>

#include "vox/common/app/algorithm_slots.h"

#include "stmlib/dsp/delay_line.h"
#include "stmlib/system/system_clock.h"

#include "beads/drivers/codec.h"
#include "beads/drivers/freeze_input.h"
#include "beads/drivers/seed_input.h"
#include "beads/drivers/system.h"
#include "beads/drivers/watchdog.h"

#include "vox/common/hw/calibrator.h"
#include "vox/common/hw/cv_reader.h"
#include "vox/common/hw/gain_control.h"
#include "vox/common/hw/normalization_detector.h"
#include "vox/common/hw/settings.h"

using namespace beads;
using namespace stmlib;
using namespace std;

namespace vox {

VoxUi ui;
Settings settings;
GainControl gain_control;

volatile uint32_t profile_last_cycles = 0;
volatile uint32_t profile_max_cycles = 0;

int16_t speech_storage[kVoxSpeechStorageSize] VOX_SECTION(".ramd1");
uint16_t reverb_buffer[kVoxReverbBufferSize] VOX_SECTION(".ramd3");
uint64_t engine_arena[kVoxArenaSize / sizeof(uint64_t)];

int active_algorithm = 0;

}  // namespace vox

using namespace vox;

Calibrator calibrator;
CvReader cv_reader;
Codec codec;
FreezeInput freeze_input;
NormalizationDetector normalization_detector;
SeedInput seed_input;

volatile bool ready = false;

static const VoxAlgorithm* algorithm = NULL;

// Blanks the output while the normalization detector settles after boot.
int codec_transition_frames;

GateFlags previous_seed;
stmlib::DelayLine<Codec::Event, 256> event_delay;

extern "C" {

void SysTick_Handler() {
  HAL_IncTick();
  Watchdog::Reload();
  if (!ready) {
    return;
  }
  ui.Poll();
}

}

static inline void ReadKnob(
    Knob* k,
    PotsAdcChannel base,
    PotsAdcChannel amount,
    CvAdcChannel cv,
    NormalizationDetectorChannel n) {
  k->base = cv_reader.pot(base);
  k->attenuverter = cv_reader.pot(amount);
  k->has_cv = !normalization_detector.normalized(n);
  k->cv = cv_reader.cv(cv);
}

static void Process(
    const Codec::Event* events,
    const Codec::Frame* in,
    Codec::Frame* out,
    size_t size) {
  const uint32_t start = DWT->CYCCNT;

  FloatFrame float_in[kMaxCodecBlockSize];
  FloatFrame float_out[kMaxCodecBlockSize];
  bool freeze_gate[kMaxCodecBlockSize];
  GateFlags seed[kMaxCodecBlockSize];
  bool gate_out[kMaxCodecBlockSize];

  normalization_detector.Process(
      events[size - 1],
      in[size - 1],
      cv_reader.cv_values());

  cv_reader.Read();
  for (int i = 0; i < VoxUi::MIX_PARAMETER_LAST; ++i) {
    ui.ProcessPot(
        VoxUi::MixParameter(i),
        cv_reader.pot(PotsAdcChannel(POTS_ADC_CHANNEL_FEEDBACK + i)));
  }

  for (size_t i = 0; i < size; ++i) {
    freeze_gate[i] = events[i] & Codec::EVENT_FREEZE;
  }

  // SEED gate, delayed by 2.5ms as in the stock firmware (lets the CVs
  // sampled with the trigger settle).
  const GateFlags* seed_input_flags = NULL;
  if (!normalization_detector.normalized(NORMALIZATION_DETECTOR_CHANNEL_SEED)) {
    for (size_t i = 0; i < size; ++i) {
      event_delay.Write(events[i]);
      Codec::Event delayed_event = event_delay.Read(size * 10);
      bool has_seed = delayed_event & Codec::EVENT_SEED;
      previous_seed = seed[i] = ExtractGateFlags(previous_seed, has_seed);
    }
    seed_input_flags = seed;
  } else {
    previous_seed = GATE_FLAG_LOW;
  }

  const State& state = settings.state();
  const bool left_normalized = normalization_detector.normalized(
      NORMALIZATION_DETECTOR_CHANNEL_AUDIO_LEFT);
  const bool right_normalized = normalization_detector.normalized(
      NORMALIZATION_DETECTOR_CHANNEL_AUDIO_RIGHT);

  gain_control.Process(
      !codec_transition_frames,
      state.gain,
      left_normalized,
      right_normalized,
      in,
      float_in,
      size);

  if (codec_transition_frames) {
    --codec_transition_frames;
    Codec::Frame silence;
    silence.l = 0;
    silence.r = 0;
    fill(&out[0], &out[size], silence);
    return;
  }

  VoxBlock b;
  ReadKnob(&b.time, POTS_ADC_CHANNEL_TIME, POTS_ADC_CHANNEL_TIME_AMOUNT,
           CV_ADC_CHANNEL_TIME, NORMALIZATION_DETECTOR_CHANNEL_TIME_CV);
  ReadKnob(&b.size, POTS_ADC_CHANNEL_SIZE, POTS_ADC_CHANNEL_SIZE_AMOUNT,
           CV_ADC_CHANNEL_SIZE, NORMALIZATION_DETECTOR_CHANNEL_SIZE_CV);
  ReadKnob(&b.shape, POTS_ADC_CHANNEL_SHAPE, POTS_ADC_CHANNEL_SHAPE_AMOUNT,
           CV_ADC_CHANNEL_SHAPE, NORMALIZATION_DETECTOR_CHANNEL_SHAPE_CV);
  ReadKnob(&b.pitch, POTS_ADC_CHANNEL_PITCH, POTS_ADC_CHANNEL_PITCH_AMOUNT,
           CV_ADC_CHANNEL_PITCH, NORMALIZATION_DETECTOR_CHANNEL_PITCH_CV);

  // PITCH CV is calibrated in semitones (12 per volt).
  const float pitch_cv_semitones = b.pitch.cv;
  b.pitch.cv = pitch_cv_semitones / 60.0f;  // 1.0 per 5V, like other CVs
  if (algorithm->config->pitch_detune_law) {
    b.pitch_semitones = cv_reader.pot(POTS_ADC_CHANNEL_PITCH) + \
        (b.pitch.has_cv ? pitch_cv_semitones * b.pitch.attenuverter : 0.0f);
  } else {
    b.pitch_semitones = 0.0f;
  }

  b.density_has_cv = !normalization_detector.normalized(
      NORMALIZATION_DETECTOR_CHANNEL_DENSITY_CV);
  b.density = cv_reader.pot(POTS_ADC_CHANNEL_DENSITY) + \
      (b.density_has_cv ? cv_reader.cv(CV_ADC_CHANNEL_DENSITY) : 0.0f);
  CONSTRAIN(b.density, 0.0f, 1.0f);

  b.assign_cv = cv_reader.cv(CV_ADC_CHANNEL_ASSIGN) - 0.002f;
  b.assign_extra = state.assignable_cv_destination == 4;
  for (int i = 0; i < VoxUi::MIX_PARAMETER_LAST; ++i) {
    float base = ui.mix_parameter(VoxUi::MixParameter(i)) * 1.005f;
    float amount = state.assignable_cv_destination == 3
        ? float(state.assignable_cv_amount[i]) / 256.0f
        : (state.assignable_cv_destination == i ? 1.0f : 0.0f);
    b.mix[i] = base + b.assign_cv * amount;
    CONSTRAIN(b.mix[i], 0.0f, 1.0f);
  }

  b.freeze_gate = freeze_gate;
  b.seed_gate = seed_input_flags;
  b.freeze_button = ui.freeze_button_state();
  b.seed_button = ui.seed_button_state();
  b.left_patched = !left_normalized;
  b.right_patched = !right_normalized;
  b.submode = state.vox_submode & 3;
  b.gate_output_mode = state.gate_output_mode;

  fill(&gate_out[0], &gate_out[size], false);
  algorithm->process(b, float_in, float_out, gate_out, size);

  if (b.gate_output_mode) {
    // Same convention as stock: OUT R carries the gate (the output stage
    // is inverting, hence the negative value).
    for (size_t i = 0; i < size; ++i) {
      float_out[i].l = (float_out[i].l + float_out[i].r) * 0.5f;
      float_out[i].r = gate_out[i] ? -0.8f : 0.0f;
    }
  }

  for (size_t i = 0; i < size; ++i) {
    float l = float_out[i].l;
    float r = float_out[i].r;
    CONSTRAIN(l, -1.0f, 0.999999f);
    CONSTRAIN(r, -1.0f, 0.999999f);
    out[i].l = static_cast<int32_t>(l * 2147483648.0f);
    out[i].r = static_cast<int32_t>(r * 2147483648.0f);
  }

  const uint32_t elapsed = DWT->CYCCNT - start;
  profile_last_cycles = elapsed;
  if (elapsed > profile_max_cycles) {
    profile_max_cycles = elapsed;
  }
}

static void EnableCycleCounter() {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

// Boot-time algorithm selection (vox_multi): PITCH chooses among the
// algorithms (fully CCW = first), the LEDs show the choice, a press of MEDIA
// confirms. Returns the chosen index.
static int SelectAlgorithm(int current) {
  ui.StartAlgorithmSelection();
  int index = current;
  // The pot filters need a few hundred reads to settle on the knob position.
  for (int i = 0; i < 400; ++i) {
    cv_reader.Read();
    uint32_t t = HAL_GetTick();
    while (HAL_GetTick() == t) { }
  }
  while (!ui.algorithm_selection_confirmed()) {
    cv_reader.Read();
    index = SelectZone(cv_reader.pot(POTS_ADC_CHANNEL_PITCH), index, kNumAlgorithms);
    ui.SetAlgorithmSelection(index, kAlgorithms[index]->selection_color);
    uint32_t t = HAL_GetTick();
    while (HAL_GetTick() == t) { }
  }
  ui.EndAlgorithmSelection();
  return index;
}

static void Init() {
  System sys;
  sys.Init(true);

  Watchdog::Start();
  freeze_input.Init();
  seed_input.Init();
  codec.Init();

  const bool multi = kNumAlgorithms > 1;
  algorithm = kAlgorithms[0];
  cv_reader.Init(&settings, false);
  calibrator.Init(&settings, &cv_reader, &normalization_detector);

  ui.Init(&algorithm->config->ui, &settings, &gain_control, &calibrator);

  // Holding SEED at power-on recovers the previous settings chunk (stock).
  const bool recovery_mode = ui.pressed_immediate(SWITCH_SEED);
  // vox_multi: holding MEDIA at power-on enters the algorithm selection.
  const bool select_mode = multi && ui.pressed_immediate(SWITCH_MEDIA);
  settings.Init(
      recovery_mode,
      multi ? uint8_t(VOX_FIRMWARE_MULTI) : algorithm->config->firmware_id);

  if (multi) {
    State* state = settings.mutable_state();
    int stored = state->vox_firmware & 7;
    if (stored >= kNumAlgorithms) stored = 0;
    int index = stored;
    if (select_mode) {
      ready = true;  // UI polling (LEDs, switches) from SysTick
      index = SelectAlgorithm(stored);
    }
    if (index != stored || state->vox_firmware != (VOX_FIRMWARE_MULTI | index)) {
      SwitchAlgorithmSlots(
          &state->vox_submode, &state->vox_flags, state->vox_slots,
          stored, index, kNumAlgorithms);
      state->vox_firmware = VOX_FIRMWARE_MULTI | index;
      settings.SaveState();
    }
    active_algorithm = index;
    algorithm = kAlgorithms[index];
    ui.set_config(&algorithm->config->ui);
  }
  cv_reader.set_pitch_law(algorithm->config->pitch_detune_law);

  EnableCycleCounter();
  algorithm->init();

  gain_control.Init(
      float(kVoxSampleRate), GainControl::SOFT_CLIPPING_MODE_OFF, true);
  normalization_detector.Init(2);  // Prescaler used by stock at 48 kHz.
  event_delay.Init();
  previous_seed = GATE_FLAG_LOW;
  codec_transition_frames = kVoxBlockRate / 4;

  ready = true;
  codec.Start(kVoxSampleRate, kVoxBlockSize, &Process);
}

int main(void) {
  Init();
  while (1) {
    ui.DoEvents();
    algorithm->idle();
  }
}
