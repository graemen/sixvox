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
// Output peak limiter.

#ifndef VOX_COMMON_DSP_PEAK_LIMITER_H_
#define VOX_COMMON_DSP_PEAK_LIMITER_H_

#include <algorithm>
#include <cmath>

#include "vox/common/dsp/fast_math.h"

namespace vox {

// Stereo-linked output peak limiter: instant attack, 60 ms exponential
// release in the dB domain. No look-ahead (keeps latency at zero); the
// platform's final clipper catches the occasional overshoot.
class PeakLimiter {
 public:
  void Init(float sample_rate, float ceiling) {
    release_ = OnePoleCoefficient(0.060f, sample_rate);
    ceiling_db_ = GainToDb(ceiling);
    gain_db_ = 0.0f;
  }

  inline void Process(float* l, float* r) {
    const float peak = std::max(fabsf(*l), fabsf(*r));
    const float target = std::min(0.0f, ceiling_db_ - GainToDb(peak + 1e-9f));
    if (target < gain_db_) {
      gain_db_ = target;
    } else {
      gain_db_ += release_ * (target - gain_db_);
    }
    const float g = DbToGain(gain_db_);
    *l *= g;
    *r *= g;
  }

  inline float gain_db() const { return gain_db_; }

 private:
  float release_;
  float ceiling_db_;
  float gain_db_;
};

}  // namespace vox

#endif  // VOX_COMMON_DSP_PEAK_LIMITER_H_
