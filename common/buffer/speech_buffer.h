// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Circular int16 audio history with absolute (32-bit, wrapping) positions.
//
// Positions count the samples ever written. A position p is readable when it
// is in [oldest(), head()). All marker tables (segments, pitch marks, LPC
// frames) use the same position counter, so they stay valid as the buffer
// scrolls; unsigned 32-bit arithmetic handles wrap-around (49 h at 24 kHz).
//
// The capacity is a power of two. vox firmwares put a 2^18-sample buffer
// (512 KB, 10.9 s at 24 kHz) in the AXI SRAM (.ramd1).

#ifndef VOX_COMMON_BUFFER_SPEECH_BUFFER_H_
#define VOX_COMMON_BUFFER_SPEECH_BUFFER_H_

#include <stdint.h>

#include <algorithm>

#include "stmlib/stmlib.h"

namespace vox {

template<int size_bits>
class SpeechBuffer {
 public:
  SpeechBuffer() { }
  ~SpeechBuffer() { }

  enum {
    kCapacity = 1 << size_bits,
    kMask = kCapacity - 1
  };

  void Init(int16_t* storage) {
    buffer_ = storage;
    std::fill(&buffer_[0], &buffer_[kCapacity], 0);
    head_ = 0;
    available_ = 0;
  }

  inline void Write(float x) {
    int32_t s = static_cast<int32_t>(x * 32767.0f);
    if (s > 32767) s = 32767;
    if (s < -32767) s = -32767;
    buffer_[head_ & kMask] = int16_t(s);
    ++head_;
    if (available_ < uint32_t(kCapacity)) ++available_;
  }

  // Forget the history (the next writes start a fresh recording).
  inline void Clear() { available_ = 0; }

  inline uint32_t head() const { return head_; }
  inline uint32_t available() const { return available_; }
  inline uint32_t oldest() const { return head_ - available_; }
  static inline uint32_t capacity() { return kCapacity; }

  // True when pos is in [oldest, head).
  inline bool Contains(uint32_t pos) const {
    return uint32_t(head_ - 1 - pos) < available_;
  }

  // Signed distance from pos to the head (> 0 when pos is in the past).
  inline int32_t Age(uint32_t pos) const { return int32_t(head_ - pos); }

  inline float Read(uint32_t pos) const {
    return float(buffer_[pos & kMask]) * (1.0f / 32768.0f);
  }

  inline float ReadLinear(uint32_t pos, float frac) const {
    const float a = Read(pos);
    const float b = Read(pos + 1);
    return a + (b - a) * frac;
  }

  inline float ReadHermite(uint32_t pos, float frac) const {
    const float xm1 = Read(pos - 1);
    const float x0 = Read(pos);
    const float x1 = Read(pos + 1);
    const float x2 = Read(pos + 2);
    const float c = (x1 - xm1) * 0.5f;
    const float v = x0 - x1;
    const float w = c + v;
    const float a = w + v + (x2 - x0) * 0.5f;
    const float b_neg = w + a;
    return (((a * frac) - b_neg) * frac + c) * frac + x0;
  }

 private:
  int16_t* buffer_;
  uint32_t head_;
  uint32_t available_;

  DISALLOW_COPY_AND_ASSIGN(SpeechBuffer);
};

typedef SpeechBuffer<18> VoxSpeechBuffer;

}  // namespace vox

#endif  // VOX_COMMON_BUFFER_SPEECH_BUFFER_H_
