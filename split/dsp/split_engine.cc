// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_split engine.

#include "vox/split/dsp/split_engine.h"

#include <algorithm>
#include <cmath>

#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/units.h"

#include "vox/common/dsp/fast_math.h"

namespace vox {

using namespace beads;
using namespace std;

void SplitEngine::Init(float sample_rate, float* delay_buffer, uint16_t* reverb_buffer) {
  sample_rate_ = sample_rate * 0.5f;
  bridge_.Init();
  bridge_send_.Init();
  decimator_.Init();
  yin_.Init(sample_rate_ * 0.5f);
  yin_.set_hop(120);  // 10 ms
  classifier_.Init(sample_rate_);
  std::fill(&lookahead_[0], &lookahead_[kLookahead], 0.0f);
  lookahead_index_ = 0;
  for (int i = 0; i < 2 * kLookahead; ++i) dry_delay_[i].l = dry_delay_[i].r = 0.0f;
  dry_index_ = 0;
  for (int i = 0; i < 2; ++i) {
    split_lp_[i].Init();
    split_lp_[i].set_f_q<stmlib::FREQUENCY_ACCURATE>(4000.0f / sample_rate_, 0.7071f);
  }
  mask_voiced_ = mask_unvoiced_ = mask_burst_ = mask_mixed_ = 0.0f;
  mask_silence_ = 1.0f;
  voiced_period_ = 200.0f;
  was_voiced_ = false;
  voiced_run_ = 0;
  was_unvoiced_ = false;
  unvoiced_run_ = 0;
  voiced_capture_.Init();
  unvoiced_capture_.Init();
  tails_.Init();
  delay_.Init(delay_buffer, sample_rate_);
  now_ = 0;
  last_edge_ = 0;
  clock_period_ = 0.0f;
  reverb_.Init(reverb_buffer, sample_rate);
  limiter_.Init(sample_rate, 0.94f);
  dry_wet_ = 1.0f;
}

void SplitEngine::Process(
    const SplitParams& p,
    const FloatFrame* in,
    FloatFrame* out,
    bool* gate,
    size_t size) {
  float l[kMaxHalfRateBlock], r[kMaxHalfRateBlock];
  float wet_l[kMaxHalfRateBlock] = { 0.0f }, wet_r[kMaxHalfRateBlock] = { 0.0f };
  float send[kMaxHalfRateBlock] = { 0.0f };
  bool voiced_gate[kMaxHalfRateBlock];
  bridge_.Down(in, l, r, size);
  const size_t n = size / 2;

  // Pitch analysis at 12 kHz.
  float mono[kMaxHalfRateBlock] = { 0.0f }, decimated[kMaxHalfRateBlock];
  for (size_t i = 0; i < n; ++i) mono[i] = 0.5f * (l[i] + r[i]);
  decimator_.Process(mono, decimated, n);
  if (yin_.Process(decimated, n / 2)) {
    classifier_.set_aperiodicity(yin_.aperiodicity());
    if (yin_.aperiodicity() < 0.25f) voiced_period_ = 2.0f * yin_.period();
  }
  classifier_.set_preset(p.preset);
  classifier_.set_bias((p.density - 0.5f) * 1.5f);

  // Controls.
  const float rate = stmlib::SemitonesToRatio(p.pitch);
  float decay = 1.0f;
  const bool tails_on = p.size > 0.03f;
  if (!p.freeze) {
    const float tau = 0.05f * FastExp2(p.size * 6.3219281f);  // 50 ms .. 4 s
    decay = FastExp2(-1.44269504f / (tau * sample_rate_));
  }
  const int zone = std::min(int(p.time * 7.0f), 6);
  const float ratios[7] = { 0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
  float delay_time;
  if (p.clock_patched && clock_period_ > 0.0f) {
    delay_time = clock_period_ * ratios[zone];
  } else {
    delay_time = 0.010f * sample_rate_ * FastExp2(p.time * 6.9068906f);  // 10 ms..1.2 s
  }
  const float feedback = p.feedback * 1.05f;
  const float balance_v = p.shape > 0.5f ? 1.0f : p.shape * 2.0f;
  const float balance_u = p.shape < 0.5f ? 1.0f : (1.0f - p.shape) * 2.0f;
  // Clean-up: CW attenuates consonants (to -24 dB), CCW the noise floor.
  const float clean_u = p.shape > 0.5f ? 1.0f - (p.shape - 0.5f) * 2.0f * 0.937f : 1.0f;
  const float clean_s = p.shape < 0.5f ? 1.0f - (0.5f - p.shape) * 2.0f * 0.99f : 1.0f;
  const float ramp = 1.0f / (0.003f * sample_rate_);

  if (p.capture && voiced_period_ > 0.0f) {
    tails_.Spawn(voiced_capture_, 0, int(2.0f * voiced_period_));
  }

  for (size_t i = 0; i < n; ++i) {
    const float x = mono[i];
    classifier_.Process(x);

    // Clock measurement (delay sync).
    if (p.clock_patched && p.clock_edge >= 0 && size_t(p.clock_edge / 2) == i) {
      const uint32_t interval = now_ - last_edge_;
      last_edge_ = now_;
      if (interval > uint32_t(0.01f * sample_rate_) && interval < uint32_t(2.4f * sample_rate_)) {
        clock_period_ = clock_period_ == 0.0f ? float(interval)
            : clock_period_ + 0.5f * (float(interval) - clock_period_);
      }
    }
    if (!p.clock_patched) clock_period_ = 0.0f;

    // 10 ms look-ahead: the masks see the classification slightly before
    // the audio they are applied to.
    const float xd = lookahead_[lookahead_index_];
    lookahead_[lookahead_index_] = x;
    lookahead_index_ = (lookahead_index_ + 1) % kLookahead;

    // Masks.
    const VuvClass c = classifier_.vuv_class();
    const bool mixed = c == VUV_VOICED && classifier_.mixed();
    const float tv = (c == VUV_VOICED && !mixed) ? 1.0f : 0.0f;
    const float tm = mixed ? 1.0f : 0.0f;
    const float tu = c == VUV_UNVOICED ? 1.0f : 0.0f;
    const float tb = c == VUV_BURST ? 1.0f : 0.0f;
    const float ts = c == VUV_SILENCE ? 1.0f : 0.0f;
#define VOX_RAMP(m, t) m += (m < t ? (std::min(ramp, t - m)) : -std::min(ramp, m - t))
    VOX_RAMP(mask_voiced_, tv);
    VOX_RAMP(mask_mixed_, tm);
    VOX_RAMP(mask_unvoiced_, tu);
    VOX_RAMP(mask_burst_, tb);
    VOX_RAMP(mask_silence_, ts);
#undef VOX_RAMP
    const float sum = mask_voiced_ + mask_mixed_ + mask_unvoiced_ + \
        mask_burst_ + mask_silence_;
    const float norm = sum > 1e-6f ? 1.0f / sum : 0.0f;
    const float mv = mask_voiced_ * norm, mm = mask_mixed_ * norm;
    const float mu = mask_unvoiced_ * norm, mb = mask_burst_ * norm;
    const float ms = mask_silence_ * norm;

    // Exact complementary split for voiced fricatives.
    const float lo = split_lp_[1].Process<stmlib::FILTER_MODE_LOW_PASS>(
        split_lp_[0].Process<stmlib::FILTER_MODE_LOW_PASS>(xd));
    const float hi = xd - lo;

    const float v_stream = xd * mv + lo * mm;
    const float s_stream = xd * ms;
    const float u_stream = xd * mu + hi * mm;
    const float b_stream = xd * mb;

    // Tail capture and spawning (on the delayed, masked signal).
    const bool voiced_now = mv + mm > 0.5f;
    const bool unvoiced_now = mu + mb > 0.5f;
    voiced_capture_.Write(v_stream);
    unvoiced_capture_.Write(u_stream + b_stream);
    if (tails_on && p.submode == SPLIT_SUBMODE_PROCESS && was_voiced_ && \
        !voiced_now && voiced_run_ > int(0.04f * sample_rate_)) {
      // Two periods, taken 20 ms before the end of the vowel.
      tails_.Spawn(voiced_capture_, int(0.020f * sample_rate_),
                   int(2.0f * voiced_period_));
    }
    if (tails_on && p.submode == SPLIT_SUBMODE_SWAP && was_unvoiced_ && \
        !unvoiced_now && unvoiced_run_ > int(0.02f * sample_rate_)) {
      // Noise tail: the last 33 ms of the consonant.
      tails_.Spawn(unvoiced_capture_, int(0.003f * sample_rate_),
                   std::min(int(Tail::kMaxLength), unvoiced_run_));
    }
    voiced_run_ = voiced_now ? voiced_run_ + 1 : 0;
    unvoiced_run_ = unvoiced_now ? unvoiced_run_ + 1 : 0;
    was_voiced_ = voiced_now;
    was_unvoiced_ = unvoiced_now;

    const float tail = tails_.Render(rate, decay) * 0.8f;
    const float v_in = p.mute_voiced ? 0.0f : v_stream;

    float out_l, out_r, s = 0.0f;
    switch (p.submode) {
      case SPLIT_SUBMODE_SPLITTER:
        out_l = v_stream + s_stream;
        out_r = u_stream + b_stream;
        break;
      case SPLIT_SUBMODE_CLEAN_UP:
        out_l = out_r = v_in + s_stream * clean_s + (u_stream + b_stream) * clean_u;
        s = out_l;
        break;
      case SPLIT_SUBMODE_SWAP:
        {
          const float v = v_in + s_stream + delay_.Process(v_in, delay_time, feedback);
          const float u = u_stream + b_stream + tail;
          out_l = out_r = balance_v * v + balance_u * u;
          s = balance_v * v;
        }
        break;
      default:
        {
          const float v = v_in + s_stream + tail;
          const float u_dry = u_stream + b_stream;
          const float u = u_dry + delay_.Process(u_dry, delay_time, feedback);
          out_l = out_r = balance_v * v + balance_u * u;
          s = balance_u * u;
        }
        break;
    }
    wet_l[i] = out_l;
    wet_r[i] = out_r;
    send[i] = s;
    voiced_gate[i] = voiced_now;
    ++now_;
  }

  // Back to 48 kHz.
  bridge_.Up(wet_l, wet_r, out, n);

  // Reverb on the send bus (the selected stream), added to the output.
  if (p.submode != SPLIT_SUBMODE_SPLITTER) {
    FloatFrame bus[2 * kMaxHalfRateBlock];
    float send_up[2 * kMaxHalfRateBlock];
    FloatFrame tmp[2 * kMaxHalfRateBlock];
    bridge_send_.Process(send, send_up, n);
    for (size_t i = 0; i < size; ++i) bus[i].l = bus[i].r = send_up[i];
    std::copy(&bus[0], &bus[size], &tmp[0]);
    const float rv = p.reverb;
    reverb_.set_mix(rv * rv * 0.9f);
    reverb_.set_time(0.35f + 0.6f * rv * rv * rv);
    reverb_.set_lp(0.6f);
    reverb_.Process(bus, size);
    for (size_t i = 0; i < size; ++i) {
      out[i].l += bus[i].l - tmp[i].l;
      out[i].r += bus[i].r - tmp[i].r;
    }
  }

  // Dry/wet against the input delayed by the look-ahead, then limiter.
  const float step = (p.dry_wet - dry_wet_) / float(size);
  for (size_t i = 0; i < size; ++i) {
    const FloatFrame dry = dry_delay_[dry_index_];
    dry_delay_[dry_index_] = in[i];
    dry_index_ = (dry_index_ + 1) % (2 * kLookahead);
    dry_wet_ += step;
    const float w = dry_wet_;
    const float g_wet = w * (2.0f - w);
    const float g_dry = (1.0f - w) * (1.0f + w);
    out[i].l = g_dry * dry.l + g_wet * out[i].l;
    out[i].r = g_dry * dry.r + g_wet * out[i].r;
    limiter_.Process(&out[i].l, &out[i].r);
    gate[i] = voiced_gate[i / 2];
  }
}

}  // namespace vox
