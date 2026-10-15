// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Real-time speech segmentation into onset slices, syllables and words.
//
// Features (1 kHz frame rate, from BandEnergy):
// - Onsets: rectified 10 ms rise of the band energies (HF bands weighted up),
//   above an adaptive threshold, with a 50 ms refractory period.
// - Syllables: troughs of a smoothed 300 Hz - 2.5 kHz energy contour (the
//   syllable-rate envelope) between two nuclei, snapped to a nearby onset;
//   pauses >= 30 ms also close a syllable.
// - Words: speech runs separated by pauses >= 120 ms.
//
// Positions are SpeechBuffer positions of the analysed samples, compensated
// for the analysis filter delays. Units are reported once final (about
// 50-150 ms after their end).

#ifndef VOX_COMMON_ANALYSIS_SEGMENTER_H_
#define VOX_COMMON_ANALYSIS_SEGMENTER_H_

#include <stdint.h>

#include <algorithm>

#include "stmlib/stmlib.h"

#include "vox/common/analysis/band_energy.h"
#include "vox/common/analysis/vad.h"

namespace vox {

enum UnitLevel {
  UNIT_LEVEL_ONSET_SLICE,
  UNIT_LEVEL_SYLLABLE,
  UNIT_LEVEL_WORD,
  UNIT_LEVEL_LAST
};

struct Unit {
  uint32_t start;
  uint32_t end;
  float energy_db;  // mean broadband level over the unit
  uint32_t flags;

  inline uint32_t length() const { return end - start; }
};

template<int capacity>
class UnitTable {
 public:
  void Init() {
    write_ = 0;
    size_ = 0;
  }

  void Push(const Unit& u) {
    units_[write_] = u;
    write_ = (write_ + 1) % capacity;
    if (size_ < capacity) ++size_;
  }

  inline int size() const { return size_; }

  // i = 0: newest.
  inline const Unit& FromNewest(int i) const {
    int index = write_ - 1 - i;
    while (index < 0) index += capacity;
    return units_[index];
  }

  // i = 0: oldest.
  inline const Unit& FromOldest(int i) const {
    return FromNewest(size_ - 1 - i);
  }

  // Drops the units whose start is older than `oldest` (relative to head).
  void Prune(uint32_t oldest, uint32_t head) {
    while (size_ > 0) {
      const Unit& u = FromOldest(0);
      const uint32_t span = head - oldest;
      if (uint32_t(head - u.start) <= span) break;
      --size_;
    }
  }

  void Clear() { size_ = 0; }

 private:
  Unit units_[capacity];
  int write_;
  int size_;
};

const int kMaxUnits = 256;

class Segmenter {
 public:
  Segmenter() { }
  ~Segmenter() { }

  void Init(float sample_rate) {
    sample_rate_ = sample_rate;
    bands_.Init(sample_rate);
    vad_.Init(sample_rate, 0.012f);
    samples_per_ms_ = sample_rate / 1000.0f;
    for (int l = 0; l < UNIT_LEVEL_LAST; ++l) {
      table_[l].Init();
      finalized_[l] = 0;
    }
    onsets_ = 0;
    word_starts_ = 0;
    for (int i = 0; i < kHistory; ++i) {
      for (int b = 0; b < kNumBands; ++b) history_[i][b] = -120.0f;
    }
    history_index_ = 0;
    for (int i = 0; i < kRecentOnsets; ++i) recent_onsets_[i] = 0;
    recent_onset_index_ = 0;
    flux_mean_ = 0.0f;
    flux_previous_ = 0.0f;
    refractory_ = 0;
    nucleus_1_ = nucleus_2_ = -120.0f;
    tracking_peak_ = true;
    extreme_ = -120.0f;
    extreme_pos_ = 0;
    nucleus_seen_ = false;
    last_nucleus_pos_ = 0;
    speech_frame_ = false;
    in_speech_ = false;
    silence_run_ = 1000;
    silence_start_ = 0;
    syllable_open_ = false;
    word_open_ = false;
    slice_open_ = false;
    word_syllables_ = 0;
    syllable_energy_ = word_energy_ = slice_energy_ = 0.0f;
    syllable_frames_ = word_frames_ = slice_frames_ = 0;
    set_sensitivity(1);
  }

  // 0 = low, 1 = medium, 2 = high.
  void set_sensitivity(int s) {
    CONSTRAIN(s, 0, 2);
    sensitivity_ = s;
    const float delta[3] = { 9.0f, 6.0f, 4.0f };
    const float hysteresis[3] = { 4.0f, 3.0f, 2.0f };
    onset_delta_ = delta[s];
    nucleus_hysteresis_ = hysteresis[s];
  }
  inline int sensitivity() const { return sensitivity_; }

  // x: sample just written at buffer position pos.
  inline void Process(float x, uint32_t pos) {
    vad_.Process(x);
    speech_frame_ = speech_frame_ || vad_.speech();
    if (bands_.Process(x)) {
      ProcessFrame(pos);
      speech_frame_ = false;
    }
  }

  inline const UnitTable<kMaxUnits>& table(UnitLevel level) const {
    return table_[level];
  }

  void Prune(uint32_t oldest, uint32_t head) {
    for (int l = 0; l < UNIT_LEVEL_LAST; ++l) table_[l].Prune(oldest, head);
  }

  void Clear() {
    for (int l = 0; l < UNIT_LEVEL_LAST; ++l) table_[l].Clear();
  }

  // Event counters (wrap freely; compare with a stored copy).
  inline uint32_t onsets() const { return onsets_; }
  inline uint32_t word_starts() const { return word_starts_; }
  inline uint32_t finalized(UnitLevel level) const { return finalized_[level]; }

  inline bool speech() const { return in_speech_; }
  inline const Vad& vad() const { return vad_; }
  inline const BandEnergy& bands() const { return bands_; }

 private:
  static const int kHistory = 16;
  static const int kRecentOnsets = 8;

  inline uint32_t Ms(float ms) const { return uint32_t(ms * samples_per_ms_); }

  uint32_t SnapToOnset(uint32_t pos) const {
    const int32_t window = int32_t(Ms(30.0f));
    int32_t best = window + 1;
    uint32_t snapped = pos;
    for (int i = 0; i < kRecentOnsets; ++i) {
      const int32_t d = int32_t(recent_onsets_[i] - pos);
      const int32_t ad = d < 0 ? -d : d;
      if (ad < best) {
        best = ad;
        snapped = recent_onsets_[i];
      }
    }
    return snapped;
  }

  // Syllable boundaries found from the energy contour land in or after the
  // consonant; the syllable starts at the consonant onset, so snap back to
  // the latest onset in [pos - 100 ms, pos + 20 ms].
  uint32_t SnapBackToOnset(uint32_t pos) const {
    const int32_t before = int32_t(Ms(100.0f));
    const int32_t after = int32_t(Ms(20.0f));
    int32_t best = -before - 1;
    uint32_t snapped = pos;
    for (int i = 0; i < kRecentOnsets; ++i) {
      const int32_t d = int32_t(recent_onsets_[i] - pos);
      if (d >= -before && d <= after && d > best) {
        best = d;
        snapped = recent_onsets_[i];
      }
    }
    return snapped;
  }

  void Emit(UnitLevel level, uint32_t start, uint32_t end, float energy,
            int frames) {
    if (int32_t(end - start) < int32_t(Ms(20.0f))) return;
    Unit u;
    u.start = start;
    u.end = end;
    u.energy_db = frames ? energy / float(frames) : -120.0f;
    u.flags = 0;
    table_[level].Push(u);
    ++finalized_[level];
  }

  void CloseSyllable(uint32_t end) {
    if (syllable_open_ && nucleus_seen_) {
      Emit(UNIT_LEVEL_SYLLABLE, syllable_start_, end, syllable_energy_,
           syllable_frames_);
      ++word_syllables_;
    }
    syllable_open_ = false;
    nucleus_seen_ = false;
    syllable_energy_ = 0.0f;
    syllable_frames_ = 0;
  }

  void CloseWord(uint32_t end) {
    if (word_open_ && word_syllables_ > 0) {
      Emit(UNIT_LEVEL_WORD, word_start_, end, word_energy_, word_frames_);
    }
    word_open_ = false;
    word_syllables_ = 0;
    word_energy_ = 0.0f;
    word_frames_ = 0;
  }

  void CloseSlice(uint32_t end) {
    if (slice_open_) {
      Emit(UNIT_LEVEL_ONSET_SLICE, slice_start_, end, slice_energy_,
           slice_frames_);
    }
    slice_open_ = false;
    slice_energy_ = 0.0f;
    slice_frames_ = 0;
  }

  void ProcessFrame(uint32_t pos) {
    const float* db = bands_.db();
    // Position of the analysed frame (band smoothing delay ~5 ms).
    const uint32_t frame_pos = pos - Ms(5.0f);
    const float level = bands_.total_db();
    const float floor_db = vad_.floor_db();

    // --- Onset detection ---
    const float* old = history_[history_index_];
    const float weights[kNumBands] = { 0.6f, 1.0f, 1.0f, 1.5f, 1.5f };
    float flux = 0.0f;
    for (int b = 0; b < kNumBands; ++b) {
      const float rise = db[b] - std::max(old[b], floor_db - 6.0f);
      if (rise > 0.0f) flux += weights[b] * rise;
    }
    for (int b = 0; b < kNumBands; ++b) history_[history_index_][b] = db[b];
    history_index_ = (history_index_ + 1) % 10;  // 10 ms difference

    flux_mean_ += 0.005f * (flux - flux_mean_);
    const float threshold = 1.5f * flux_mean_ + onset_delta_ * 3.0f;
    if (refractory_ > 0) --refractory_;
    const bool onset = flux > threshold && flux_previous_ <= threshold && \
        refractory_ == 0 && level > floor_db + 6.0f;
    flux_previous_ = flux;
    if (onset) {
      const uint32_t onset_pos = frame_pos - Ms(5.0f);
      refractory_ = 50;
      ++onsets_;
      recent_onsets_[recent_onset_index_] = onset_pos;
      recent_onset_index_ = (recent_onset_index_ + 1) % kRecentOnsets;
      CloseSlice(onset_pos);
      slice_open_ = true;
      slice_start_ = onset_pos;
    }

    // --- Speech / silence ---
    if (speech_frame_) {
      if (!in_speech_) {
        in_speech_ = true;
        const uint32_t start = SnapToOnset(frame_pos - Ms(10.0f));
        if (!syllable_open_) {
          syllable_open_ = true;
          syllable_start_ = start;
          nucleus_seen_ = false;
        }
        if (!word_open_) {
          word_open_ = true;
          word_start_ = start;
          ++word_starts_;
        }
        tracking_peak_ = true;
        extreme_ = -120.0f;
      }
      silence_run_ = 0;
    } else {
      if (in_speech_) {
        in_speech_ = false;
        silence_start_ = frame_pos;
      }
      ++silence_run_;
      if (silence_run_ == 30) {
        CloseSyllable(silence_start_);
        CloseSlice(silence_start_);
      } else if (silence_run_ == 120) {
        CloseWord(silence_start_);
      }
    }

    // --- Syllable nuclei and troughs ---
    const float n = std::max(db[1], db[2]);
    nucleus_1_ += 0.05f * (n - nucleus_1_);
    nucleus_2_ += 0.05f * (nucleus_1_ - nucleus_2_);
    const float contour = nucleus_2_;
    const uint32_t contour_pos = frame_pos - Ms(35.0f);

    if (in_speech_ && syllable_open_) {
      if (tracking_peak_) {
        if (contour > extreme_) {
          extreme_ = contour;
          extreme_pos_ = contour_pos;
        } else if (contour < extreme_ - nucleus_hysteresis_) {
          if (extreme_ > floor_db + 10.0f && \
              int32_t(extreme_pos_ - last_nucleus_pos_) > int32_t(Ms(100.0f))) {
            nucleus_seen_ = true;
            last_nucleus_pos_ = extreme_pos_;
          }
          tracking_peak_ = false;
          extreme_ = contour;
          extreme_pos_ = contour_pos;
        }
      } else {
        if (contour < extreme_) {
          extreme_ = contour;
          extreme_pos_ = contour_pos;
        } else if (contour > extreme_ + nucleus_hysteresis_) {
          // Trough between two nuclei: syllable boundary.
          if (nucleus_seen_) {
            const uint32_t boundary = SnapBackToOnset(extreme_pos_);
            if (int32_t(boundary - syllable_start_) > int32_t(Ms(60.0f))) {
              CloseSyllable(boundary);
              syllable_open_ = true;
              syllable_start_ = boundary;
            }
          }
          tracking_peak_ = true;
          extreme_ = contour;
          extreme_pos_ = contour_pos;
        }
      }
    }

    // Caps on unit lengths.
    if (syllable_open_ && int32_t(frame_pos - syllable_start_) > int32_t(Ms(600.0f))) {
      nucleus_seen_ = true;
      CloseSyllable(frame_pos);
      syllable_open_ = in_speech_;
      syllable_start_ = frame_pos;
    }
    if (slice_open_ && int32_t(frame_pos - slice_start_) > int32_t(Ms(400.0f))) {
      CloseSlice(frame_pos);
    }
    if (word_open_ && int32_t(frame_pos - word_start_) > int32_t(Ms(2500.0f))) {
      CloseSyllable(frame_pos);
      CloseWord(frame_pos);
      if (in_speech_) {
        word_open_ = syllable_open_ = true;
        word_start_ = syllable_start_ = frame_pos;
      }
    }

    if (syllable_open_) { syllable_energy_ += level; ++syllable_frames_; }
    if (word_open_) { word_energy_ += level; ++word_frames_; }
    if (slice_open_) { slice_energy_ += level; ++slice_frames_; }
  }

  float sample_rate_;
  float samples_per_ms_;
  BandEnergy bands_;
  Vad vad_;
  UnitTable<kMaxUnits> table_[UNIT_LEVEL_LAST];
  uint32_t finalized_[UNIT_LEVEL_LAST];
  uint32_t onsets_;
  uint32_t word_starts_;
  int sensitivity_;
  float onset_delta_;
  float nucleus_hysteresis_;

  float history_[kHistory][kNumBands];
  int history_index_;
  uint32_t recent_onsets_[kRecentOnsets];
  int recent_onset_index_;
  float flux_mean_;
  float flux_previous_;
  int refractory_;

  float nucleus_1_;
  float nucleus_2_;
  bool tracking_peak_;
  float extreme_;
  uint32_t extreme_pos_;
  bool nucleus_seen_;
  uint32_t last_nucleus_pos_;

  bool speech_frame_;
  bool in_speech_;
  int silence_run_;
  uint32_t silence_start_;

  bool syllable_open_;
  uint32_t syllable_start_;
  bool word_open_;
  uint32_t word_start_;
  int word_syllables_;
  bool slice_open_;
  uint32_t slice_start_;
  float syllable_energy_, word_energy_, slice_energy_;
  int syllable_frames_, word_frames_, slice_frames_;

  DISALLOW_COPY_AND_ASSIGN(Segmenter);
};

}  // namespace vox

#endif  // VOX_COMMON_ANALYSIS_SEGMENTER_H_
