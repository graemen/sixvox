// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// VOX platform for a computer: replaces vox/common/app/vox_platform.cc in a
// VOX core (vox_core_api.h). The audio callback below is the module's,
// line for line; what differs is only where the hardware comes from:
//
// - The ADCs read codes computed from the host's knob positions and jack
//   voltages, through the module's default CV calibration (or the user's,
//   if the stock calibration procedure is run in the emulation). The real
//   CvReader then applies the pot laws, filtering and calibration.
// - The jack normalization state is given by the host.
// - The 1 kHz SysTick (UI poll) and the main loop are run from the codec
//   callback: the UI poll every 48 frames, the main loop after every block.
// - The boot-time algorithm selection of vox_multi, a blocking loop on the
//   module, is a state machine stepped by the 1 kHz tick.

#include "vox/host/vox_core_api.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "stmlib/dsp/delay_line.h"
#include "stmlib/system/system_clock.h"
#include "stmlib/system/storage.h"

#include "beads/drivers/codec.h"

#include "vox/common/app/algorithm_slots.h"
#include "vox/common/app/vox_platform.h"
#include "vox/common/hw/calibrator.h"
#include "vox/common/hw/cv_reader.h"
#include "vox/common/hw/gain_control.h"
#include "vox/common/hw/normalization_detector.h"
#include "vox/common/hw/settings.h"

#ifndef VOX_CORE_NAME
#define VOX_CORE_NAME "vox"
#endif

using namespace beads;
using namespace stmlib;
using namespace std;

// --- Emulated hardware -------------------------------------------------------

namespace vox_host {

uint16_t cv_adc_values[CV_ADC_CHANNEL_LAST];
uint16_t pots_adc_values[POTS_ADC_CHANNEL_LAST];
uint8_t led_intensity[LED_LAST];
volatile bool switch_pressed[SWITCH_LAST];
bool jack_normalized[NORMALIZATION_DETECTOR_CHANNEL_LAST];
Storage storage;

}  // namespace vox_host

namespace stmlib {
SystemClock system_clock;
}

// Declared but never defined in cv_reader.h (inherited from beads/): the
// firmware build tolerates it, a host link does not.
namespace beads {
CvChannel::CvChannel() { }
CvChannel::~CvChannel() { }
PotChannel::PotChannel() { }
PotChannel::~PotChannel() { }
}

// --- Platform objects (as vox_platform.cc) ----------------------------------

namespace vox {

VoxUi ui;
Settings settings;
GainControl gain_control;

volatile uint32_t profile_last_cycles = 0;
volatile uint32_t profile_max_cycles = 0;

int16_t speech_storage[kVoxSpeechStorageSize];
uint16_t reverb_buffer[kVoxReverbBufferSize];
uint64_t engine_arena[kVoxArenaSize / sizeof(uint64_t)];

int active_algorithm = 0;

}  // namespace vox

using namespace vox;

namespace {

Calibrator calibrator;
CvReader cv_reader;
NormalizationDetector normalization_detector;

const VoxAlgorithm* algorithm = NULL;

int codec_transition_frames;
GateFlags previous_seed;
stmlib::DelayLine<Codec::Event, 256> event_delay;

VoxCorePhase phase = VOX_PHASE_OFF;
int frames_to_tick = 0;
int selection_settle = 0;
int selection_index = 0;
int stored_index = 0;
float cpu_load = 0.0f;

// The module's default calibration (Settings::Init), used to turn jack
// voltages into the codes its ADCs would read. CV values are 1.0 per 5 V,
// except PITCH (semitones, 12 per volt); the input stage is inverting.
uint16_t VoltsToCode(int channel, float volts) {
  float value;
  float scale, offset;
  if (channel == CV_ADC_CHANNEL_PITCH) {
    value = volts * 12.0f;
    scale = -120.0f;
    offset = 60.0f;
  } else if (channel == CV_ADC_CHANNEL_ASSIGN || channel == CV_ADC_CHANNEL_DENSITY) {
    value = volts / 5.0f;
    scale = -2.0f;
    offset = 1.0f;
  } else {
    value = volts / 5.0f;
    scale = -3.2f;
    offset = 1.6f;
  }
  float code = (value - offset) / scale * 65536.0f;
  if (code < 0.0f) code = 0.0f;
  if (code > 65535.0f) code = 65535.0f;
  return uint16_t(code);
}

const char* AlgorithmName(const VoxAlgorithm* a) {
  switch (a->config->firmware_id) {
    case VOX_FIRMWARE_PSOLA: return "psola";
    case VOX_FIRMWARE_LPC: return "lpc";
    case VOX_FIRMWARE_SYLLABLE: return "syllable";
    case VOX_FIRMWARE_SPLIT: return "split";
    case VOX_FIRMWARE_SEQUENCER: return "sequencer";
    case VOX_FIRMWARE_CHAIN: return "chain";
    default: return "unknown";
  }
}

inline void ReadKnob(
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

// The codec callback of vox_platform.cc, unchanged except for the
// normalization detector (whose state comes from the host).
void Process(
    const Codec::Event* events,
    const Codec::Frame* in,
    Codec::Frame* out,
    size_t size) {
  FloatFrame float_in[kMaxCodecBlockSize];
  FloatFrame float_out[kMaxCodecBlockSize];
  bool freeze_gate[kMaxCodecBlockSize];
  GateFlags seed[kMaxCodecBlockSize];
  bool gate_out[kMaxCodecBlockSize];

  cv_reader.Read();
  for (int i = 0; i < VoxUi::MIX_PARAMETER_LAST; ++i) {
    ui.ProcessPot(
        VoxUi::MixParameter(i),
        cv_reader.pot(PotsAdcChannel(POTS_ADC_CHANNEL_FEEDBACK + i)));
  }

  for (size_t i = 0; i < size; ++i) {
    freeze_gate[i] = events[i] & Codec::EVENT_FREEZE;
  }

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

  const float pitch_cv_semitones = b.pitch.cv;
  b.pitch.cv = pitch_cv_semitones / 60.0f;
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
}

// Second half of the module's Init(): from the chosen algorithm to the
// start of the codec.
void FinishBoot(int index) {
  const bool multi = kNumAlgorithms > 1;
  if (multi) {
    State* state = settings.mutable_state();
    if (index != stored_index || \
        state->vox_firmware != (VOX_FIRMWARE_MULTI | index)) {
      SwitchAlgorithmSlots(
          &state->vox_submode, &state->vox_flags, state->vox_slots,
          stored_index, index, kNumAlgorithms);
      state->vox_firmware = VOX_FIRMWARE_MULTI | index;
      settings.SaveState();
    }
    active_algorithm = index;
    algorithm = kAlgorithms[index];
    ui.set_config(&algorithm->config->ui);
  }
  cv_reader.set_pitch_law(algorithm->config->pitch_detune_law);

  algorithm->init();

  gain_control.Init(
      float(kVoxSampleRate), GainControl::SOFT_CLIPPING_MODE_OFF, true);
  event_delay.Init();
  previous_seed = GATE_FLAG_LOW;
  codec_transition_frames = kVoxBlockRate / 4;
  phase = VOX_PHASE_RUNNING;
}

// One step (1 ms) of SelectAlgorithm() in vox_platform.cc.
void SelectionTick() {
  cv_reader.Read();
  if (selection_settle > 0) {
    --selection_settle;
    return;
  }
  if (ui.algorithm_selection_confirmed()) {
    ui.EndAlgorithmSelection();
    FinishBoot(selection_index);
    return;
  }
  selection_index = SelectZone(
      cv_reader.pot(POTS_ADC_CHANNEL_PITCH), selection_index, kNumAlgorithms);
  ui.SetAlgorithmSelection(
      selection_index, kAlgorithms[selection_index]->selection_color);
}

}  // namespace

// --- C API ------------------------------------------------------------------

extern "C" {

int vox_core_api_version(void) { return VOX_CORE_API_VERSION; }

const char* vox_core_firmware_name(void) { return VOX_CORE_NAME; }

int vox_core_num_algorithms(void) { return kNumAlgorithms; }

const char* vox_core_algorithm_name(int index) {
  if (index < 0 || index >= kNumAlgorithms) return "";
  return AlgorithmName(kAlgorithms[index]);
}

void vox_core_set_storage(
    const uint8_t* persistent, size_t persistent_size,
    const uint8_t* state, size_t state_size) {
  vox_host::Storage& s = vox_host::storage;
  s.persistent_size = 0;
  s.state_size = 0;
  if (persistent && persistent_size == sizeof(PersistentData)) {
    memcpy(s.persistent, persistent, persistent_size);
    s.persistent_size = persistent_size;
  }
  if (state && state_size == sizeof(State)) {
    memcpy(s.state, state, state_size);
    s.state_size = state_size;
  }
}

size_t vox_core_get_persistent(uint8_t* buffer, size_t capacity) {
  const vox_host::Storage& s = vox_host::storage;
  if (!buffer || capacity < s.persistent_size) return s.persistent_size;
  memcpy(buffer, s.persistent, s.persistent_size);
  return s.persistent_size;
}

size_t vox_core_get_state(uint8_t* buffer, size_t capacity) {
  const vox_host::Storage& s = vox_host::storage;
  if (!buffer || capacity < s.state_size) return s.state_size;
  memcpy(buffer, s.state, s.state_size);
  return s.state_size;
}

void vox_core_set_controls(const VoxCoreControls* c) {
  for (int i = 0; i < VOX_POT_LAST; ++i) {
    float v = c->pot[i];
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    vox_host::pots_adc_values[i] = uint16_t(v * 65535.0f);
  }
  for (int i = 0; i < VOX_CV_LAST; ++i) {
    vox_host::cv_adc_values[i] = VoltsToCode(i, c->cv_volts[i]);
  }
  for (int i = 0; i < VOX_JACK_LAST; ++i) {
    vox_host::jack_normalized[i] = !c->patched[i];
  }
  for (int i = 0; i < VOX_BUTTON_LAST; ++i) {
    vox_host::switch_pressed[i] = c->button[i] != 0;
  }
}

// The module's Init(), up to the point where it either runs the selection
// loop (vox_multi, MEDIA held) or starts the algorithm.
void vox_core_power_on(void) {
  phase = VOX_PHASE_OFF;
  system_clock.Init();
  const bool multi = kNumAlgorithms > 1;
  algorithm = kAlgorithms[0];
  active_algorithm = 0;
  cv_reader.Init(&settings, false);
  calibrator.Init(&settings, &cv_reader, &normalization_detector);
  ui.Init(&algorithm->config->ui, &settings, &gain_control, &calibrator);

  const bool recovery_mode = ui.pressed_immediate(SWITCH_SEED);
  const bool select_mode = multi && ui.pressed_immediate(SWITCH_MEDIA);
  settings.Init(
      recovery_mode,
      multi ? uint8_t(VOX_FIRMWARE_MULTI) : algorithm->config->firmware_id);

  frames_to_tick = 0;
  stored_index = 0;
  if (multi) {
    stored_index = settings.state().vox_firmware & 7;
    if (stored_index >= kNumAlgorithms) stored_index = 0;
  }
  if (select_mode) {
    ui.StartAlgorithmSelection();
    selection_index = stored_index;
    selection_settle = 400;
    active_algorithm = stored_index;
    phase = VOX_PHASE_SELECTING;
  } else {
    FinishBoot(stored_index);
  }
}

void vox_core_process(
    const float* in_l,
    const float* in_r,
    const uint8_t* gates,
    float* out_l,
    float* out_r) {
  const size_t n = VOX_CORE_BLOCK_SIZE;
  const auto start = chrono::steady_clock::now();

  if (phase == VOX_PHASE_RUNNING) {
    Codec::Event events[VOX_CORE_BLOCK_SIZE];
    Codec::Frame in[VOX_CORE_BLOCK_SIZE];
    Codec::Frame out[VOX_CORE_BLOCK_SIZE];
    for (size_t i = 0; i < n; ++i) {
      events[i] = Codec::Event(gates ? gates[i] & 3 : 0);
      float l = in_l[i] / 5.0f, r = in_r[i] / 5.0f;
      CONSTRAIN(l, -1.0f, 0.999999f);
      CONSTRAIN(r, -1.0f, 0.999999f);
      in[i].l = static_cast<int32_t>(l * 2147483648.0f);
      in[i].r = static_cast<int32_t>(r * 2147483648.0f);
    }
    Process(events, in, out, n);
    // The output stage is inverting (the firmware writes a negative value to
    // get a positive gate).
    for (size_t i = 0; i < n; ++i) {
      out_l[i] = -float(out[i].l) / 2147483648.0f * 5.0f;
      out_r[i] = -float(out[i].r) / 2147483648.0f * 5.0f;
    }
  } else {
    for (size_t i = 0; i < n; ++i) out_l[i] = out_r[i] = 0.0f;
  }

  // 1 kHz SysTick: 48 frames = 4 blocks.
  frames_to_tick -= int(n);
  while (frames_to_tick <= 0) {
    frames_to_tick += kVoxSampleRate / 1000;
    if (phase != VOX_PHASE_OFF) ui.Poll();
    if (phase == VOX_PHASE_SELECTING) SelectionTick();
  }

  // Main loop.
  if (phase == VOX_PHASE_RUNNING) {
    ui.DoEvents();
    algorithm->idle();
  }

  const double elapsed = chrono::duration<double>(
      chrono::steady_clock::now() - start).count();
  const float load = float(elapsed * kVoxSampleRate / double(n));
  cpu_load += (load > cpu_load ? 0.1f : 0.001f) * (load - cpu_load);
}

void vox_core_get_status(VoxCoreStatus* status) {
  for (int i = 0; i < VOX_LED_LAST; ++i) {
    status->led[i] = float(vox_host::led_intensity[i]) / 255.0f;
  }
  status->phase = phase;
  status->algorithm = phase == VOX_PHASE_SELECTING ? selection_index : active_algorithm;
  status->cpu_load = cpu_load;
  status->settings_saves = vox_host::storage.state_saves;
}

}  // extern "C"
