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
// Envelope follower.

#ifndef VOX_COMMON_DSP_ENVELOPE_H_
#define VOX_COMMON_DSP_ENVELOPE_H_

#include <cmath>

#include "vox/common/dsp/fast_math.h"

namespace vox {

// Peak envelope follower with separate attack / release (linear domain).
class EnvelopeFollower {
 public:
  void Init(float sample_rate, float attack_s, float release_s) {
    sample_rate_ = sample_rate;
    set_times(attack_s, release_s);
    value_ = 0.0f;
  }

  void set_times(float attack_s, float release_s) {
    attack_ = OnePoleCoefficient(attack_s, sample_rate_);
    release_ = OnePoleCoefficient(release_s, sample_rate_);
  }

  inline float Process(float x) {
    x = fabsf(x);
    value_ += (x > value_ ? attack_ : release_) * (x - value_);
    return value_;
  }

  inline float value() const { return value_; }

 private:
  float sample_rate_;
  float attack_;
  float release_;
  float value_;
};

}  // namespace vox

#endif  // VOX_COMMON_DSP_ENVELOPE_H_
