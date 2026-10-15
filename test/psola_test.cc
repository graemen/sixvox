// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Host tests for the vox_psola engine.
//
//   psola_test                         self tests
//   psola_test process in.wav out.wav [--submode N] [--pitch st] [--size x]
//       [--shape x] [--density x] [--time x] [--freeze-at s] [--scale N]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "vox/psola/dsp/psola_engine.h"
#include "vox/test/test_signals.h"
#include "vox/test/test_util.h"
#include "vox/test/wav_io.h"

using namespace vox;
using namespace vox_test;
using beads::FloatFrame;

static const float kSr = 48000.0f;
static const size_t kBlock = 12;

static int16_t speech_storage[VoxSpeechBuffer::kCapacity];
static PitchMark marks[VoxMarkTable::kCapacity];
static uint16_t reverb_buffer[32768];

static PsolaParams DefaultParams() {
  PsolaParams p;
  p.time = 0.0f;
  p.size = 0.5f;
  p.shape = 0.5f;
  p.density = 0.765f;  // 1x
  p.pitch = 0.0f;
  p.feedback = 0.0f;
  p.dry_wet = 1.0f;
  p.reverb = 0.0f;
  p.freeze = false;
  p.hold = false;
  p.trigger = false;
  p.latch = false;
  p.submode = PSOLA_SUBMODE_PSOLA;
  p.scale = PSOLA_SCALE_CHROMATIC;
  return p;
}

static void Run(
    PsolaEngine* e,
    const std::vector<float>& x,
    std::vector<float>* y,
    std::function<void(size_t, PsolaParams*)> params) {
  y->assign(x.size(), 0.0f);
  FloatFrame in[kBlock], out[kBlock];
  bool g[kBlock];
  for (size_t start = 0; start + kBlock <= x.size(); start += kBlock) {
    for (size_t i = 0; i < kBlock; ++i) in[i].l = in[i].r = x[start + i];
    PsolaParams p = DefaultParams();
    params(start, &p);
    e->Process(p, in, out, g, kBlock);
    for (size_t i = 0; i < kBlock; ++i) (*y)[start + i] = out[i].l;
  }
}

// Steady vowel /a/ with a glottal-pulse source; f0 glides linearly.
static std::vector<float> Vowel(float f0_a, float f0_b, float seconds,
                                float silence_after = 0.0f) {
  const size_t n = size_t(seconds * kSr);
  std::vector<float> x(n + size_t(silence_after * kSr), 0.0f);
  Resonator rs[3];
  const float formants[3] = { 700, 1220, 2600 };
  for (int k = 0; k < 3; ++k) rs[k].Init(formants[k], 80.0f + 40.0f * k, kSr);
  float phase = 0.0f, t1 = 0.0f, t2 = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    const float f0 = f0_a + (f0_b - f0_a) * float(i) / float(n);
    phase += f0 / kSr;
    float pulse = 0.0f;
    if (phase >= 1.0f) { phase -= 1.0f; pulse = 1.0f; }
    t1 += 0.03f * (pulse * 40.0f - t1);
    t2 += 0.03f * (t1 - t2);
    float y = 0.0f;
    for (int k = 0; k < 3; ++k) y += rs[k].Process(t2) * (k ? 0.6f : 1.0f);
    const float env = std::min(1.0f, std::min(i / 960.0f, (n - i) / 960.0f));
    x[i] = y * env;
  }
  const float peak = Peak(x);
  for (float& v : x) v *= 0.7f / peak;
  return x;
}

// Fundamental by normalized autocorrelation (smallest strong lag).
static float Pitch(const std::vector<float>& y, size_t a, size_t b) {
  std::vector<float> c(700, 0.0f);
  float best = 0.0f;
  for (size_t lag = 50; lag < 700; ++lag) {
    double s = 0, e1 = 1e-12, e2 = 1e-12;
    for (size_t i = a; i + lag < b; ++i) {
      s += y[i] * y[i + lag]; e1 += y[i] * y[i]; e2 += y[i + lag] * y[i + lag];
    }
    c[lag] = float(s / sqrt(e1 * e2));
    best = std::max(best, c[lag]);
  }
  for (size_t lag = 51; lag + 1 < 700; ++lag) {
    if (c[lag] >= 0.9f * best && c[lag] >= c[lag - 1] && c[lag] >= c[lag + 1]) {
      // Parabolic refinement.
      const float d = 0.5f * (c[lag - 1] - c[lag + 1]) / (c[lag - 1] - 2 * c[lag] + c[lag + 1]);
      return kSr / (float(lag) + d);
    }
  }
  return 0.0f;
}

// LPC spectral envelope (order 14, autocorrelation method on a 2048-sample
// Hann window, analysed at 24 kHz) in dB at 80 points from 150 Hz to 4 kHz.
static std::vector<float> Envelope(const std::vector<float>& y, size_t a,
                                   bool log_axis = false) {
  const int n = 2048, order = 14;
  std::vector<double> x(n);
  for (int i = 0; i < n; ++i) {
    x[i] = y[a + 2 * i] * (0.5 - 0.5 * cos(2 * M_PI * i / n));
  }
  double r[order + 1];
  for (int k = 0; k <= order; ++k) {
    r[k] = 0;
    for (int i = k; i < n; ++i) r[k] += x[i] * x[i - k];
  }
  r[0] *= 1.0001;
  double c[order + 1] = { 1.0 }, tmp[order + 1], e = r[0];
  for (int i = 1; i <= order; ++i) {
    double acc = r[i];
    for (int j = 1; j < i; ++j) acc += c[j] * r[i - j];
    const double k = -acc / e;
    for (int j = 0; j <= order; ++j) tmp[j] = c[j];
    for (int j = 1; j < i; ++j) c[j] = tmp[j] + k * tmp[i - j];
    c[i] = k;
    e *= 1.0 - k * k;
  }
  std::vector<float> env;
  const int points = log_axis ? 241 : 80;
  for (int b = 0; b < points; ++b) {
    // Log axis: 150 Hz * 2^(b / 48), up to 4.8 kHz (quarter-tone steps).
    const double f = log_axis ? 150.0 * pow(2.0, b / 48.0)
                              : 150.0 + (4000.0 - 150.0) * b / 79.0;
    double re = 0, im = 0;
    for (int j = 0; j <= order; ++j) {
      re += c[j] * cos(2 * M_PI * f * j / 24000.0);
      im -= c[j] * sin(2 * M_PI * f * j / 24000.0);
    }
    env.push_back(float(-10.0 * log10(re * re + im * im)));
  }
  return env;
}

// RMS difference (dB) between two envelopes after removing the mean level.
static float EnvelopeDistance(const std::vector<float>& a, const std::vector<float>& b) {
  double ma = 0, mb = 0;
  for (size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
  ma /= a.size(); mb /= b.size();
  double d = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    const double v = (a[i] - ma) - (b[i] - mb);
    d += v * v;
  }
  return float(sqrt(d / a.size()));
}

// Spectral envelope sampled on the harmonics of f0 (DFT at h * f0 over one
// second), interpolated on a log-frequency axis (150 Hz * 2^(b / 48)).
static std::vector<float> HarmonicEnvelope(const std::vector<float>& y, size_t a,
                                           float f0) {
  const size_t n = 24000;
  std::vector<float> h_db;
  for (int h = 1; h * f0 < 5000.0f; ++h) {
    double re = 0, im = 0;
    for (size_t i = 0; i < n; ++i) {
      const double w = 0.5 - 0.5 * cos(2 * M_PI * i / n);
      re += w * y[a + i] * cos(2 * M_PI * f0 * h * i / 48000.0);
      im += w * y[a + i] * sin(2 * M_PI * f0 * h * i / 48000.0);
    }
    h_db.push_back(float(10 * log10(re * re + im * im + 1e-12)));
  }
  std::vector<float> env;
  for (int b = 0; b < 241; ++b) {
    const float f = 150.0f * powf(2.0f, b / 48.0f);
    const float h = f / f0 - 1.0f;  // index into h_db
    const int i = std::max(0, std::min(int(h), int(h_db.size()) - 2));
    const float t = std::max(0.0f, std::min(1.0f, h - float(i)));
    env.push_back(h_db[i] + (h_db[i + 1] - h_db[i]) * t);
  }
  return env;
}

// Shift (semitones) that best maps the input envelope onto the output one,
// on the log-frequency axis (correlation of mean-removed envelopes, over the
// 300 Hz - 3 kHz region).
static float EnvelopeShift(const std::vector<float>& in_raw,
                           const std::vector<float>& out_raw) {
  // Remove the spectral tilt (least-squares line) so that only the formant
  // pattern is compared, then use the Pearson correlation for each shift.
  auto detrend = [](const std::vector<float>& v) {
    const double n = double(v.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < v.size(); ++i) {
      sx += i; sy += v[i]; sxx += double(i) * i; sxy += double(i) * v[i];
    }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    const double offset = (sy - slope * sx) / n;
    std::vector<float> r(v.size());
    for (size_t i = 0; i < v.size(); ++i) r[i] = float(v[i] - (offset + slope * i));
    return r;
  };
  const std::vector<float> in = detrend(in_raw), out = detrend(out_raw);
  float best = -2.0f, best_shift = 0.0f;
  for (int s = -48; s <= 48; ++s) {   // quarter tones, +/- 12 semitones
    double n = 0, mi = 0, mo = 0;
    for (int b = 56; b < 200; ++b) {
      const int j = b - s;
      if (j < 0 || j >= int(in.size())) continue;
      mi += in[j]; mo += out[b]; n += 1;
    }
    mi /= n; mo /= n;
    double sio = 0, sii = 1e-12, soo = 1e-12;
    for (int b = 56; b < 200; ++b) {
      const int j = b - s;
      if (j < 0 || j >= int(in.size())) continue;
      sio += (in[j] - mi) * (out[b] - mo);
      sii += (in[j] - mi) * (in[j] - mi);
      soo += (out[b] - mo) * (out[b] - mo);
    }
    const float c = float(sio / sqrt(sii * soo));
    if (c > best) { best = c; best_shift = s / 4.0f; }
  }
  return best_shift;
}

// Frequency of the envelope maximum between 400 and 1600 Hz (F1 region).
static float __attribute__((unused)) EnvelopePeak(const std::vector<float>& env) {
  size_t best = 0;
  float best_value = -1e9f;
  for (size_t i = 0; i < env.size(); ++i) {
    const float f = 150.0f + (4000.0f - 150.0f) * i / 79.0f;
    if (f >= 400.0f && f <= 1600.0f && env[i] > best_value) {
      best_value = env[i];
      best = i;
    }
  }
  return 150.0f + (4000.0f - 150.0f) * best / 79.0f;
}

static void TestMarks() {
  printf("pitch marks\n");
  std::vector<float> x = Vowel(150.0f, 150.0f, 2.0f), y;
  static PsolaEngine e;
  e.Init(kSr, speech_storage, marks, reverb_buffer);
  Run(&e, x, &y, [](size_t, PsolaParams*) { });
  const VoxMarkTable& t = e.marker().table();
  // Spacing between consecutive voiced marks in the steady part.
  size_t voiced = 0, good = 0;
  for (uint32_t i = t.first() + 1; i <= t.last(); ++i) {
    const PitchMark& a = t.At(i - 1);
    const PitchMark& b = t.At(i);
    if (a.position < 12000 || b.position > 36000) continue;
    if (a.type != MARK_VOICED || b.type != MARK_VOICED) continue;
    ++voiced;
    const float spacing = float(b.position - a.position);
    if (fabsf(spacing - 160.0f) <= 1.0f) ++good;
  }
  Check(voiced > 140 && good >= voiced * 98 / 100, "one mark per period (150 Hz)",
        "%zu of %zu spacings within 1 sample of 160", good, voiced);
}

static void TestPitchShift() {
  printf("pitch shift (formants preserved)\n");
  std::vector<float> x = Vowel(150.0f, 150.0f, 3.0f), y;
  const size_t a = size_t(1.5f * kSr), b = size_t(2.5f * kSr);
  const std::vector<float> env_in = Envelope(x, a);
  // Reference: plain resampling by an octave moves the envelope.
  {
    std::vector<float> resampled(x.size(), 0.0f);
    for (size_t i = 0; 2 * i + 1 < x.size(); ++i) resampled[i + a / 2] = x[2 * i];
    Info("envelope distance of a 2x resampled vowel", "%.1f dB",
         EnvelopeDistance(env_in, Envelope(resampled, a)));
  }
  for (float st : { -12.0f, 7.0f, 12.0f }) {
    static PsolaEngine e;
    e.Init(kSr, speech_storage, marks, reverb_buffer);
    Run(&e, x, &y, [&](size_t, PsolaParams* p) { p->pitch = st; });
    const float f = Pitch(y, a, b);
    const float expected = 150.0f * powf(2.0f, st / 12.0f);
    const float d = EnvelopeDistance(env_in, Envelope(y, a));
    char name[64];
    snprintf(name, sizeof(name), "%+.0f st: F0, spectral envelope kept", st);
    Check(fabsf(f / expected - 1.0f) < 0.01f && d < 4.0f,
          name, "%.1f Hz (%.1f), envelope distance %.1f dB", f, expected, d);
  }
}

static void TestFormantShift() {
  printf("formant shift (SIZE)\n");
  std::vector<float> x = Vowel(150.0f, 150.0f, 3.0f), y;
  const size_t a = size_t(1.5f * kSr), b = size_t(2.5f * kSr);
  const std::vector<float> env_in = HarmonicEnvelope(x, a, 150.0f);
  for (float size : { 0.75f, 0.25f }) {
    static PsolaEngine e;
    e.Init(kSr, speech_storage, marks, reverb_buffer);
    Run(&e, x, &y, [&](size_t, PsolaParams* p) { p->size = size; });
    const float f = Pitch(y, a, b);
    const float shift = EnvelopeShift(env_in, HarmonicEnvelope(y, a, 150.0f));
    const float expected = (size - 0.5f) * 24.0f;
    char name[64];
    snprintf(name, sizeof(name), "SIZE %+.0f st: F0 kept, envelope shifted", expected);
    Check(fabsf(f / 150.0f - 1.0f) < 0.01f && fabsf(shift - expected) <= 1.5f,
          name, "%.1f Hz, envelope %+.2f st", f, shift);
  }
}

static void TestIntonation() {
  printf("intonation (SHAPE)\n");
  // Gliding vowel 110 -> 190 Hz.
  std::vector<float> x = Vowel(110.0f, 190.0f, 3.0f), y;
  auto f0_track = [&](const std::vector<float>& s, std::vector<float>* out) {
    out->clear();
    for (size_t a = size_t(0.5f * kSr); a + 2400 < size_t(2.7f * kSr); a += 2400) {
      out->push_back(Pitch(s, a, a + 2400));
    }
  };
  std::vector<float> in_track, out_track;
  f0_track(x, &in_track);
  {
    static PsolaEngine e;
    e.Init(kSr, speech_storage, marks, reverb_buffer);
    Run(&e, x, &y, [&](size_t, PsolaParams* p) { p->shape = 1.0f; });
    f0_track(y, &out_track);
    float lo = 1e9f, hi = 0.0f;
    for (float f : out_track) { lo = std::min(lo, f); hi = std::max(hi, f); }
    Check(hi / lo < 1.02f && fabsf(lo / 130.81f - 1.0f) < 0.02f,
          "CW: monotone at C3 (PITCH 0)", "%.1f - %.1f Hz", lo, hi);
  }
  {
    static PsolaEngine e;
    e.Init(kSr, speech_storage, marks, reverb_buffer);
    Run(&e, x, &y, [&](size_t, PsolaParams* p) { p->shape = 1.0f; p->pitch = 7.0f; });
    f0_track(y, &out_track);
    const float f = out_track[out_track.size() / 2];
    Check(fabsf(f / (130.81f * 1.4983f) - 1.0f) < 0.02f, "CW + PITCH +7: G3",
          "%.1f Hz", f);
  }
  {
    static PsolaEngine e;
    e.Init(kSr, speech_storage, marks, reverb_buffer);
    Run(&e, x, &y, [&](size_t, PsolaParams* p) { p->shape = 0.0f; });
    f0_track(y, &out_track);
    const float in_range = log2f(in_track.back() / in_track.front());
    const float out_range = log2f(out_track.back() / out_track.front());
    Check(out_range > 2.0f * in_range, "CCW: intonation exaggerated",
          "range %.2f oct vs %.2f oct", out_range, in_range);
  }
  {
    static PsolaEngine e;
    e.Init(kSr, speech_storage, marks, reverb_buffer);
    Run(&e, x, &y, [&](size_t, PsolaParams* p) {
      p->submode = PSOLA_SUBMODE_QUANTIZE;
      p->shape = 1.0f;
      p->scale = PSOLA_SCALE_MAJOR;
    });
    std::vector<float> fine;
    for (size_t a = size_t(0.5f * kSr); a + 1200 < size_t(2.7f * kSr); a += 1200) {
      fine.push_back(Pitch(y, a, a + 1200));
    }
    // Every estimate within 25 cents of a C major note (outside glides).
    const int major[7] = { 0, 2, 4, 5, 7, 9, 11 };
    size_t on = 0;
    for (float f : fine) {
      const float note = 12.0f * log2f(f / 16.3516f);
      float best = 100.0f;
      for (int o = 0; o < 10; ++o) {
        for (int k = 0; k < 7; ++k) best = std::min(best, fabsf(note - (12 * o + major[k])));
      }
      on += best < 0.25f;
    }
    Check(on >= fine.size() * 8 / 10, "yellow: F0 snapped to C major",
          "%zu of %zu estimates within 25 cents", on, fine.size());
  }
}

static void TestTimeStretch() {
  printf("time stretch / scrub\n");
  std::vector<float> x = Vowel(150.0f, 150.0f, 2.0f, 4.0f), y;
  static PsolaEngine e;
  e.Init(kSr, speech_storage, marks, reverb_buffer);
  std::vector<double> positions;
  const size_t freeze_at = size_t(2.1f * kSr);
  Run(&e, x, &y, [&](size_t s, PsolaParams* p) {
    p->freeze = s >= freeze_at;
    p->time = 0.1f;
    p->density = 0.53f + 0.47f * 0.25f;  // 0.5x
    if (s >= freeze_at && s < freeze_at + kBlock) p->trigger = true;
    if (s % 4800 == 0) positions.push_back(e.read_position());
  });
  // Read speed while frozen (positions sampled every 100 ms of output).
  const size_t k0 = size_t((freeze_at + kSr * 0.3f) / 4800);
  const double speed = (positions[k0 + 5] - positions[k0]) / (5 * 2400.0);
  Check(fabs(speed - 0.5) < 0.02, "DENSITY 0.5x: read head speed", "%.3f", speed);
  const size_t a = freeze_at + size_t(0.4f * kSr);
  const float f = Pitch(y, a, a + 24000);
  Check(fabsf(f / 150.0f - 1.0f) < 0.01f, "pitch unchanged while stretched", "%.1f Hz", f);
  // Noon = hold: sustained vowel.
  static PsolaEngine e2;
  e2.Init(kSr, speech_storage, marks, reverb_buffer);
  Run(&e2, x, &y, [&](size_t s, PsolaParams* p) {
    p->freeze = s >= freeze_at;
    p->time = 0.3f;
    p->density = 0.5f;
  });
  const float level_late = Rms(y, y.size() - 48000, y.size());
  const float level_early = Rms(y, freeze_at + 4800, freeze_at + 52800);
  Check(level_late > 0.5f * level_early && level_early > 0.05f,
        "noon: sustained (held) vowel", "%.3f -> %.3f RMS", level_early, level_late);
}

static void TestSpeech() {
  printf("speech through the engine\n");
  GroundTruth gt;
  SpeechLikeOptions o = DefaultSpeechOptions(15.0f, kSr);
  o.seed = 71;
  std::vector<float> x = SpeechLike(o, &gt), y;
  static PsolaEngine e;
  e.Init(kSr, speech_storage, marks, reverb_buffer);
  Run(&e, x, &y, [](size_t, PsolaParams* p) { p->pitch = 5.0f; });
  // Voiced marks inside ground-truth vowels.
  const VoxMarkTable& t = e.marker().table();
  size_t in_vowel = 0, voiced_in_vowel = 0, voiced_outside = 0, outside = 0;
  for (uint32_t i = t.first(); i <= t.last(); ++i) {
    const PitchMark& m = t.At(i);
    const size_t pos48 = size_t(m.position) * 2;
    if (pos48 >= gt.label.size()) continue;
    bool deep = false;
    for (const Span& v : gt.vowels) {
      if (pos48 > v.start + 1920 && pos48 + 1920 < v.end) { deep = true; break; }
    }
    if (deep) { ++in_vowel; voiced_in_vowel += m.type == MARK_VOICED; }
    else if (gt.label[pos48] != LABEL_VOICED) { ++outside; voiced_outside += m.type == MARK_VOICED; }
  }
  Check(voiced_in_vowel >= in_vowel * 95 / 100, "marks inside vowels are voiced",
        "%zu of %zu", voiced_in_vowel, in_vowel);
  Check(voiced_outside <= outside / 10, "marks outside vowels mostly not voiced",
        "%zu of %zu voiced", voiced_outside, outside);
  Check(AllFinite(y) && Peak(y) < 1.2f, "output finite and bounded", "peak %.2f", Peak(y));
}

static void TestScrubClicks() {
  printf("TIME sweep while frozen\n");
  GroundTruth gt;
  SpeechLikeOptions o = DefaultSpeechOptions(8.0f, kSr);
  std::vector<float> x = SpeechLike(o, &gt), y;
  x.resize(x.size() + size_t(4.0f * kSr), 0.0f);
  static PsolaEngine e;
  e.Init(kSr, speech_storage, marks, reverb_buffer);
  const size_t freeze_at = size_t(8.0f * kSr);
  Run(&e, x, &y, [&](size_t s, PsolaParams* p) {
    p->freeze = s >= freeze_at;
    if (s >= freeze_at) p->time = float(s - freeze_at) / (4.0f * kSr);
  });
  float step = 0.0f, input_step = 0.0f;
  for (size_t i = freeze_at + 4800; i < y.size(); ++i) step = std::max(step, fabsf(y[i] - y[i - 1]));
  for (size_t i = 1; i < freeze_at; ++i) input_step = std::max(input_step, fabsf(x[i] - x[i - 1]));
  Check(step <= input_step * 1.5f, "no clicks", "max step %.3f (input %.3f)", step, input_step);
}

static void TestStability() {
  printf("stability (40 s random controls)\n");
  GroundTruth gt;
  SpeechLikeOptions o = DefaultSpeechOptions(40.0f, kSr);
  o.seed = 72;
  std::vector<float> x = SpeechLike(o, &gt), y;
  static PsolaEngine e;
  e.Init(kSr, speech_storage, marks, reverb_buffer);
  TestRandom rng(11);
  PsolaParams q = DefaultParams();
  Run(&e, x, &y, [&](size_t s, PsolaParams* p) {
    if (s % 4800 == 0) {
      q.time = rng.Uniform(); q.size = rng.Uniform(); q.shape = rng.Uniform();
      q.density = rng.Uniform(); q.pitch = rng.Range(-24, 24); q.feedback = rng.Uniform();
      q.dry_wet = rng.Uniform(); q.reverb = rng.Uniform(); q.freeze = rng.Uniform() < 0.3f;
      q.hold = rng.Uniform() < 0.2f; q.latch = rng.Uniform() < 0.2f;
      q.submode = uint8_t(rng.Int(4)); q.scale = uint8_t(rng.Int(4));
    }
    *p = q;
    p->trigger = (s % 9600) < kBlock;
  });
  Check(AllFinite(y), "no NaN / Inf", "ok");
  Check(Peak(y) < 1.5f, "output bounded", "peak %.2f", Peak(y));
}

static void TestPerformance() {
  printf("performance (host, informational)\n");
  GroundTruth gt;
  SpeechLikeOptions o = DefaultSpeechOptions(10.0f, kSr);
  std::vector<float> x = SpeechLike(o, &gt), y;
  static PsolaEngine e;
  e.Init(kSr, speech_storage, marks, reverb_buffer);
  auto t0 = std::chrono::high_resolution_clock::now();
  Run(&e, x, &y, [](size_t, PsolaParams* p) { p->pitch = 24.0f; p->size = 0.25f; p->reverb = 0.5f; });
  auto t1 = std::chrono::high_resolution_clock::now();
  const double s = std::chrono::duration<double>(t1 - t0).count();
  Info("speed (+24 st, formants -6 st: worst-case overlap)", "%.1fx real time (%.0f ns/sample)",
       10.0 / s, 1e9 * s / x.size());
}

static int ProcessFile(int argc, char** argv) {
  if (argc < 4) return 2;
  PsolaParams base = DefaultParams();
  float freeze_at = -1.0f;
  for (int i = 4; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    const float v = float(atof(argv[i + 1]));
    if (a == "--submode") base.submode = uint8_t(v);
    else if (a == "--pitch") base.pitch = v;
    else if (a == "--size") base.size = v;
    else if (a == "--shape") base.shape = v;
    else if (a == "--density") base.density = v;
    else if (a == "--time") base.time = v;
    else if (a == "--scale") base.scale = uint8_t(v);
    else if (a == "--freeze-at") freeze_at = v;
    else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  Audio in;
  if (!ReadWav(argv[2], &in)) return 1;
  Resample(&in, int(kSr));
  std::vector<float> x(in.size()), y;
  for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * (in.l[i] + in.r[i]);
  static PsolaEngine e;
  e.Init(kSr, speech_storage, marks, reverb_buffer);
  Run(&e, x, &y, [&](size_t s, PsolaParams* p) {
    *p = base;
    p->freeze = freeze_at >= 0.0f && s >= size_t(freeze_at * kSr);
  });
  Audio out;
  out.sample_rate = int(kSr);
  out.channels = 2;
  out.l = y;
  for (float& v : out.l) v = std::max(-1.0f, std::min(1.0f, v));
  out.r = out.l;
  printf("%u marks, %u grains\n", e.marker().table().size(), e.grains_started());
  return WriteWav(argv[3], out) ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !strcmp(argv[1], "process")) return ProcessFile(argc, argv);
  TestMarks();
  TestPitchShift();
  TestFormantShift();
  TestIntonation();
  TestTimeStretch();
  TestSpeech();
  TestScrubClicks();
  TestStability();
  TestPerformance();
  return Summary();
}
