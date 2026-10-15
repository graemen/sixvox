// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// "Speaking chip": a lo-fi LPC vocoder in the style of early-1980s speech
// synthesis chips, built from original parameters (no ROM data copied).
//
// - 8 kHz internal rate (the 24 kHz speech is decimated by 3), order 10,
//   25 ms frames, parameters interpolated in 8 steps per frame.
// - Reflection coefficients quantized as log-area ratios on a uniform grid
//   (5, 5, 4, 4, 3, 3, 3, 3, 3, 2 bits), energy 4 bits (4 dB steps), pitch
//   6 bits (log scale, 50-400 Hz), 1 voicing bit.
// - Voiced excitation: a 32-sample decaying chirp repeated at the pitch
//   period. Unvoiced: a 1-bit pseudo-random sequence.
// - Output quantized to 8 bits; the 8 kHz samples are held (zero-order
//   hold) at a "chip clock" that SIZE can detune (pitch and formants move
//   together, like detuning the chip's clock).

#ifndef VOX_LPC_DSP_SPEAKING_CHIP_H_
#define VOX_LPC_DSP_SPEAKING_CHIP_H_

#include <stdint.h>

#include <algorithm>
#include <cmath>

#include "stmlib/stmlib.h"
#include "stmlib/dsp/sample_rate_converter.h"

#include "beads/dsp/sample_rate_conversion_filters.h"

#include "vox/common/analysis/lpc.h"
#include "vox/common/dsp/fast_math.h"

namespace vox {

class SpeakingChip {
 public:
  SpeakingChip() { }
  ~SpeakingChip() { }

  enum {
    kOrder = 10,
    kFrame = 200,      // 25 ms at 8 kHz
    kWindow = 240,
    kSteps = 8,
    kChirp = 32,
  };

  void Init() {
    decimator_.Init();
    std::fill(&history_[0], &history_[kWindow], 0.0f);
    history_count_ = 0;
    frame_count_ = 0;
    for (int i = 0; i <= kOrder; ++i) k_[i] = k_from_[i] = k_to_[i] = 0.0f;
    energy_ = energy_from_ = energy_to_ = 0.0f;
    period_ = period_from_ = period_to_ = 80.0f;
    voiced_ = false;
    synthesis_.Init();
    step_ = 0;
    step_count_ = 0;
    excitation_phase_ = 0;
    lfsr_ = 0x1u;
    clock_phase_ = 1.0f;
    held_ = 0.0f;
    previous_ = 0.0f;
    deemphasis_ = 0.0f;
    // Original voiced excitation: decaying chirp, zero mean, unit energy.
    float sum = 0.0f, energy = 0.0f;
    for (int i = 0; i < kChirp; ++i) {
      const float t = float(i) / float(kChirp);
      chirp_[i] = sinf(2.0f * float(M_PI) * (1.5f * t + 2.5f * t * t)) * expf(-4.0f * t);
      sum += chirp_[i];
    }
    for (int i = 0; i < kChirp; ++i) {
      chirp_[i] -= sum / float(kChirp);
      energy += chirp_[i] * chirp_[i];
    }
    for (int i = 0; i < kChirp; ++i) chirp_[i] /= sqrtf(energy);
  }

  // Feeds the 24 kHz source (6 samples per block) and the voicing / pitch
  // at the read position; returns 6 samples at 24 kHz into out.
  // clock_ratio: chip clock relative to 8 kHz.
  void Process(const float* in, size_t n, bool voiced, float period_24k,
               float clock_ratio, float* out) {
    float decimated[8];
    decimator_.Process(in, decimated, n);
    for (size_t i = 0; i < n / 3; ++i) {
      // Pre-emphasis 0.9375 (15/16).
      const float x = decimated[i] - 0.9375f * previous_;
      previous_ = decimated[i];
      history_[history_count_ % kWindow] = x;
      ++history_count_;
      if (++frame_count_ >= kFrame) {
        frame_count_ = 0;
        Analyze(voiced, period_24k / 3.0f);
      }
    }
    // Synthesis at the chip clock, zero-order hold to 24 kHz.
    const float increment = 8000.0f * clock_ratio / 24000.0f;
    for (size_t i = 0; i < n; ++i) {
      clock_phase_ += increment;
      while (clock_phase_ >= 1.0f) {
        clock_phase_ -= 1.0f;
        held_ = Synthesize();
      }
      out[i] = held_;
    }
  }

 private:
  // Log-area-ratio quantization of a reflection coefficient.
  static float QuantizeK(float k, int bits, float lar_min, float lar_max) {
    CONSTRAIN(k, -0.99f, 0.99f);
    float lar = logf((1.0f + k) / (1.0f - k));
    CONSTRAIN(lar, lar_min, lar_max);
    const int levels = (1 << bits) - 1;
    const float q = floorf((lar - lar_min) / (lar_max - lar_min) * levels + 0.5f);
    lar = lar_min + q * (lar_max - lar_min) / levels;
    const float e = expf(lar);
    return (e - 1.0f) / (e + 1.0f);
  }

  void Analyze(bool voiced, float period_8k) {
    float w[kWindow], r[kOrder + 1], a[kOrder + 1], k[kOrder + 1];
    for (int i = 0; i < kWindow; ++i) {
      const float x = history_[(history_count_ + i) % kWindow];
      w[i] = x * (0.54f - 0.46f * cosf(2.0f * float(M_PI) * i / (kWindow - 1)));
    }
    Autocorrelation(w, kWindow, kOrder, r);
    r[0] = r[0] * 1.0001f + 1e-10f;
    const float error = Levinson(r, kOrder, k, a);
    // Quantized parameters.
    static const int bits[kOrder] = { 5, 5, 4, 4, 3, 3, 3, 3, 3, 2 };
    static const float lar_range[kOrder][2] = {
      { -4.0f, 1.5f }, { -2.0f, 3.0f }, { -2.5f, 2.0f }, { -2.0f, 2.0f },
      { -1.6f, 1.6f }, { -1.4f, 1.4f }, { -1.2f, 1.2f }, { -1.2f, 1.2f },
      { -1.0f, 1.0f }, { -1.0f, 1.0f },
    };
    for (int i = 1; i <= kOrder; ++i) {
      k_from_[i] = k_to_[i];
      k_to_[i] = QuantizeK(k[i], bits[i - 1], lar_range[i - 1][0], lar_range[i - 1][1]);
    }
    // Energy: residual RMS, 4 bits in 4 dB steps below full scale.
    const float rms = sqrtf(error / float(kWindow) * 2.5f + 1e-12f);
    float db = 20.0f * log10f(rms);
    int level = int(floorf(-db / 4.0f + 0.5f));
    CONSTRAIN(level, 0, 15);
    energy_from_ = energy_to_;
    energy_to_ = level == 15 ? 0.0f : powf(10.0f, -4.0f * level / 20.0f);
    // Pitch: 6 bits, log scale 50-400 Hz (period 160 .. 20 at 8 kHz).
    voiced_ = voiced;
    float p = period_8k;
    CONSTRAIN(p, 20.0f, 160.0f);
    const float lp = log2f(p / 20.0f) / 3.0f;  // 0..1
    const float pq = floorf(lp * 63.0f + 0.5f) / 63.0f;
    period_from_ = period_to_;
    period_to_ = 20.0f * powf(2.0f, pq * 3.0f);
    step_ = 0;
    step_count_ = 0;
  }

  float Synthesize() {
    // Parameter interpolation, 8 steps per frame.
    if (step_count_ == 0) {
      const float t = float(step_ + 1) / float(kSteps);
      for (int i = 1; i <= kOrder; ++i) k_[i] = k_from_[i] + (k_to_[i] - k_from_[i]) * t;
      energy_ = energy_from_ + (energy_to_ - energy_from_) * t;
      period_ = period_from_ + (period_to_ - period_from_) * t;
      if (step_ < kSteps - 1) ++step_;
    }
    if (++step_count_ >= kFrame / kSteps) step_count_ = 0;

    float e;
    if (voiced_) {
      e = excitation_phase_ < kChirp ? chirp_[excitation_phase_] * sqrtf(period_) : 0.0f;
      if (++excitation_phase_ >= int(period_)) excitation_phase_ = 0;
    } else {
      lfsr_ = (lfsr_ >> 1) ^ (uint32_t(-int32_t(lfsr_ & 1u)) & 0xB400u);
      e = (lfsr_ & 1u) ? 1.0f : -1.0f;
    }
    float y = synthesis_.Process(e * energy_, k_, kOrder);
    // De-emphasis and 8-bit output.
    deemphasis_ = y + 0.9375f * deemphasis_;
    float out = deemphasis_;
    CONSTRAIN(out, -1.0f, 127.0f / 128.0f);
    return floorf(out * 128.0f + 0.5f) / 128.0f;
  }

  stmlib::SampleRateConverter<stmlib::SRC_DOWN, 3, 48> decimator_;
  float history_[kWindow];
  uint32_t history_count_;
  int frame_count_;
  float k_[kOrder + 1], k_from_[kOrder + 1], k_to_[kOrder + 1];
  float energy_, energy_from_, energy_to_;
  float period_, period_from_, period_to_;
  bool voiced_;
  LatticeSynthesis synthesis_;
  int step_;
  int step_count_;
  int excitation_phase_;
  uint32_t lfsr_;
  float clock_phase_;
  float held_;
  float previous_;
  float deemphasis_;
  float chirp_[kChirp];

  DISALLOW_COPY_AND_ASSIGN(SpeakingChip);
};

}  // namespace vox

#endif  // VOX_LPC_DSP_SPEAKING_CHIP_H_
