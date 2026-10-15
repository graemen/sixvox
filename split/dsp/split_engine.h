// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_split engine: real-time voiced / unvoiced / burst / silence
// classification, used to process vowels and consonants separately, or to
// send them to separate outputs.
//
//  IN -> 48->24k -> classifier (+ YIN at 12 kHz)
//               -> 10 ms look-ahead delay -> masks (3 ms ramps) -> V, U, B
//  V -> vowel tails | U+B -> consonant delay (sub-mode routing) -> balance
//  -> 24->48k -> dry/wet (dry delayed 10 ms), reverb send, limiter -> OUT

#ifndef VOX_SPLIT_DSP_SPLIT_ENGINE_H_
#define VOX_SPLIT_DSP_SPLIT_ENGINE_H_

#include "stmlib/stmlib.h"
#include "stmlib/dsp/filter.h"

#include "beads/dsp/frame.h"
#include "beads/dsp/fx/reverb.h"

#include "vox/common/analysis/vuv.h"
#include "vox/common/analysis/yin.h"
#include "vox/common/dsp/half_rate.h"
#include "vox/common/dsp/peak_limiter.h"
#include "vox/split/dsp/consonant_delay.h"
#include "vox/split/dsp/vowel_tails.h"

namespace vox {

enum SplitSubmode {
  SPLIT_SUBMODE_PROCESS,   // green: V -> tails, U -> delay, reverb on U
  SPLIT_SUBMODE_SWAP,      // yellow: V -> delay + reverb, U -> noise tails
  SPLIT_SUBMODE_SPLITTER,  // red: OUT L = V, OUT R = U + B
  SPLIT_SUBMODE_CLEAN_UP,  // blue: consonant / noise-floor attenuation
};

struct SplitParams {
  float shape;      // stream balance (green/yellow); clean-up amounts (blue)
  float size;       // tail decay: off .. 4 s
  float pitch;      // tail transposition, semitones
  float time;       // delay time 10 ms - 1.2 s, or clock ratio
  float feedback;   // delay feedback 0..1.05
  float density;    // classifier bias (0.5 = neutral)
  float dry_wet;
  float reverb;
  bool freeze;      // hold the tails
  bool capture;     // force a tail now (SEED)
  int clock_edge;   // SEED rising edge index in the block (48 kHz), or -1
  bool clock_patched;
  bool mute_voiced; // SEED + FREEZE latch: input vowels muted
  uint8_t submode;
  uint8_t preset;   // classifier preset
};

class SplitEngine {
 public:
  SplitEngine() { }
  ~SplitEngine() { }

  void Init(float sample_rate, float* delay_buffer, uint16_t* reverb_buffer);

  // gate[i]: voiced gate, aligned with the (delayed) audio.
  void Process(
      const SplitParams& p,
      const beads::FloatFrame* in,
      beads::FloatFrame* out,
      bool* gate,
      size_t size);

  inline const VuvClassifier& classifier() const { return classifier_; }
  inline const Yin& yin() const { return yin_; }
  inline const TailBank& tails() const { return tails_; }
  inline VuvClass current_class() const { return classifier_.vuv_class(); }
  inline float clock_period() const { return clock_period_; }
  // Latency of the processed path relative to the input (24 kHz samples).
  static const int kLookahead = 240;

 private:
  float sample_rate_;   // 24 kHz
  HalfRateBridge bridge_;
  Interpolator2 bridge_send_;
  Decimator2 decimator_;
  Yin yin_;
  VuvClassifier classifier_;

  float lookahead_[kLookahead];
  int lookahead_index_;
  beads::FloatFrame dry_delay_[2 * kLookahead];
  int dry_index_;

  stmlib::Svf split_lp_[2];
  float mask_voiced_, mask_unvoiced_, mask_burst_, mask_mixed_, mask_silence_;
  float voiced_period_;   // last voiced period at 24 kHz
  bool was_voiced_;
  int voiced_run_;
  bool was_unvoiced_;
  int unvoiced_run_;

  CaptureRing voiced_capture_;
  CaptureRing unvoiced_capture_;
  TailBank tails_;
  ConsonantDelay delay_;

  uint32_t now_;
  uint32_t last_edge_;
  float clock_period_;

  beads::Reverb reverb_;
  PeakLimiter limiter_;
  float dry_wet_;

  DISALLOW_COPY_AND_ASSIGN(SplitEngine);
};

}  // namespace vox

#endif  // VOX_SPLIT_DSP_SPLIT_ENGINE_H_
