// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Five-band log energy analyser (24 kHz input, 1 kHz feature rate).
//
// Bands (state variable band-pass, Q ~ 1): 80-300 Hz, 300-900 Hz,
// 900-2500 Hz, 2.5-5 kHz, 5-10 kHz. Each band's power is smoothed with a
// 5 ms one-pole and reported in dB every 24 input samples.

#ifndef VOX_COMMON_ANALYSIS_BAND_ENERGY_H_
#define VOX_COMMON_ANALYSIS_BAND_ENERGY_H_

#include "stmlib/stmlib.h"
#include "stmlib/dsp/filter.h"

#include "vox/common/dsp/fast_math.h"

namespace vox {

const int kNumBands = 5;
const float kFeatureRate = 1000.0f;

class BandEnergy {
 public:
  void Init(float sample_rate) {
    const float centers[kNumBands] = { 155.0f, 520.0f, 1500.0f, 3550.0f, 7070.0f };
    const float q[kNumBands] = { 0.8f, 0.9f, 0.9f, 1.4f, 1.4f };
    for (int b = 0; b < kNumBands; ++b) {
      filter_[b].Init();
      filter_[b].set_f_q<stmlib::FREQUENCY_ACCURATE>(
          centers[b] / sample_rate, q[b]);
      power_[b] = 0.0f;
      db_[b] = -120.0f;
    }
    power_total_ = 0.0f;
    db_total_ = -120.0f;
    zero_crossings_ = 0;
    zcr_ = 0.0f;
    previous_ = 0.0f;
    coefficient_ = OnePoleCoefficient(0.005f, sample_rate);
    decimation_ = int(sample_rate / kFeatureRate);
    counter_ = 0;
  }

  // Returns true when a new feature frame is available.
  inline bool Process(float x) {
    for (int b = 0; b < kNumBands; ++b) {
      const float y = filter_[b].Process<stmlib::FILTER_MODE_BAND_PASS_NORMALIZED>(x);
      power_[b] += coefficient_ * (y * y - power_[b]);
    }
    power_total_ += coefficient_ * (x * x - power_total_);
    if ((x >= 0.0f) != (previous_ >= 0.0f)) ++zero_crossings_;
    previous_ = x;
    if (++counter_ >= decimation_) {
      counter_ = 0;
      for (int b = 0; b < kNumBands; ++b) {
        db_[b] = 3.0103f * FastLog2(power_[b] + 1e-12f);  // 10 log10
      }
      db_total_ = 3.0103f * FastLog2(power_total_ + 1e-12f);
      zcr_ = float(zero_crossings_) / float(decimation_);
      zero_crossings_ = 0;
      return true;
    }
    return false;
  }

  inline const float* db() const { return db_; }
  inline float db(int band) const { return db_[band]; }
  inline float total_db() const { return db_total_; }
  // Zero crossings per sample over the last feature frame.
  inline float zcr() const { return zcr_; }
  inline int decimation() const { return decimation_; }

 private:
  stmlib::Svf filter_[kNumBands];
  float power_[kNumBands];
  float db_[kNumBands];
  float power_total_;
  float db_total_;
  int zero_crossings_;
  float zcr_;
  float previous_;
  float coefficient_;
  int decimation_;
  int counter_;
};

}  // namespace vox

#endif  // VOX_COMMON_ANALYSIS_BAND_ENERGY_H_
