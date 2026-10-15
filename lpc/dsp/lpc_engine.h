// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_lpc engine: linear-prediction analysis / resynthesis.
//
//  IN L -> 48->24k -> SpeechBuffer -> 12 kHz YIN -> voicing / F0 track
//  read head (TIME, DENSITY) -> source reader (30 ms Hann grains at 50%
//  overlap: transparent at 1x) -> pre-emphasis -> lattice inverse filter
//  (coefficients analysed at the read head every 10 ms) -> residual
//  excitation mix (residual / pulse / noise / whitened IN R carrier, SHAPE)
//  -> lattice synthesis (warped-analysis coefficients for SIZE, slewed by
//  FEEDBACK) -> de-emphasis -> 24->48k -> dry/wet, reverb, limiter -> OUT
//
// With SIZE at noon and SHAPE on "residual" the synthesis filter is the
// exact inverse of the analysis filter: the voice passes unchanged.

#ifndef VOX_LPC_DSP_LPC_ENGINE_H_
#define VOX_LPC_DSP_LPC_ENGINE_H_

#include "stmlib/stmlib.h"

#include "beads/dsp/frame.h"

#include "vox/common/analysis/lpc.h"
#include "vox/common/analysis/yin.h"
#include "vox/common/buffer/speech_buffer.h"
#include "vox/common/dsp/half_rate.h"
#include "vox/common/dsp/output_stage.h"
#include "vox/lpc/dsp/excitation.h"
#include "vox/lpc/dsp/speaking_chip.h"

namespace vox {

enum LpcSubmode {
  LPC_SUBMODE_RESYNTH,     // green: residual-centred SHAPE
  LPC_SUBMODE_CHIP,        // red: speaking chip
  LPC_SUBMODE_SYNTH,       // blue: pulse / noise excitation only
  LPC_SUBMODE_CROSS,       // magenta: IN R carrier
};

struct LpcParams {
  float time;
  float size;       // formant warp (0.5 = none); chip clock in red
  float shape;      // excitation morph
  float density;    // frame playback speed: noon = hold
  float pitch;      // semitones
  float slew;       // FEEDBACK: envelope smoothing 0..500 ms
  float dry_wet;
  float reverb;
  bool freeze;
  bool hold;        // SEED held (live): hold the current frame
  bool trigger;     // SEED rising edge (frozen): back to TIME
  bool latch;       // SEED + FREEZE: sustained frame
  bool smooth_f0;   // MEDIA + SEED: smoothed detected F0 for "follow"
  bool carrier_present;
  uint8_t submode;
};

struct VoiceTrackEntry {
  uint32_t position;
  uint16_t period;      // 24 kHz samples x 4
  uint8_t aperiodicity; // x 255
  uint8_t pad;
};

class LpcEngine {
 public:
  LpcEngine() { }
  ~LpcEngine() { }

  enum {
    kOrder = 24,
    kWindow = 480,      // 20 ms
    kHop = 240,         // 10 ms
    kGrain = 720,       // 30 ms source reader grains
    kTrackSize = 4096,
    kStagesPerHop = 40,  // 40 blocks of 6 samples = one 10 ms hop
  };

  void Init(float sample_rate, int16_t* buffer_storage, VoiceTrackEntry* track,
            uint16_t* reverb_buffer);

  void Process(
      const LpcParams& p,
      const beads::FloatFrame* in,
      beads::FloatFrame* out,
      bool* gate,
      size_t size);

  inline const VoxSpeechBuffer& buffer() const { return buffer_; }
  inline double read_position() const { return read_; }
  inline bool voiced() const { return voiced_; }
  inline float last_f0() const { return f0_out_; }
  inline const float* synthesis_k() const { return k_synth_; }
  inline uint32_t analyses() const { return analyses_; }

 private:
  void AnalysisStage(const LpcParams& p, uint32_t centre);
  void LookupVoice(uint32_t position, bool* voiced, float* period);
  float ReadSource();

  float sample_rate_;
  VoxSpeechBuffer buffer_;
  Yin yin_;
  Decimator2 decimator_;
  HalfRateBridge bridge_;
  OutputStage output_;

  VoiceTrackEntry* track_;
  uint32_t track_write_;
  uint32_t track_size_;

  double read_;
  double offset_;
  bool was_frozen_;
  bool latched_;
  double latch_position_;

  // Source reader.
  struct ReaderGrain {
    uint32_t start;
    int32_t elapsed;
    bool active;
  };
  ReaderGrain reader_[2];
  int reader_next_;
  int32_t reader_countdown_;
  float hann_[kGrain];

  // Analysis / synthesis.
  int stage_;
  uint32_t frame_centre_;
  float lambda_;
  float r_plain_[kOrder + 1];
  float r_warped_[kOrder + 1];
  float pending_inverse_[kOrder + 1];
  float pending_warped_[kOrder + 1];
  bool pending_voiced_;
  float pending_period_;
  float k_inverse_[kOrder + 1], k_inverse_from_[kOrder + 1], k_inverse_to_[kOrder + 1];
  float k_warped_from_[kOrder + 1], k_warped_to_[kOrder + 1];
  float k_synth_[kOrder + 1];
  float window_[kWindow];
  float frame_[kWindow];
  float scratch_[kWindow];
  int32_t interpolation_;
  LatticeAnalysis inverse_;
  LatticeSynthesis synthesis_;
  float pre_previous_;
  float de_previous_;
  float residual_rms_;
  float carrier_level_;

  // Excitation.
  BlitPulse pulse_;
  WhiteNoise noise_;
  CarrierWhitener whitener_;
  float voicing_mix_;
  bool voiced_;
  float period_;
  float smoothed_period_;
  float f0_out_;

  SpeakingChip chip_;

  uint32_t analyses_;

  DISALLOW_COPY_AND_ASSIGN(LpcEngine);
};

}  // namespace vox

#endif  // VOX_LPC_DSP_LPC_ENGINE_H_
