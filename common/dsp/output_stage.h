// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Shared 48 kHz output stage: dry/wet crossfade, stock Beads reverb, peak
// limiter. Used by the granular / resynthesis VOX firmwares.

#ifndef VOX_COMMON_DSP_OUTPUT_STAGE_H_
#define VOX_COMMON_DSP_OUTPUT_STAGE_H_

#include "stmlib/stmlib.h"

#include "beads/dsp/frame.h"
#include "beads/dsp/fx/reverb.h"

#include "vox/common/dsp/peak_limiter.h"

namespace vox {

class OutputStage {
 public:
  OutputStage() { }
  ~OutputStage() { }

  void Init(float sample_rate, uint16_t* reverb_buffer) {
    reverb_.Init(reverb_buffer, sample_rate);
    limiter_.Init(sample_rate, 0.94f);
    dry_wet_ = 1.0f;
  }

  // wet: processed signal, replaced by the output. dry: input signal.
  void Process(
      const beads::FloatFrame* dry,
      beads::FloatFrame* wet,
      float dry_wet,
      float reverb_amount,
      size_t size) {
    const float r = reverb_amount;
    reverb_.set_mix(r * r * 0.9f);
    reverb_.set_time(0.35f + 0.6f * r * r * r);
    reverb_.set_lp(0.6f);
    reverb_.Process(wet, size);

    const float step = (dry_wet - dry_wet_) / float(size);
    for (size_t i = 0; i < size; ++i) {
      dry_wet_ += step;
      // Equal-power-ish crossfade (sine/cosine approximated by a parabola).
      const float w = dry_wet_;
      const float g_wet = w * (2.0f - w);
      const float g_dry = (1.0f - w) * (1.0f + w);
      wet[i].l = g_dry * dry[i].l + g_wet * wet[i].l;
      wet[i].r = g_dry * dry[i].r + g_wet * wet[i].r;
      limiter_.Process(&wet[i].l, &wet[i].r);
    }
  }

 private:
  beads::Reverb reverb_;
  PeakLimiter limiter_;
  float dry_wet_;

  DISALLOW_COPY_AND_ASSIGN(OutputStage);
};

}  // namespace vox

#endif  // VOX_COMMON_DSP_OUTPUT_STAGE_H_
