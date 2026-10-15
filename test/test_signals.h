// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Synthetic speech-like test material with ground truth, for the host tests.
//
// Structure: phrases (1.5-3 s, separated by 0.45-0.8 s pauses) made of words
// (1-3 syllables, separated by 130-250 ms pauses). A syllable is an onset
// consonant (fricative noise, or a plosive: 25 ms closure + 10 ms burst)
// followed by a vowel (glottal pulse train with a -12 dB/oct source tilt
// through three formant resonators). Each phrase has its own level, F0 range
// and declination. Ground truth: syllable/word/phrase spans, onset positions,
// per-sample F0 (0 when unvoiced) and class labels.

#ifndef VOX_TEST_TEST_SIGNALS_H_
#define VOX_TEST_TEST_SIGNALS_H_

#include <math.h>
#include <stdint.h>

#include <algorithm>
#include <vector>

namespace vox_test {

enum Label {
  LABEL_SILENCE,
  LABEL_VOICED,
  LABEL_UNVOICED,
  LABEL_BURST
};

struct Span {
  size_t start;
  size_t end;
};

struct GroundTruth {
  std::vector<Span> syllables;
  std::vector<Span> vowels;
  std::vector<Span> words;
  std::vector<Span> phrases;
  std::vector<size_t> onsets;     // consonant onsets (syllable starts)
  std::vector<float> f0;          // per sample, Hz, 0 = not voiced
  std::vector<uint8_t> label;     // per sample, Label
  std::vector<float> phrase_level_db;
};

class TestRandom {
 public:
  explicit TestRandom(uint32_t seed) : state_(seed ? seed : 1) { }
  float Uniform() {
    state_ = state_ * 1664525u + 1013904223u;
    return float(state_ >> 8) / 16777216.0f;
  }
  float Bipolar() { return 2.0f * Uniform() - 1.0f; }
  float Range(float a, float b) { return a + (b - a) * Uniform(); }
  int Int(int n) { return int(Uniform() * n) % n; }
 private:
  uint32_t state_;
};

struct Resonator {
  float b0, a1, a2, y1, y2;
  void Init(float f, float bw, float sr) {
    const float r = expf(-float(M_PI) * bw / sr);
    a1 = -2.0f * r * cosf(2.0f * float(M_PI) * f / sr);
    a2 = r * r;
    b0 = 1.0f - r;
    y1 = y2 = 0.0f;
  }
  float Process(float x) {
    const float y = b0 * x - a1 * y1 - a2 * y2;
    y2 = y1;
    y1 = y;
    return y;
  }
};

struct SpeechLikeOptions {
  float seconds;
  float sample_rate;
  uint32_t seed;
  float level_spread_db;   // phrase levels: -6 +/- spread
  float f0_min;
  float f0_max;
  float noise_db;          // background noise level
  float peak;              // final peak normalization
};

inline SpeechLikeOptions DefaultSpeechOptions(float seconds, float sr) {
  SpeechLikeOptions o;
  o.seconds = seconds;
  o.sample_rate = sr;
  o.seed = 1234;
  o.level_spread_db = 4.0f;
  o.f0_min = 90.0f;
  o.f0_max = 220.0f;
  o.noise_db = -66.0f;
  o.peak = 0.9f;
  return o;
}

inline std::vector<float> SpeechLike(const SpeechLikeOptions& o, GroundTruth* gt) {
  const float sr = o.sample_rate;
  TestRandom rng(o.seed);
  const size_t n = size_t(o.seconds * sr);
  std::vector<float> s(n, 0.0f);
  gt->f0.assign(n, 0.0f);
  gt->label.assign(n, LABEL_SILENCE);
  const float vowels[6][3] = {
    { 700, 1220, 2600 }, { 300, 2300, 3000 }, { 500, 1000, 2400 },
    { 400, 1900, 2600 }, { 600, 900, 2500 }, { 350, 800, 2300 }
  };
  auto ms = [&](float x) { return size_t(x * 0.001f * sr); };

  float hp_x = 0.0f, hp_y = 0.0f;
  const float hp_r = expf(-2.0f * float(M_PI) * 2500.0f / sr);
  size_t t = ms(300.0f);
  while (t + ms(1500.0f) < n) {
    const size_t phrase_len = size_t(rng.Range(1.5f, 3.0f) * sr);
    const size_t phrase_end = std::min(n - ms(200.0f), t + phrase_len);
    const float level_db = -6.0f + o.level_spread_db * rng.Bipolar();
    const float level = powf(10.0f, level_db / 20.0f);
    const float f0_base = rng.Range(o.f0_min, o.f0_max * 0.8f);
    Span phrase = { t, 0 };
    size_t p = t;
    while (p + ms(300.0f) < phrase_end) {
      Span word = { p, 0 };
      const int syllables = 1 + rng.Int(3);
      for (int k = 0; k < syllables && p + ms(250.0f) < phrase_end; ++k) {
        Span syllable = { p, 0 };
        gt->onsets.push_back(p);
        // Onset consonant.
        const bool fricative = rng.Uniform() < 0.5f;
        if (fricative) {
          const size_t len = ms(rng.Range(50.0f, 90.0f));
          for (size_t i = 0; i < len && p + i < n; ++i) {
            const float noise = rng.Bipolar();
            hp_y = hp_r * (hp_y + noise - hp_x);  // high-pass ~2.5 kHz
            hp_x = noise;
            const float env = sinf(float(M_PI) * float(i) / float(len));
            s[p + i] += level * 0.5f * env * hp_y;
            gt->label[p + i] = LABEL_UNVOICED;
          }
          p += len;
        } else {
          p += ms(25.0f);  // closure (silence)
          const size_t len = ms(10.0f);
          for (size_t i = 0; i < len && p + i < n; ++i) {
            const float noise = rng.Bipolar();
            const float env = expf(-float(i) / float(ms(3.0f)));
            s[p + i] += level * 0.8f * env * noise;
            gt->label[p + i] = LABEL_BURST;
          }
          p += len;
        }
        // Vowel.
        const int v = rng.Int(6);
        Resonator r[3];
        for (int f = 0; f < 3; ++f) r[f].Init(vowels[v][f], 70.0f + 40.0f * f, sr);
        const size_t len = ms(rng.Range(110.0f, 200.0f));
        Span vowel = { p, p + len };
        float phase = 0.0f, tilt_1 = 0.0f, tilt_2 = 0.0f;
        const float tilt = 1.0f - expf(-2.0f * float(M_PI) * 250.0f / sr);
        const size_t attack = ms(20.0f), release = ms(35.0f);
        for (size_t i = 0; i < len && p + i < n; ++i) {
          const float progress = float(p + i - phrase.start) / float(phrase_len);
          const float f0 = f0_base * (1.15f - 0.3f * progress) * \
              (1.0f + 0.04f * sinf(float(M_PI) * float(i) / float(len)));
          phase += f0 / sr;
          float pulse = 0.0f;
          if (phase >= 1.0f) {
            phase -= 1.0f;
            pulse = 1.0f;
          }
          tilt_1 += tilt * (pulse * 40.0f - tilt_1);
          tilt_2 += tilt * (tilt_1 - tilt_2);
          float y = 0.0f;
          for (int f = 0; f < 3; ++f) {
            y += r[f].Process(tilt_2) * (f == 0 ? 1.0f : (f == 1 ? 0.6f : 0.3f));
          }
          float env = 1.0f;
          if (i < attack) env = float(i) / float(attack);
          if (len - i < release) env = float(len - i) / float(release);
          s[p + i] += level * 2.5f * env * y;
          gt->f0[p + i] = f0;
          gt->label[p + i] = LABEL_VOICED;
        }
        p += len;
        syllable.end = p;
        gt->syllables.push_back(syllable);
        gt->vowels.push_back(vowel);
      }
      word.end = p;
      gt->words.push_back(word);
      p += ms(rng.Range(130.0f, 250.0f));
    }
    phrase.end = p;
    gt->phrases.push_back(phrase);
    gt->phrase_level_db.push_back(level_db);
    t = p + ms(rng.Range(450.0f, 800.0f));
  }
  float peak = 1e-9f;
  for (size_t i = 0; i < n; ++i) peak = std::max(peak, fabsf(s[i]));
  const float noise = powf(10.0f, o.noise_db / 20.0f);
  for (size_t i = 0; i < n; ++i) {
    s[i] = s[i] * (o.peak / peak) + noise * rng.Bipolar();
  }
  return s;
}

// Matches detected boundaries against reference boundaries: fraction of
// references with a detection within +/- tolerance.
inline float BoundaryRecall(
    const std::vector<size_t>& reference,
    const std::vector<size_t>& detected,
    size_t tolerance) {
  if (reference.empty()) return 1.0f;
  size_t hits = 0;
  for (size_t r : reference) {
    for (size_t d : detected) {
      const size_t diff = d > r ? d - r : r - d;
      if (diff <= tolerance) {
        ++hits;
        break;
      }
    }
  }
  return float(hits) / float(reference.size());
}

// Fraction of detections that match a reference within tolerance.
inline float BoundaryPrecision(
    const std::vector<size_t>& reference,
    const std::vector<size_t>& detected,
    size_t tolerance) {
  return BoundaryRecall(detected, reference, tolerance);
}

}  // namespace vox_test

#endif  // VOX_TEST_TEST_SIGNALS_H_
