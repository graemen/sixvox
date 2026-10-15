// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// YIN fundamental frequency estimator (de Cheveigné & Kawahara, 2002),
// designed for the audio interrupt: the difference function of each analysis
// frame is computed in slices over the following calls, so the cost per call
// is bounded (about 3.6k multiply-adds per 3-sample call at 12 kHz).
//
// Input: 12 kHz (the 24 kHz speech signal decimated by 2). Window 25 ms,
// hop 5 ms, search range set by f0_min / f0_max (default 50-600 Hz, wider
// than natural speech to follow Morphagene varispeed). Parabolic
// interpolation of the period. Each result describes the analysis window
// that ended `latency()` samples before the newest sample.

#ifndef VOX_COMMON_ANALYSIS_YIN_H_
#define VOX_COMMON_ANALYSIS_YIN_H_

#include <stdint.h>

#include <algorithm>
#include <cmath>

#include "stmlib/stmlib.h"

namespace vox {

class Yin {
 public:
  Yin() { }
  ~Yin() { }

  enum {
    kWindow = 300,      // 25 ms at 12 kHz
    kMaxLag = 256,      // 47 Hz at 12 kHz
    kHop = 60,          // default hop: 5 ms
    kMaxHop = 120,      // 10 ms
    kRing = 1024,
  };

  void Init(float sample_rate) {
    sample_rate_ = sample_rate;
    std::fill(&ring_[0], &ring_[kRing], 0.0f);
    write_ = 0;
    since_hop_ = 0;
    computing_ = false;
    next_lag_ = 0;
    frames_ = 0;
    f0_ = 0.0f;
    period_ = 0.0f;
    aperiodicity_ = 1.0f;
    rms_ = 0.0f;
    set_range(50.0f, 600.0f);
    set_threshold(0.15f);
    hop_ = kHop;
  }

  // Analysis hop in samples (60 = 5 ms, 120 = 10 ms at 12 kHz).
  void set_hop(int hop) { hop_ = hop < 30 ? 30 : (hop > kMaxHop ? kMaxHop : hop); }

  void set_range(float f0_min, float f0_max) {
    min_lag_ = int(sample_rate_ / f0_max);
    max_lag_ = int(sample_rate_ / f0_min);
    if (min_lag_ < 2) min_lag_ = 2;
    if (max_lag_ > kMaxLag - 2) max_lag_ = kMaxLag - 2;
  }

  void set_threshold(float t) { threshold_ = t; }

  // Feeds n samples. Returns true when a new estimate is available.
  bool Process(const float* x, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      ring_[write_] = x[i];
      write_ = (write_ + 1) & (kRing - 1);
    }
    since_hop_ += int(n);
    bool result = false;
    if (computing_) {
      // Spread the lags over the calls of one hop.
      const int calls_per_hop = hop_ / int(n > 0 ? n : 1);
      const int chunk = (max_lag_ + 1) / (calls_per_hop > 1 ? calls_per_hop - 1 : 1) + 1;
      result = ComputeLags(chunk);
    }
    if (since_hop_ >= hop_) {
      since_hop_ -= hop_;
      if (computing_) {
        // Finish the previous frame before starting a new one.
        result = ComputeLags(kMaxLag) || result;
      }
      Snapshot();
    }
    return result;
  }

  inline float f0() const { return f0_; }
  inline float period() const { return period_; }  // samples at 12 kHz
  inline float aperiodicity() const { return aperiodicity_; }
  inline float rms() const { return rms_; }
  inline uint32_t frames() const { return frames_; }
  // Delay between the newest input sample and the centre of the analysed
  // window when an estimate is published.
  inline int latency() const { return hop_ + (kWindow + max_lag_) / 2; }

 private:
  void Snapshot() {
    const int length = kWindow + max_lag_ + 1;
    int read = (write_ - length) & (kRing - 1);
    for (int i = 0; i < length; ++i) {
      frame_[i] = ring_[read];
      read = (read + 1) & (kRing - 1);
    }
    computing_ = true;
    next_lag_ = 1;
    float energy = 0.0f;
    for (int i = 0; i < kWindow; ++i) energy += frame_[i] * frame_[i];
    frame_rms_ = sqrtf(energy / float(kWindow));
    energy_0_ = energy;
    // Energy of the shifted window, for every lag (incremental).
    float e = energy;
    shifted_energy_[0] = e;
    for (int tau = 1; tau <= max_lag_ + 1; ++tau) {
      e += frame_[tau + kWindow - 1] * frame_[tau + kWindow - 1] - \
          frame_[tau - 1] * frame_[tau - 1];
      shifted_energy_[tau] = e;
    }
    difference_[0] = 0.0f;
  }

  bool ComputeLags(int count) {
    const int end = std::min(next_lag_ + count, max_lag_ + 2);
    for (int tau = next_lag_; tau < end; ++tau) {
      // d(tau) = sum (a - b)^2 = e(0) + e(tau) - 2 r(tau); one multiply-add
      // per sample, four independent accumulators.
      const float* a = frame_;
      const float* b = frame_ + tau;
      float r0 = 0.0f, r1 = 0.0f, r2 = 0.0f, r3 = 0.0f;
      for (int j = 0; j < kWindow; j += 4) {
        r0 += a[j] * b[j];
        r1 += a[j + 1] * b[j + 1];
        r2 += a[j + 2] * b[j + 2];
        r3 += a[j + 3] * b[j + 3];
      }
      const float d = energy_0_ + shifted_energy_[tau] - 2.0f * (r0 + r1 + r2 + r3);
      difference_[tau] = d > 0.0f ? d : 0.0f;
    }
    next_lag_ = end;
    if (next_lag_ >= max_lag_ + 2) {
      Finish();
      computing_ = false;
      return true;
    }
    return false;
  }

  void Finish() {
    // Cumulative mean normalized difference.
    float sum = 0.0f;
    float cmnd[kMaxLag + 2];
    cmnd[0] = 1.0f;
    for (int tau = 1; tau <= max_lag_ + 1; ++tau) {
      sum += difference_[tau];
      cmnd[tau] = sum > 0.0f ? difference_[tau] * float(tau) / sum : 1.0f;
    }
    // First dip below the threshold (then its local minimum), else the
    // global minimum.
    int best = -1;
    for (int tau = min_lag_; tau <= max_lag_; ++tau) {
      if (cmnd[tau] < threshold_) {
        while (tau + 1 <= max_lag_ && cmnd[tau + 1] < cmnd[tau]) ++tau;
        best = tau;
        break;
      }
    }
    if (best < 0) {
      best = min_lag_;
      for (int tau = min_lag_ + 1; tau <= max_lag_; ++tau) {
        if (cmnd[tau] < cmnd[best]) best = tau;
      }
    }
    // Parabolic interpolation.
    float refined = float(best);
    if (best > 1 && best < max_lag_ + 1) {
      const float a = cmnd[best - 1], b = cmnd[best], c = cmnd[best + 1];
      const float denominator = a - 2.0f * b + c;
      if (denominator > 1e-9f) {
        float delta = 0.5f * (a - c) / denominator;
        CONSTRAIN(delta, -0.5f, 0.5f);
        refined += delta;
      }
    }
    aperiodicity_ = cmnd[best];
    period_ = refined;
    f0_ = sample_rate_ / refined;
    rms_ = frame_rms_;
    ++frames_;
  }

  float sample_rate_;
  float ring_[kRing];
  int write_;
  int since_hop_;
  bool computing_;
  int next_lag_;
  int min_lag_;
  int max_lag_;
  float threshold_;
  float frame_[kWindow + kMaxLag + 2];
  float difference_[kMaxLag + 2];
  float shifted_energy_[kMaxLag + 2];
  float energy_0_;
  float frame_rms_;
  int hop_;

  uint32_t frames_;
  float f0_;
  float period_;
  float aperiodicity_;
  float rms_;

  DISALLOW_COPY_AND_ASSIGN(Yin);
};

}  // namespace vox

#endif  // VOX_COMMON_ANALYSIS_YIN_H_
