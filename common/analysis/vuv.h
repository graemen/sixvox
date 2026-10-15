// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Frame classifier: silence / voiced / unvoiced (fricatives, aspiration) /
// burst (plosive releases), plus a "mixed" flag for voiced frames with strong
// high-frequency content (voiced fricatives).
//
// Features, every 5 ms (24 kHz input): zero-crossing rate, first
// autocorrelation coefficient (spectral tilt), high-band vs low-band energy,
// level above the noise floor, and the YIN aperiodicity supplied by the
// caller. The voiced score is a weighted sum of clipped feature terms; a
// class change needs two consecutive frames (10 ms) except for bursts.

#ifndef VOX_COMMON_ANALYSIS_VUV_H_
#define VOX_COMMON_ANALYSIS_VUV_H_

#include <algorithm>

#include "stmlib/stmlib.h"

#include "vox/common/analysis/band_energy.h"
#include "vox/common/analysis/vad.h"
#include "vox/common/dsp/fast_math.h"

namespace vox {

enum VuvClass {
  VUV_SILENCE,
  VUV_VOICED,
  VUV_UNVOICED,
  VUV_BURST
};

enum VuvPreset {
  VUV_PRESET_SPEECH,
  VUV_PRESET_SPEECH_OVER_MUSIC,
  VUV_PRESET_PHONE,
  VUV_PRESET_LAST
};

class VuvClassifier {
 public:
  VuvClassifier() { }
  ~VuvClassifier() { }

  void Init(float sample_rate) {
    sample_rate_ = sample_rate;
    bands_.Init(sample_rate);
    vad_.Init(sample_rate, 0.012f);
    hop_ = int(0.005f * sample_rate);
    count_ = 0;
    crossings_ = 0;
    previous_ = 0.0f;
    r0_ = r1_ = 0.0f;
    aperiodicity_ = 1.0f;
    class_ = candidate_ = VUV_SILENCE;
    candidate_count_ = 0;
    burst_hops_ = 0;
    previous_hf_ = -120.0f;
    previous_level_ = -120.0f;
    score_ = 0.0f;
    mixed_ = false;
    bias_ = 0.0f;
    frames_ = 0;
    set_preset(VUV_PRESET_SPEECH);
  }

  void set_preset(int preset) {
    preset_ = preset;
    // Weights: aperiodicity, tilt, zero-crossing rate, HF/LF balance.
    static const float w[VUV_PRESET_LAST][4] = {
      { 1.5f, 1.0f, 1.0f, 1.0f },   // speech
      { 2.5f, 0.7f, 0.7f, 0.5f },   // speech over music: trust periodicity
      { 2.0f, 0.5f, 1.2f, 0.3f },   // phone: band-limited, HF unreliable
    };
    CONSTRAIN(preset, 0, VUV_PRESET_LAST - 1);
    for (int i = 0; i < 4; ++i) weight_[i] = w[preset][i];
  }

  // Positive values favour "voiced" (DENSITY control).
  void set_bias(float bias) { bias_ = bias; }

  // Latest YIN aperiodicity (0 = perfectly periodic, 1 = noise).
  void set_aperiodicity(float a) { aperiodicity_ = a; }

  // Returns true when a new frame decision is available.
  inline bool Process(float x) {
    vad_.Process(x);
    bands_.Process(x);
    if ((x >= 0.0f) != (previous_ >= 0.0f)) ++crossings_;
    r0_ += x * x;
    r1_ += x * previous_;
    previous_ = x;
    if (++count_ >= hop_) {
      Decide();
      count_ = 0;
      crossings_ = 0;
      r0_ = r1_ = 0.0f;
      return true;
    }
    return false;
  }

  inline VuvClass vuv_class() const { return class_; }
  inline bool mixed() const { return mixed_; }
  inline float score() const { return score_; }
  inline uint32_t frames() const { return frames_; }
  inline const Vad& vad() const { return vad_; }
  // Delay of a decision relative to the frame it describes, in samples.
  inline int latency() const { return 2 * hop_; }

 private:
  static inline float Clip(float x) {
    return x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
  }

  void Decide() {
    const float* db = bands_.db();
    const float lf = std::max(db[0], std::max(db[1], db[2]));
    const float hf = std::max(db[3], db[4]);
    const float zcr = float(crossings_) / float(hop_);
    const float k1 = r0_ > 1e-12f ? r1_ / r0_ : 0.0f;
    const float level = 3.0103f * FastLog2(r0_ / float(hop_) + 1e-12f);
    const float floor_db = vad_.floor_db();

    float v = 0.0f;
    v += weight_[0] * Clip((0.35f - aperiodicity_) / 0.15f);
    v += weight_[1] * Clip((k1 - 0.5f) / 0.3f);
    v += weight_[2] * Clip((0.15f - zcr) / 0.1f);
    v += weight_[3] * Clip((lf - hf - 3.0f) / 6.0f);
    v += bias_ * 2.0f;
    score_ = v;
    mixed_ = v > 0.0f && hf > lf - 6.0f;

    VuvClass c;
    const bool audible = level > std::max(floor_db + 6.0f, -65.0f);
    // Burst: sudden HF rise from (near) silence.
    const bool burst = audible && hf - previous_hf_ > 15.0f && \
        level - previous_level_ > 12.0f && v < 1.0f;
    previous_hf_ = hf;
    previous_level_ = level;
    if (burst) {
      burst_hops_ = 4;  // up to 20 ms
    }
    if (burst_hops_ > 0) {
      --burst_hops_;
      c = audible ? VUV_BURST : VUV_SILENCE;
      class_ = candidate_ = c;
      candidate_count_ = 0;
      ++frames_;
      return;
    }
    if (!audible) {
      c = VUV_SILENCE;
    } else {
      c = v > 0.0f ? VUV_VOICED : VUV_UNVOICED;
    }
    if (c == class_) {
      candidate_count_ = 0;
    } else if (c == candidate_) {
      if (++candidate_count_ >= 1) {
        class_ = c;
        candidate_count_ = 0;
      }
    } else {
      candidate_ = c;
      candidate_count_ = 0;
    }
    ++frames_;
  }

  float sample_rate_;
  BandEnergy bands_;
  Vad vad_;
  int hop_;
  int count_;
  int crossings_;
  float previous_;
  float r0_;
  float r1_;
  float aperiodicity_;
  VuvClass class_;
  VuvClass candidate_;
  int candidate_count_;
  int burst_hops_;
  float previous_hf_;
  float previous_level_;
  float score_;
  bool mixed_;
  float bias_;
  int preset_;
  float weight_[4];
  uint32_t frames_;

  DISALLOW_COPY_AND_ASSIGN(VuvClassifier);
};

}  // namespace vox

#endif  // VOX_COMMON_ANALYSIS_VUV_H_
