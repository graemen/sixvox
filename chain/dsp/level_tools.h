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
// Music-bed ducker (the voice activity detector is in vox/common/analysis).

#ifndef VOX_CHAIN_DSP_LEVEL_TOOLS_H_
#define VOX_CHAIN_DSP_LEVEL_TOOLS_H_

#include <algorithm>

#include "stmlib/stmlib.h"

#include "vox/chain/dsp/dynamics.h"
#include "vox/common/analysis/vad.h"
#include "vox/common/dsp/fast_math.h"

namespace vox {

// Computes the gain applied to the music bed from the processed voice.
// Full ducking is reached when the voice envelope is above -25 dBFS; the
// ducking starts at -45 dBFS. A 150 ms hold bridges the gaps between words.
class Ducker {
 public:
  void Init(float sample_rate) {
    key_.Init(sample_rate, 0.002f, 0.040f);
    duck_ = OnePoleCoefficient(0.008f, sample_rate);
    recover_ = OnePoleCoefficient(0.100f, sample_rate);
    hold_samples_ = int32_t(0.150f * sample_rate);
    hold_ = 0;
    held_amount_ = 0.0f;
    gain_db_ = 0.0f;
    depth_db_ = 12.0f;
  }

  void set_depth_db(float depth_db) { depth_db_ = depth_db; }

  inline float Process(float voice) {
    const float key_db = GainToDb(key_.Process(voice));
    float amount = (key_db + 45.0f) * (1.0f / 20.0f);
    CONSTRAIN(amount, 0.0f, 1.0f);
    if (amount >= held_amount_) {
      held_amount_ = amount;
      hold_ = hold_samples_;
    } else if (hold_ > 0) {
      --hold_;
    } else {
      held_amount_ = amount;
    }
    const float target = -depth_db_ * held_amount_;
    gain_db_ += (target < gain_db_ ? duck_ : recover_) * (target - gain_db_);
    return DbToGain(gain_db_);
  }

  inline float gain_db() const { return gain_db_; }

 private:
  EnvelopeFollower key_;
  float duck_;
  float recover_;
  int32_t hold_samples_;
  int32_t hold_;
  float held_amount_;
  float gain_db_;
  float depth_db_;
};

}  // namespace vox

#endif  // VOX_CHAIN_DSP_LEVEL_TOOLS_H_
