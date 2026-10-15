// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Pitch marks for pitch-synchronous processing (PSOLA), placed on a
// SpeechBuffer from a YIN track.
//
// - Voiced regions (aperiodicity < 0.2): one mark per period, at the
//   negative peak of a lightly smoothed waveform searched within +/- T0/4
//   of the predicted position (consistent epoch placement).
// - Uncertain regions (0.2 .. 0.45, e.g. speech over music): marks at the
//   tracked period, flagged so that the synthesis can fall back to plain
//   granular processing.
// - Unvoiced regions: marks every 8 ms with +/- 1 ms jitter.
//
// An octave-jump guard holds the previous period unless the new one
// persists for three YIN frames. A running mean of log2(F0) over voiced
// marks (2 s time constant) is kept for intonation control.

#ifndef VOX_COMMON_ANALYSIS_PITCH_MARKER_H_
#define VOX_COMMON_ANALYSIS_PITCH_MARKER_H_

#include <stdint.h>

#include <algorithm>
#include <cmath>

#include "stmlib/stmlib.h"

#include "vox/common/buffer/speech_buffer.h"
#include "vox/common/dsp/fast_math.h"

namespace vox {

enum MarkType {
  MARK_UNVOICED,
  MARK_UNCERTAIN,
  MARK_VOICED
};

struct PitchMark {
  uint32_t position;
  uint16_t period;    // samples (24 kHz) to the next mark / local T0
  uint8_t type;
  uint8_t confidence; // 255 * (1 - aperiodicity)
};

template<int capacity_bits>
class MarkTable {
 public:
  enum { kCapacity = 1 << capacity_bits, kMask = kCapacity - 1 };

  void Init(PitchMark* storage) {
    marks_ = storage;
    write_ = 0;
    size_ = 0;
  }

  void Push(const PitchMark& m) {
    marks_[write_ & kMask] = m;
    ++write_;
    if (size_ < uint32_t(kCapacity)) ++size_;
  }

  inline uint32_t size() const { return size_; }
  inline uint32_t first() const { return write_ - size_; }   // absolute index
  inline uint32_t last() const { return write_ - 1; }
  inline const PitchMark& At(uint32_t index) const { return marks_[index & kMask]; }

  // Drops marks whose position is no longer in [oldest, ...).
  void Prune(uint32_t oldest, uint32_t head) {
    while (size_ > 0) {
      const PitchMark& m = At(first());
      if (uint32_t(head - m.position) <= uint32_t(head - oldest)) break;
      --size_;
    }
  }

  // Absolute index of the mark nearest to pos (size() must be > 0).
  uint32_t Nearest(uint32_t pos) const {
    uint32_t lo = first(), hi = last();
    // Positions increase with the index; compare as signed offsets.
    while (hi - lo > 1) {
      const uint32_t mid = lo + (hi - lo) / 2;
      if (int32_t(At(mid).position - pos) <= 0) lo = mid; else hi = mid;
    }
    const int32_t dl = int32_t(pos - At(lo).position);
    const int32_t dh = int32_t(At(hi).position - pos);
    return (dl < 0 ? -dl : dl) <= (dh < 0 ? -dh : dh) ? lo : hi;
  }

  void Clear() { size_ = 0; }

 private:
  PitchMark* marks_;
  uint32_t write_;
  uint32_t size_;
};

typedef MarkTable<13> VoxMarkTable;  // 8192 marks (64 KB)

class PitchMarker {
 public:
  PitchMarker() { }
  ~PitchMarker() { }

  void Init(PitchMark* storage, float sample_rate) {
    table_.Init(storage);
    sample_rate_ = sample_rate;
    track_write_ = 0;
    track_size_ = 0;
    next_ = 0;
    started_ = false;
    rng_ = 0x1234567;
    accepted_period_ = 0.0f;
    pending_period_ = 0.0f;
    pending_count_ = 0;
    mean_log_f0_ = 7.0f;  // ~128 Hz
    mean_coefficient_ = 1.0f / (2.0f * 200.0f);  // 2 s at 200 marks/s
    lost_ = 0;
  }

  // A YIN estimate for the window centred on buffer position `centre`.
  // period: 24 kHz samples.
  void AddEstimate(uint32_t centre, float period, float aperiodicity) {
    uint8_t type = aperiodicity < 0.2f ? MARK_VOICED
        : (aperiodicity < 0.45f ? MARK_UNCERTAIN : MARK_UNVOICED);
    if (type != MARK_UNVOICED) {
      // Octave-jump guard.
      if (accepted_period_ > 0.0f) {
        const float ratio = period / accepted_period_;
        if (ratio > 1.74f || ratio < 0.575f) {
          if (pending_period_ > 0.0f && fabsf(period / pending_period_ - 1.0f) < 0.15f) {
            ++pending_count_;
          } else {
            pending_period_ = period;
            pending_count_ = 1;
          }
          if (pending_count_ >= 3) {
            accepted_period_ = period;
            pending_count_ = 0;
          } else {
            period = accepted_period_;
            type = MARK_UNCERTAIN;
          }
        } else {
          accepted_period_ += 0.5f * (period - accepted_period_);
          period = accepted_period_;
          pending_count_ = 0;
        }
      } else {
        accepted_period_ = period;
      }
    }
    TrackEntry& e = track_[track_write_ & (kTrack - 1)];
    e.centre = centre;
    e.period = period;
    e.type = type;
    e.confidence = uint8_t(255.0f * std::max(0.0f, std::min(1.0f, 1.0f - aperiodicity)));
    ++track_write_;
    if (track_size_ < kTrack) ++track_size_;
    if (type == MARK_VOICED) {
      lost_ = 0;
    } else if (lost_ < 1000) {
      ++lost_;
    }
  }

  // Places marks up to the newest estimate. Call once per block.
  template<int size_bits>
  void Update(const SpeechBuffer<size_bits>& buffer) {
    if (track_size_ == 0) return;
    const TrackEntry& newest = track_[(track_write_ - 1) & (kTrack - 1)];
    if (!started_) {
      next_ = newest.centre;
      started_ = true;
    }
    // Keep a margin for the refinement search.
    int guard = 0;
    while (int32_t(newest.centre - next_) > 200 && guard++ < 64) {
      const TrackEntry& e = Lookup(next_);
      PitchMark m;
      m.type = e.type;
      m.confidence = e.confidence;
      uint32_t position = next_;
      float period;
      if (e.type == MARK_VOICED) {
        period = e.period;
        position = Refine(buffer, next_, int(period * 0.25f));
      } else if (e.type == MARK_UNCERTAIN) {
        period = e.period;
      } else {
        period = 0.008f * sample_rate_;
        position += uint32_t(int32_t(Random() * 48.0f) - 24);
      }
      if (period < 20.0f) period = 20.0f;
      if (period > 1000.0f) period = 1000.0f;
      if (!buffer.Contains(position)) {
        next_ = buffer.head();
        break;
      }
      // Marks must stay ordered.
      if (table_.size() && int32_t(position - table_.At(table_.last()).position) < 8) {
        position = table_.At(table_.last()).position + 8;
      }
      m.position = position;
      m.period = uint16_t(period + 0.5f);
      table_.Push(m);
      if (e.type == MARK_VOICED) {
        mean_log_f0_ += mean_coefficient_ * (FastLog2(sample_rate_ / period) - mean_log_f0_);
      }
      next_ = position + uint32_t(period + 0.5f);
    }
  }

  // Restart the marking from the current head (after a freeze/unfreeze).
  void Restart(uint32_t head) {
    next_ = head;
    track_size_ = 0;
  }

  inline const VoxMarkTable& table() const { return table_; }
  inline VoxMarkTable* mutable_table() { return &table_; }
  inline float mean_log_f0() const { return mean_log_f0_; }
  inline bool tracking_lost() const { return lost_ > 40; }  // 200 ms
  inline uint32_t newest_marked() const { return next_; }

 private:
  struct TrackEntry {
    uint32_t centre;
    float period;
    uint8_t type;
    uint8_t confidence;
  };
  enum { kTrack = 64 };

  const TrackEntry& Lookup(uint32_t pos) const {
    // Newest entry whose centre is at or before pos (else the oldest).
    for (int i = 0; i < track_size_; ++i) {
      const TrackEntry& e = track_[(track_write_ - 1 - i) & (kTrack - 1)];
      if (int32_t(pos - e.centre) >= 0) return e;
    }
    return track_[(track_write_ - track_size_) & (kTrack - 1)];
  }

  template<int size_bits>
  uint32_t Refine(const SpeechBuffer<size_bits>& buffer, uint32_t pos, int radius) {
    if (radius < 2) return pos;
    float best = 1e9f;
    uint32_t best_pos = pos;
    for (int d = -radius; d <= radius; ++d) {
      const uint32_t p = pos + uint32_t(d);
      float s = 0.0f;
      for (int k = -3; k <= 3; ++k) s += buffer.Read(p + uint32_t(k));
      if (s < best) { best = s; best_pos = p; }
    }
    return best_pos;
  }

  float Random() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return float(rng_ >> 8) / 16777216.0f;
  }

  VoxMarkTable table_;
  float sample_rate_;
  TrackEntry track_[kTrack];
  uint32_t track_write_;
  int track_size_;
  uint32_t next_;
  bool started_;
  uint32_t rng_;
  float accepted_period_;
  float pending_period_;
  int pending_count_;
  float mean_log_f0_;
  float mean_coefficient_;
  int lost_;

  DISALLOW_COPY_AND_ASSIGN(PitchMarker);
};

}  // namespace vox

#endif  // VOX_COMMON_ANALYSIS_PITCH_MARKER_H_
