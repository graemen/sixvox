// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_psola engine: TD-PSOLA driven by a live pitch tracker.
// Formant-preserving pitch shift, pitch-independent time stretch / scrub,
// intonation control (flatten, exaggerate, quantize to a scale).
//
//  IN -> 48->24k -> SpeechBuffer (+feedback) -> 12 kHz YIN -> PitchMarker
//  read head (TIME, DENSITY) -> nearest analysis mark -> 2-period Hann grain
//  (formant resampling) placed at the synthesis period (PITCH, SHAPE)
//  -> overlap-add -> 24->48k -> dry/wet, reverb, limiter -> OUT
//
// Uncertain regions (aperiodicity 0.2-0.45) fall back to 30 ms asynchronous
// grains; unvoiced regions use 8 ms grains with random offsets.

#ifndef VOX_PSOLA_DSP_PSOLA_ENGINE_H_
#define VOX_PSOLA_DSP_PSOLA_ENGINE_H_

#include "stmlib/stmlib.h"

#include "beads/dsp/frame.h"

#include "vox/common/analysis/pitch_marker.h"
#include "vox/common/analysis/yin.h"
#include "vox/common/buffer/speech_buffer.h"
#include "vox/common/dsp/half_rate.h"
#include "vox/common/dsp/output_stage.h"

namespace vox {

enum PsolaSubmode {
  PSOLA_SUBMODE_PSOLA,      // green
  PSOLA_SUBMODE_QUANTIZE,   // yellow: F0 snapped to a scale
  PSOLA_SUBMODE_STUTTER,    // red: each period repeated N times
  PSOLA_SUBMODE_NARROW,     // blue: 70-400 Hz tracking range
};

enum PsolaScale {
  PSOLA_SCALE_CHROMATIC,
  PSOLA_SCALE_MAJOR,
  PSOLA_SCALE_MINOR,
  PSOLA_SCALE_PENTATONIC,
  PSOLA_SCALE_LAST
};

struct PsolaParams {
  float time;       // read position
  float size;       // formant shift (0.5 = none); stutter count in red
  float shape;      // intonation (0.5 = natural); correction in yellow
  float density;    // read speed: noon = hold, CW forward, CCW reverse
  float pitch;      // semitones (stock law + 1V/oct)
  float feedback;
  float dry_wet;
  float reverb;
  bool freeze;
  bool hold;        // SEED held (live): hold the current mark
  bool trigger;     // SEED rising edge (frozen): jump back to TIME
  bool latch;       // SEED + FREEZE: loop between TIME and the latch point
  uint8_t submode;
  uint8_t scale;
};

// Hann window table (257 points over [0, 1]), filled by PsolaEngine::Init.
extern float psola_hann_lut[257];

class PsolaGrain {
 public:
  void Init() { active_ = false; }
  // Plays source [centre - half, centre + half) at `rate` with a Hann
  // window over its output duration.
  void Start(uint32_t centre, float half, float rate, float gain) {
    start_ = centre - uint32_t(half);
    length_ = 2.0f * half;
    rate_ = rate;
    gain_ = gain;
    duration_ = int32_t(length_ / rate);
    if (duration_ < 4) duration_ = 4;
    inv_duration_ = 1.0f / float(duration_);
    elapsed_ = 0;
    position_ = 0.0f;
    active_ = true;
  }
  inline bool active() const { return active_; }
  inline int32_t remaining() const { return duration_ - elapsed_; }
  template<int bits>
  inline float Render(const SpeechBuffer<bits>& b) {
    if (!active_) return 0.0f;
    const float x = float(elapsed_) * inv_duration_ * 256.0f;
    const int32_t xi = int32_t(x);
    const float w = psola_hann_lut[xi] + (psola_hann_lut[xi + 1] - psola_hann_lut[xi]) * (x - float(xi));
    const int32_t pi = int32_t(position_);
    const float s = b.ReadHermite(start_ + uint32_t(pi), position_ - float(pi));
    position_ += rate_;
    if (++elapsed_ >= duration_) active_ = false;
    return s * w * gain_;
  }
 private:
  bool active_;
  uint32_t start_;
  float length_;
  float rate_;
  float gain_;
  int32_t duration_;
  float inv_duration_;
  int32_t elapsed_;
  float position_;
};

class PsolaEngine {
 public:
  PsolaEngine() { }
  ~PsolaEngine() { }

  static const int kNumGrains = 12;

  void Init(float sample_rate, int16_t* buffer_storage, PitchMark* mark_storage,
            uint16_t* reverb_buffer);

  void Process(
      const PsolaParams& p,
      const beads::FloatFrame* in,
      beads::FloatFrame* out,
      bool* gate,
      size_t size);

  inline const PitchMarker& marker() const { return marker_; }
  inline const VoxSpeechBuffer& buffer() const { return buffer_; }
  inline double read_position() const { return read_; }
  inline float last_output_f0() const { return last_f0_; }
  inline uint32_t grains_started() const { return grains_started_; }
  inline float input_level() const { return input_level_; }
  inline bool voiced() const { return voiced_; }

 private:
  float TargetLogF0(const PsolaParams& p, float log_f0_source);
  void Synthesize(const PsolaParams& p);
  void StartGrain(uint32_t centre, float half, float rate, float gain);

  float sample_rate_;   // 24 kHz
  VoxSpeechBuffer buffer_;
  PitchMarker marker_;
  Yin yin_;
  Decimator2 decimator_;
  HalfRateBridge bridge_;
  OutputStage output_;
  PsolaGrain grains_[kNumGrains];

  double read_;          // absolute read position (buffer timeline)
  double offset_;        // scrub offset added to the TIME position
  bool was_frozen_;
  bool latched_;
  double loop_a_, loop_b_;
  float countdown_;      // output samples to the next synthesis mark
  float glide_log_f0_;
  int stutter_left_;
  uint32_t stutter_mark_;
  float feedback_sample_;
  float input_level_;
  float last_f0_;
  bool voiced_;
  uint32_t grains_started_;
  uint32_t rng_;
  uint8_t submode_;

  DISALLOW_COPY_AND_ASSIGN(PsolaEngine);
};

}  // namespace vox

#endif  // VOX_PSOLA_DSP_PSOLA_ENGINE_H_
