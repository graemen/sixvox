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
// Voice activity detector.

#ifndef VOX_COMMON_ANALYSIS_VAD_H_
#define VOX_COMMON_ANALYSIS_VAD_H_

#include <algorithm>

#include "stmlib/stmlib.h"

#include "vox/common/dsp/envelope.h"
#include "vox/common/dsp/fast_math.h"

namespace vox {

// Energy-over-noise-floor voice activity detector with hangover.
class Vad {
 public:
  // release: envelope release time. 50 ms suits ducking/gating; speech
  // segmentation uses a faster release to see short pauses.
  void Init(float sample_rate, float release = 0.050f) {
    envelope_.Init(sample_rate, 0.005f, release);
    floor_db_ = -60.0f;
    floor_up_ = 3.0f / sample_rate;     // floor rises at most 3 dB/s
    floor_down_ = OnePoleCoefficient(0.1f, sample_rate);
    hangover_samples_ = int32_t(0.150f * sample_rate);
    hangover_ = 0;
    active_ = false;
    speech_ = false;
  }

  inline bool Process(float x) {
    const float level_db = GainToDb(envelope_.Process(x));
    if (level_db < floor_db_) {
      floor_db_ += floor_down_ * (level_db - floor_db_);
    } else {
      floor_db_ += floor_up_;
    }
    CONSTRAIN(floor_db_, -90.0f, -20.0f);
    const bool speech = level_db > std::max(floor_db_ + 9.0f, -60.0f);
    if (speech) {
      hangover_ = hangover_samples_;
    } else if (hangover_ > 0) {
      --hangover_;
    }
    speech_ = speech;
    active_ = speech || hangover_ > 0;
    return active_;
  }

  inline bool active() const { return active_; }
  // Raw decision, without hangover.
  inline bool speech() const { return speech_; }
  inline float floor_db() const { return floor_db_; }

 private:
  EnvelopeFollower envelope_;
  float floor_db_;
  float floor_up_;
  float floor_down_;
  int32_t hangover_samples_;
  int32_t hangover_;
  bool active_;
  bool speech_;
};

}  // namespace vox

#endif  // VOX_COMMON_ANALYSIS_VAD_H_
