// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// WSOLA (waveform-similarity overlap-add) time-scaling of a region of a
// SpeechBuffer to an arbitrary output duration, pitch preserved.
//
// 20 ms Hann frames with 50% overlap (10 ms synthesis hop). Each new frame is
// taken near its nominal position (start + elapsed * stretch ratio), at the
// offset (+/- 8 ms) whose waveform best matches the natural continuation of
// the previous frame. The search is done on a 4x decimated grid first, then
// refined to the sample. An optional `rate` resamples the frames (pitch and
// formants shift together, as with tape).

#ifndef VOX_COMMON_SYNTH_WSOLA_H_
#define VOX_COMMON_SYNTH_WSOLA_H_

#include <stdint.h>

#include <algorithm>
#include <cmath>

#include "stmlib/stmlib.h"

#include "vox/common/buffer/speech_buffer.h"
#include "vox/common/synth/grain.h"

namespace vox {

template<int frame_size = 480>
class Wsola {
 public:
  Wsola() { }
  ~Wsola() { }

  enum {
    kFrame = frame_size,
    kHop = frame_size / 2,
    kTolerance = frame_size * 2 / 5,   // 8 ms at 24 kHz
  };

  void Init() {
    active_ = false;
    for (int i = 0; i < kFrame; ++i) {
      // Periodic Hann: overlapping frames at 50% sum to exactly 1.
      window_[i] = 0.5f - 0.5f * cosf(2.0f * float(M_PI) * float(i) / float(kFrame));
      accumulator_[i] = 0.0f;
    }
  }

  // Plays [start, start + length) of the source over out_length samples.
  void Start(uint32_t start, uint32_t length, uint32_t out_length, float rate,
             float gain, int32_t attack) {
    start_ = start;
    length_ = length;
    out_length_ = out_length < 2 ? 2 : out_length;
    rate_ = rate;
    gain_ = gain;
    stretch_ = float(length) / float(out_length_);  // source per output sample
    elapsed_ = 0;
    previous_ = start;
    first_frame_ = true;
    std::fill(&accumulator_[0], &accumulator_[kFrame], 0.0f);
    attack_ = attack < 1 ? 1 : attack;
    forced_release_ = -1;
    active_ = true;
  }

  void Release(int32_t samples) {
    if (!active_ || forced_release_ >= 0) return;
    forced_level_ = elapsed_ < attack_ ? Fade(float(elapsed_) / float(attack_)) : 1.0f;
    forced_release_ = samples < 1 ? 1 : samples;
    forced_count_ = forced_release_;
  }

  inline bool active() const { return active_; }
  inline bool releasing() const { return forced_release_ >= 0; }
  inline int32_t remaining() const { return out_length_ - elapsed_; }

  template<int size_bits>
  inline float Render(const SpeechBuffer<size_bits>& buffer) {
    if (!active_) return 0.0f;
    const int32_t phase = elapsed_ % kHop;
    if (phase == 0) {
      AddFrame(buffer);
    }
    const int32_t index = elapsed_ % kFrame;
    float y = accumulator_[index];
    accumulator_[index] = 0.0f;

    float env = elapsed_ < attack_ ? Fade(float(elapsed_) / float(attack_)) : 1.0f;
    const int32_t tail = out_length_ - elapsed_;
    if (tail < kHop) env *= Fade(float(tail) / float(kHop));
    if (forced_release_ >= 0) {
      env = forced_level_ * float(forced_count_) / float(forced_release_);
      if (--forced_count_ <= 0) active_ = false;
    }
    ++elapsed_;
    if (elapsed_ >= out_length_) active_ = false;
    return y * env * gain_;
  }

 private:
  // Squared-error-free normalized cross-correlation between the frames
  // starting at a and b, every `step` samples over `length` samples.
  template<int size_bits>
  static float Similarity(const SpeechBuffer<size_bits>& buffer, uint32_t a,
                          uint32_t b, int length, int step) {
    float ab = 0.0f, aa = 1e-9f, bb = 1e-9f;
    for (int i = 0; i < length; i += step) {
      const float x = buffer.Read(a + i);
      const float y = buffer.Read(b + i);
      ab += x * y;
      aa += x * x;
      bb += y * y;
    }
    return ab / sqrtf(aa * bb);
  }

  template<int size_bits>
  void AddFrame(const SpeechBuffer<size_bits>& buffer) {
    // Source span covered by one frame (at the resampling rate).
    const uint32_t frame_span = uint32_t(float(kFrame) * rate_) + 4;
    const uint32_t last_valid = length_ > frame_span ? length_ - frame_span : 0;
    int32_t nominal = int32_t(float(elapsed_) * stretch_);
    CONSTRAIN(nominal, 0, int32_t(last_valid));

    uint32_t position = start_ + uint32_t(nominal);
    if (!first_frame_ && last_valid > 0) {
      // Natural continuation of the previous frame.
      const uint32_t target = previous_ + uint32_t(float(kHop) * rate_);
      const int compare = int(float(kHop) * rate_);
      int32_t lo = nominal - kTolerance, hi = nominal + kTolerance;
      if (lo < 0) lo = 0;
      if (hi > int32_t(last_valid)) hi = int32_t(last_valid);
      float best = -2.0f;
      int32_t best_offset = nominal;
      for (int32_t o = lo; o <= hi; o += 4) {
        const float s = Similarity(buffer, start_ + o, target, compare, 4);
        if (s > best) { best = s; best_offset = o; }
      }
      const int32_t coarse = best_offset;
      for (int32_t o = coarse - 3; o <= coarse + 3; ++o) {
        if (o < lo || o > hi || o == coarse) continue;
        const float s = Similarity(buffer, start_ + o, target, compare, 1);
        if (s > best) { best = s; best_offset = o; }
      }
      position = start_ + uint32_t(best_offset);
    }
    previous_ = position;
    first_frame_ = false;

    // Overlap-add the windowed frame.
    const int32_t base = elapsed_ % kFrame;
    float read = 0.0f;
    for (int i = 0; i < kFrame; ++i) {
      const int32_t ri = int32_t(read);
      const float x = buffer.ReadHermite(position + uint32_t(ri), read - float(ri));
      accumulator_[(base + i) % kFrame] += window_[i] * x;
      read += rate_;
    }
  }

  bool active_;
  uint32_t start_;
  uint32_t length_;
  int32_t out_length_;
  float rate_;
  float gain_;
  float stretch_;
  int32_t elapsed_;
  uint32_t previous_;
  bool first_frame_;
  int32_t attack_;
  int32_t forced_release_;
  int32_t forced_count_;
  float forced_level_;
  float window_[kFrame];
  float accumulator_[kFrame];

  DISALLOW_COPY_AND_ASSIGN(Wsola);
};

}  // namespace vox

#endif  // VOX_COMMON_SYNTH_WSOLA_H_
