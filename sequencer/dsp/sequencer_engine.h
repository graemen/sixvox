// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_sequencer engine: detected syllables or words become the steps of a
// clocked pattern, played in order, shuffled or randomized, either truncated
// to the step or time-stretched to fit it (WSOLA), with stutters/ratchets.
//
//  IN -> 48->24k -> [pause gate] -> SpeechBuffer -> Segmenter -> unit pool
//  SEED jack (clock) / internal clock -> Clock -> step events (+ swing,
//  ratchets) -> PatternGenerator -> step player (2 voices) -> 24->48k ->
//  dry/wet, reverb, limiter -> OUT

#ifndef VOX_SEQUENCER_DSP_SEQUENCER_ENGINE_H_
#define VOX_SEQUENCER_DSP_SEQUENCER_ENGINE_H_

#include "stmlib/stmlib.h"

#include "beads/dsp/frame.h"

#include "vox/common/analysis/segmenter.h"
#include "vox/common/analysis/vad.h"
#include "vox/common/buffer/speech_buffer.h"
#include "vox/common/dsp/half_rate.h"
#include "vox/common/dsp/output_stage.h"
#include "vox/common/synth/grain.h"
#include "vox/common/synth/wsola.h"

namespace vox {

enum SequencerSubmode {
  SEQUENCER_SUBMODE_SYLLABLE_TRUNCATE,  // green
  SEQUENCER_SUBMODE_WORD_TRUNCATE,      // yellow
  SEQUENCER_SUBMODE_SYLLABLE_STRETCH,   // cyan
  SEQUENCER_SUBMODE_WORD_STRETCH,       // magenta
};

enum SequencerGateMode {
  SEQUENCER_GATE_OFF,
  SEQUENCER_GATE_STEP_TRIGGER,
  SEQUENCER_GATE_ACCENT,
};

struct SequencerParams {
  float time;       // pattern offset in the pool
  float size;       // pattern length, 1-16 steps
  float shape;      // order: in order, reverse, shuffle, random/cycle, random/step
  float density;    // external clock: ratio; internal: tempo
  float pitch;      // semitones, sampled at each step
  float repeat;     // FEEDBACK: stutter / ratchet probability
  float dry_wet;
  float reverb;
  bool freeze;
  int clock_edge;   // index (48 kHz frame in the block) of a SEED rising edge, -1
  bool clock_patched;
  bool tap;         // SEED button pressed: tap tempo / manual advance
  bool reset;       // reset gate rising edge (ASSIGN CV, 5th destination)
  bool lock;        // SEED + FREEZE pattern lock
  uint8_t submode;
  bool pause_removal;
  uint8_t swing;    // 0, 1, 2 = none, 16%, 33%
  uint8_t gate_mode;
};

class SequencerEngine {
 public:
  SequencerEngine() { }
  ~SequencerEngine() { }

  static const int kMaxSteps = 16;

  void Init(float sample_rate, int16_t* buffer_storage, uint16_t* reverb_buffer);

  void Process(
      const SequencerParams& p,
      const beads::FloatFrame* in,
      beads::FloatFrame* out,
      bool* gate,
      size_t size);

  inline const Segmenter& segmenter() const { return segmenter_; }
  inline const VoxSpeechBuffer& buffer() const { return buffer_; }
  inline uint32_t steps() const { return steps_; }
  inline uint32_t step_period() const { return uint32_t(step_period_); }
  inline bool locked_to_clock() const { return clock_period_ > 0.0f; }
  inline int step_index() const { return step_index_; }
  inline bool accent() const { return accent_; }
  inline float input_level() const { return input_level_; }
  // Unit (from the oldest) played by the last step, for tests.
  inline uint32_t last_unit_start() const { return last_start_; }

 private:
  UnitLevel level(const SequencerParams& p) const;
  bool stretch(const SequencerParams& p) const;
  void ClockEdge(const SequencerParams& p, uint32_t now);
  void ScheduleStep(const SequencerParams& p, uint32_t when);
  void Step(const SequencerParams& p, bool ratchet);
  void Regenerate(int length);
  int UnitForStep(const SequencerParams& p, int step, int pool, int length);
  void PlayUnit(const SequencerParams& p, const Unit& u, uint32_t duration);

  float sample_rate_;
  VoxSpeechBuffer buffer_;
  Segmenter segmenter_;
  Vad pause_vad_;
  HalfRateBridge bridge_;
  OutputStage output_;
  Grain grains_[2];
  Wsola<480> stretchers_[2];
  int voice_;

  // Clock.
  uint32_t now_;
  uint32_t last_edge_;
  float clock_period_;       // samples between external clock edges (0 = none)
  int mismatches_;
  int edge_count_;
  float intervals_[3];
  float internal_phase_;
  float tap_period_;
  float tap_density_;
  uint32_t last_tap_;
  int taps_;
  float tap_intervals_[3];
  float step_period_;

  // Scheduler: pending step events (multiplied clock, swing, ratchets).
  struct Pending {
    uint32_t when;
    bool ratchet;
  };
  static const int kMaxPending = 16;
  Pending pending_[kMaxPending];
  int num_pending_;

  // Pattern.
  int step_index_;
  int permutation_[kMaxSteps];
  int locked_[kMaxSteps];
  bool was_locked_;
  bool reset_pending_;
  uint32_t rng_;
  uint32_t shuffle_seed_;
  int previous_unit_;
  bool accent_;
  uint32_t steps_;
  uint32_t last_start_;
  float pitch_;

  // Pause removal.
  int32_t silence_run_;
  int32_t fade_in_;

  float input_level_;
  int32_t gate_samples_;
  int32_t accent_samples_;

  DISALLOW_COPY_AND_ASSIGN(SequencerEngine);
};

}  // namespace vox

#endif  // VOX_SEQUENCER_DSP_SEQUENCER_ENGINE_H_
