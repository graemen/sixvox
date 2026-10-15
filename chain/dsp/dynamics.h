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
// Dynamics processors for the voice chain: expander/gate, de-esser,
// compressor. All gain computations are done in the log (dB) domain.

#ifndef VOX_CHAIN_DSP_DYNAMICS_H_
#define VOX_CHAIN_DSP_DYNAMICS_H_

#include <algorithm>
#include <cmath>

#include "stmlib/stmlib.h"
#include "stmlib/dsp/filter.h"

#include "vox/chain/dsp/tone.h"
#include "vox/common/dsp/envelope.h"
#include "vox/common/dsp/peak_limiter.h"
#include "vox/common/dsp/fast_math.h"

namespace vox {

// Downward expander / noise gate with hold.
//
// Below threshold, the gain drops by (ratio - 1) dB per dB, down to -range.
// A ratio above ~20 behaves like a gate.
class Expander {
 public:
  void Init(float sample_rate) {
    sample_rate_ = sample_rate;
    detector_.Init(sample_rate, 0.001f, 0.100f);
    open_ = OnePoleCoefficient(0.001f, sample_rate);
    close_ = OnePoleCoefficient(0.080f, sample_rate);
    hold_samples_ = int32_t(0.050f * sample_rate);
    hold_ = 0;
    gain_db_ = 0.0f;
    set(-100.0f, 1.0f, 40.0f);
  }

  // ratio 1.0 = bypass.
  void set(float threshold_db, float ratio, float range_db) {
    threshold_db_ = threshold_db;
    slope_ = ratio - 1.0f;
    range_db_ = range_db;
  }

  inline float Process(float x) {
    const float env_db = GainToDb(detector_.Process(x));
    float target_db = 0.0f;
    if (slope_ > 0.0f) {
      if (env_db >= threshold_db_) {
        hold_ = hold_samples_;
      } else if (hold_ > 0) {
        --hold_;
      } else {
        target_db = std::max(-range_db_, (env_db - threshold_db_) * slope_);
      }
    }
    gain_db_ += (target_db > gain_db_ ? open_ : close_) * (target_db - gain_db_);
    return DbToGain(gain_db_);
  }

  inline float gain_db() const { return gain_db_; }

 private:
  float sample_rate_;
  EnvelopeFollower detector_;
  float open_;
  float close_;
  int32_t hold_samples_;
  int32_t hold_;
  float threshold_db_;
  float slope_;
  float range_db_;
  float gain_db_;
};

// Dynamic de-esser.
//
// The sibilance detector compares the energy in a 6.5 kHz band to the full
// band energy. When the ratio exceeds the threshold, a 5 kHz high shelf is
// pulled down by up to 12 dB. The shelf coefficients are refreshed every
// kUpdatePeriod samples (0.25 ms at 48 kHz); a 0 dB shelf is an exact
// pass-through, so the processor is transparent when it is not working.
class DeEsser {
 public:
  void Init(float sample_rate) {
    sidechain_.Init();
    sidechain_.set_f_q<stmlib::FREQUENCY_ACCURATE>(6500.0f / sample_rate, 1.5f);
    shelf_.Init(FixedFrequencyBiquad::HIGH_SHELF, 5000.0f, 0.7071f, sample_rate);
    band_.Init(sample_rate, 0.0005f, 0.040f);
    full_.Init(sample_rate, 0.0005f, 0.040f);
    attack_ = OnePoleCoefficient(0.0005f, sample_rate);
    release_ = OnePoleCoefficient(0.040f, sample_rate);
    reduction_db_ = 0.0f;
    counter_ = 0;
    set_depth(0.0f);
  }

  // depth: 0 (off) .. 1 (threshold -14 dB, up to 12 dB of reduction).
  void set_depth(float depth) {
    depth_ = depth;
    threshold_db_ = -4.0f - 10.0f * depth;
    max_reduction_db_ = depth > 0.0f ? 3.0f + 9.0f * depth : 0.0f;
  }

  inline float Process(float x) {
    const float band = sidechain_.Process<
        stmlib::FILTER_MODE_BAND_PASS_NORMALIZED>(x);
    const float band_level = band_.Process(band);
    const float full_level = full_.Process(x);

    float target = 0.0f;
    if (max_reduction_db_ > 0.0f && band_level > 0.003f) {  // ~ -50 dBFS
      const float ratio_db = GainToDb(band_level) - GainToDb(full_level);
      target = (ratio_db - threshold_db_) * 1.5f;
      CONSTRAIN(target, 0.0f, max_reduction_db_);
    }
    reduction_db_ += (target > reduction_db_ ? attack_ : release_) * \
        (target - reduction_db_);
    if (++counter_ >= kUpdatePeriod) {
      counter_ = 0;
      // Values below 0.05 dB snap to an exact 0 dB (pass-through) shelf.
      shelf_.set_gain_db(reduction_db_ < 0.05f ? 0.0f : -reduction_db_);
    }
    return shelf_.Process(x);
  }

  inline float reduction_db() const { return reduction_db_; }

 private:
  static const int kUpdatePeriod = 12;

  stmlib::Svf sidechain_;
  FixedFrequencyBiquad shelf_;
  EnvelopeFollower band_;
  EnvelopeFollower full_;
  float attack_;
  float release_;
  float depth_;
  float threshold_db_;
  float max_reduction_db_;
  float reduction_db_;
  int counter_;
};

// Feed-forward, log-domain compressor with soft knee and program-dependent
// release: the release slows down (up to 3x) when gain reduction has been
// sustained for a while, which avoids pumping on continuous speech while
// staying fast on isolated peaks.
class Compressor {
 public:
  void Init(float sample_rate) {
    sample_rate_ = sample_rate;
    rms_coefficient_ = OnePoleCoefficient(0.005f, sample_rate);
    peak_.Init(sample_rate, 0.0001f, 0.050f);
    sustain_coefficient_ = OnePoleCoefficient(1.0f, sample_rate);
    mean_square_ = 0.0f;
    gr_db_ = 0.0f;
    sustain_ = 0.0f;
    set(0.0f, 1.0f, 6.0f, 0.005f, 0.100f, 0.0f);
  }

  void set(
      float threshold_db,
      float ratio,
      float knee_db,
      float attack_s,
      float release_s,
      float makeup_db) {
    threshold_db_ = threshold_db;
    slope_ = 1.0f / ratio - 1.0f;  // <= 0
    knee_db_ = knee_db;
    attack_ = OnePoleCoefficient(attack_s, sample_rate_);
    release_s_ = release_s;
    release_ = OnePoleCoefficient(release_s, sample_rate_);
    release_slow_ = OnePoleCoefficient(release_s * 3.0f, sample_rate_);
    makeup_db_ = makeup_db;
  }

  // Static gain curve: gain change in dB (<= 0) for an input level in dB.
  inline float GainComputer(float level_db) const {
    const float over = level_db - threshold_db_;
    if (2.0f * over <= -knee_db_) {
      return 0.0f;
    } else if (2.0f * over >= knee_db_) {
      return slope_ * over;
    } else {
      const float t = over + 0.5f * knee_db_;
      return slope_ * t * t / (2.0f * knee_db_);
    }
  }

  // Returns the gain (linear, including make-up) to apply to x. The
  // detector_offset_db is added to the detected level ("push").
  inline float Process(float x, float detector_offset_db) {
    mean_square_ += rms_coefficient_ * (x * x - mean_square_);
    const float rms_db = 0.5f * GainToDb(mean_square_ + 1e-12f);
    // The peak path only takes over on transients well above the RMS level
    // (speech crest factor is typically 12-15 dB).
    const float peak_db = GainToDb(peak_.Process(x)) - 10.0f;
    const float level_db = std::max(rms_db, peak_db) + detector_offset_db;
    const float target = GainComputer(level_db);

    if (target < gr_db_) {
      gr_db_ += attack_ * (target - gr_db_);
    } else {
      const float r = release_ + (release_slow_ - release_) * sustain_;
      gr_db_ += r * (target - gr_db_);
    }
    float s = -gr_db_ * (1.0f / 6.0f);
    CONSTRAIN(s, 0.0f, 1.0f);
    sustain_ += sustain_coefficient_ * (s - sustain_);
    return DbToGain(gr_db_ + makeup_db_);
  }

  inline float gain_reduction_db() const { return gr_db_; }

 private:
  float sample_rate_;
  float rms_coefficient_;
  EnvelopeFollower peak_;
  float sustain_coefficient_;
  float mean_square_;
  float gr_db_;
  float sustain_;

  float threshold_db_;
  float slope_;
  float knee_db_;
  float attack_;
  float release_s_;
  float release_;
  float release_slow_;
  float makeup_db_;
};

}  // namespace vox

#endif  // VOX_CHAIN_DSP_DYNAMICS_H_
