// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_sequencer engine.

#include "vox/sequencer/dsp/sequencer_engine.h"

#include <algorithm>
#include <cmath>

#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/units.h"

#include "vox/common/dsp/fast_math.h"

namespace vox {

using namespace beads;
using namespace std;

static inline uint32_t NextRandom(uint32_t* state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static inline float RandomFloat(uint32_t* state) {
  return float(NextRandom(state) >> 8) / 16777216.0f;
}

// Fisher-Yates permutation of 0..n-1 from a seed.
static void Permutation(int* out, int n, uint32_t seed) {
  uint32_t state = seed ? seed : 1;
  for (int i = 0; i < n; ++i) out[i] = i;
  for (int i = n - 1; i > 0; --i) {
    const int j = int(NextRandom(&state) % uint32_t(i + 1));
    std::swap(out[i], out[j]);
  }
}

void SequencerEngine::Init(
    float sample_rate,
    int16_t* buffer_storage,
    uint16_t* reverb_buffer) {
  sample_rate_ = sample_rate * 0.5f;
  buffer_.Init(buffer_storage);
  segmenter_.Init(sample_rate_);
  pause_vad_.Init(sample_rate_, 0.012f);
  bridge_.Init();
  output_.Init(sample_rate, reverb_buffer);
  for (int i = 0; i < 2; ++i) {
    grains_[i].Init();
    stretchers_[i].Init();
  }
  voice_ = 0;

  now_ = 0;
  last_edge_ = 0;
  clock_period_ = 0.0f;
  mismatches_ = 0;
  edge_count_ = 0;
  for (int i = 0; i < 3; ++i) intervals_[i] = tap_intervals_[i] = 0.0f;
  internal_phase_ = 0.0f;
  tap_period_ = 0.0f;
  tap_density_ = 0.0f;
  last_tap_ = 0;
  taps_ = 0;
  step_period_ = 0.25f * sample_rate_;
  num_pending_ = 0;

  step_index_ = 0;
  for (int i = 0; i < kMaxSteps; ++i) permutation_[i] = locked_[i] = i;
  was_locked_ = false;
  reset_pending_ = false;
  rng_ = 0x2545f491;
  shuffle_seed_ = 0x9e3779b9;
  previous_unit_ = -1;
  accent_ = false;
  steps_ = 0;
  last_start_ = 0;
  pitch_ = 0.0f;
  silence_run_ = 0;
  fade_in_ = 0;
  input_level_ = 0.0f;
  gate_samples_ = 0;
  accent_samples_ = 0;
}

UnitLevel SequencerEngine::level(const SequencerParams& p) const {
  return (p.submode == SEQUENCER_SUBMODE_WORD_TRUNCATE || \
          p.submode == SEQUENCER_SUBMODE_WORD_STRETCH)
      ? UNIT_LEVEL_WORD
      : UNIT_LEVEL_SYLLABLE;
}

bool SequencerEngine::stretch(const SequencerParams& p) const {
  return p.submode == SEQUENCER_SUBMODE_SYLLABLE_STRETCH || \
      p.submode == SEQUENCER_SUBMODE_WORD_STRETCH;
}

void SequencerEngine::ScheduleStep(const SequencerParams& p, uint32_t when) {
  // Swing: every second step is delayed.
  if (p.swing && (step_index_ & 1)) {
    const float amount = p.swing == 1 ? 0.16f : 0.33f;
    when += uint32_t(amount * step_period_);
  }
  if (when == now_) {
    Step(p, false);
  } else if (num_pending_ < kMaxPending) {
    pending_[num_pending_].when = when;
    pending_[num_pending_].ratchet = false;
    ++num_pending_;
  }
}

void SequencerEngine::ClockEdge(const SequencerParams& p, uint32_t t) {
  const float interval = float(t - last_edge_);
  last_edge_ = t;
  ++edge_count_;
  if (edge_count_ >= 2 && interval > 0.01f * sample_rate_ && \
      interval < 4.0f * sample_rate_) {
    intervals_[2] = intervals_[1];
    intervals_[1] = intervals_[0];
    intervals_[0] = interval;
    if (clock_period_ == 0.0f) {
      clock_period_ = interval;
    } else {
      const float ratio = interval / clock_period_;
      if (ratio > 1.2f || ratio < 0.8f) {
        if (++mismatches_ >= 2) {
          clock_period_ = interval;  // tempo change: re-lock
          mismatches_ = 0;
        }
      } else {
        mismatches_ = 0;
        float a = intervals_[0], b = intervals_[1], c = intervals_[2];
        if (c == 0.0f) c = b == 0.0f ? a : b;
        if (b == 0.0f) b = a;
        const float median = max(min(a, b), min(max(a, b), c));
        clock_period_ += 0.4f * (median - clock_period_);
      }
    }
  }

  // DENSITY selects the clock ratio: /4 /3 /2 x1 x2 x3 x4.
  int zone = int(p.density * 7.0f);
  CONSTRAIN(zone, 0, 6);
  const int divide[7] = { 4, 3, 2, 1, 1, 1, 1 };
  const int multiply[7] = { 1, 1, 1, 1, 2, 3, 4 };
  const int d = divide[zone], m = multiply[zone];
  if (clock_period_ > 0.0f) {
    step_period_ = clock_period_ * float(d) / float(m);
  }
  // A new edge cancels the multiplied steps predicted from the last one.
  int kept = 0;
  for (int i = 0; i < num_pending_; ++i) {
    if (pending_[i].ratchet) pending_[kept++] = pending_[i];
  }
  num_pending_ = kept;
  if ((edge_count_ % d) == 0) {
    ScheduleStep(p, t);
    if (m > 1 && clock_period_ > 0.0f) {
      for (int k = 1; k < m && num_pending_ < kMaxPending; ++k) {
        pending_[num_pending_].when = t + uint32_t(clock_period_ * float(k) / float(m));
        pending_[num_pending_].ratchet = false;
        ++num_pending_;
      }
    }
  }
}

void SequencerEngine::Regenerate(int length) {
  Permutation(permutation_, length, NextRandom(&rng_));
}

int SequencerEngine::UnitForStep(
    const SequencerParams& p,
    int step,
    int pool,
    int length) {
  int zone = int(p.shape * 5.0f);
  CONSTRAIN(zone, 0, 4);
  int k;
  if (p.lock) {
    k = locked_[step] % length;
  } else {
    switch (zone) {
      case 0: k = step; break;
      case 1: k = length - 1 - step; break;
      case 2:
        {
          int perm[kMaxSteps];
          Permutation(perm, length, shuffle_seed_);
          k = perm[step];
        }
        break;
      case 3: k = permutation_[step] % length; break;
      default: k = int(NextRandom(&rng_) % uint32_t(length)); break;
    }
  }
  const int offset = int(p.time * float(pool - 1) + 0.5f);
  // Index counted from the newest unit; step 0 is the earliest of the window.
  int index = p.freeze
      ? pool - 1 - (offset + k)
      : offset + (length - 1 - k);
  index %= pool;
  if (index < 0) index += pool;
  return index;
}

void SequencerEngine::PlayUnit(
    const SequencerParams& p,
    const Unit& u,
    uint32_t duration) {
  const int32_t fade = int32_t(0.003f * sample_rate_);
  grains_[voice_].Release(fade);
  stretchers_[voice_].Release(fade);
  voice_ ^= 1;
  const float rate = stmlib::SemitonesToRatio(pitch_);
  const int32_t attack = int32_t(0.002f * sample_rate_);
  const float ratio = float(u.length()) / float(duration);
  if (stretch(p) && ratio > 0.25f && ratio < 4.0f) {
    stretchers_[voice_].Start(u.start, u.length(), duration, rate, 0.9f, attack);
  } else {
    grains_[voice_].Start(u.start, u.length(), rate, 0.9f, false, attack, fade);
  }
}

void SequencerEngine::Step(const SequencerParams& p, bool ratchet) {
  const UnitTable<kMaxUnits>& t = segmenter_.table(level(p));
  // Live: ignore units about to scroll out of the buffer (1 s margin).
  int pool = t.size();
  if (!p.freeze) {
    const uint32_t limit = buffer_.available() > uint32_t(sample_rate_)
        ? buffer_.available() - uint32_t(sample_rate_) : 0;
    while (pool > 0 && uint32_t(buffer_.head() - t.FromNewest(pool - 1).start) > limit) {
      --pool;
    }
  }
  int length = 1 + int(p.size * 15.0f + 0.5f);
  CONSTRAIN(length, 1, kMaxSteps);

  if (!ratchet) {
    if (reset_pending_) {
      step_index_ = 0;
      reset_pending_ = false;
    }
    if (step_index_ >= length) step_index_ = 0;
    if (step_index_ == 0 && !p.lock) Regenerate(length);
  }

  if (pool > 0) {
    int index;
    int ratchets = 1;
    if (ratchet && previous_unit_ >= 0 && previous_unit_ < pool) {
      index = previous_unit_;
    } else if (!ratchet && previous_unit_ >= 0 && previous_unit_ < pool && \
               RandomFloat(&rng_) < p.repeat) {
      index = previous_unit_;  // stutter
    } else {
      index = UnitForStep(p, step_index_, pool, length);
    }
    if (!ratchet && p.repeat > 0.75f && \
        RandomFloat(&rng_) < (p.repeat - 0.75f) * 4.0f) {
      ratchets = 2 + int(NextRandom(&rng_) % 3u);
    }
    previous_unit_ = index;

    const Unit& u = t.FromNewest(index);
    if (!ratchet) pitch_ = p.pitch;

    // Accent: unit energy in the top quarter of the pool's range.
    float lo = 1000.0f, hi = -1000.0f;
    for (int i = 0; i < pool; ++i) {
      const float e = t.FromNewest(i).energy_db;
      lo = min(lo, e);
      hi = max(hi, e);
    }
    accent_ = u.energy_db >= lo + 0.75f * (hi - lo);

    const uint32_t step_length = uint32_t(step_period_) / uint32_t(ratchets);
    if (buffer_.Contains(u.start) && buffer_.Contains(u.end)) {
      if (ratchets > 1) {
        Unit part = u;
        part.end = u.start + u.length() / uint32_t(ratchets);
        PlayUnit(p, stretch(p) ? part : u, step_length);
        for (int r = 1; r < ratchets && num_pending_ < kMaxPending; ++r) {
          pending_[num_pending_].when = now_ + step_length * uint32_t(r);
          pending_[num_pending_].ratchet = true;
          ++num_pending_;
        }
      } else if (ratchet && stretch(p)) {
        Unit part = u;
        part.end = u.start + uint32_t(float(u.length()) * float(step_length) / step_period_);
        PlayUnit(p, part, step_length);
      } else {
        PlayUnit(p, u, step_length);
      }
      last_start_ = u.start;
    }
    gate_samples_ = int32_t(0.005f * sample_rate_ * 2.0f);
    accent_samples_ = accent_ ? int32_t(float(step_length) * 2.0f) - 48 : 0;
  }
  if (!ratchet) {
    ++step_index_;
    ++steps_;
  }
}

void SequencerEngine::Process(
    const SequencerParams& p,
    const FloatFrame* in,
    FloatFrame* out,
    bool* gate,
    size_t size) {
  float l[kMaxHalfRateBlock], r[kMaxHalfRateBlock], wet[kMaxHalfRateBlock];
  bridge_.Down(in, l, r, size);
  const size_t n = size / 2;

  segmenter_.set_sensitivity(1);
  const int32_t keep = int32_t(0.150f * sample_rate_);
  const int32_t fade = int32_t(0.005f * sample_rate_);

  if (p.reset) reset_pending_ = true;

  // Pattern lock: regenerate the fixed shuffle and freeze the order.
  if (p.lock && !was_locked_) {
    shuffle_seed_ = NextRandom(&rng_);
    const int length = 1 + int(p.size * 15.0f + 0.5f);
    int zone = int(p.shape * 5.0f);
    CONSTRAIN(zone, 0, 4);
    int perm[kMaxSteps];
    Permutation(perm, length, shuffle_seed_);
    for (int s = 0; s < kMaxSteps; ++s) {
      if (zone == 0) locked_[s] = s;
      else if (zone == 1) locked_[s] = length - 1 - (s % length);
      else if (zone == 2) locked_[s] = perm[s % length];
      else if (zone == 3) locked_[s] = permutation_[s % length];
      else locked_[s] = int(NextRandom(&rng_) % uint32_t(length));
    }
  }
  was_locked_ = p.lock;

  // Internal clock tempo, or tap tempo.
  if (!p.clock_patched) {
    clock_period_ = 0.0f;
    edge_count_ = 0;
    if (p.tap) {
      const uint32_t interval = now_ - last_tap_;
      last_tap_ = now_;
      if (interval < uint32_t(2.0f * sample_rate_)) {
        tap_intervals_[2] = tap_intervals_[1];
        tap_intervals_[1] = tap_intervals_[0];
        tap_intervals_[0] = float(interval);
        ++taps_;
        const int k = taps_ >= 3 ? 3 : taps_;
        float sum = 0.0f;
        for (int i = 0; i < k; ++i) sum += tap_intervals_[i];
        tap_period_ = sum / float(k);
        tap_density_ = p.density;
      } else {
        taps_ = 0;
      }
    }
    if (tap_period_ > 0.0f && fabsf(p.density - tap_density_) > 0.05f) {
      tap_period_ = 0.0f;  // DENSITY moved: back to the knob tempo
    }
    if (tap_period_ > 0.0f) {
      step_period_ = tap_period_ * 0.25f;  // taps are quarter notes
    } else {
      const float bpm = 40.0f * FastExp2(p.density * 2.5849625f);  // 40-240
      step_period_ = sample_rate_ * 60.0f / (bpm * 4.0f);
    }
  } else if (p.tap) {
    ScheduleStep(p, now_);  // manual advance
  }

  for (size_t i = 0; i < n; ++i) {
    const float x = 0.5f * (l[i] + r[i]);
    input_level_ += (fabsf(x) > input_level_ ? 0.05f : 0.0002f) * \
        (fabsf(x) - input_level_);

    // Recording (with optional pause removal).
    pause_vad_.Process(x);
    if (!p.freeze) {
      bool write = true;
      if (p.pause_removal) {
        silence_run_ = pause_vad_.speech() ? 0 : silence_run_ + 1;
        if (silence_run_ >= keep) {
          write = false;
          fade_in_ = fade;
        }
      }
      if (write) {
        float s = x;
        if (fade_in_ > 0) {
          s *= 1.0f - float(fade_in_) / float(fade);
          --fade_in_;
        }
        buffer_.Write(s);
        segmenter_.Process(s, buffer_.head() - 1);
      }
    }

    // Clock.
    if (p.clock_patched) {
      if (p.clock_edge >= 0 && size_t(p.clock_edge / 2) == i) {
        ClockEdge(p, now_);
      }
    } else {
      internal_phase_ += 1.0f / step_period_;
      if (internal_phase_ >= 1.0f) {
        internal_phase_ -= 1.0f;
        ScheduleStep(p, now_);
      }
    }

    // Pending events.
    for (int k = 0; k < num_pending_; ) {
      if (int32_t(now_ - pending_[k].when) >= 0) {
        const bool ratchet = pending_[k].ratchet;
        pending_[k] = pending_[--num_pending_];
        Step(p, ratchet);
      } else {
        ++k;
      }
    }

    float y = 0.0f;
    for (int v = 0; v < 2; ++v) {
      y += grains_[v].Render(buffer_);
      y += stretchers_[v].Render(buffer_);
    }
    wet[i] = y;
    ++now_;
  }

  segmenter_.Prune(buffer_.oldest(), buffer_.head());

  bridge_.Up(wet, wet, out, n);
  output_.Process(in, out, p.dry_wet, p.reverb, size);

  for (size_t i = 0; i < size; ++i) {
    if (p.gate_mode == SEQUENCER_GATE_ACCENT) {
      gate[i] = accent_samples_ > 0;
    } else {
      gate[i] = gate_samples_ > 0;
    }
    if (gate_samples_ > 0) --gate_samples_;
    if (accent_samples_ > 0) --accent_samples_;
  }
}

}  // namespace vox
