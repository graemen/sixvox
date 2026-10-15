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
// Tone shaping: RBJ biquads and the body <-> presence/air tilt EQ.

#ifndef VOX_CHAIN_DSP_TONE_H_
#define VOX_CHAIN_DSP_TONE_H_

#include <cmath>

#include "stmlib/stmlib.h"

#include "vox/common/dsp/fast_math.h"

namespace vox {

// Transposed direct form II biquad whose center frequency is fixed at Init
// time, so that gain changes only cost a sqrt and a FastExp2.
class FixedFrequencyBiquad {
 public:
  enum Type {
    LOW_SHELF,
    HIGH_SHELF,
    PEAK
  };

  void Init(Type type, float frequency, float q, float sample_rate) {
    type_ = type;
    const float w0 = 2.0f * float(M_PI) * frequency / sample_rate;
    cos_w0_ = cosf(w0);
    const float sin_w0 = sinf(w0);
    // Shelves use S = 1 (alpha = sin/2 * sqrt(2)); the peak uses Q.
    alpha_ = type == PEAK ? sin_w0 / (2.0f * q) : sin_w0 * 0.70710678f;
    s1_ = s2_ = 0.0f;
    gain_db_ = 1000.0f;  // Forces the first computation.
    set_gain_db(0.0f);
  }

  void set_gain_db(float gain_db) {
    if (fabsf(gain_db - gain_db_) < 0.01f) {
      return;
    }
    gain_db_ = gain_db;
    if (gain_db == 0.0f) {
      // Exact pass-through.
      b0_ = 1.0f;
      b1_ = b2_ = a1_ = a2_ = 0.0f;
      return;
    }
    const float A = DbToGain(gain_db * 0.5f);  // 10^(dB/40)
    const float c = cos_w0_;
    float b0, b1, b2, a0, a1, a2;
    if (type_ == PEAK) {
      b0 = 1.0f + alpha_ * A;
      b1 = -2.0f * c;
      b2 = 1.0f - alpha_ * A;
      a0 = 1.0f + alpha_ / A;
      a1 = -2.0f * c;
      a2 = 1.0f - alpha_ / A;
    } else {
      const float k = 2.0f * sqrtf(A) * alpha_;
      if (type_ == LOW_SHELF) {
        b0 = A * ((A + 1.0f) - (A - 1.0f) * c + k);
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * c);
        b2 = A * ((A + 1.0f) - (A - 1.0f) * c - k);
        a0 = (A + 1.0f) + (A - 1.0f) * c + k;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * c);
        a2 = (A + 1.0f) + (A - 1.0f) * c - k;
      } else {
        b0 = A * ((A + 1.0f) + (A - 1.0f) * c + k);
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * c);
        b2 = A * ((A + 1.0f) + (A - 1.0f) * c - k);
        a0 = (A + 1.0f) - (A - 1.0f) * c + k;
        a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * c);
        a2 = (A + 1.0f) - (A - 1.0f) * c - k;
      }
    }
    const float inv_a0 = 1.0f / a0;
    b0_ = b0 * inv_a0;
    b1_ = b1 * inv_a0;
    b2_ = b2 * inv_a0;
    a1_ = a1 * inv_a0;
    a2_ = a2 * inv_a0;
  }

  inline float Process(float x) {
    const float y = b0_ * x + s1_;
    s1_ = b1_ * x - a1_ * y + s2_;
    s2_ = b2_ * x - a2_ * y;
    return y;
  }

 private:
  Type type_;
  float cos_w0_;
  float alpha_;
  float gain_db_;
  float b0_, b1_, b2_, a1_, a2_;
  float s1_, s2_;
};

// Body <-> presence/air tilt. tilt in [-1, 1]:
//  -1: +6 dB low shelf (150 Hz), -3 dB presence (3 kHz), -2 dB air (10 kHz)
//  +1: -2 dB low shelf, +5 dB presence, +6 dB air
class TiltEq {
 public:
  void Init(float sample_rate) {
    low_.Init(FixedFrequencyBiquad::LOW_SHELF, 150.0f, 0.7071f, sample_rate);
    presence_.Init(FixedFrequencyBiquad::PEAK, 3000.0f, 1.0f, sample_rate);
    air_.Init(FixedFrequencyBiquad::HIGH_SHELF, 10000.0f, 0.7071f, sample_rate);
    tilt_ = 0.0f;
    set_tilt(0.0f);
  }

  // Call once per block. The tilt is smoothed to avoid zipper noise.
  void set_tilt(float tilt) {
    tilt_ += 0.05f * (tilt - tilt_);
    const float t = tilt_;
    low_.set_gain_db(t < 0.0f ? -6.0f * t : -2.0f * t);
    presence_.set_gain_db(t < 0.0f ? 3.0f * t : 5.0f * t);
    air_.set_gain_db(t < 0.0f ? 2.0f * t : 6.0f * t);
  }

  inline float Process(float x) {
    return air_.Process(presence_.Process(low_.Process(x)));
  }

 private:
  FixedFrequencyBiquad low_;
  FixedFrequencyBiquad presence_;
  FixedFrequencyBiquad air_;
  float tilt_;
};

}  // namespace vox

#endif  // VOX_CHAIN_DSP_TONE_H_
