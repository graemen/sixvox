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
// Fast log2 / exp2 and dB conversions for per-sample dynamics processing.
// Accuracy (least-squares fits): log2 within 5e-6, exp2 within 2e-7 relative.

#ifndef VOX_COMMON_DSP_FAST_MATH_H_
#define VOX_COMMON_DSP_FAST_MATH_H_

#include <stdint.h>
#include <string.h>

namespace vox {

inline float FastLog2(float x) {
  // x must be > 0 and normal. Split into exponent and mantissa in [1, 2),
  // then use a 6th order polynomial for log2(m).
  uint32_t bits;
  memcpy(&bits, &x, 4);
  const float e = float(int32_t((bits >> 23) & 0xff) - 127);
  bits = (bits & 0x007fffff) | 0x3f800000;
  float m;
  memcpy(&m, &bits, 4);
  m -= 1.0f;
  const float p = m * (1.44251703f + m * (-0.71789838f + m * (0.45689417f + \
      m * (-0.27736505f + m * (0.12191414f + m * (-0.0260663f))))));
  return e + p;
}

inline float FastExp2(float x) {
  if (x < -126.0f) {
    return 0.0f;
  }
  if (x > 126.0f) {
    x = 126.0f;
  }
  const float xi = float(int32_t(x < 0.0f ? x - 1.0f : x));
  const float f = x - xi;  // [0, 1)
  // 5th order polynomial for 2^f on [0, 1).
  const float p = 1.0f + f * (0.69315275f + f * (0.24015319f + \
      f * (0.05582818f + f * (0.00898895f + f * 0.00187664f))));
  uint32_t bits;
  memcpy(&bits, &p, 4);
  bits += uint32_t(int32_t(xi)) << 23;
  float r;
  memcpy(&r, &bits, 4);
  return r;
}

// 20 * log10(x) = 6.0206 * log2(x)
const float kDbPerLog2 = 6.02059991f;
const float kLog2PerDb = 0.16609640f;

inline float GainToDb(float g) {
  return kDbPerLog2 * FastLog2(g > 1e-9f ? g : 1e-9f);
}

inline float DbToGain(float db) {
  return FastExp2(db * kLog2PerDb);
}

// One-pole smoothing coefficient for a time constant in seconds.
inline float OnePoleCoefficient(float time_constant, float sample_rate) {
  // 1 - exp(-1 / (tau * sr)), with exp computed by FastExp2.
  const float x = 1.0f / (time_constant * sample_rate);
  return 1.0f - FastExp2(-x * 1.44269504f);
}

}  // namespace vox

#endif  // VOX_COMMON_DSP_FAST_MATH_H_
