// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// 48 kHz <-> 24 kHz bridge. The codec always runs at 48 kHz (the PCM3060 is
// noisy at 24 kHz); speech processing runs at 24 kHz. Uses the same 48-tap
// polyphase filters as the stock Beads firmware.

#ifndef VOX_COMMON_DSP_HALF_RATE_H_
#define VOX_COMMON_DSP_HALF_RATE_H_

#include "stmlib/stmlib.h"
#include "stmlib/dsp/sample_rate_converter.h"

#include "beads/dsp/frame.h"
#include "beads/dsp/sample_rate_conversion_filters.h"

namespace vox {

const size_t kMaxHalfRateBlock = 32;

// Mono 2:1 decimator.
class Decimator2 {
 public:
  void Init() { src_.Init(); }
  // in: size samples, out: size / 2 samples. size must be even.
  inline void Process(const float* in, float* out, size_t size) {
    src_.Process(in, out, size);
  }
 private:
  stmlib::SampleRateConverter<stmlib::SRC_DOWN, 2, 48> src_;
};

// Mono 1:2 interpolator.
class Interpolator2 {
 public:
  void Init() { src_.Init(); }
  // in: size samples, out: 2 * size samples.
  inline void Process(const float* in, float* out, size_t size) {
    src_.Process(in, out, size);
  }
 private:
  stmlib::SampleRateConverter<stmlib::SRC_UP, 2, 48> src_;
};

// Stereo 48 kHz frames <-> two 24 kHz mono channels.
class HalfRateBridge {
 public:
  void Init() {
    for (int i = 0; i < 2; ++i) {
      down_[i].Init();
      up_[i].Init();
    }
  }

  // in: size frames at 48 kHz (size even, <= 2 * kMaxHalfRateBlock).
  // l, r: size / 2 samples at 24 kHz.
  void Down(const beads::FloatFrame* in, float* l, float* r, size_t size) {
    float x[2 * kMaxHalfRateBlock] = { 0.0f };
    for (size_t i = 0; i < size; ++i) x[i] = in[i].l;
    down_[0].Process(x, l, size);
    for (size_t i = 0; i < size; ++i) x[i] = in[i].r;
    down_[1].Process(x, r, size);
  }

  // l, r: size samples at 24 kHz. out: 2 * size frames at 48 kHz.
  void Up(const float* l, const float* r, beads::FloatFrame* out, size_t size) {
    float y[2 * kMaxHalfRateBlock] = { 0.0f };
    up_[0].Process(l, y, size);
    for (size_t i = 0; i < 2 * size; ++i) out[i].l = y[i];
    up_[1].Process(r, y, size);
    for (size_t i = 0; i < 2 * size; ++i) out[i].r = y[i];
  }

 private:
  Decimator2 down_[2];
  Interpolator2 up_[2];
};

}  // namespace vox

#endif  // VOX_COMMON_DSP_HALF_RATE_H_
