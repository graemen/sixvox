// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_syllable engine.

#include "vox/syllable/dsp/syllable_engine.h"

#include <algorithm>
#include <cmath>

#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/units.h"

#include "vox/common/dsp/fast_math.h"

namespace vox {

using namespace beads;
using namespace std;

static const int kNumSlots = SyllableEngine::kNumVoices + 2;

void SyllableEngine::Init(
    float sample_rate,
    int16_t* buffer_storage,
    uint16_t* reverb_buffer) {
  sample_rate_ = sample_rate * 0.5f;
  buffer_.Init(buffer_storage);
  segmenter_.Init(sample_rate_);
  pause_vad_.Init(sample_rate_, 0.012f);
  bridge_.Init();
  output_.Init(sample_rate, reverb_buffer);
  for (int i = 0; i < kNumVoices; ++i) voices_[i].Init();
  phase_ = 0.0f;
  rng_ = 0x12345678;
  silence_run_ = 0;
  fade_in_ = 0;
  latched_ = false;
  latch_voice_ = -1;
  feedback_sample_ = 0.0f;
  gain_ = 1.0f;
  input_level_ = 0.0f;
  onsets_seen_ = 0;
  words_seen_ = 0;
  gate_samples_ = 0;
  triggers_ = 0;
}

static inline float RandomFloat(uint32_t* state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return float(x >> 8) / 16777216.0f;
}

UnitLevel SyllableEngine::level(const SyllableParams& p) const {
  switch (p.submode) {
    case SYLLABLE_SUBMODE_WORD:
      return UNIT_LEVEL_WORD;
    case SYLLABLE_SUBMODE_ONSET_SLICE:
      return UNIT_LEVEL_ONSET_SLICE;
    default:
      return UNIT_LEVEL_SYLLABLE;
  }
}

bool SyllableEngine::SelectUnit(
    const SyllableParams& p,
    uint32_t* start,
    uint32_t* length,
    bool* reverse) {
  const UnitTable<kMaxUnits>& t = segmenter_.table(level(p));
  const int n = t.size();
  if (n == 0) return false;

  // TIME: live = counted back from the newest unit; frozen = from the oldest.
  int index = int(p.time * float(n - 1) + 0.5f);
  if (p.freeze) index = n - 1 - index;  // convert to "from newest"

  // SHAPE: scatter around the addressed unit.
  const float sh = p.shape;
  const int spread = sh < 0.5f
      ? int(sh * 4.0f + 0.5f)
      : int(2.0f + (sh - 0.5f) * 28.0f + 0.5f);
  if (spread) {
    index += int(RandomFloat(&rng_) * float(2 * spread + 1)) - spread;
  }
  CONSTRAIN(index, 0, n - 1);

  const Unit& u = t.FromNewest(index);
  uint32_t s = u.start;
  uint32_t l;
  if (p.size < 0.5f) {
    // Fraction of the unit, from its start (down to the onset only).
    const float f = 0.15f + 0.85f * (p.size * 2.0f);
    l = uint32_t(float(u.length()) * f);
    const uint32_t min_length = uint32_t(0.02f * sample_rate_);
    if (l < min_length) l = min_length;
  } else {
    // 1..8 consecutive units.
    int count = 1 + int((p.size - 0.5f) * 14.0f + 0.5f);
    int last = index - (count - 1);
    if (last < 0) last = 0;
    l = t.FromNewest(last).end - s;
  }
  if (!buffer_.Contains(s) || !buffer_.Contains(s + l + 2)) {
    return false;
  }
  *start = s;
  *length = l;
  *reverse = sh > 0.75f && RandomFloat(&rng_) < (sh - 0.75f) * 2.0f;
  return true;
}

void SyllableEngine::StartGrain(
    uint32_t start,
    uint32_t length,
    float rate,
    bool reverse,
    float shape) {
  // Voice allocation: at most kNumVoices sounding grains; the oldest one is
  // faded out (3 ms) when a new grain needs room.
  int sounding = 0;
  int oldest = -1;
  uint32_t oldest_age = 0;
  for (int i = 0; i < kNumVoices; ++i) {
    if (voices_[i].active() && !voices_[i].releasing()) {
      ++sounding;
      if (voices_[i].age() >= oldest_age) {
        oldest_age = voices_[i].age();
        oldest = i;
      }
    }
  }
  const int32_t steal_fade = int32_t(0.003f * sample_rate_);
  int slot = -1;
  for (int i = 0; i < kNumVoices; ++i) {
    if (!voices_[i].active()) {
      slot = i;
      break;
    }
  }
  if (slot < 0 || sounding >= kNumVoices - 1) {
    if (oldest >= 0) voices_[oldest].Release(steal_fade);
  }
  if (slot < 0) {
    // Every slot busy (some releasing): cut the oldest releasing voice.
    uint32_t age = 0;
    for (int i = 0; i < kNumVoices; ++i) {
      if (voices_[i].releasing() && voices_[i].age() >= age) {
        age = voices_[i].age();
        slot = i;
      }
    }
    if (slot < 0) slot = 0;
  }
  const int32_t attack = int32_t((0.0015f + 0.006f * shape) * sample_rate_);
  const int32_t release = int32_t((0.003f + 0.020f * shape) * sample_rate_);
  voices_[slot].Start(start, length, rate, 0.9f, reverse, attack, release);
  if (latched_) latch_voice_ = slot;
  ++triggers_;
}

void SyllableEngine::Trigger(const SyllableParams& p) {
  uint32_t start, length;
  bool reverse;
  if (!SelectUnit(p, &start, &length, &reverse)) return;
  latch_start_ = start;
  latch_length_ = length;
  latch_reverse_ = reverse;
  StartGrain(start, length, stmlib::SemitonesToRatio(p.pitch), reverse, p.shape);
}

void SyllableEngine::Process(
    const SyllableParams& p,
    const FloatFrame* in,
    FloatFrame* out,
    bool* gate,
    size_t size) {
  float l[kMaxHalfRateBlock], r[kMaxHalfRateBlock], wet[kMaxHalfRateBlock];
  bridge_.Down(in, l, r, size);
  const size_t n = size / 2;

  segmenter_.set_sensitivity(p.sensitivity);
  const bool pause_removal = p.pause_removal || \
      p.submode == SYLLABLE_SUBMODE_SYLLABLE_PAUSE_REMOVAL;
  const int32_t keep = int32_t(0.150f * sample_rate_);
  const int32_t fade = int32_t(0.005f * sample_rate_);
  const float rate = stmlib::SemitonesToRatio(p.pitch);

  // Block-rate triggers (button / SEED gate).
  if (p.trigger) Trigger(p);

  // Latch: loop the last triggered unit seamlessly.
  if (p.latch && !latched_) {
    latched_ = true;
    latch_voice_ = -1;
    if (latch_length_ == 0 || !buffer_.Contains(latch_start_)) {
      Trigger(p);
    } else {
      StartGrain(latch_start_, latch_length_, rate, latch_reverse_, p.shape);
    }
  } else if (!p.latch) {
    latched_ = false;
  }

  // Trigger generator (DENSITY), only when the SEED jack is not patched.
  const float d = p.density;
  float regular_rate = 0.0f, random_rate = 0.0f;
  if (!p.seed_patched) {
    if (d > 0.55f) {
      regular_rate = 0.5f * FastExp2((d - 0.55f) / 0.45f * 5.3219281f);
    } else if (d < 0.45f) {
      random_rate = 0.5f * FastExp2((0.45f - d) / 0.45f * 5.3219281f);
    }
  }

  float target_gain = 1.0f;
  for (size_t i = 0; i < n; ++i) {
    const float x = 0.5f * (l[i] + r[i]);
    input_level_ += (fabsf(x) > input_level_ ? 0.05f : 0.0002f) * \
        (fabsf(x) - input_level_);

    // Recording.
    pause_vad_.Process(x);
    const bool speech = pause_vad_.speech();
    if (!p.freeze) {
      bool write = true;
      if (pause_removal) {
        silence_run_ = speech ? 0 : silence_run_ + 1;
        if (silence_run_ >= keep) {
          write = false;
          fade_in_ = fade;
        }
      }
      if (write) {
        float s = x + p.feedback * 0.95f * feedback_sample_;
        s = stmlib::SoftLimit(s);
        if (fade_in_ > 0) {
          s *= 1.0f - float(fade_in_) / float(fade);
          --fade_in_;
        }
        buffer_.Write(s);
        segmenter_.Process(s, buffer_.head() - 1);
      }
    }

    // Trigger generator.
    if (regular_rate > 0.0f) {
      phase_ += regular_rate / sample_rate_;
      if (phase_ >= 1.0f) {
        phase_ -= 1.0f;
        Trigger(p);
      }
    } else if (random_rate > 0.0f) {
      if (RandomFloat(&rng_) < random_rate / sample_rate_) Trigger(p);
    }

    // Latch re-trigger, one release time before the end.
    if (latched_ && latch_voice_ >= 0) {
      const Grain& v = voices_[latch_voice_];
      const int32_t release = int32_t((0.003f + 0.020f * p.shape) * sample_rate_);
      if (!v.active() || ((v.releasing() || v.remaining() <= release) && \
                          int32_t(v.age()) >= release)) {
        StartGrain(latch_start_, latch_length_, rate, latch_reverse_, p.shape);
      }
    }

    // Render.
    float y = 0.0f;
    int sounding = 0;
    for (int v = 0; v < kNumVoices; ++v) {
      if (voices_[v].active()) {
        y += voices_[v].Render(buffer_);
        sounding += !voices_[v].releasing();
      }
    }
    target_gain = 1.0f / sqrtf(float(sounding > 1 ? sounding : 1));
    gain_ += 0.002f * (target_gain - gain_);
    y *= gain_;
    feedback_sample_ = y;
    wet[i] = y;
  }

  segmenter_.Prune(buffer_.oldest(), buffer_.head());

  bridge_.Up(wet, wet, out, n);
  output_.Process(in, out, p.dry_wet, p.reverb, size);

  // Gate output: 5 ms trigger on each new onset (word starts in word mode).
  const bool word_mode = level(p) == UNIT_LEVEL_WORD;
  const uint32_t events = word_mode ? segmenter_.word_starts() : segmenter_.onsets();
  uint32_t& seen = word_mode ? words_seen_ : onsets_seen_;
  if (events != seen) {
    seen = events;
    gate_samples_ = int32_t(0.005f * sample_rate_ * 2.0f);
  }
  if (word_mode) onsets_seen_ = segmenter_.onsets();
  else words_seen_ = segmenter_.word_starts();
  for (size_t i = 0; i < size; ++i) {
    gate[i] = gate_samples_ > 0;
    if (gate_samples_ > 0) --gate_samples_;
  }
}

}  // namespace vox
