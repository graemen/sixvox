// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_psola engine.

#include "vox/psola/dsp/psola_engine.h"

#include <algorithm>
#include <cmath>

#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/units.h"

#include "vox/common/dsp/fast_math.h"

namespace vox {

using namespace beads;
using namespace std;

float psola_hann_lut[257];

static const float kLog2C0 = 4.0313842f;   // log2(16.3516 Hz)
static const float kLog2C3 = 7.0313842f;   // log2(130.81 Hz)
static const float kMargin = 1200.0f;      // 50 ms

void PsolaEngine::Init(
    float sample_rate,
    int16_t* buffer_storage,
    PitchMark* mark_storage,
    uint16_t* reverb_buffer) {
  for (int i = 0; i <= 256; ++i) {
    psola_hann_lut[i] = 0.5f - 0.5f * cosf(2.0f * float(M_PI) * float(i) / 256.0f);
  }
  sample_rate_ = sample_rate * 0.5f;
  buffer_.Init(buffer_storage);
  marker_.Init(mark_storage, sample_rate_);
  yin_.Init(sample_rate_ * 0.5f);
  decimator_.Init();
  bridge_.Init();
  output_.Init(sample_rate, reverb_buffer);
  for (int i = 0; i < kNumGrains; ++i) grains_[i].Init();
  read_ = 0.0;
  offset_ = 0.0;
  was_frozen_ = false;
  latched_ = false;
  loop_a_ = loop_b_ = 0.0;
  countdown_ = 0.0f;
  glide_log_f0_ = 7.0f;
  stutter_left_ = 0;
  stutter_mark_ = 0;
  feedback_sample_ = 0.0f;
  input_level_ = 0.0f;
  last_f0_ = 0.0f;
  voiced_ = false;
  grains_started_ = 0;
  rng_ = 0x8badf00d;
  submode_ = 0xff;
}

float PsolaEngine::TargetLogF0(const PsolaParams& p, float log_f0) {
  const float shift = p.pitch * (1.0f / 12.0f);
  const float mean = marker_.mean_log_f0();
  if (p.submode == PSOLA_SUBMODE_QUANTIZE) {
    // Snap to the scale (root = PITCH), SHAPE = correction strength.
    static const uint16_t masks[PSOLA_SCALE_LAST] = {
      0xfff,                                   // chromatic
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),
    };
    const float shifted = log_f0 + shift;
    const float note = 12.0f * (shifted - kLog2C0);
    const int root = int(floorf(p.pitch + 0.5f));
    const uint16_t mask = masks[p.scale < PSOLA_SCALE_LAST ? p.scale : 0];
    float best = note;
    float best_distance = 100.0f;
    const int center = int(floorf(note + 0.5f));
    for (int k = center - 6; k <= center + 6; ++k) {
      const int degree = ((k - root) % 12 + 12) % 12;
      if (!(mask & (1 << degree))) continue;
      const float d = fabsf(float(k) - note);
      if (d < best_distance) {
        best_distance = d;
        best = float(k);
      }
    }
    const float quantized = kLog2C0 + best / 12.0f;
    return shifted + p.shape * (quantized - shifted);
  }
  if (p.shape > 0.55f) {
    // Towards monotone, at the note set by PITCH (C3 at 0 semitones).
    const float c = (p.shape - 0.55f) / 0.45f;
    return (1.0f - c) * (log_f0 + shift) + c * (kLog2C3 + shift);
  } else if (p.shape < 0.45f) {
    // Exaggerated intonation around the running mean.
    const float e = 1.0f + 2.0f * (0.45f - p.shape) / 0.45f;
    return mean + (log_f0 - mean) * e + shift;
  }
  return log_f0 + shift;
}

void PsolaEngine::StartGrain(uint32_t centre, float half, float rate, float gain) {
  int slot = -1;
  int32_t least = 0x7fffffff;
  for (int i = 0; i < kNumGrains; ++i) {
    if (!grains_[i].active()) {
      slot = i;
      break;
    }
    if (grains_[i].remaining() < least) {
      least = grains_[i].remaining();
      slot = i;
    }
  }
  grains_[slot].Start(centre, half, rate, gain);
  ++grains_started_;
}

void PsolaEngine::Synthesize(const PsolaParams& p) {
  const VoxMarkTable& marks = marker_.table();
  const uint32_t read = uint32_t(int64_t(floor(read_)));
  float formant = 1.0f;
  int stutter = 1;
  if (p.submode == PSOLA_SUBMODE_STUTTER) {
    stutter = 1 + int(std::max(0.0f, p.size - 0.5f) * 14.0f + 0.5f);
  } else {
    float st = (p.size - 0.5f) * 24.0f;
    if (fabsf(st) < 0.5f) st = 0.0f;
    formant = stmlib::SemitonesToRatio(st);
  }

  uint32_t index = marks.Nearest(read);
  if (stutter > 1) {
    if (stutter_left_ > 0 && int32_t(index - stutter_mark_) >= 0 && \
        index - stutter_mark_ < 64) {
      index = stutter_mark_;
      --stutter_left_;
    } else {
      stutter_mark_ = index;
      stutter_left_ = stutter - 1;
    }
  }
  const PitchMark& m = marks.At(index);
  voiced_ = m.type == MARK_VOICED;

  if (m.type == MARK_VOICED) {
    const float period = float(m.period);
    const float log_f0 = FastLog2(sample_rate_ / period);
    float target = TargetLogF0(p, log_f0);
    if (p.submode == PSOLA_SUBMODE_QUANTIZE) {
      // 20 ms glide between quantized notes.
      const float k = std::min(1.0f, period / (0.02f * sample_rate_));
      glide_log_f0_ += k * (target - glide_log_f0_);
      target = glide_log_f0_;
    } else {
      glide_log_f0_ = target;
    }
    float out_period = sample_rate_ / FastExp2(target);
    CONSTRAIN(out_period, 20.0f, 1000.0f);
    last_f0_ = sample_rate_ / out_period;
    float gain = formant * out_period / period;
    CONSTRAIN(gain, 0.25f, 2.0f);
    if (buffer_.Contains(m.position - uint32_t(period) - 4) && \
        buffer_.Contains(m.position + uint32_t(period) + 4)) {
      StartGrain(m.position, period, formant, gain);
    }
    countdown_ += out_period;
  } else if (m.type == MARK_UNCERTAIN) {
    // Asynchronous fallback: 30 ms grains, resampled by the pitch ratio.
    const float rate = stmlib::SemitonesToRatio(p.pitch) * formant;
    const float half = 360.0f;
    if (buffer_.Contains(read - 400) && buffer_.Contains(read + uint32_t(2.0f * half * rate) + 4)) {
      StartGrain(read + uint32_t(half), half, rate, 1.0f);
    }
    countdown_ += half / rate;
    last_f0_ = 0.0f;
  } else {
    // Unvoiced: 8 ms grains at random offsets around the read head.
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    const int32_t jitter = int32_t(rng_ % 97u) - 48;
    const uint32_t centre = read + uint32_t(jitter);
    const float half = 192.0f;
    if (buffer_.Contains(centre - 200) && buffer_.Contains(centre + 200)) {
      StartGrain(centre, half, formant, 1.0f);
    }
    countdown_ += half / formant;
    last_f0_ = 0.0f;
  }
}

void PsolaEngine::Process(
    const PsolaParams& p,
    const FloatFrame* in,
    FloatFrame* out,
    bool* gate,
    size_t size) {
  float l[kMaxHalfRateBlock], r[kMaxHalfRateBlock];
  float wet[kMaxHalfRateBlock] = { 0.0f };
  float written[kMaxHalfRateBlock] = { 0.0f };
  float decimated[kMaxHalfRateBlock];
  bridge_.Down(in, l, r, size);
  const size_t n = size / 2;

  if (p.submode != submode_) {
    submode_ = p.submode;
    if (p.submode == PSOLA_SUBMODE_NARROW) yin_.set_range(70.0f, 400.0f);
    else yin_.set_range(50.0f, 600.0f);
  }

  // Recording and analysis.
  const bool record = !p.freeze;
  if (record && was_frozen_) {
    marker_.Restart(buffer_.head());
  }
  if (record != !was_frozen_) {
    offset_ = 0.0;
  }
  was_frozen_ = p.freeze;
  if (record) {
    for (size_t i = 0; i < n; ++i) {
      const float x = 0.5f * (l[i] + r[i]);
      input_level_ += (fabsf(x) > input_level_ ? 0.05f : 0.0002f) * \
          (fabsf(x) - input_level_);
      written[i] = stmlib::SoftLimit(x + p.feedback * 0.95f * feedback_sample_);
      buffer_.Write(written[i]);
    }
    decimator_.Process(written, decimated, n);
    if (yin_.Process(decimated, n / 2)) {
      // Window centre in 24 kHz buffer positions (+ decimator delay).
      const uint32_t centre = buffer_.head() - uint32_t(2 * yin_.latency()) - 24;
      marker_.AddEstimate(centre, 2.0f * yin_.period(), yin_.aperiodicity());
    }
    marker_.Update(buffer_);
    marker_.mutable_table()->Prune(buffer_.oldest() + uint32_t(kMargin), buffer_.head());
  }

  // Read speed from DENSITY: noon = hold, CW = 0 .. +2, CCW = 0 .. -2,
  // with a flat zone around 1x.
  float speed = 0.0f;
  if (p.density > 0.53f) speed = (p.density - 0.53f) / 0.47f * 2.0f;
  else if (p.density < 0.47f) speed = -(0.47f - p.density) / 0.47f * 2.0f;
  if (fabsf(speed - 1.0f) < 0.06f) speed = 1.0f;
  if (p.hold && record) speed = 0.0f;
  if (p.trigger && !record) offset_ = 0.0;

  const VoxMarkTable& marks = marker_.table();
  const bool ready = marks.size() > 4;
  // Valid read range: marked, with margins.
  const double newest = ready ? double(marker_.newest_marked()) - 2.0 * kMargin : 0.0;
  const double oldest = double(buffer_.oldest()) + 2.0 * kMargin;
  const double span = newest - oldest;

  for (size_t i = 0; i < n; ++i) {
    float y = 0.0f;
    if (ready && span > 0.0) {
      // TIME: live = delay behind the write head (at least 100 ms, so
      // that the region is already marked); frozen = absolute position.
      // The scrub offset integrates (speed - 1) when live, so that speed 1
      // keeps a constant delay, and integrates speed when frozen.
      const double base = record
          ? double(buffer_.head()) - 2400.0 - double(p.time) * span
          : oldest + double(p.time) * span;
      offset_ += double(speed) - (record ? 1.0 : 0.0);
      double position = base + offset_;
      if (p.latch && !latched_) {
        latched_ = true;
        loop_a_ = base;
        loop_b_ = position;
        if (loop_b_ < loop_a_) std::swap(loop_a_, loop_b_);
        if (loop_b_ - loop_a_ < 0.05 * sample_rate_) loop_b_ = loop_a_ + 0.2 * sample_rate_;
      } else if (!p.latch) {
        latched_ = false;
      }
      if (latched_) {
        const double length = loop_b_ - loop_a_;
        if (position > loop_b_) { offset_ -= length; position -= length; }
        if (position < loop_a_) { offset_ += length; position += length; }
        if (loop_a_ < oldest || loop_b_ > newest) latched_ = false;
      }
      if (position > newest) { offset_ -= position - newest; position = newest; }
      if (position < oldest) { offset_ += oldest - position; position = oldest; }
      read_ = position;

      countdown_ -= 1.0f;
      int guard = 0;
      while (countdown_ <= 0.0f && guard++ < 4) Synthesize(p);
      if (countdown_ < -1000.0f) countdown_ = 0.0f;
    }
    for (int g = 0; g < kNumGrains; ++g) {
      if (grains_[g].active()) y += grains_[g].Render(buffer_);
    }
    feedback_sample_ = y;
    wet[i] = y;
  }

  bridge_.Up(wet, wet, out, n);
  output_.Process(in, out, p.dry_wet, p.reverb, size);
  for (size_t i = 0; i < size; ++i) gate[i] = voiced_;
}

}  // namespace vox
