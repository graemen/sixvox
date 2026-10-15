// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Mono feedback delay (10 ms - 1.2 s at 24 kHz) with a 300 Hz high-pass and
// a soft clipper in the loop. The delay time glides (50 ms) to avoid zipper
// noise when TIME moves.

#ifndef VOX_SPLIT_DSP_CONSONANT_DELAY_H_
#define VOX_SPLIT_DSP_CONSONANT_DELAY_H_

#include <algorithm>

#include "stmlib/stmlib.h"
#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/filter.h"

namespace vox {

class ConsonantDelay {
 public:
  ConsonantDelay() { }
  ~ConsonantDelay() { }

  enum { kMaxDelay = 28800 };  // 1.2 s at 24 kHz

  void Init(float* buffer, float sample_rate) {
    buffer_ = buffer;
    std::fill(&buffer_[0], &buffer_[kMaxDelay], 0.0f);
    write_ = 0;
    delay_ = 0.25f * sample_rate;
    glide_ = 1.0f / (0.05f * sample_rate);
    hp_.Init();
    hp_.set_f_q<stmlib::FREQUENCY_ACCURATE>(300.0f / sample_rate, 0.7071f);
  }

  // delay in samples, feedback 0..1.05.
  inline float Process(float in, float delay, float feedback) {
    CONSTRAIN(delay, 2.0f, float(kMaxDelay - 4));
    delay_ += glide_ * (delay - delay_);
    const float read = float(write_) - delay_;
    int32_t ri = int32_t(read < 0.0f ? read - 1.0f : read);
    const float f = read - float(ri);
    ri = ((ri % kMaxDelay) + kMaxDelay) % kMaxDelay;
    const int32_t rj = (ri + 1) % kMaxDelay;
    const float y = buffer_[ri] + (buffer_[rj] - buffer_[ri]) * f;
    const float loop = hp_.Process<stmlib::FILTER_MODE_HIGH_PASS>(y);
    buffer_[write_] = stmlib::SoftLimit(in + feedback * loop);
    write_ = (write_ + 1) % kMaxDelay;
    return y;
  }

 private:
  float* buffer_;
  int32_t write_;
  float delay_;
  float glide_;
  stmlib::Svf hp_;

  DISALLOW_COPY_AND_ASSIGN(ConsonantDelay);
};

}  // namespace vox

#endif  // VOX_SPLIT_DSP_CONSONANT_DELAY_H_
