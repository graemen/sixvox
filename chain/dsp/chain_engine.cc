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
// vox_chain DSP engine.

#include "vox/chain/dsp/chain_engine.h"

#include <algorithm>

namespace vox {

using namespace beads;
using namespace std;
using namespace stmlib;

const size_t kMaxBlockSize = 32;

/* extern */
const ChainPreset kChainPresets[CHAIN_PRESET_LAST] = {
  // hpf   thr   range  ratio  time   tilt   deess  char  duck  out
  {  80.0f, -6.0f, 24.0f, 3.0f,  0.10f, 0.00f, 0.10f, 1.0f, 12.0f, 0.0f },
  { 100.0f, -8.0f, 30.0f, 5.0f,  0.00f, 0.15f, 0.15f, 1.0f, 15.0f, 0.0f },
  { 100.0f, -10.0f, 34.0f, 9.0f, -0.20f, 0.20f, 0.20f, 1.0f, 18.0f, 1.0f },
  { 150.0f, -6.0f, 14.0f, 1.5f,  0.00f, 0.00f, 0.00f, 2.0f, 12.0f, 0.0f },
};

// Bipolar control from a 0..1 knob, with a dead zone around the center.
static inline float Bipolar(float knob, float dead_zone) {
  float x = 2.0f * knob - 1.0f;
  if (x > dead_zone) {
    return (x - dead_zone) / (1.0f - dead_zone);
  } else if (x < -dead_zone) {
    return (x + dead_zone) / (1.0f - dead_zone);
  }
  return 0.0f;
}

void ChainEngine::Init(float sample_rate, uint16_t* reverb_buffer) {
  sample_rate_ = sample_rate;
  preset_ = 0xff;

  expander_.Init(sample_rate);
  hpf_.Init();
  deesser_.Init(sample_rate);
  compressor_.Init(sample_rate);
  tilt_eq_.Init(sample_rate);
  character_.Init(sample_rate);
  vad_.Init(sample_rate);
  ducker_.Init(sample_rate);
  reverb_.Init(reverb_buffer, sample_rate);
  limiter_.Init(sample_rate, 0.94f);  // -0.5 dBFS

  push_db_ = 0.0f;
  dry_wet_ = 1.0f;
  mute_gain_ = 1.0f;
  mute_coefficient_ = OnePoleCoefficient(0.020f / 3.0f, sample_rate);
  bypass_amount_ = 0.0f;
  bypass_coefficient_ = OnePoleCoefficient(0.010f / 3.0f, sample_rate);
  output_gain_ = 1.0f;

  input_meter_.Init(sample_rate, 0.0005f, 0.150f);
  output_meter_.Init(sample_rate, 0.0005f, 0.150f);
  bed_meter_.Init(sample_rate, 0.0005f, 0.150f);
}

void ChainEngine::Configure(const ChainParams& p) {
  const uint8_t preset_index = p.preset < CHAIN_PRESET_LAST ? p.preset : 0;
  const ChainPreset& preset = kChainPresets[preset_index];

  if (preset_index != preset_) {
    preset_ = preset_index;
    hpf_.set_f_q<FREQUENCY_ACCURATE>(preset.hpf_hz / sample_rate_, 0.7071f);
  }

  // Expander / gate (FEEDBACK knob, CCW = off).
  if (p.gate < 0.03f) {
    expander_.set(-100.0f, 1.0f, 40.0f);
  } else {
    const float g = (p.gate - 0.03f) / 0.97f;
    expander_.set(-70.0f + 45.0f * g, 2.0f + 20.0f * g * g, 40.0f);
  }

  // De-esser (PITCH knob, CCW = off).
  float deess = 0.0f;
  if (p.deess >= 0.02f) {
    deess = min(1.0f, p.deess + preset.deess_bias);
  }
  deesser_.set_depth(deess);

  // Compressor (DENSITY = amount, TIME = attack/release macro).
  float time = p.time + preset.time_bias;
  CONSTRAIN(time, 0.0f, 1.0f);
  const float attack = 0.001f * FastExp2(time * 4.9068906f);   // 1..30 ms
  const float release = 0.040f * FastExp2(time * 4.3219281f);  // 40..800 ms
  if (p.amount < 0.02f) {
    compressor_.set(0.0f, 1.0f, 6.0f, attack, release, 0.0f);
  } else {
    const float a = (p.amount - 0.02f) / 0.98f;
    const float threshold = preset.threshold_min_db - \
        preset.threshold_range_db * a;
    const float ratio = 1.0f + preset.ratio_range * a;
    compressor_.set(threshold, ratio, 6.0f, attack, release, 0.0f);
    // Auto make-up: restore the gain lost by a -18 dBFS (typical speech RMS
    // after the input auto-gain) signal. The output limiter catches peaks.
    const float makeup = -compressor_.GainComputer(-18.0f);
    compressor_.set(threshold, ratio, 6.0f, attack, release, makeup);
  }

  // Push (SEED gate) and parallel blend, smoothed per block (~8 ms).
  push_db_ += 0.03f * ((p.push ? 6.0f : 0.0f) - push_db_);
  dry_wet_ += 0.03f * (p.dry_wet - dry_wet_);

  // Tone and character.
  float tilt = Bipolar(p.tilt, 0.04f) + preset.tilt_bias;
  CONSTRAIN(tilt, -1.0f, 1.0f);
  tilt_eq_.set_tilt(tilt);

  float character = Bipolar(p.character, 0.04f) * preset.character_scale;
  CONSTRAIN(character, -1.0f, 1.0f);
  character_.set_amount(character);

  // Ducker depth: preset default, offset by the ASSIGN CV (+/-24 dB at 5V).
  float depth = preset.duck_depth_db + 24.0f * p.duck_cv;
  CONSTRAIN(depth, 0.0f, 36.0f);
  ducker_.set_depth_db(depth);

  // Reverb: small booth/room ambience.
  const float r = p.reverb;
  reverb_.set_mix(r * r * 0.7f);
  reverb_.set_time(0.25f + 0.55f * r * r * r);
  reverb_.set_lp(0.6f);

  output_gain_ = DbToGain(preset.output_gain_db);
}

void ChainEngine::Process(
    const ChainParams& p,
    const FloatFrame* in,
    FloatFrame* out,
    bool* vad,
    size_t size) {
  // Long blocks (host tests) are split.
  while (size > kMaxBlockSize) {
    Process(p, in, out, vad, kMaxBlockSize);
    in += kMaxBlockSize;
    out += kMaxBlockSize;
    vad += kMaxBlockSize;
    size -= kMaxBlockSize;
  }

  Configure(p);

  float bed[kMaxBlockSize];
  const float mute_target = p.mute ? 0.0f : 1.0f;

  for (size_t i = 0; i < size; ++i) {
    const float x = in[i].l;
    input_meter_.Process(x);

    float v = x * expander_.Process(x);
    v = hpf_.Process<FILTER_MODE_HIGH_PASS>(v);
    v = deesser_.Process(v);
    const float compressed = v * compressor_.Process(v, push_db_);
    v += (compressed - v) * dry_wet_;
    v = tilt_eq_.Process(v);
    v = character_.Process(v);
    mute_gain_ += mute_coefficient_ * (mute_target - mute_gain_);
    v *= mute_gain_;

    vad[i] = vad_.Process(v);

    const float bed_in = p.bed_present ? in[i].r : 0.0f;
    bed_meter_.Process(bed_in);
    bed[i] = bed_in * ducker_.Process(v);

    out[i].l = v;
    out[i].r = v;
  }

  reverb_.Process(out, size);

  for (size_t i = 0; i < size; ++i) {
    out[i].l = (out[i].l + bed[i]) * output_gain_;
    out[i].r = (out[i].r + bed[i]) * output_gain_;
    limiter_.Process(&out[i].l, &out[i].r);
  }

  const float bypass_target = p.bypass ? 1.0f : 0.0f;
  for (size_t i = 0; i < size; ++i) {
    bypass_amount_ += bypass_coefficient_ * (bypass_target - bypass_amount_);
    const float dry = in[i].l + (p.bed_present ? in[i].r : 0.0f);
    out[i].l += (dry - out[i].l) * bypass_amount_;
    out[i].r += (dry - out[i].r) * bypass_amount_;
    output_meter_.Process(max(fabsf(out[i].l), fabsf(out[i].r)));
  }
}

float ChainEngine::meter(Meter m) const {
  float v = 0.0f;
  switch (m) {
    case METER_INPUT:
      v = (GainToDb(input_meter_.value()) + 48.0f) * (1.0f / 48.0f);
      break;
    case METER_GAIN_REDUCTION:
      v = -compressor_.gain_reduction_db() * (1.0f / 18.0f);
      break;
    case METER_OUTPUT:
      v = (GainToDb(output_meter_.value()) + 48.0f) * (1.0f / 48.0f);
      break;
    case METER_BED:
      v = (GainToDb(bed_meter_.value()) + ducker_.gain_db() + 48.0f) * \
          (1.0f / 48.0f);
      break;
    default:
      break;
  }
  CONSTRAIN(v, 0.0f, 1.0f);
  return v;
}

}  // namespace vox
