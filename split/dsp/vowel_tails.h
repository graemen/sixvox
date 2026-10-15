// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Vowel tails: when a voiced run ends, its last pitch periods keep looping as
// a decaying tail under the ongoing speech, lengthening vowels without the
// output drifting behind real time. Noise tails (looped 33 ms of a
// consonant) use the same machinery.
//
// A tail is a loop table of two pitch periods taken 20 ms before the end of
// the run (the steady part of the vowel), with a crossfaded loop point, read
// with Hermite interpolation at the PITCH rate.

#ifndef VOX_SPLIT_DSP_VOWEL_TAILS_H_
#define VOX_SPLIT_DSP_VOWEL_TAILS_H_

#include <stdint.h>

#include <algorithm>
#include <cmath>

#include "stmlib/stmlib.h"

namespace vox {

class CaptureRing {
 public:
  enum { kSize = 4096, kMask = kSize - 1 };
  void Init() {
    std::fill(&data_[0], &data_[kSize], 0.0f);
    write_ = 0;
  }
  inline void Write(float x) {
    data_[write_ & kMask] = x;
    ++write_;
  }
  // i samples before the newest one (0 = newest).
  inline float Past(int i) const { return data_[(write_ - 1 - i) & kMask]; }
 private:
  float data_[kSize];
  uint32_t write_;
};

class Tail {
 public:
  enum { kMaxLength = 800, kCrossfade = 96 };

  void Init() {
    active_ = false;
    level_ = 0.0f;
  }

  // Builds the loop from `ring`, ending `end_offset` samples before its
  // newest sample. length: loop length in samples.
  void Start(const CaptureRing& ring, int end_offset, int length) {
    CONSTRAIN(length, 2 * kCrossfade, int(kMaxLength));
    length_ = length;
    const int x = std::min(int(kCrossfade), length / 4);
    // seg[k], k = 0 .. length + x - 1, oldest first.
    const int total = length + x;
    float energy = 0.0f;
    for (int i = 0; i < length; ++i) {
      const float s = ring.Past(end_offset + total - 1 - (x + i));
      table_[i] = s;
    }
    for (int j = 0; j < x; ++j) {
      const int i = length - x + j;
      const float w = float(j + 1) / float(x);
      table_[i] = table_[i] * (1.0f - w) + ring.Past(end_offset + total - 1 - j) * w;
    }
    for (int i = 0; i < length; ++i) energy += table_[i] * table_[i];
    rms_ = sqrtf(energy / float(length));
    phase_ = 0.0f;
    level_ = 1.0f;
    fade_in_ = 0.0f;
    release_ = -1.0f;
    active_ = rms_ > 1e-4f;
    age_ = 0;
  }

  void Release(float samples) {
    if (active_ && release_ < 0.0f) {
      release_ = samples;
      release_count_ = samples;
    }
  }

  inline bool active() const { return active_; }
  inline bool releasing() const { return release_ >= 0.0f; }
  inline uint32_t age() const { return age_; }
  inline float rms() const { return rms_; }

  // decay: per-sample level multiplier (1 = hold).
  inline float Render(float rate, float decay) {
    if (!active_) return 0.0f;
    const int i = int(phase_);
    const float f = phase_ - float(i);
    const int n = length_;
    const float xm1 = table_[(i + n - 1) % n];
    const float x0 = table_[i];
    const float x1 = table_[(i + 1) % n];
    const float x2 = table_[(i + 2) % n];
    const float c = (x1 - xm1) * 0.5f;
    const float v = x0 - x1;
    const float w = c + v;
    const float a = w + v + (x2 - x0) * 0.5f;
    const float b = w + a;
    const float s = (((a * f) - b) * f + c) * f + x0;
    phase_ += rate;
    while (phase_ >= float(n)) phase_ -= float(n);
    if (fade_in_ < 1.0f) fade_in_ += 1.0f / 120.0f;
    level_ *= decay;
    float gain = level_ * std::min(fade_in_, 1.0f);
    if (release_ >= 0.0f) {
      gain *= release_count_ / release_;
      release_count_ -= 1.0f;
      if (release_count_ <= 0.0f) active_ = false;
    }
    if (level_ < 1e-4f) active_ = false;
    ++age_;
    return s * gain;
  }

 private:
  bool active_;
  float table_[kMaxLength];
  int length_;
  float phase_;
  float level_;
  float fade_in_;
  float release_;
  float release_count_;
  float rms_;
  uint32_t age_;
};

class TailBank {
 public:
  enum { kNumTails = 4 };

  void Init() {
    for (int i = 0; i < kNumTails; ++i) tails_[i].Init();
    spawned_ = 0;
  }

  void Spawn(const CaptureRing& ring, int end_offset, int length) {
    int slot = -1;
    uint32_t oldest = 0;
    int oldest_slot = 0;
    for (int i = 0; i < kNumTails; ++i) {
      if (!tails_[i].active()) {
        slot = i;
        break;
      }
      if (tails_[i].age() >= oldest) {
        oldest = tails_[i].age();
        oldest_slot = i;
      }
    }
    if (slot < 0) {
      // Fade out the oldest over 10 ms and reuse... the slot is still busy,
      // so cut it: tails are quiet by then most of the time.
      tails_[oldest_slot].Init();
      slot = oldest_slot;
    }
    tails_[slot].Start(ring, end_offset, length);
    ++spawned_;
  }

  inline float Render(float rate, float decay) {
    float y = 0.0f;
    for (int i = 0; i < kNumTails; ++i) y += tails_[i].Render(rate, decay);
    return y;
  }

  inline int active() const {
    int n = 0;
    for (int i = 0; i < kNumTails; ++i) n += tails_[i].active();
    return n;
  }
  inline uint32_t spawned() const { return spawned_; }

 private:
  Tail tails_[kNumTails];
  uint32_t spawned_;
};

}  // namespace vox

#endif  // VOX_SPLIT_DSP_VOWEL_TAILS_H_
