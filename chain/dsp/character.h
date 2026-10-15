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
// Voice "character" models, morphed by a single control in [-1, 1]:
//
//    -1 ........ 0 ........ 1/3 ........ 2/3 ........ 1
//   lo-fi     clean       radio      telephone    megaphone
//
// All models run continuously (they are cheap) so that their filter states
// are always warm and morphing between them never clicks.

#ifndef VOX_CHAIN_DSP_CHARACTER_H_
#define VOX_CHAIN_DSP_CHARACTER_H_

#include <cmath>

#include "stmlib/stmlib.h"
#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/filter.h"

#include "vox/common/dsp/fast_math.h"

namespace vox {

class Character {
 public:
  void Init(float sample_rate) {
    sample_rate_ = sample_rate;
    InitFilter(&radio_hp_, 200.0f, 0.7071f);
    InitFilter(&radio_lp_, 5000.0f, 0.7071f);
    InitFilter(&tel_hp_[0], 300.0f, 0.5412f);  // 4th order Butterworth
    InitFilter(&tel_hp_[1], 300.0f, 1.3066f);
    InitFilter(&tel_lp_[0], 3400.0f, 0.5412f);
    InitFilter(&tel_lp_[1], 3400.0f, 1.3066f);
    InitFilter(&mega_hp_, 500.0f, 0.7071f);
    InitFilter(&mega_lp_, 4000.0f, 1.2f);
    for (int i = 0; i < kCombSize; ++i) {
      comb_[i] = 0.0f;
    }
    comb_write_ = 0;
    comb_delay_ = int(sample_rate / 1200.0f);
    if (comb_delay_ >= kCombSize) comb_delay_ = kCombSize - 1;

    tel_hold_ = 0.0f;
    tel_phase_ = 1.0f;
    lofi_lp_.Init();
    lofi_hold_ = 0.0f;
    lofi_phase_ = 1.0f;
    amount_ = 0.0f;
    set_amount(0.0f);
  }

  // Call once per block. amount in [-1, 1].
  void set_amount(float amount) {
    CONSTRAIN(amount, -1.0f, 1.0f);
    amount_ += 0.05f * (amount - amount_);
    // Lo-fi: sample rate 24 kHz -> 4 kHz (exponential), 16 -> 6 bits.
    const float a = amount_ < 0.0f ? -amount_ : 0.0f;
    const float rate = 24000.0f * FastExp2(-a * 2.5849625f);  // /6 at a=1
    lofi_increment_ = rate / sample_rate_;
    lofi_lp_.set_f_q<stmlib::FREQUENCY_ACCURATE>(
        0.45f * rate / sample_rate_, 0.7071f);
    lofi_levels_ = FastExp2(15.0f - 10.0f * a);
  }

  inline float Process(float x) {
    // --- Positive side models (always running) ---
    // Radio: 200 Hz - 5 kHz band, gentle drive.
    float radio = radio_lp_.Process<stmlib::FILTER_MODE_LOW_PASS>(
        radio_hp_.Process<stmlib::FILTER_MODE_HIGH_PASS>(x));
    radio = 0.75f * stmlib::SoftClip(1.8f * radio);

    // Telephone: 300 - 3400 Hz (4th order), 8 kHz zero-order hold, 8-bit
    // mu-law companding.
    float tel = x;
    tel = tel_hp_[0].Process<stmlib::FILTER_MODE_HIGH_PASS>(tel);
    tel = tel_hp_[1].Process<stmlib::FILTER_MODE_HIGH_PASS>(tel);
    tel = tel_lp_[0].Process<stmlib::FILTER_MODE_LOW_PASS>(tel);
    tel = tel_lp_[1].Process<stmlib::FILTER_MODE_LOW_PASS>(tel);
    tel_phase_ += 8000.0f / sample_rate_;
    if (tel_phase_ >= 1.0f) {
      tel_phase_ -= 1.0f;
      tel_hold_ = MuLaw8(1.6f * tel);
    }
    tel = tel_hold_;

    // Megaphone: 500 Hz - 4 kHz with a resonant top, hard drive, short comb.
    float mega = mega_lp_.Process<stmlib::FILTER_MODE_LOW_PASS>(
        mega_hp_.Process<stmlib::FILTER_MODE_HIGH_PASS>(x));
    mega = stmlib::SoftClip(5.0f * mega);
    int read = comb_write_ - comb_delay_;
    if (read < 0) read += kCombSize;
    const float delayed = comb_[read];
    mega = mega + 0.45f * delayed;
    comb_[comb_write_] = mega;
    comb_write_ = (comb_write_ + 1) % kCombSize;
    mega *= 0.45f;

    // --- Lo-fi (always running) ---
    const float lofi_in = lofi_lp_.Process<stmlib::FILTER_MODE_LOW_PASS>(x);
    lofi_phase_ += lofi_increment_;
    if (lofi_phase_ >= 1.0f) {
      lofi_phase_ -= 1.0f;
      lofi_hold_ = floorf(lofi_in * lofi_levels_ + 0.5f) / lofi_levels_;
    }

    // --- Morph ---
    const float a = amount_;
    if (a < 0.0f) {
      return x + (lofi_hold_ - x) * (-a < 0.25f ? -a * 4.0f : 1.0f);
    } else {
      const float position = a * 3.0f;
      if (position < 1.0f) {
        return x + (radio - x) * position;
      } else if (position < 2.0f) {
        return radio + (tel - radio) * (position - 1.0f);
      } else {
        return tel + (mega - tel) * std::min(position - 2.0f, 1.0f);
      }
    }
  }

 private:
  static const int kCombSize = 64;

  void InitFilter(stmlib::Svf* f, float frequency, float q) {
    f->Init();
    f->set_f_q<stmlib::FREQUENCY_ACCURATE>(frequency / sample_rate_, q);
  }

  // 8-bit mu-law encode/decode round trip (mu = 255).
  static inline float MuLaw8(float x) {
    CONSTRAIN(x, -1.0f, 1.0f);
    const float sign = x < 0.0f ? -1.0f : 1.0f;
    const float ax = fabsf(x);
    // Compress: log2(1 + 255 |x|) / log2(256)
    float y = FastLog2(1.0f + 255.0f * ax) * 0.125f;
    y = floorf(y * 127.0f + 0.5f) * (1.0f / 127.0f);
    // Expand: (256^y - 1) / 255
    return sign * (FastExp2(y * 8.0f) - 1.0f) * (1.0f / 255.0f);
  }

  float sample_rate_;
  float amount_;

  stmlib::Svf radio_hp_;
  stmlib::Svf radio_lp_;

  stmlib::Svf tel_hp_[2];
  stmlib::Svf tel_lp_[2];
  float tel_hold_;
  float tel_phase_;

  stmlib::Svf mega_hp_;
  stmlib::Svf mega_lp_;
  float comb_[kCombSize];
  int comb_write_;
  int comb_delay_;

  stmlib::Svf lofi_lp_;
  float lofi_hold_;
  float lofi_phase_;
  float lofi_increment_;
  float lofi_levels_;
};

}  // namespace vox

#endif  // VOX_CHAIN_DSP_CHARACTER_H_
