// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Host tests for the vox_lpc engine and the shared LPC primitives.
//
//   lpc_test                           self tests
//   lpc_test process voice.wav out.wav [--carrier carrier.wav] [--submode N]
//       [--shape x] [--size x] [--pitch st] [--density x] [--slew x]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "vox/lpc/dsp/lpc_engine.h"
#include "vox/test/test_signals.h"
#include "vox/test/test_util.h"
#include "vox/test/wav_io.h"

using namespace vox;
using namespace vox_test;
using beads::FloatFrame;

static const float kSr = 48000.0f;
static const size_t kBlock = 12;

static int16_t speech_storage[VoxSpeechBuffer::kCapacity];
static VoiceTrackEntry track[LpcEngine::kTrackSize];
static uint16_t reverb_buffer[32768];

static LpcParams DefaultParams() {
  LpcParams p;
  p.time = 0.0f;
  p.size = 0.5f;
  p.shape = 0.5f;      // residual
  p.density = 0.765f;  // 1x
  p.pitch = 0.0f;
  p.slew = 0.0f;
  p.dry_wet = 1.0f;
  p.reverb = 0.0f;
  p.freeze = false;
  p.hold = false;
  p.trigger = false;
  p.latch = false;
  p.smooth_f0 = false;
  p.carrier_present = false;
  p.submode = LPC_SUBMODE_RESYNTH;
  return p;
}

static void Run(
    LpcEngine* e,
    const std::vector<float>& voice,
    const std::vector<float>* carrier,
    std::vector<float>* y,
    std::function<void(size_t, LpcParams*)> params,
    double* worst_block_seconds = NULL) {
  y->assign(voice.size(), 0.0f);
  std::vector<double> times;
  FloatFrame in[kBlock], out[kBlock];
  bool g[kBlock];
  double worst = 0.0;
  for (size_t start = 0; start + kBlock <= voice.size(); start += kBlock) {
    for (size_t i = 0; i < kBlock; ++i) {
      in[i].l = voice[start + i];
      in[i].r = carrier ? (*carrier)[start + i] : voice[start + i];
    }
    LpcParams p = DefaultParams();
    p.carrier_present = carrier != NULL;
    params(start, &p);
    auto t0 = std::chrono::high_resolution_clock::now();
    e->Process(p, in, out, g, kBlock);
    auto t1 = std::chrono::high_resolution_clock::now();
    if (worst_block_seconds) times.push_back(std::chrono::duration<double>(t1 - t0).count());
    for (size_t i = 0; i < kBlock; ++i) (*y)[start + i] = out[i].l;
  }
  if (worst_block_seconds && !times.empty()) {
    // 99.9th percentile (robust to host scheduling hiccups).
    std::sort(times.begin(), times.end());
    worst = times[times.size() * 999 / 1000];
    *worst_block_seconds = worst;
  }
}

static std::vector<float> Vowel(float f0_a, float f0_b, float seconds, float silence = 0.0f) {
  const size_t n = size_t(seconds * kSr);
  std::vector<float> x(n + size_t(silence * kSr), 0.0f);
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

static float Pitch(const std::vector<float>& y, size_t a, size_t b, float* strength = NULL) {
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
  if (strength) *strength = best;
  for (size_t lag = 51; lag + 1 < 700; ++lag) {
    if (c[lag] >= 0.9f * best && c[lag] >= c[lag - 1] && c[lag] >= c[lag + 1]) {
      const float d = 0.5f * (c[lag - 1] - c[lag + 1]) / (c[lag - 1] - 2 * c[lag] + c[lag + 1]);
      return kSr / (float(lag) + d);
    }
  }
  return 0.0f;
}

// Smooth spectral envelope: LPC (order 14) of a 2048-sample window at 24 kHz,
// on a log-frequency axis (150 Hz * 2^(b / 48), 241 points).
static std::vector<float> Envelope(const std::vector<float>& y, size_t a) {
  const int n = 2048, order = 14;
  std::vector<float> x(n);
  for (int i = 0; i < n; ++i) x[i] = y[a + 2 * i] * (0.5f - 0.5f * cosf(2 * M_PI * i / n));
  float r[order + 1], k[order + 1], c[order + 1];
  Autocorrelation(x.data(), n, order, r);
  r[0] *= 1.0001f;
  Levinson(r, order, k, c);
  std::vector<float> env;
  for (int b = 0; b < 241; ++b) {
    const double f = 150.0 * pow(2.0, b / 48.0);
    double re = 0, im = 0;
    for (int j = 0; j <= order; ++j) {
      re += c[j] * cos(2 * M_PI * f * j / 24000.0);
      im -= c[j] * sin(2 * M_PI * f * j / 24000.0);
    }
    env.push_back(float(-10.0 * log10(re * re + im * im)));
  }
  return env;
}

// Detrended Pearson correlation of two envelopes over 300 Hz - 3 kHz, at a
// shift of s quarter tones.
static float EnvelopeCorrelation(const std::vector<float>& in_raw,
                                 const std::vector<float>& out_raw, int s) {
  auto detrend = [](const std::vector<float>& v) {
    const double n = double(v.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < v.size(); ++i) { sx += i; sy += v[i]; sxx += double(i) * i; sxy += double(i) * v[i]; }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    const double offset = (sy - slope * sx) / n;
    std::vector<float> r(v.size());
    for (size_t i = 0; i < v.size(); ++i) r[i] = float(v[i] - (offset + slope * i));
    return r;
  };
  const std::vector<float> in = detrend(in_raw), out = detrend(out_raw);
  double n = 0, mi = 0, mo = 0;
  for (int b = 56; b < 200; ++b) { const int j = b - s; if (j < 0 || j >= 241) continue; mi += in[j]; mo += out[b]; n += 1; }
  mi /= n; mo /= n;
  double sio = 0, sii = 1e-12, soo = 1e-12;
  for (int b = 56; b < 200; ++b) {
    const int j = b - s;
    if (j < 0 || j >= 241) continue;
    sio += (in[j] - mi) * (out[b] - mo);
    sii += (in[j] - mi) * (in[j] - mi);
    soo += (out[b] - mo) * (out[b] - mo);
  }
  return float(sio / sqrt(sii * soo));
}

static float EnvelopeShift(const std::vector<float>& in, const std::vector<float>& out) {
  float best = -2.0f, shift = 0.0f;
  for (int s = -48; s <= 48; ++s) {
    const float c = EnvelopeCorrelation(in, out, s);
    if (c > best) { best = c; shift = s / 4.0f; }
  }
  return shift;
}

static void TestLattice() {
  printf("lattice filters\n");
  TestRandom rng(1);
  float k[kMaxLpcOrder + 1];
  for (int i = 1; i <= kMaxLpcOrder; ++i) k[i] = 0.6f * rng.Bipolar();
  LatticeAnalysis a;
  LatticeSynthesis s;
  a.Init();
  s.Init();
  float worst = 0.0f;
  for (int i = 0; i < 48000; ++i) {
    const float x = rng.Bipolar();
    const float y = s.Process(a.Process(x, k, kMaxLpcOrder), k, kMaxLpcOrder);
    worst = std::max(worst, fabsf(y - x));
  }
  Check(worst < 1e-4f, "synthesis(analysis(x)) = x (order 24)", "max error %.1e", worst);
}

static size_t FindLag(const std::vector<float>& x, const std::vector<float>& y,
                      size_t lo, size_t hi) {
  size_t best_lag = lo;
  double best = -1e30;
  for (size_t lag = lo; lag < hi; ++lag) {
    double s = 0;
    for (size_t i = 96000; i < 144000; i += 2) s += x[i] * y[i + lag];
    if (s > best) { best = s; best_lag = lag; }
  }
  return best_lag;
}

static void TestTransparency() {
  printf("transparency (residual excitation, neutral settings)\n");
  GroundTruth gt;
  SpeechLikeOptions o = DefaultSpeechOptions(8.0f, kSr);
  o.seed = 81;
  std::vector<float> x = SpeechLike(o, &gt), y;
  static LpcEngine e;
  e.Init(kSr, speech_storage, track, reverb_buffer);
  Run(&e, x, NULL, &y, [](size_t, LpcParams*) { });
  const size_t lag = FindLag(x, y, 2900, 3200);
  // Compare with the band-limited input (the 24 kHz path cuts above 10 kHz).
  static HalfRateBridge bridge;
  bridge.Init();
  std::vector<float> ref(x.size(), 0.0f);
  FloatFrame in[kBlock], out[kBlock];
  float l[kBlock], r[kBlock];
  for (size_t s = 0; s + kBlock <= x.size(); s += kBlock) {
    for (size_t i = 0; i < kBlock; ++i) in[i].l = in[i].r = x[s + i];
    bridge.Down(in, l, r, kBlock);
    bridge.Up(l, l, out, kBlock / 2);
    for (size_t i = 0; i < kBlock; ++i) ref[s + i] = out[i].l;
  }
  const size_t ref_lag = FindLag(ref, y, 2900, 3200);
  double err = 0, energy = 0;
  for (size_t i = 96000; i + ref_lag < x.size() - 4800; ++i) {
    const double d = y[i + ref_lag] - ref[i];
    err += d * d;
    energy += double(ref[i]) * ref[i];
  }
  const float null_db = float(10 * log10(err / energy + 1e-30));
  Check(null_db < -30.0f, "output = delayed input", "residual %.1f dB (delay %.1f ms)",
        null_db, lag / 48.0f);
}

static void TestFormantWarp() {
  printf("formant warp (SIZE)\n");
  std::vector<float> x = Vowel(150.0f, 150.0f, 4.0f), y;
  const size_t a = size_t(2.0f * kSr);
  const std::vector<float> env_in = Envelope(x, a - 3000);
  for (float size : { 1.0f, 0.0f }) {
    static LpcEngine e;
    e.Init(kSr, speech_storage, track, reverb_buffer);
    Run(&e, x, NULL, &y, [&](size_t, LpcParams* p) { p->size = size; });
    const float shift = EnvelopeShift(env_in, Envelope(y, a));
    const float f = Pitch(y, a, a + 24000);
    char name[64];
    snprintf(name, sizeof(name), "SIZE %s: formants %s, F0 kept", size > 0.5f ? "CW" : "CCW",
             size > 0.5f ? "up" : "down");
    const bool direction = size > 0.5f ? shift >= 2.0f : shift <= -2.0f;
    Check(direction && fabsf(f / 150.0f - 1.0f) < 0.01f, name,
          "envelope %+.2f st, %.1f Hz", shift, f);
  }
}

static void TestExcitations() {
  printf("excitation sources (SHAPE)\n");
  std::vector<float> x = Vowel(110.0f, 190.0f, 4.0f), y;
  const size_t a = size_t(2.5f * kSr);
  const std::vector<float> env_in = Envelope(x, a - 3000);
  {
    static LpcEngine e;
    e.Init(kSr, speech_storage, track, reverb_buffer);
    Run(&e, x, NULL, &y, [](size_t, LpcParams* p) { p->shape = 1.0f; });
    const float f1 = Pitch(y, size_t(1.0f * kSr), size_t(1.2f * kSr));
    const float f2 = Pitch(y, size_t(3.0f * kSr), size_t(3.2f * kSr));
    Check(fabsf(f1 / 130.81f - 1.0f) < 0.01f && fabsf(f2 / 130.81f - 1.0f) < 0.01f,
          "CW: fixed pulse at C3 (input glides)", "%.1f / %.1f Hz", f1, f2);
  }
  {
    static LpcEngine e;
    e.Init(kSr, speech_storage, track, reverb_buffer);
    Run(&e, x, NULL, &y, [](size_t, LpcParams* p) { p->shape = 1.0f; p->pitch = 7.0f; });
    const float f = Pitch(y, size_t(2.0f * kSr), size_t(2.2f * kSr));
    Check(fabsf(f / 196.0f - 1.0f) < 0.01f, "CW + PITCH +7: G3", "%.1f Hz", f);
  }
  {
    static LpcEngine e;
    e.Init(kSr, speech_storage, track, reverb_buffer);
    Run(&e, x, NULL, &y, [](size_t, LpcParams* p) { p->shape = 0.75f; p->pitch = 12.0f; });
    // Input F0 at the read position (delay ~62.5 ms) vs output.
    const size_t t = size_t(2.0f * kSr);
    const float f_in = Pitch(x, t - 3000, t - 3000 + 4800);
    const float f = Pitch(y, t, t + 4800);
    Check(fabsf(f / (2.0f * f_in) - 1.0f) < 0.03f, "pulse follows F0 (+12 st)",
          "%.1f Hz (input %.1f Hz)", f, f_in);
  }
  {
    static LpcEngine e;
    e.Init(kSr, speech_storage, track, reverb_buffer);
    Run(&e, x, NULL, &y, [](size_t, LpcParams* p) { p->shape = 0.0f; });
    // Periodicity in the pitch range (lags beyond the formant ringing).
    float strength = 0.0f, strength_in = 0.0f;
    for (size_t lag = 200; lag < 500; ++lag) {
      double s1 = 0, e1 = 1e-12, e2 = 1e-12, s2 = 0, f1 = 1e-12, f2 = 1e-12;
      for (size_t i = a; i < a + 9600; ++i) {
        s1 += y[i] * y[i + lag]; e1 += y[i] * y[i]; e2 += y[i + lag] * y[i + lag];
        s2 += x[i] * x[i + lag]; f1 += x[i] * x[i]; f2 += x[i + lag] * x[i + lag];
      }
      strength = std::max(strength, float(s1 / sqrt(e1 * e2)));
      strength_in = std::max(strength_in, float(s2 / sqrt(f1 * f2)));
    }
    const float c = EnvelopeCorrelation(env_in, Envelope(y, a), 0);
    Check(strength < 0.3f && c > 0.9f, "CCW: whisper (aperiodic, same envelope)",
          "periodicity %.2f (input %.2f), envelope correlation %.2f", strength,
          strength_in, c);
  }
}

static void TestCrossSynthesis() {
  printf("cross-synthesis (IN R carrier)\n");
  std::vector<float> x = Vowel(150.0f, 150.0f, 4.0f), y;
  std::vector<float> carrier(x.size());
  float phase = 0.0f;
  for (size_t i = 0; i < x.size(); ++i) {
    phase += 110.0f / kSr;
    if (phase >= 1.0f) phase -= 1.0f;
    carrier[i] = 0.5f * (2.0f * phase - 1.0f);  // saw
  }
  static LpcEngine e;
  e.Init(kSr, speech_storage, track, reverb_buffer);
  Run(&e, x, &carrier, &y, [](size_t, LpcParams* p) {
    p->submode = LPC_SUBMODE_CROSS;
    p->shape = 1.0f;
  });
  const size_t a = size_t(2.0f * kSr);
  const float f = Pitch(y, a, a + 24000);
  const float c_voice = EnvelopeCorrelation(Envelope(x, a - 3000), Envelope(y, a), 0);
  const float c_carrier = EnvelopeCorrelation(Envelope(carrier, a), Envelope(y, a), 0);
  Check(fabsf(f / 110.0f - 1.0f) < 0.01f && c_voice > 0.8f && c_voice > c_carrier,
        "carrier pitch, voice envelope", "%.1f Hz, envelope correlation %.2f "
        "(voice) vs %.2f (carrier)", f, c_voice, c_carrier);
}

static void TestHold() {
  printf("hold (DENSITY noon)\n");
  std::vector<float> x = Vowel(150.0f, 150.0f, 2.0f, 3.0f), y;
  static LpcEngine e;
  e.Init(kSr, speech_storage, track, reverb_buffer);
  const size_t freeze_at = size_t(2.05f * kSr);
  Run(&e, x, NULL, &y, [&](size_t s, LpcParams* p) {
    p->freeze = s >= freeze_at;
    p->time = 0.3f;
    p->density = 0.5f;
    p->shape = 1.0f;  // pulse: the held frame sings
    p->pitch = 5.0f;
  });
  const float early = Rms(y, freeze_at + 9600, freeze_at + 33600);
  const float late = Rms(y, y.size() - 48000, y.size());
  const float f = Pitch(y, y.size() - 48000, y.size() - 24000);
  Check(early > 0.03f && late > 0.7f * early && fabsf(f / 174.6f - 1.0f) < 0.01f,
        "held frame sings at the PITCH note", "%.3f -> %.3f RMS, %.1f Hz", early, late, f);
}

static void TestChip() {
  printf("speaking chip (red)\n");
  std::vector<float> x = Vowel(150.0f, 150.0f, 4.0f), y;
  static LpcEngine e;
  e.Init(kSr, speech_storage, track, reverb_buffer);
  Run(&e, x, NULL, &y, [](size_t, LpcParams* p) { p->submode = LPC_SUBMODE_CHIP; });
  const size_t a = size_t(2.0f * kSr);
  const float level = Db(Rms(y, a, a + 48000)) - Db(Rms(x, a, a + 48000));
  const float c = EnvelopeCorrelation(Envelope(x, a), Envelope(y, a), 0);
  const float f = Pitch(y, a, a + 24000);
  Check(AllFinite(y) && fabsf(level) < 6.0f && c > 0.6f && fabsf(f / 150.0f - 1.0f) < 0.05f,
        "chip voice: level, envelope, pitch", "%+.1f dB, envelope correlation %.2f, %.1f Hz",
        level, c, f);
  GroundTruth gt;
  SpeechLikeOptions o = DefaultSpeechOptions(10.0f, kSr);
  o.seed = 82;
  std::vector<float> speech = SpeechLike(o, &gt);
  static LpcEngine e2;
  e2.Init(kSr, speech_storage, track, reverb_buffer);
  Run(&e2, speech, NULL, &y, [](size_t, LpcParams* p) { p->submode = LPC_SUBMODE_CHIP; });
  const float speech_level = Db(Rms(y, 48000, y.size())) - Db(Rms(speech, 48000, speech.size()));
  Check(AllFinite(y) && fabsf(speech_level) < 6.0f, "chip voice on speech: level",
        "%+.1f dB", speech_level);
}

static void TestStability(double* worst_ratio) {
  printf("stability (40 s random controls)\n");
  GroundTruth gt;
  SpeechLikeOptions o = DefaultSpeechOptions(40.0f, kSr);
  o.seed = 83;
  std::vector<float> x = SpeechLike(o, &gt), y;
  std::vector<float> carrier(x.size());
  TestRandom noise(4);
  for (float& v : carrier) v = 0.3f * noise.Bipolar();
  static LpcEngine e;
  e.Init(kSr, speech_storage, track, reverb_buffer);
  TestRandom rng(12);
  LpcParams q = DefaultParams();
  float max_k = 0.0f;
  Run(&e, x, &carrier, &y, [&](size_t s, LpcParams* p) {
    if (s % 4800 == 0) {
      q.time = rng.Uniform(); q.size = rng.Uniform(); q.shape = rng.Uniform();
      q.density = rng.Uniform(); q.pitch = rng.Range(-24, 24); q.slew = rng.Uniform();
      q.dry_wet = rng.Uniform(); q.reverb = rng.Uniform(); q.freeze = rng.Uniform() < 0.3f;
      q.hold = rng.Uniform() < 0.2f; q.latch = rng.Uniform() < 0.2f;
      q.smooth_f0 = rng.Uniform() < 0.5f; q.submode = uint8_t(rng.Int(4));
    }
    *p = q;
    p->carrier_present = true;
    p->trigger = (s % 9600) < kBlock;
    for (int k = 1; k <= LpcEngine::kOrder; ++k) max_k = std::max(max_k, fabsf(e.synthesis_k()[k]));
  });
  Check(AllFinite(y), "no NaN / Inf", "ok");
  Check(Peak(y) < 1.5f, "output bounded", "peak %.2f", Peak(y));
  Check(max_k < 0.996f, "reflection coefficients stay inside the unit circle",
        "max |k| %.4f", max_k);
  (void)worst_ratio;
}

static void TestPerformance() {
  printf("performance (host, informational)\n");
  std::vector<float> x = Vowel(150.0f, 150.0f, 10.0f), y;
  static LpcEngine e;
  e.Init(kSr, speech_storage, track, reverb_buffer);
  double worst = 0.0;
  auto t0 = std::chrono::high_resolution_clock::now();
  Run(&e, x, NULL, &y, [](size_t, LpcParams* p) { p->size = 0.8f; p->shape = 0.9f; p->reverb = 0.5f; },
      &worst);
  auto t1 = std::chrono::high_resolution_clock::now();
  const double s = std::chrono::duration<double>(t1 - t0).count();
  const double mean_block = s / (x.size() / kBlock);
  Info("speed (warped analysis on)", "%.1fx real time (%.0f ns/sample)", 10.0 / s,
       1e9 * s / x.size());
  Info("99.9th percentile block vs mean block", "%.1fx (analysis spread over 20 blocks)",
       worst / mean_block);
}

static int ProcessFile(int argc, char** argv) {
  if (argc < 4) return 2;
  LpcParams base = DefaultParams();
  std::string carrier_path;
  for (int i = 4; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    if (a == "--carrier") { carrier_path = argv[i + 1]; continue; }
    const float v = float(atof(argv[i + 1]));
    if (a == "--submode") base.submode = uint8_t(v);
    else if (a == "--shape") base.shape = v;
    else if (a == "--size") base.size = v;
    else if (a == "--pitch") base.pitch = v;
    else if (a == "--density") base.density = v;
    else if (a == "--slew") base.slew = v;
    else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  Audio in, carrier;
  if (!ReadWav(argv[2], &in)) return 1;
  Resample(&in, int(kSr));
  std::vector<float>* carrier_ptr = NULL;
  if (!carrier_path.empty()) {
    if (!ReadWav(carrier_path, &carrier)) return 1;
    Resample(&carrier, int(kSr));
    carrier.l.resize(in.size(), 0.0f);
    carrier_ptr = &carrier.l;
  }
  static LpcEngine e;
  e.Init(kSr, speech_storage, track, reverb_buffer);
  std::vector<float> y;
  Run(&e, in.l, carrier_ptr, &y, [&](size_t, LpcParams* p) {
    *p = base;
    p->carrier_present = carrier_ptr != NULL;
  });
  Audio out;
  out.sample_rate = int(kSr);
  out.channels = 2;
  out.l = y;
  for (float& v : out.l) v = std::max(-1.0f, std::min(1.0f, v));
  out.r = out.l;
  return WriteWav(argv[3], out) ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !strcmp(argv[1], "process")) return ProcessFile(argc, argv);
  TestLattice();
  TestTransparency();
  TestFormantWarp();
  TestExcitations();
  TestCrossSynthesis();
  TestHold();
  TestChip();
  TestStability(NULL);
  TestPerformance();
  return Summary();
}
