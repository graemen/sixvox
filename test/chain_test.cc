// Copyright 2026 VOX contributors. MIT license.
//
// Host test harness for the vox_chain DSP engine.
//
//   chain_test selftest [--write-dir DIR]
//       Runs the unit/acceptance tests on synthetic signals. Exit code 0 when
//       everything passes. With --write-dir, renders example files.
//
//   chain_test process in.wav out.wav [--bed bed.wav] [options]
//       Processes a file exactly as the firmware would (12-frame blocks,
//       48 kHz; other rates are resampled). Options (knob positions 0..1):
//         --preset 0..3   (0 podcast, 1 broadcast, 2 announcer, 3 raw)
//         --amount --time --tilt --character --deess --gate --mix --reverb
//         --duck-cv -1..1   --bypass   --normalize (peak-normalize input to
//         0.9 first, standing in for the module's auto-gain)
//       Prints loudness / gain reduction statistics.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "vox/chain/dsp/chain_engine.h"
#include "vox/common/dsp/fast_math.h"
#include "vox/test/wav_io.h"

using namespace vox;
using namespace vox_test;
using beads::FloatFrame;

static const float kSr = 48000.0f;
static const size_t kBlock = 12;

static uint16_t reverb_buffer[32768];

// ---------------------------------------------------------------------------
// Utilities

static int failures = 0;
static int checks = 0;

static void Check(bool ok, const char* name, const char* fmt, ...) {
  ++checks;
  char detail[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(detail, sizeof(detail), fmt, args);
  va_end(args);
  printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", name, detail);
  if (!ok) ++failures;
}

static ChainParams NeutralParams() {
  ChainParams p;
  p.amount = 0.0f;
  p.time = 0.5f;
  p.tilt = 0.5f;
  p.character = 0.5f;
  p.deess = 0.0f;
  p.gate = 0.0f;
  p.dry_wet = 1.0f;
  p.reverb = 0.0f;
  p.duck_cv = 0.0f;
  p.bypass = false;
  p.push = false;
  p.mute = false;
  p.bed_present = false;
  p.preset = CHAIN_PRESET_PODCAST;
  return p;
}

// Runs the engine over a whole signal in firmware-sized blocks. A callback
// can change parameters before each block.
template<typename F>
static void Run(
    ChainEngine* engine,
    const std::vector<float>& voice,
    const std::vector<float>* bed,
    std::vector<float>* out_l,
    std::vector<float>* out_r,
    std::vector<bool>* vad,
    F params_for_block) {
  const size_t n = voice.size();
  out_l->resize(n);
  if (out_r) out_r->resize(n);
  if (vad) vad->resize(n);
  FloatFrame in[kBlock], out[kBlock];
  bool v[kBlock];
  for (size_t start = 0; start < n; start += kBlock) {
    const size_t size = std::min(kBlock, n - start);
    for (size_t i = 0; i < size; ++i) {
      in[i].l = voice[start + i];
      in[i].r = bed ? (*bed)[start + i] : voice[start + i];
    }
    ChainParams p = params_for_block(start);
    engine->Process(p, in, out, v, size);
    for (size_t i = 0; i < size; ++i) {
      (*out_l)[start + i] = out[i].l;
      if (out_r) (*out_r)[start + i] = out[i].r;
      if (vad) (*vad)[start + i] = v[i];
    }
  }
}

static std::vector<float> Sine(float freq, float amplitude, float seconds) {
  std::vector<float> s(size_t(seconds * kSr));
  for (size_t i = 0; i < s.size(); ++i) {
    s[i] = amplitude * sinf(2.0f * float(M_PI) * freq * i / kSr);
  }
  return s;
}

static float Rms(const std::vector<float>& x, size_t a, size_t b) {
  double s = 0.0;
  for (size_t i = a; i < b; ++i) s += double(x[i]) * x[i];
  return sqrtf(float(s / double(b - a)));
}

static float Db(float x) { return 20.0f * log10f(std::max(x, 1e-9f)); }

static uint32_t rng_state = 1;
static float Random() {
  rng_state = rng_state * 1664525u + 1013904223u;
  return float(rng_state >> 8) / 16777216.0f;
}
static float RandomBipolar() { return 2.0f * Random() - 1.0f; }

// ITU-R BS.1770 K-weighting + gated integrated loudness (mono).
struct KWeighting {
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0, u1 = 0, u2 = 0, z1 = 0, z2 = 0;
  float Process(float x) {
    const double b0 = 1.53512485958697, b1 = -2.69169618940638,
        b2 = 1.19839281085285, a1 = -1.69065929318241, a2 = 0.73248077421585;
    double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    const double c1 = -1.99004745483398, c2 = 0.99007225036621;
    double z = y - 2.0 * u1 + u2 - c1 * z1 - c2 * z2;
    u2 = u1; u1 = y; z2 = z1; z1 = z;
    return float(z);
  }
};

// Ungated K-weighted loudness of a segment (used per phrase).
static float SegmentLoudness(const std::vector<float>& x, size_t a, size_t b) {
  KWeighting k;
  double p = 0.0;
  for (size_t i = a; i < b; ++i) {
    const float w = k.Process(x[i]);
    p += double(w) * w;
  }
  return float(-0.691 + 10.0 * log10(p / double(b - a) + 1e-20));
}

static float IntegratedLoudness(const std::vector<float>& x, size_t a, size_t b) {
  KWeighting k;
  std::vector<float> w(b - a);
  for (size_t i = a; i < b; ++i) w[i - a] = k.Process(x[i]);
  const size_t block = size_t(0.4f * kSr), hop = block / 4;
  std::vector<double> powers;
  for (size_t s = 0; s + block <= w.size(); s += hop) {
    double p = 0;
    for (size_t i = s; i < s + block; ++i) p += double(w[i]) * w[i];
    powers.push_back(p / block);
  }
  auto loudness = [](double p) { return -0.691 + 10.0 * log10(p + 1e-20); };
  double sum = 0; size_t count = 0;
  for (double p : powers) if (loudness(p) > -70.0) { sum += p; ++count; }
  if (!count) return -100.0f;
  const double relative_gate = loudness(sum / count) - 10.0;
  sum = 0; count = 0;
  for (double p : powers) if (loudness(p) > relative_gate) { sum += p; ++count; }
  return count ? float(loudness(sum / count)) : -100.0f;
}

// ---------------------------------------------------------------------------
// Synthetic speech-like material: phrases of syllables (fricative/plosive
// onsets + formant-filtered glottal pulse vowels), random phrase levels.

struct Resonator {
  float b0, a1, a2, y1 = 0, y2 = 0;
  void Init(float f, float bw) {
    const float r = expf(-float(M_PI) * bw / kSr);
    a1 = -2.0f * r * cosf(2.0f * float(M_PI) * f / kSr);
    a2 = r * r;
    b0 = 1.0f - r;
  }
  float Process(float x) {
    float y = b0 * x - a1 * y1 - a2 * y2;
    y2 = y1; y1 = y;
    return y;
  }
};

struct Phrase { size_t start, end; float level_db; };

static std::vector<float> SpeechLike(
    float seconds, uint32_t seed, float level_spread_db,
    std::vector<Phrase>* phrases) {
  rng_state = seed;
  const size_t n = size_t(seconds * kSr);
  std::vector<float> s(n, 0.0f);
  const float vowels[5][3] = {
    { 700, 1220, 2600 }, { 300, 2300, 3000 }, { 500, 1000, 2400 },
    { 400, 1900, 2600 }, { 600, 900, 2500 }
  };
  size_t t = size_t(0.3f * kSr);
  float hp_state = 0.0f, hp_prev = 0.0f;
  while (t < n) {
    const size_t phrase_len = size_t((1.5f + 1.5f * Random()) * kSr);
    const float level_db = -6.0f + level_spread_db * RandomBipolar();
    const float level = powf(10.0f, level_db / 20.0f);
    const float f0_base = 100.0f + 80.0f * Random();
    Phrase ph = { t, std::min(n, t + phrase_len), level_db };
    size_t p = t;
    while (p < ph.end) {
      // Onset consonant.
      const bool fricative = Random() < 0.5f;
      const size_t c_len = size_t((fricative ? 0.06f : 0.012f) * kSr);
      for (size_t i = 0; i < c_len && p + i < n; ++i) {
        float noise = RandomBipolar();
        float hp = noise - hp_prev + 0.6f * hp_state;  // crude high-pass
        hp_prev = noise;
        hp_state = hp;
        float env = sinf(float(M_PI) * i / c_len);
        s[p + i] += level * (fricative ? 0.35f : 0.6f) * env * hp;
      }
      p += c_len;
      // Vowel.
      const int v = int(Random() * 5) % 5;
      Resonator r[3];
      for (int k = 0; k < 3; ++k) r[k].Init(vowels[v][k], 80.0f + 40.0f * k);
      const size_t v_len = size_t((0.12f + 0.08f * Random()) * kSr);
      float phase = 0.0f;
      float tilt_1 = 0.0f, tilt_2 = 0.0f;  // -12 dB/oct glottal source tilt
      const float tilt_coefficient = 1.0f - expf(-2.0f * float(M_PI) * 250.0f / kSr);
      for (size_t i = 0; i < v_len && p + i < n; ++i) {
        const float progress = float(p + i - ph.start) / phrase_len;
        const float f0 = f0_base * (1.1f - 0.2f * progress);
        phase += f0 / kSr;
        float pulse = 0.0f;
        if (phase >= 1.0f) { phase -= 1.0f; pulse = 1.0f; }
        tilt_1 += tilt_coefficient * (pulse * 40.0f - tilt_1);
        tilt_2 += tilt_coefficient * (tilt_1 - tilt_2);
        pulse = tilt_2;
        float y = 0.0f;
        for (int k = 0; k < 3; ++k) y += r[k].Process(pulse) * (k == 0 ? 1.0f : 0.5f);
        float env = sinf(float(M_PI) * i / v_len);
        s[p + i] += level * 2.5f * env * y;
      }
      p += v_len + size_t(0.03f * Random() * kSr);
    }
    phrases->push_back(ph);
    t = ph.end + size_t((0.3f + 0.3f * Random()) * kSr);
  }
  for (size_t i = 0; i < n; ++i) s[i] += 0.0005f * RandomBipolar();  // -66 dB
  float peak = 0.0f;
  for (float x : s) peak = std::max(peak, fabsf(x));
  for (float& x : s) x *= 0.9f / peak;  // stand-in for the auto-gain
  return s;
}

// ---------------------------------------------------------------------------
// Tests

static void TestFastMath() {
  printf("fast math\n");
  float max_log_err = 0.0f, max_exp_err = 0.0f;
  for (float x = 1e-6f; x < 100.0f; x *= 1.01f) {
    max_log_err = std::max(max_log_err, fabsf(FastLog2(x) - log2f(x)));
  }
  for (float x = -30.0f; x < 30.0f; x += 0.013f) {
    max_exp_err = std::max(max_exp_err, fabsf(FastExp2(x) / exp2f(x) - 1.0f));
  }
  Check(max_log_err < 1e-4f, "FastLog2 accuracy", "max err %.2e", max_log_err);
  Check(max_exp_err < 1e-5f, "FastExp2 accuracy", "max rel err %.2e", max_exp_err);
  Check(fabsf(DbToGain(-6.0206f) - 0.5f) < 1e-4f, "DbToGain(-6.02 dB)", "%.5f",
        DbToGain(-6.0206f));
}

static void TestBypassNull() {
  printf("bypass\n");
  ChainEngine e;
  e.Init(kSr, reverb_buffer);
  std::vector<float> x = Sine(440.0f, 0.5f, 1.0f), y;
  for (size_t i = 0; i < x.size(); ++i) x[i] += 0.1f * RandomBipolar();
  ChainParams p = NeutralParams();
  p.bypass = true;
  p.amount = 0.8f;
  p.character = 1.0f;
  Run(&e, x, NULL, &y, NULL, NULL, [&](size_t) { return p; });
  float err = 0.0f;
  for (size_t i = size_t(0.1f * kSr); i < x.size(); ++i) {
    err = std::max(err, fabsf(y[i] - x[i]));
  }
  Check(err < 1e-5f, "bypass is a null (after 10 ms fade)", "max err %.2e", err);

  // Click-free toggling: bypass flips every 100 ms on a 1 kHz sine with the
  // character set to telephone (very different wet signal). The largest
  // sample-to-sample step must stay close to that of the signals themselves.
  e.Init(kSr, reverb_buffer);
  x = Sine(1000.0f, 0.5f, 1.0f);
  p = NeutralParams();
  p.character = 0.85f;
  Run(&e, x, NULL, &y, NULL, NULL, [&](size_t start) {
    ChainParams q = p;
    q.bypass = (start / size_t(0.1f * kSr)) & 1;
    return q;
  });
  float max_step = 0.0f;
  for (size_t i = size_t(0.2f * kSr); i < y.size(); ++i) {
    max_step = std::max(max_step, fabsf(y[i] - y[i - 1]));
  }
  // Steady-state step of the processed telephone signal, for reference.
  ChainEngine e2;
  e2.Init(kSr, reverb_buffer);
  std::vector<float> y2;
  Run(&e2, x, NULL, &y2, NULL, NULL, [&](size_t) { return p; });
  float ref_step = 2.0f * float(M_PI) * 1000.0f / kSr * 0.5f;
  for (size_t i = size_t(0.2f * kSr); i < y2.size(); ++i) {
    ref_step = std::max(ref_step, fabsf(y2[i] - y2[i - 1]));
  }
  Check(max_step <= ref_step * 1.05f, "bypass toggle click-free",
        "max step %.3f vs signal %.3f", max_step, ref_step);
}

static void TestNeutralTransparency() {
  printf("neutral settings\n");
  const float freqs[] = { 200.0f, 1000.0f, 4000.0f, 12000.0f };
  for (float f : freqs) {
    ChainEngine e;
    e.Init(kSr, reverb_buffer);
    std::vector<float> x = Sine(f, 0.25f, 1.0f), y;
    ChainParams p = NeutralParams();
    Run(&e, x, NULL, &y, NULL, NULL, [&](size_t) { return p; });
    const float gain = Db(Rms(y, size_t(0.5f * kSr), x.size())) - \
        Db(Rms(x, size_t(0.5f * kSr), x.size()));
    char name[64];
    snprintf(name, sizeof(name), "neutral gain at %.0f Hz", f);
    Check(fabsf(gain) < 0.25f, name, "%+.2f dB", gain);
  }
}

static void TestCompressorCurve() {
  printf("compressor\n");
  // Static curve: threshold -20, ratio 4, knee 6, no make-up.
  Compressor c;
  c.Init(kSr);
  c.set(-20.0f, 4.0f, 6.0f, 0.005f, 0.100f, 0.0f);
  const float levels[] = { -40.0f, -23.0f, -20.0f, -17.0f, -10.0f, 0.0f };
  float worst = 0.0f;
  for (float level_db : levels) {
    // Sine with RMS = level_db.
    const float amplitude = powf(10.0f, level_db / 20.0f) * sqrtf(2.0f);
    std::vector<float> x = Sine(1000.0f, amplitude, 1.0f);
    float g = 1.0f;
    for (float s : x) g = c.Process(s, 0.0f);
    const float measured = Db(g);
    const float expected = c.GainComputer(level_db);
    worst = std::max(worst, fabsf(measured - expected));
  }
  Check(worst < 0.5f, "static curve matches design (+/-0.5 dB)",
        "worst deviation %.2f dB", worst);

  // Attack: a 20 dB step above threshold reaches 90% of the final GR in
  // about 2.2 attack time constants.
  c.Init(kSr);
  c.set(-30.0f, 8.0f, 0.0f, 0.005f, 0.200f, 0.0f);
  std::vector<float> quiet = Sine(1000.0f, 0.01f, 0.5f);
  std::vector<float> loud = Sine(1000.0f, 0.7f, 0.5f);
  for (float s : quiet) c.Process(s, 0.0f);
  float final_gr = 0.0f;
  {
    Compressor c2;
    c2.Init(kSr);
    c2.set(-30.0f, 8.0f, 0.0f, 0.005f, 0.200f, 0.0f);
    for (float s : loud) c2.Process(s, 0.0f);
    final_gr = c2.gain_reduction_db();
  }
  size_t t90 = 0;
  for (size_t i = 0; i < loud.size(); ++i) {
    c.Process(loud[i], 0.0f);
    if (!t90 && c.gain_reduction_db() <= 0.9f * final_gr) t90 = i;
  }
  const float t90_ms = 1000.0f * t90 / kSr;
  Check(t90_ms > 4.0f && t90_ms < 25.0f, "attack time (5 ms setting)",
        "90%% of %.1f dB GR in %.1f ms", final_gr, t90_ms);
}

static void TestDeEsser() {
  printf("de-esser\n");
  // Sibilant: band-limited noise around 7 kHz. Vowel: 300 Hz + harmonics.
  const size_t one_second = 48000;
  std::vector<float> sib(one_second), vowel(one_second);
  stmlib::Svf bp;
  bp.Init();
  bp.set_f_q<stmlib::FREQUENCY_ACCURATE>(7000.0f / kSr, 2.0f);
  for (size_t i = 0; i < sib.size(); ++i) {
    sib[i] = 0.5f * bp.Process<stmlib::FILTER_MODE_BAND_PASS_NORMALIZED>(RandomBipolar());
    vowel[i] = 0.3f * sinf(2.0f * float(M_PI) * 300.0f * i / kSr) + \
        0.1f * sinf(2.0f * float(M_PI) * 600.0f * i / kSr);
  }
  DeEsser d;
  d.Init(kSr);
  d.set_depth(1.0f);
  std::vector<float> y(sib.size());
  for (size_t i = 0; i < sib.size(); ++i) y[i] = d.Process(sib[i]);
  const float sib_reduction = Db(Rms(sib, 24000, 48000)) - Db(Rms(y, 24000, 48000));
  d.Init(kSr);
  d.set_depth(1.0f);
  for (size_t i = 0; i < vowel.size(); ++i) y[i] = d.Process(vowel[i]);
  const float vowel_reduction = Db(Rms(vowel, 24000, 48000)) - Db(Rms(y, 24000, 48000));
  Check(sib_reduction > 6.0f, "sibilance attenuated at full depth",
        "%.1f dB", sib_reduction);
  Check(fabsf(vowel_reduction) < 0.3f, "vowel untouched", "%.2f dB", vowel_reduction);
  d.Init(kSr);
  d.set_depth(0.0f);
  float err = 0.0f;
  for (size_t i = 0; i < sib.size(); ++i) err = std::max(err, fabsf(d.Process(sib[i]) - sib[i]));
  Check(err < 1e-5f, "depth 0 is transparent (split reconstructs)", "max err %.2e", err);
}

static void TestExpander() {
  printf("expander / gate\n");
  ChainEngine e;
  e.Init(kSr, reverb_buffer);
  ChainParams p = NeutralParams();
  p.gate = 0.8f;  // threshold ~ -35 dBFS
  std::vector<float> quiet = Sine(1000.0f, 0.001f, 1.0f), y;   // -60 dBFS
  Run(&e, quiet, NULL, &y, NULL, NULL, [&](size_t) { return p; });
  const float att = Db(Rms(quiet, 24000, 48000)) - Db(Rms(y, 24000, 48000));
  Check(att > 20.0f, "noise floor gated (-60 dBFS in)", "%.1f dB down", att);
  e.Init(kSr, reverb_buffer);
  std::vector<float> loud = Sine(1000.0f, 0.1f, 1.0f);  // -20 dBFS
  Run(&e, loud, NULL, &y, NULL, NULL, [&](size_t) { return p; });
  const float loss = Db(Rms(loud, 24000, 48000)) - Db(Rms(y, 24000, 48000));
  Check(fabsf(loss) < 0.3f, "speech level passes (-20 dBFS in)", "%.2f dB", loss);
}

static void TestDucker() {
  printf("ducker\n");
  // Voice: 1 s of silence, 1 s of -12 dBFS tone, 1.5 s silence. Bed: steady.
  const size_t n = size_t(3.5f * kSr);
  std::vector<float> voice(n, 0.0f), bed(n), y_l, y_r;
  for (size_t i = 0; i < n; ++i) {
    bed[i] = 0.2f * sinf(2.0f * float(M_PI) * 220.0f * i / kSr);
    if (i >= size_t(kSr) && i < size_t(2.0f * kSr)) {
      voice[i] = 0.25f * sinf(2.0f * float(M_PI) * 500.0f * i / kSr);
    }
  }
  ChainEngine e;
  e.Init(kSr, reverb_buffer);
  ChainParams p = NeutralParams();
  p.bed_present = true;
  p.preset = CHAIN_PRESET_BROADCAST;  // -15 dB duck
  std::vector<bool> vad;
  size_t onset_12db = 0, release_1db = 0;
  Run(&e, voice, &bed, &y_l, &y_r, &vad, [&](size_t start) {
    if (start >= size_t(kSr) && !onset_12db && e.bed_gain_db() <= -12.0f) {
      onset_12db = start;
    }
    if (start >= size_t(2.0f * kSr) && !release_1db && e.bed_gain_db() > -1.0f) {
      release_1db = start;
    }
    return p;
  });
  const float onset_ms = 1000.0f * (onset_12db - kSr) / kSr;
  const float release_ms = 1000.0f * (release_1db - 2.0f * kSr) / kSr;
  Check(onset_12db && onset_ms < 30.0f, "bed -12 dB within 30 ms of speech",
        "%.1f ms", onset_ms);
  Check(release_1db && release_ms < 500.0f, "bed back within 1 dB in 500 ms",
        "%.1f ms (includes 150 ms hold)", release_ms);
  // VAD.
  size_t vad_on = 0;
  for (size_t i = size_t(kSr); i < size_t(2.0f * kSr); ++i) vad_on += vad[i];
  size_t vad_false = 0;
  for (size_t i = 0; i < size_t(0.9f * kSr); ++i) vad_false += vad[i];
  Check(vad_on > size_t(0.95f * kSr), "VAD on during speech",
        "%.1f%%", 100.0f * vad_on / kSr);
  Check(vad_false == 0, "VAD off in silence", "%zu samples", vad_false);
}

static void TestLoudnessConsistency(const char* write_dir) {
  printf("loudness consistency (synthetic speech, +/-9 dB phrase levels)\n");
  std::vector<Phrase> phrases;
  std::vector<float> x = SpeechLike(30.0f, 1234, 9.0f, &phrases), y;
  const char* names[] = { "podcast", "broadcast", "announcer", "raw" };
  float spread_in = 0.0f;
  {
    float lo = 1000.0f, hi = -1000.0f;
    for (const Phrase& ph : phrases) {
      if (ph.end - ph.start < size_t(kSr)) continue;
      const float l = SegmentLoudness(x, ph.start, ph.end);
      lo = std::min(lo, l); hi = std::max(hi, l);
    }
    spread_in = hi - lo;
  }
  printf("  input: %zu phrases, phrase loudness spread %.1f LU, "
         "integrated %.1f LUFS\n", phrases.size(), spread_in,
         IntegratedLoudness(x, 0, x.size()));
  for (int preset = 0; preset < CHAIN_PRESET_LAST; ++preset) {
    ChainEngine e;
    e.Init(kSr, reverb_buffer);
    ChainParams p = NeutralParams();
    p.preset = preset;
    p.amount = 0.75f;
    p.deess = 0.3f;
    p.gate = 0.2f;
    std::vector<float> yr;
    Run(&e, x, NULL, &y, &yr, NULL, [&](size_t) { return p; });
    float lo = 1000.0f, hi = -1000.0f;
    for (const Phrase& ph : phrases) {
      if (ph.end - ph.start < size_t(kSr)) continue;
      const float l = SegmentLoudness(y, ph.start, ph.end);
      lo = std::min(lo, l); hi = std::max(hi, l);
    }
    const float spread = hi - lo;
    const float reduction = 100.0f * (1.0f - spread / spread_in);
    char name[96];
    snprintf(name, sizeof(name), "%s @ DENSITY 0.75: spread reduced", names[preset]);
    if (preset == CHAIN_PRESET_BROADCAST) {
      Check(reduction >= 60.0f, name, "%.1f -> %.1f LU (%.0f%%), %.1f LUFS",
            spread_in, spread, reduction, IntegratedLoudness(y, 0, y.size()));
    } else {
      printf("  [INFO] %-46s %.1f -> %.1f LU (%.0f%%), %.1f LUFS\n", name,
             spread_in, spread, reduction, IntegratedLoudness(y, 0, y.size()));
    }
    if (write_dir) {
      Audio a;
      a.sample_rate = int(kSr);
      a.channels = 2;
      a.l = y;
      a.r = yr;
      WriteWav(std::string(write_dir) + "/speechlike_" + names[preset] + ".wav", a);
    }
  }
  if (write_dir) {
    Audio a;
    a.sample_rate = int(kSr);
    a.channels = 2;
    a.l = x;
    a.r = x;
    WriteWav(std::string(write_dir) + "/speechlike_input.wav", a);
  }
}

static void TestStability() {
  printf("stability (60 s random controls, noise + bursts)\n");
  ChainEngine e;
  e.Init(kSr, reverb_buffer);
  rng_state = 99;
  const size_t n = size_t(60.0f * kSr);
  std::vector<float> x(n), bed(n), y_l, y_r;
  for (size_t i = 0; i < n; ++i) {
    const float burst = ((i / 4800) % 7 == 0) ? 4.0f : 0.3f;  // hot bursts
    x[i] = burst * RandomBipolar();
    bed[i] = 0.5f * RandomBipolar();
  }
  ChainParams p = NeutralParams();
  Run(&e, x, &bed, &y_l, &y_r, NULL, [&](size_t start) {
    if ((start % 2400) == 0) {  // new random settings every 50 ms
      p.amount = Random(); p.time = Random(); p.tilt = Random();
      p.character = Random(); p.deess = Random(); p.gate = Random();
      p.dry_wet = Random(); p.reverb = Random(); p.duck_cv = RandomBipolar();
      p.bypass = Random() < 0.1f; p.push = Random() < 0.3f;
      p.mute = Random() < 0.1f; p.bed_present = Random() < 0.5f;
      p.preset = uint8_t(Random() * 4) & 3;
    }
    return p;
  });
  size_t bad = 0;
  float peak = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    if (!std::isfinite(y_l[i]) || !std::isfinite(y_r[i])) ++bad;
    else peak = std::max(peak, std::max(fabsf(y_l[i]), fabsf(y_r[i])));
  }
  Check(bad == 0, "no NaN / Inf", "%zu bad samples", bad);
  Check(peak < 50.0f, "output bounded", "peak %.2f (platform clips at 1.0)", peak);
}

static void TestCharacterModels() {
  printf("character models\n");
  const float positions[] = { 0.0f, 0.25f, 0.5f, 0.66f, 0.83f, 1.0f };
  const char* names[] = { "lo-fi (full)", "lo-fi (half)", "clean", "radio",
                          "telephone", "megaphone" };
  std::vector<Phrase> phrases;
  std::vector<float> x = SpeechLike(5.0f, 7, 3.0f, &phrases), y;
  for (int i = 0; i < 6; ++i) {
    ChainEngine e;
    e.Init(kSr, reverb_buffer);
    ChainParams p = NeutralParams();
    p.character = positions[i];
    Run(&e, x, NULL, &y, NULL, NULL, [&](size_t) { return p; });
    const float diff = IntegratedLoudness(y, 0, y.size()) - \
        IntegratedLoudness(x, 0, x.size());
    char name[64];
    snprintf(name, sizeof(name), "%s level vs clean input", names[i]);
    Check(fabsf(diff) < 6.0f, name, "%+.1f LU", diff);
  }
}

static void TestPerformance() {
  printf("performance (host, informational)\n");
  ChainEngine e;
  e.Init(kSr, reverb_buffer);
  std::vector<float> x(size_t(10.0f * kSr)), y_l, y_r;
  for (float& s : x) s = 0.3f * RandomBipolar();
  ChainParams p = NeutralParams();
  p.amount = 0.6f; p.deess = 0.5f; p.gate = 0.3f; p.character = 0.7f;
  p.reverb = 0.5f; p.bed_present = true;
  auto t0 = std::chrono::high_resolution_clock::now();
  Run(&e, x, &x, &y_l, &y_r, NULL, [&](size_t) { return p; });
  auto t1 = std::chrono::high_resolution_clock::now();
  const double seconds = std::chrono::duration<double>(t1 - t0).count();
  printf("  [INFO] %.1fx real time on this host (%.0f ns/sample)\n",
         10.0 / seconds, 1e9 * seconds / x.size());
}

static int SelfTest(const char* write_dir) {
  TestFastMath();
  TestBypassNull();
  TestNeutralTransparency();
  TestCompressorCurve();
  TestDeEsser();
  TestExpander();
  TestDucker();
  TestCharacterModels();
  TestLoudnessConsistency(write_dir);
  TestStability();
  TestPerformance();
  printf("\n%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// File processing

static int ProcessFile(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: chain_test process in.wav out.wav [options]\n");
    return 2;
  }
  std::string in_path = argv[2], out_path = argv[3], bed_path;
  ChainParams p = NeutralParams();
  p.amount = 0.5f;
  p.deess = 0.3f;
  bool normalize = false;
  for (int i = 4; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](float* v) {
      if (i + 1 < argc) *v = float(atof(argv[++i]));
    };
    float preset = 0.0f;
    if (a == "--bed" && i + 1 < argc) bed_path = argv[++i];
    else if (a == "--preset") { next(&preset); p.preset = uint8_t(preset); }
    else if (a == "--amount") next(&p.amount);
    else if (a == "--time") next(&p.time);
    else if (a == "--tilt") next(&p.tilt);
    else if (a == "--character") next(&p.character);
    else if (a == "--deess") next(&p.deess);
    else if (a == "--gate") next(&p.gate);
    else if (a == "--mix") next(&p.dry_wet);
    else if (a == "--reverb") next(&p.reverb);
    else if (a == "--duck-cv") next(&p.duck_cv);
    else if (a == "--bypass") p.bypass = true;
    else if (a == "--normalize") normalize = true;
    else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  Audio in, bed;
  if (!ReadWav(in_path, &in)) return 1;
  Resample(&in, int(kSr));
  if (normalize) {
    float peak = 1e-9f;
    for (float s : in.l) peak = std::max(peak, fabsf(s));
    for (float& s : in.l) s *= 0.9f / peak;
  }
  std::vector<float>* bed_ptr = NULL;
  if (!bed_path.empty()) {
    if (!ReadWav(bed_path, &bed)) return 1;
    Resample(&bed, int(kSr));
    bed.l.resize(in.size(), 0.0f);
    bed_ptr = &bed.l;
    p.bed_present = true;
  }
  ChainEngine e;
  e.Init(kSr, reverb_buffer);
  Audio out;
  out.sample_rate = int(kSr);
  out.channels = 2;
  double gr_sum = 0.0;
  float gr_max = 0.0f;
  size_t blocks = 0;
  Run(&e, in.l, bed_ptr, &out.l, &out.r, NULL, [&](size_t) {
    gr_sum += e.gain_reduction_db();
    gr_max = std::min(gr_max, e.gain_reduction_db());
    ++blocks;
    return p;
  });
  for (size_t i = 0; i < out.size(); ++i) {
    out.l[i] = std::max(-1.0f, std::min(0.999999f, out.l[i]));
    out.r[i] = std::max(-1.0f, std::min(0.999999f, out.r[i]));
  }
  if (!WriteWav(out_path, out)) return 1;
  printf("in : %.1f LUFS integrated\n", IntegratedLoudness(in.l, 0, in.size()));
  printf("out: %.1f LUFS integrated\n", IntegratedLoudness(out.l, 0, out.size()));
  printf("gain reduction: mean %.1f dB, max %.1f dB\n",
         gr_sum / std::max<size_t>(blocks, 1), gr_max);
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !strcmp(argv[1], "selftest")) {
    const char* dir = NULL;
    if (argc >= 4 && !strcmp(argv[2], "--write-dir")) dir = argv[3];
    return SelfTest(dir);
  }
  if (argc >= 2 && !strcmp(argv[1], "process")) {
    return ProcessFile(argc, argv);
  }
  fprintf(stderr, "usage: chain_test selftest [--write-dir DIR]\n"
                  "       chain_test process in.wav out.wav [options]\n");
  return 2;
}
