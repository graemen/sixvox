// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_syllable engine: granular playback whose grains start and end on the
// syllables / words / onset slices found by the real-time segmenter.
//
//  IN (L+R)/2 -> 48->24k -> [pause gate] -> SpeechBuffer (+feedback)
//                                       \-> Segmenter -> unit tables
//  triggers (DENSITY / SEED) -> unit selection (TIME, SIZE, SHAPE) -> grains
//  -> 24->48k -> dry/wet, reverb, limiter -> OUT

#ifndef VOX_SYLLABLE_DSP_SYLLABLE_ENGINE_H_
#define VOX_SYLLABLE_DSP_SYLLABLE_ENGINE_H_

#include "stmlib/stmlib.h"

#include "beads/dsp/frame.h"

#include "vox/common/analysis/segmenter.h"
#include "vox/common/analysis/vad.h"
#include "vox/common/buffer/speech_buffer.h"
#include "vox/common/dsp/half_rate.h"
#include "vox/common/dsp/output_stage.h"
#include "vox/common/synth/grain.h"

namespace vox {

enum SyllableSubmode {
  SYLLABLE_SUBMODE_SYLLABLE,       // green
  SYLLABLE_SUBMODE_WORD,           // yellow
  SYLLABLE_SUBMODE_ONSET_SLICE,    // red
  SYLLABLE_SUBMODE_SYLLABLE_PAUSE_REMOVAL,  // blue
};

struct SyllableParams {
  float time;            // 0..1 unit address
  float size;            // 0..1 grain length in units (0.5 = one unit)
  float shape;           // 0..1 scatter / envelope softness / reverse
  float density;         // 0..1 trigger generator (0.5 = off)
  float pitch;           // semitones
  float feedback;        // 0..1
  float dry_wet;         // 0..1
  float reverb;          // 0..1
  bool freeze;
  bool trigger;          // SEED button press or SEED gate rising edge
  bool seed_patched;     // SEED jack patched: triggers come from it only
  bool latch;            // SEED + FREEZE latch state
  uint8_t submode;
  bool pause_removal;    // in addition to the blue sub-mode
  uint8_t sensitivity;   // 0..2
};

class SyllableEngine {
 public:
  SyllableEngine() { }
  ~SyllableEngine() { }

  static const int kNumVoices = 6;

  void Init(float sample_rate, int16_t* buffer_storage, uint16_t* reverb_buffer);

  // in/out: size frames at 48 kHz (size even). gate[i]: onset trigger out.
  void Process(
      const SyllableParams& p,
      const beads::FloatFrame* in,
      beads::FloatFrame* out,
      bool* gate,
      size_t size);

  inline const Segmenter& segmenter() const { return segmenter_; }
  inline const VoxSpeechBuffer& buffer() const { return buffer_; }
  inline uint32_t triggers() const { return triggers_; }
  inline int active_voices() const;
  inline float input_level() const { return input_level_; }

  // Exposed for tests: the unit the next trigger would play.
  bool SelectUnit(const SyllableParams& p, uint32_t* start, uint32_t* length,
                  bool* reverse);

 private:
  UnitLevel level(const SyllableParams& p) const;
  void Trigger(const SyllableParams& p);
  void StartGrain(uint32_t start, uint32_t length, float rate, bool reverse,
                  float shape);

  float sample_rate_;   // 24 kHz internal rate
  VoxSpeechBuffer buffer_;
  Segmenter segmenter_;
  Vad pause_vad_;
  HalfRateBridge bridge_;
  OutputStage output_;
  Grain voices_[kNumVoices];

  // Trigger generator.
  float phase_;
  uint32_t rng_;

  // Pause removal.
  int32_t silence_run_;
  int32_t fade_in_;

  // Latch.
  bool latched_;
  uint32_t latch_start_;
  uint32_t latch_length_;
  bool latch_reverse_;
  int latch_voice_;

  float feedback_sample_;
  float gain_;
  float input_level_;
  uint32_t onsets_seen_;
  uint32_t words_seen_;
  int32_t gate_samples_;
  uint32_t triggers_;

  DISALLOW_COPY_AND_ASSIGN(SyllableEngine);
};

inline int SyllableEngine::active_voices() const {
  int n = 0;
  for (int i = 0; i < kNumVoices; ++i) n += voices_[i].active();
  return n;
}

}  // namespace vox

#endif  // VOX_SYLLABLE_DSP_SYLLABLE_ENGINE_H_
