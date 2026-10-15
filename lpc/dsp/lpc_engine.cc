// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_lpc engine.

#include "vox/lpc/dsp/lpc_engine.h"

#include <algorithm>
#include <cmath>

#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/units.h"

#include "vox/common/dsp/fast_math.h"

namespace vox {

using namespace beads;
using namespace std;

static const float kPreEmphasis = 0.94f;
static const double kLiveDelay = 1500.0;  // 62.5 ms: room for window + grains
static const double kMargin = 1200.0;
static const float kLog2C3 = 7.0313842f;

void LpcEngine::Init(
    float sample_rate,
    int16_t* buffer_storage,
    VoiceTrackEntry* track,
    uint16_t* reverb_buffer) {
  sample_rate_ = sample_rate * 0.5f;
  buffer_.Init(buffer_storage);
  yin_.Init(sample_rate_ * 0.5f);
  yin_.set_hop(120);  // 10 ms
  decimator_.Init();
  bridge_.Init();
  output_.Init(sample_rate, reverb_buffer);
  track_ = track;
  track_write_ = 0;
  track_size_ = 0;
  read_ = 0.0;
  offset_ = 0.0;
  was_frozen_ = false;
  latched_ = false;
  latch_position_ = 0.0;
  for (int i = 0; i < 2; ++i) reader_[i].active = false;
  reader_next_ = 0;
  reader_countdown_ = 0;
  for (int i = 0; i < kGrain; ++i) {
    hann_[i] = 0.5f - 0.5f * cosf(2.0f * float(M_PI) * float(i) / float(kGrain));
  }
  for (int i = 0; i < kWindow; ++i) {
    window_[i] = 0.54f - 0.46f * cosf(2.0f * float(M_PI) * float(i) / float(kWindow - 1));
  }
  for (int i = 0; i <= kOrder; ++i) {
    k_inverse_[i] = k_inverse_from_[i] = k_inverse_to_[i] = 0.0f;
    k_warped_from_[i] = k_warped_to_[i] = k_synth_[i] = 0.0f;
  }
  stage_ = 0;
  interpolation_ = kHop;
  frame_centre_ = 0;
  lambda_ = 0.0f;
  pending_voiced_ = false;
  pending_period_ = 200.0f;
  for (int i = 0; i <= kOrder; ++i) {
    pending_inverse_[i] = pending_warped_[i] = r_plain_[i] = r_warped_[i] = 0.0f;
  }
  inverse_.Init();
  synthesis_.Init();
  pre_previous_ = 0.0f;
  de_previous_ = 0.0f;
  residual_rms_ = 1e-6f;
  carrier_level_ = 0.0f;
  pulse_.Init();
  noise_.Init();
  whitener_.Init();
  voicing_mix_ = 0.0f;
  voiced_ = false;
  period_ = 200.0f;
  smoothed_period_ = 200.0f;
  f0_out_ = 0.0f;
  chip_.Init();
  analyses_ = 0;
}

void LpcEngine::LookupVoice(uint32_t position, bool* voiced, float* period) {
  // Newest entry at or before the position (entries are in time order).
  *voiced = false;
  *period = period_;
  if (track_size_ == 0) return;
  uint32_t lo = track_write_ - track_size_, hi = track_write_ - 1;
  const VoiceTrackEntry* t = track_;
  const uint32_t mask = kTrackSize - 1;
  if (int32_t(position - t[lo & mask].position) < 0) return;
  while (hi - lo > 1) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (int32_t(t[mid & mask].position - position) <= 0) lo = mid; else hi = mid;
  }
  const VoiceTrackEntry& e = int32_t(t[hi & mask].position - position) <= 0
      ? t[hi & mask] : t[lo & mask];
  *voiced = e.aperiodicity < 51;  // 0.2
  *period = float(e.period) * 0.25f;
}

// The analysis of one 10 ms hop is spread over the 40 audio blocks of the
// hop (bounded CPU per block):
//   stage 0       commit the previous results, snapshot the windowed frame
//   stages 1-5    plain autocorrelation, 5 lags per block
//   stage 6       Levinson-Durbin (inverse filter)
//   stages 7-30   warped autocorrelation, one all-pass stage per block
//   stage 31      Levinson-Durbin (synthesis filter), voicing lookup
//   stages 32-39  idle
void LpcEngine::AnalysisStage(const LpcParams& p, uint32_t centre) {
  const int stage = stage_;
  stage_ = (stage_ + 1) % kStagesPerHop;
  if (stage == 0) {
    for (int i = 0; i <= kOrder; ++i) {
      k_inverse_from_[i] = k_inverse_to_[i];
      k_warped_from_[i] = k_warped_to_[i];
      k_inverse_to_[i] = pending_inverse_[i];
      k_warped_to_[i] = pending_warped_[i];
    }
    voiced_ = pending_voiced_;
    period_ = pending_period_;
    interpolation_ = 0;
    // Pre-emphasized, windowed frame centred on the read position.
    float previous = buffer_.Read(centre - kWindow / 2 - 1);
    for (int i = 0; i < kWindow; ++i) {
      const float x = buffer_.Read(centre - kWindow / 2 + uint32_t(i));
      frame_[i] = (x - kPreEmphasis * previous) * window_[i];
      previous = x;
    }
    frame_centre_ = centre;
    lambda_ = p.submode == LPC_SUBMODE_CHIP ? 0.0f : (p.size - 0.5f) * 0.4f;
    if (fabsf(lambda_) < 0.01f) lambda_ = 0.0f;
    ++analyses_;
  } else if (stage <= 5) {
    for (int k = (stage - 1) * 5; k < stage * 5 && k <= kOrder; ++k) {
      float acc = 0.0f;
      for (int i = k; i < kWindow; ++i) acc += frame_[i] * frame_[i - k];
      r_plain_[k] = acc;
    }
  } else if (stage == 6) {
    float a[kOrder + 1];
    r_plain_[0] += 1e-9f;
    ConditionAutocorrelation(r_plain_, kOrder, 60.0f, sample_rate_);
    Levinson(r_plain_, kOrder, pending_inverse_, a);
  } else if (stage <= 6 + kOrder) {
    if (lambda_ == 0.0f) return;
    if (stage == 7) {
      float s0 = 0.0f;
      for (int i = 0; i < kWindow; ++i) {
        scratch_[i] = frame_[i];
        s0 += frame_[i] * frame_[i];
      }
      r_warped_[0] = s0;
    }
    const int first = stage - 6;
    for (int k = first; k < first + 1 && k <= kOrder; ++k) {
      float previous_in = 0.0f, previous_out = 0.0f, acc = 0.0f;
      for (int i = 0; i < kWindow; ++i) {
        const float x = scratch_[i];
        const float y = -lambda_ * x + previous_in + lambda_ * previous_out;
        previous_in = x;
        previous_out = y;
        scratch_[i] = y;
        acc += frame_[i] * y;
      }
      r_warped_[k] = acc;
    }
  } else if (stage == 7 + kOrder) {
    if (lambda_ != 0.0f) {
      float a[kOrder + 1];
      r_warped_[0] += 1e-9f;
      ConditionAutocorrelation(r_warped_, kOrder, 60.0f, sample_rate_);
      Levinson(r_warped_, kOrder, pending_warped_, a);
    } else {
      for (int i = 0; i <= kOrder; ++i) pending_warped_[i] = pending_inverse_[i];
    }
    LookupVoice(frame_centre_, &pending_voiced_, &pending_period_);
  }
}

float LpcEngine::ReadSource() {
  // Two 30 ms Hann grains at 50% overlap, each started at the read head:
  // at 1x speed they tile the source exactly (transparent).
  if (--reader_countdown_ <= 0) {
    reader_countdown_ = kGrain / 2;
    ReaderGrain& g = reader_[reader_next_];
    g.start = uint32_t(int64_t(floor(read_)));
    g.elapsed = 0;
    g.active = true;
    reader_next_ ^= 1;
  }
  float s = 0.0f;
  for (int i = 0; i < 2; ++i) {
    ReaderGrain& g = reader_[i];
    if (!g.active) continue;
    s += hann_[g.elapsed] * buffer_.Read(g.start + uint32_t(g.elapsed));
    if (++g.elapsed >= kGrain) g.active = false;
  }
  return s;
}

void LpcEngine::Process(
    const LpcParams& p,
    const FloatFrame* in,
    FloatFrame* out,
    bool* gate,
    size_t size) {
  float l[kMaxHalfRateBlock], r[kMaxHalfRateBlock];
  float wet[kMaxHalfRateBlock] = { 0.0f };
  float source[kMaxHalfRateBlock] = { 0.0f };
  float decimated[kMaxHalfRateBlock];
  bridge_.Down(in, l, r, size);
  const size_t n = size / 2;

  // Recording and pitch tracking.
  const bool record = !p.freeze;
  if (record != !was_frozen_) offset_ = 0.0;
  was_frozen_ = p.freeze;
  if (record) {
    for (size_t i = 0; i < n; ++i) buffer_.Write(l[i]);
    decimator_.Process(l, decimated, n);
    if (yin_.Process(decimated, n / 2)) {
      VoiceTrackEntry& e = track_[track_write_ & (kTrackSize - 1)];
      e.position = buffer_.head() - uint32_t(2 * yin_.latency()) - 24;
      e.period = uint16_t(std::min(16383.0f, 2.0f * yin_.period() * 4.0f));
      e.aperiodicity = uint8_t(std::min(255.0f, yin_.aperiodicity() * 255.0f));
      ++track_write_;
      if (track_size_ < kTrackSize) ++track_size_;
    }
    // Forget pitch entries that left the buffer.
    while (track_size_ > 0 && !buffer_.Contains(
        track_[(track_write_ - track_size_) & (kTrackSize - 1)].position)) {
      --track_size_;
    }
  }

  // Read speed (as vox_psola): noon = hold, CW 0..+2 (flat zone at 1x).
  float speed = 0.0f;
  if (p.density > 0.53f) speed = (p.density - 0.53f) / 0.47f * 2.0f;
  else if (p.density < 0.47f) speed = -(0.47f - p.density) / 0.47f * 2.0f;
  if (fabsf(speed - 1.0f) < 0.06f) speed = 1.0f;
  if (p.hold && record) speed = 0.0f;
  if (p.trigger && !record) offset_ = 0.0;
  if (p.latch && !latched_) {
    latched_ = true;
    latch_position_ = read_;
  } else if (!p.latch) {
    latched_ = false;
  }

  const double newest = double(buffer_.head()) - kLiveDelay;
  const double oldest = double(buffer_.oldest()) + kMargin;
  const double span = newest - oldest;
  const bool ready = buffer_.available() > uint32_t(kLiveDelay + 2 * kMargin);

  // Excitation controls.
  const float ratio = stmlib::SemitonesToRatio(p.pitch);
  const float fixed_period = sample_rate_ / (FastExp2(kLog2C3) * ratio);
  float slew = 1.0f;
  if (p.slew > 0.02f) {
    const float tau = 0.5f * p.slew * p.slew;  // up to 500 ms
    slew = 1.0f - FastExp2(-1.44269504f / (tau * sample_rate_));
  }
  const float voicing_step = 1.0f / (0.01f * sample_rate_);

  // One analysis stage per block (at the read head).
  if (ready) AnalysisStage(p, uint32_t(int64_t(floor(read_))));

  for (size_t i = 0; i < n; ++i) {
    // Read head.
    if (ready && span > 0.0) {
      const double base = record ? newest - double(p.time) * span
                                 : oldest + double(p.time) * span;
      offset_ += double(speed) - (record ? 1.0 : 0.0);
      double position = base + offset_;
      if (latched_) {
        position = latch_position_;
        offset_ = position - base;
      }
      if (position > newest) { offset_ -= position - newest; position = newest; }
      if (position < oldest) { offset_ += oldest - position; position = oldest; }
      read_ = position;
    }
    if (!ready) {
      wet[i] = 0.0f;
      continue;
    }

    const float t = float(++interpolation_) / float(kHop);
    const float tt = t > 1.0f ? 1.0f : t;
    float k_warped[kOrder + 1];
    for (int k = 1; k <= kOrder; ++k) {
      k_inverse_[k] = k_inverse_from_[k] + (k_inverse_to_[k] - k_inverse_from_[k]) * tt;
      k_warped[k] = k_warped_from_[k] + (k_warped_to_[k] - k_warped_from_[k]) * tt;
      k_synth_[k] += slew * (k_warped[k] - k_synth_[k]);
    }

    const float s = ReadSource();
    source[i] = s;
    const float pre = s - kPreEmphasis * pre_previous_;
    pre_previous_ = s;
    const float residual = inverse_.Process(pre, k_inverse_, kOrder);
    residual_rms_ += 0.004f * (residual * residual - residual_rms_);
    const float level = sqrtf(residual_rms_);

    // Pitch of the excitation.
    smoothed_period_ += (p.smooth_f0 ? 0.0002f : 1.0f) * (period_ - smoothed_period_);
    const float follow_period = smoothed_period_ / ratio;
    voicing_mix_ += voiced_ ? std::min(voicing_step, 1.0f - voicing_mix_)
                            : -std::min(voicing_step, voicing_mix_);

    float excitation = residual;
    if (p.submode != LPC_SUBMODE_CHIP) {
      const float sh = p.shape;
      if (p.submode == LPC_SUBMODE_CROSS) {
        const float carrier = p.carrier_present ? whitener_.Process(r[i]) * level : 0.0f;
        excitation = residual + (carrier - residual) * sh;
        f0_out_ = 0.0f;
      } else {
        // Pulse period: follow (detected x PITCH) <-> fixed (PITCH note).
        float fixed_amount = 0.0f;
        float residual_amount = 0.0f;
        float auto_amount = 0.0f;   // pulse+noise by voicing
        float noise_amount = 0.0f;
        float pulse_amount = 0.0f;
        if (p.submode == LPC_SUBMODE_SYNTH) {
          if (sh < 0.333f) {
            const float x = sh / 0.333f;
            noise_amount = 1.0f - x; auto_amount = x;
          } else if (sh < 0.666f) {
            const float x = (sh - 0.333f) / 0.333f;
            auto_amount = 1.0f - x; pulse_amount = x;
          } else {
            pulse_amount = 1.0f;
            fixed_amount = (sh - 0.666f) / 0.334f;
          }
        } else {
          if (sh < 0.25f) {
            const float x = sh / 0.25f;
            noise_amount = 1.0f - x; auto_amount = x;
          } else if (sh < 0.5f) {
            const float x = (sh - 0.25f) / 0.25f;
            auto_amount = 1.0f - x; residual_amount = x;
          } else if (sh < 0.75f) {
            const float x = (sh - 0.5f) / 0.25f;
            residual_amount = 1.0f - x; pulse_amount = x;
          } else {
            pulse_amount = 1.0f;
            fixed_amount = (sh - 0.75f) / 0.25f;
          }
        }
        // Log-domain blend of the two pulse periods.
        const float period = FastExp2(
            FastLog2(follow_period) * (1.0f - fixed_amount) + \
            FastLog2(fixed_period) * fixed_amount);
        const float pulse = pulse_.Process(std::max(period, 8.0f)) * level;
        const float noise = noise_.Process() * level;
        const float voiced_mix = pulse * voicing_mix_ + noise * (1.0f - voicing_mix_);
        excitation = residual * residual_amount + noise * noise_amount + \
            voiced_mix * auto_amount + pulse * pulse_amount;
        f0_out_ = (pulse_amount + auto_amount * voicing_mix_) > 0.5f
            ? sample_rate_ / period : 0.0f;
      }
    }

    const float y = synthesis_.Process(excitation, k_synth_, kOrder);
    const float de = y + kPreEmphasis * de_previous_;
    de_previous_ = de;
    wet[i] = de;
  }

  if (p.submode == LPC_SUBMODE_CHIP && ready) {
    // The chip analyses the source at the read head itself.
    float st = (p.size - 0.5f) * 24.0f;
    if (fabsf(st) < 0.5f) st = 0.0f;
    chip_.Process(source, n, voiced_, period_ / ratio, stmlib::SemitonesToRatio(st), wet);
  }

  bridge_.Up(wet, wet, out, n);
  output_.Process(in, out, p.dry_wet, p.reverb, size);
  for (size_t i = 0; i < size; ++i) gate[i] = voiced_;
}

}  // namespace vox
