// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Linear prediction: (frequency-warped) autocorrelation, Levinson-Durbin,
// and lattice inverse / synthesis filters.
//
// Frequency warping (Härmä et al.): replacing the unit delays of the
// autocorrelation by first-order all-passes D(z) = (z^-1 - lambda) /
// (1 - lambda z^-1) gives a predictor whose spectral envelope is warped.
// Using warped analysis with ordinary (unwarped) synthesis moves the
// formants: lambda > 0 moves them down, lambda < 0 up, while the excitation
// (the pitch) is untouched. The synthesis lattice stays stable since the
// reflection coefficients still come from a valid autocorrelation.

#ifndef VOX_COMMON_ANALYSIS_LPC_H_
#define VOX_COMMON_ANALYSIS_LPC_H_

#include <algorithm>
#include <cmath>

#include "stmlib/stmlib.h"

namespace vox {

const int kMaxLpcOrder = 24;

// r[0..order] of x[0..n), plain autocorrelation.
inline void Autocorrelation(const float* x, int n, int order, float* r) {
  for (int k = 0; k <= order; ++k) {
    float s = 0.0f;
    for (int i = k; i < n; ++i) s += x[i] * x[i - k];
    r[k] = s;
  }
}

// Warped autocorrelation: r[k] = sum x[i] * y_k[i], y_k = D^k(x).
// scratch: n floats.
inline void WarpedAutocorrelation(const float* x, int n, int order, float lambda,
                                  float* r, float* scratch) {
  for (int i = 0; i < n; ++i) scratch[i] = x[i];
  float s0 = 0.0f;
  for (int i = 0; i < n; ++i) s0 += x[i] * x[i];
  r[0] = s0;
  for (int k = 1; k <= order; ++k) {
    // y_k[i] = -lambda * y_{k-1}[i] + y_{k-1}[i-1] + lambda * y_k[i-1]
    float previous_in = 0.0f, previous_out = 0.0f, s = 0.0f;
    for (int i = 0; i < n; ++i) {
      const float in = scratch[i];
      const float out = -lambda * in + previous_in + lambda * previous_out;
      previous_in = in;
      previous_out = out;
      scratch[i] = out;
      s += x[i] * out;
    }
    r[k] = s;
  }
}

// Levinson-Durbin. Returns the prediction error; k[1..order] reflection
// coefficients (k[0] unused), a[0..order] predictor (a[0] = 1).
inline float Levinson(const float* r, int order, float* k, float* a) {
  float tmp[kMaxLpcOrder + 1];
  float error = r[0];
  a[0] = 1.0f;
  for (int i = 1; i <= order; ++i) a[i] = 0.0f;
  k[0] = 0.0f;
  if (error <= 0.0f) {
    for (int i = 1; i <= order; ++i) k[i] = 0.0f;
    return 0.0f;
  }
  for (int i = 1; i <= order; ++i) {
    float acc = r[i];
    for (int j = 1; j < i; ++j) acc += a[j] * r[i - j];
    float ki = -acc / error;
    CONSTRAIN(ki, -0.995f, 0.995f);
    k[i] = ki;
    for (int j = 0; j < i; ++j) tmp[j] = a[j];
    for (int j = 1; j < i; ++j) a[j] = tmp[j] + ki * tmp[i - j];
    a[i] = ki;
    error *= 1.0f - ki * ki;
    if (error <= r[0] * 1e-9f) {
      for (int j = i + 1; j <= order; ++j) k[j] = 0.0f;
      break;
    }
  }
  return error;
}

// Lag window (Gaussian, bandwidth in Hz) and white noise correction.
inline void ConditionAutocorrelation(float* r, int order, float bandwidth,
                                     float sample_rate) {
  r[0] *= 1.0001f;
  const float a = 2.0f * float(M_PI) * bandwidth / sample_rate;
  for (int i = 1; i <= order; ++i) {
    const float x = a * float(i);
    r[i] *= expf(-0.5f * x * x);
  }
}

// Lattice inverse filter (FIR): x -> residual e.
class LatticeAnalysis {
 public:
  void Init() { std::fill(&b_[0], &b_[kMaxLpcOrder + 1], 0.0f); }
  inline float Process(float x, const float* k, int order) {
    float f = x;
    float b_previous = x;  // b_0[n]
    for (int i = 1; i <= order; ++i) {
      const float b_delayed = b_[i - 1];  // b_{i-1}[n-1]
      const float f_new = f + k[i] * b_delayed;
      const float b_new = b_delayed + k[i] * f;
      b_[i - 1] = b_previous;
      b_previous = b_new;
      f = f_new;
    }
    b_[order] = b_previous;
    return f;
  }
 private:
  float b_[kMaxLpcOrder + 1];
};

// Lattice synthesis filter (all-pole): e -> x. Exact inverse of
// LatticeAnalysis for identical coefficients.
class LatticeSynthesis {
 public:
  void Init() { std::fill(&b_[0], &b_[kMaxLpcOrder + 1], 0.0f); }
  inline float Process(float e, const float* k, int order) {
    float f = e;
    for (int i = order; i >= 1; --i) {
      f -= k[i] * b_[i - 1];
      b_[i] = b_[i - 1] + k[i] * f;
    }
    b_[0] = f;
    return f;
  }
 private:
  float b_[kMaxLpcOrder + 1];
};

}  // namespace vox

#endif  // VOX_COMMON_ANALYSIS_LPC_H_
