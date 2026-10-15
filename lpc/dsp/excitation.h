// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Excitation sources for the LPC synthesis: band-limited pulse train
// (BLIT, windowed-sinc impulses at fractional positions), white noise, and a
// carrier pre-whitener (order-8 LPC inverse filter) for cross-synthesis.
// Pulse and noise are normalized to unit RMS.

#ifndef VOX_LPC_DSP_EXCITATION_H_
#define VOX_LPC_DSP_EXCITATION_H_

#include <stdint.h>

#include <algorithm>
#include <cmath>

#include "stmlib/stmlib.h"

#include "vox/common/analysis/lpc.h"

namespace vox {

class BlitPulse {
 public:
  enum { kTaps = 16, kPhases = 32, kRing = 64 };

  void Init() {
    // Windowed sinc, cutoff 0.45 fs, Blackman window.
    for (int p = 0; p < kPhases; ++p) {
      const float frac = float(p) / float(kPhases);
      float sum = 0.0f;
      for (int t = 0; t < kTaps; ++t) {
        const float x = float(t) - float(kTaps / 2) + 1.0f - frac;
        const float sinc = fabsf(x) < 1e-6f ? 1.0f
            : sinf(float(M_PI) * 0.9f * x) / (float(M_PI) * 0.9f * x);
        const float w_pos = (float(t) + 1.0f - frac) / float(kTaps);
        const float window = 0.42f - 0.5f * cosf(2.0f * float(M_PI) * w_pos) + \
            0.08f * cosf(4.0f * float(M_PI) * w_pos);
        kernel_[p][t] = sinc * window;
        sum += kernel_[p][t];
      }
      for (int t = 0; t < kTaps; ++t) kernel_[p][t] /= sum;
    }
    std::fill(&ring_[0], &ring_[kRing], 0.0f);
    index_ = 0;
    phase_ = 0.0f;
    dc_ = 0.0f;
  }

  // period in samples (>= 2). Returns one sample.
  inline float Process(float period) {
    phase_ += 1.0f;
    if (phase_ >= period) {
      phase_ -= period;
      // Impulse at a fractional position `phase_` samples in the past.
      const float amplitude = sqrtf(period);
      float frac = phase_;
      int whole = int(frac);
      frac -= float(whole);
      const int p = int(frac * float(kPhases)) % kPhases;
      for (int t = 0; t < kTaps; ++t) {
        ring_[(index_ + t + 1 - whole) & (kRing - 1)] += amplitude * kernel_[p][t];
      }
    }
    index_ = (index_ + 1) & (kRing - 1);
    float y = ring_[index_];
    ring_[index_] = 0.0f;
    // Remove the DC of the impulse train (keeps the excitation zero-mean).
    dc_ += 0.002f * (y - dc_);
    return y - dc_;
  }

  inline void Reset() { phase_ = 0.0f; }

 private:
  float kernel_[kPhases][kTaps];
  float ring_[kRing];
  int index_;
  float phase_;
  float dc_;
};

class WhiteNoise {
 public:
  void Init() { state_ = 0x13579bdf; }
  inline float Process() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    // Uniform [-1, 1) has RMS 1/sqrt(3).
    return (float(int32_t(state_)) / 2147483648.0f) * 1.7320508f;
  }
 private:
  uint32_t state_;
};

// Flattens the carrier's spectrum so that the speech envelope dominates in
// cross-synthesis. Order 8, 10 ms frames, coefficients interpolated.
class CarrierWhitener {
 public:
  enum { kOrder = 8, kFrame = 240 };

  void Init() {
    std::fill(&frame_[0], &frame_[kFrame], 0.0f);
    count_ = 0;
    for (int i = 0; i <= kOrder; ++i) k_[i] = target_[i] = 0.0f;
    filter_.Init();
    rms_ = 1e-3f;
  }

  inline float Process(float x) {
    frame_[count_] = x;
    if (++count_ >= kFrame) {
      count_ = 0;
      float w[kFrame], r[kOrder + 1], a[kOrder + 1];
      for (int i = 0; i < kFrame; ++i) {
        w[i] = frame_[i] * (0.54f - 0.46f * cosf(2.0f * float(M_PI) * i / (kFrame - 1)));
      }
      Autocorrelation(w, kFrame, kOrder, r);
      r[0] = r[0] * 1.001f + 1e-9f;
      Levinson(r, kOrder, target_, a);
    }
    for (int i = 1; i <= kOrder; ++i) k_[i] += 0.01f * (target_[i] - k_[i]);
    const float e = filter_.Process(x, k_, kOrder);
    rms_ += 0.002f * (e * e - rms_);
    return e / sqrtf(rms_ + 1e-9f);  // unit RMS
  }

 private:
  float frame_[kFrame];
  int count_;
  float k_[kOrder + 1];
  float target_[kOrder + 1];
  LatticeAnalysis filter_;
  float rms_;
};

}  // namespace vox

#endif  // VOX_LPC_DSP_EXCITATION_H_
