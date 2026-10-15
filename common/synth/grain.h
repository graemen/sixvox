// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// A grain playing a region of a SpeechBuffer at a given rate, with
// raised-cosine attack/release fades, and an optional early release (used for
// voice stealing and step changes).

#ifndef VOX_COMMON_SYNTH_GRAIN_H_
#define VOX_COMMON_SYNTH_GRAIN_H_

#include <stdint.h>

#include "stmlib/stmlib.h"

#include "vox/common/buffer/speech_buffer.h"

namespace vox {

// 0 -> 0, 1 -> 1, raised-cosine shape (cubic smoothstep approximation).
inline float Fade(float x) {
  if (x <= 0.0f) return 0.0f;
  if (x >= 1.0f) return 1.0f;
  return x * x * (3.0f - 2.0f * x);
}

class Grain {
 public:
  void Init() {
    active_ = false;
    age_ = 0;
  }

  // Plays [start, start + length) of the buffer (source samples). rate > 0;
  // reverse plays from the end towards the start. attack/release are in
  // output samples.
  void Start(
      uint32_t start,
      uint32_t length,
      float rate,
      float gain,
      bool reverse,
      int32_t attack,
      int32_t release) {
    start_ = start;
    length_ = float(length);
    rate_ = rate;
    gain_ = gain;
    reverse_ = reverse;
    position_ = 0.0f;
    const float duration = length_ / rate;
    duration_ = int32_t(duration);
    if (duration_ < 2) duration_ = 2;
    attack_ = attack < 1 ? 1 : attack;
    release_ = release < 1 ? 1 : release;
    if (attack_ + release_ > duration_) {
      attack_ = release_ = duration_ / 2;
      if (attack_ < 1) attack_ = release_ = 1;
    }
    elapsed_ = 0;
    forced_release_ = -1;
    forced_level_ = 1.0f;
    active_ = true;
    age_ = 0;
  }

  // Fade out over `samples` output samples from now.
  void Release(int32_t samples) {
    if (!active_ || forced_release_ >= 0) return;
    forced_level_ = Envelope();
    forced_release_ = samples < 1 ? 1 : samples;
    forced_count_ = forced_release_;
  }

  inline bool active() const { return active_; }
  inline bool releasing() const { return forced_release_ >= 0; }
  inline uint32_t age() const { return age_; }
  inline float progress() const { return position_ / length_; }
  // Output samples left before the grain ends on its own.
  inline int32_t remaining() const { return duration_ - elapsed_; }

  template<int size_bits>
  inline float Render(const SpeechBuffer<size_bits>& buffer) {
    if (!active_) return 0.0f;
    float env = Envelope();
    if (forced_release_ >= 0) {
      env = forced_level_ * float(forced_count_) / float(forced_release_);
      if (--forced_count_ <= 0) {
        active_ = false;
      }
    }
    float p = reverse_ ? length_ - 1.0f - position_ : position_;
    if (p < 0.0f) p = 0.0f;
    const int32_t pi = int32_t(p);
    const float pf = p - float(pi);
    const float s = buffer.ReadHermite(start_ + uint32_t(pi), pf);
    position_ += rate_;
    ++elapsed_;
    ++age_;
    if (elapsed_ >= duration_ || position_ >= length_) {
      active_ = false;
    }
    return s * env * gain_;
  }

 private:
  inline float Envelope() const {
    if (elapsed_ < attack_) {
      return Fade(float(elapsed_) / float(attack_));
    }
    const int32_t remaining = duration_ - elapsed_;
    if (remaining < release_) {
      return Fade(float(remaining) / float(release_));
    }
    return 1.0f;
  }

  bool active_;
  uint32_t start_;
  float length_;
  float rate_;
  float gain_;
  bool reverse_;
  float position_;
  int32_t duration_;
  int32_t attack_;
  int32_t release_;
  int32_t elapsed_;
  int32_t forced_release_;
  int32_t forced_count_;
  float forced_level_;
  uint32_t age_;
};

}  // namespace vox

#endif  // VOX_COMMON_SYNTH_GRAIN_H_
