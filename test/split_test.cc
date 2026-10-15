// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Host tests for the vox_split engine.
//
//   split_test                         self tests
//   split_test process in.wav out.wav [--submode N] [--shape x] [--size x]
//       [--pitch st] [--time x] [--feedback x] [--density x] [--mix x]
//       [--reverb x] [--preset N]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "vox/split/dsp/split_engine.h"
#include "vox/test/test_signals.h"
#include "vox/test/test_util.h"
#include "vox/test/wav_io.h"

using namespace vox;
using namespace vox_test;
using beads::FloatFrame;

static const float kSr = 48000.0f;
static const size_t kBlock = 12;

static float delay_buffer[ConsonantDelay::kMaxDelay];
static uint16_t reverb_buffer[32768];

static SplitParams DefaultParams() {
  SplitParams p;
  p.shape = 0.5f;
  p.size = 0.0f;
  p.pitch = 0.0f;
  p.time = 0.4f;
  p.feedback = 0.0f;
  p.density = 0.5f;
  p.dry_wet = 1.0f;
  p.reverb = 0.0f;
  p.freeze = false;
  p.capture = false;
  p.clock_edge = -1;
  p.clock_patched = false;
  p.mute_voiced = false;
  p.submode = SPLIT_SUBMODE_PROCESS;
  p.preset = 0;
  return p;
}

static void Run(
    SplitEngine* e,
    const std::vector<float>& x,
    std::vector<float>* yl,
    std::vector<float>* yr,
    std::function<void(size_t, SplitParams*)> params) {
  yl->assign(x.size(), 0.0f);
  yr->assign(x.size(), 0.0f);
  FloatFrame in[kBlock], out[kBlock];
  bool g[kBlock];
  for (size_t start = 0; start + kBlock <= x.size(); start += kBlock) {
    for (size_t i = 0; i < kBlock; ++i) in[i].l = in[i].r = x[start + i];
    SplitParams p = DefaultParams();
    params(start, &p);
    e->Process(p, in, out, g, kBlock);
    for (size_t i = 0; i < kBlock; ++i) {
      (*yl)[start + i] = out[i].l;
      (*yr)[start + i] = out[i].r;
    }
  }
}

static std::vector<float> Speech(float seconds, uint32_t seed, GroundTruth* gt) {
  SpeechLikeOptions o = DefaultSpeechOptions(seconds, kSr);
  o.seed = seed;
  return SpeechLike(o, gt);
}

static void TestSplitter() {
  printf("splitter (red)\n");
  GroundTruth gt;
  std::vector<float> x = Speech(15.0f, 61, &gt), yl, yr;
  static SplitEngine e;
  e.Init(kSr, delay_buffer, reverb_buffer);
  Run(&e, x, &yl, &yr, [](size_t, SplitParams* p) { p->submode = SPLIT_SUBMODE_SPLITTER; });
  std::vector<float> sum(x.size());
  for (size_t i = 0; i < x.size(); ++i) sum[i] = yl[i] + yr[i];
  // Reference: the input through the same 48 -> 24 -> 48 kHz conversion,
  // delayed by the look-ahead.
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
  const size_t lookahead = 2 * SplitEngine::kLookahead;
  double err = 0.0, energy = 0.0;
  for (size_t i = 48000; i + lookahead < x.size(); ++i) {
    const double d = sum[i + lookahead] - ref[i];
    err += d * d;
    energy += double(ref[i]) * ref[i];
  }
  const float null_db = float(10.0 * log10(err / energy + 1e-30));
  Check(null_db < -40.0f, "OUT L + OUT R = delayed input", "residual %.1f dB", null_db);

  // Separation, measured on the ground-truth labels.
  double v_l = 0, v_r = 0, u_l = 0, u_r = 0;
  for (size_t i = 0; i + lookahead < x.size(); ++i) {
    const uint8_t label = gt.label[i];
    const double a = yl[i + lookahead], b = yr[i + lookahead];
    if (label == LABEL_VOICED) { v_l += a * a; v_r += b * b; }
    else if (label == LABEL_UNVOICED || label == LABEL_BURST) { u_l += a * a; u_r += b * b; }
  }
  Check(v_l / (v_l + v_r) > 0.95, "vowel energy on OUT L", "%.1f%%",
        100.0 * v_l / (v_l + v_r));
  Check(u_r / (u_l + u_r) > 0.85, "consonant energy on OUT R", "%.1f%%",
        100.0 * u_r / (u_l + u_r));
}

static void TestBalance() {
  printf("stream balance (SHAPE)\n");
  GroundTruth gt;
  std::vector<float> x = Speech(12.0f, 62, &gt), yl, yr;
  const size_t lag = 2 * SplitEngine::kLookahead + 50;
  auto energies = [&](float shape, double* v, double* u) {
    static SplitEngine e;
    e.Init(kSr, delay_buffer, reverb_buffer);
    Run(&e, x, &yl, &yr, [&](size_t, SplitParams* p) {
      p->shape = shape;
      p->feedback = 0.0f;
      p->time = 0.0f;
    });
    *v = *u = 0.0;
    for (size_t i = 0; i + lag < x.size(); ++i) {
      // Skip 10 ms around label changes.
      if (i < 480 || i + 480 >= x.size()) continue;
      const uint8_t label = gt.label[i];
      if (gt.label[i - 480] != label || gt.label[i + 480] != label) continue;
      const double a = yl[i + lag];
      if (label == LABEL_VOICED) *v += a * a;
      else if (label == LABEL_UNVOICED) *u += a * a;
    }
  };
  double v_mid, u_mid, v_cw, u_cw, v_ccw, u_ccw;
  energies(0.5f, &v_mid, &u_mid);
  energies(1.0f, &v_cw, &u_cw);
  energies(0.0f, &v_ccw, &u_ccw);
  Check(10 * log10(u_mid / u_cw) > 20.0, "CW: consonants removed",
        "%.1f dB down, vowels %+.1f dB", 10 * log10(u_mid / u_cw), 10 * log10(v_cw / v_mid));
  Check(10 * log10(v_mid / v_ccw) > 20.0, "CCW: vowels removed",
        "%.1f dB down, consonants %+.1f dB", 10 * log10(v_mid / v_ccw),
        10 * log10(u_ccw / u_mid));
}

// Dominant period by autocorrelation (24..600 samples at 48 kHz).
static float Pitch(const std::vector<float>& y, size_t a, size_t b) {
  float best = 0.0f;
  size_t best_lag = 0;
  std::vector<float> c(600, 0.0f);
  for (size_t lag = 60; lag < 600; ++lag) {
    double s = 0, e1 = 1e-12, e2 = 1e-12;
    for (size_t i = a; i + lag < b; ++i) {
      s += y[i] * y[i + lag];
      e1 += y[i] * y[i];
      e2 += y[i + lag] * y[i + lag];
    }
    c[lag] = float(s / sqrt(e1 * e2));
    if (c[lag] > best) { best = c[lag]; best_lag = lag; }
  }
  // Smallest lag nearly as good as the best (avoids octave-down picks).
  for (size_t lag = 60; lag < 600; ++lag) {
    if (c[lag] >= 0.9f * best && (lag == 60 || c[lag] >= c[lag - 1]) && \
        (lag + 1 >= 600 || c[lag] >= c[lag + 1])) {
      return kSr / lag;
    }
  }
  return best_lag ? kSr / best_lag : 0.0f;
}

static void TestTails() {
  printf("vowel tails\n");
  // One long vowel (f0 = 150 Hz), then silence.
  const size_t n = size_t(3.0f * kSr);
  std::vector<float> x(n, 0.0f), yl, yr;
  Resonator rs[3];
  const float formants[3] = { 700, 1200, 2600 };
  for (int k = 0; k < 3; ++k) rs[k].Init(formants[k], 80.0f + 40.0f * k, kSr);
  float phase = 0.0f, t1 = 0.0f, t2 = 0.0f;
  const size_t on = size_t(0.3f * kSr), off = size_t(0.8f * kSr);
  for (size_t i = on; i < off; ++i) {
    phase += 150.0f / kSr;
    float pulse = 0.0f;
    if (phase >= 1.0f) { phase -= 1.0f; pulse = 1.0f; }
    t1 += 0.03f * (pulse * 40.0f - t1);
    t2 += 0.03f * (t1 - t2);
    float y = 0.0f;
    for (int k = 0; k < 3; ++k) y += rs[k].Process(t2);
    const float env = std::min(1.0f, std::min((i - on) / 960.0f, (off - i) / 1680.0f));
    x[i] = 0.5f * y * env;
  }
  const float peak = Peak(x);
  for (float& v : x) v *= 0.7f / peak;
  const size_t lag = 2 * SplitEngine::kLookahead + 50;
  const size_t after_a = off + lag + size_t(0.05f * kSr);
  const size_t after_b = off + lag + size_t(0.35f * kSr);
  for (float pitch : { 0.0f, 12.0f }) {
    static SplitEngine e;
    e.Init(kSr, delay_buffer, reverb_buffer);
    Run(&e, x, &yl, &yr, [&](size_t, SplitParams* p) {
      p->size = 0.7f;
      p->pitch = pitch;
      p->time = 0.0f;
    });
    const float level = Db(Rms(yl, after_a, after_b)) - Db(Rms(x, on + 4800, off - 4800));
    const float f = Pitch(yl, after_a, after_b);
    char name[64];
    snprintf(name, sizeof(name), "tail after the vowel, PITCH %+.0f", pitch);
    const float expected = 150.0f * powf(2.0f, pitch / 12.0f);
    Check(level > -12.0f && fabsf(f / expected - 1.0f) < 0.03f, name,
          "%.1f dB vs vowel, %.1f Hz (%.0f expected)", level, f, expected);
  }
  {
    static SplitEngine e;
    e.Init(kSr, delay_buffer, reverb_buffer);
    Run(&e, x, &yl, &yr, [&](size_t, SplitParams* p) { p->size = 0.0f; p->time = 0.0f; });
    const float level = Db(Rms(yl, after_a, after_b));
    Check(level < -60.0f, "SIZE CCW: no tail", "%.1f dBFS", level);
  }
  {
    static SplitEngine e;
    e.Init(kSr, delay_buffer, reverb_buffer);
    Run(&e, x, &yl, &yr, [&](size_t s, SplitParams* p) {
      p->size = 0.3f;
      p->time = 0.0f;
      p->freeze = s > off + lag;
    });
    const float late = Db(Rms(yl, n - 9600, n));
    const float early = Db(Rms(yl, after_a, after_b));
    Check(late > early - 3.0f, "FREEZE holds the tail (2 s later)",
          "%.1f dB vs %.1f dB", late, early);
  }
}

static void TestDelay() {
  printf("consonant delay\n");
  // A noise burst (unvoiced) then silence: the echo position gives the delay.
  const size_t n = size_t(3.0f * kSr);
  std::vector<float> x(n, 0.0f), yl, yr;
  TestRandom rng(3);
  float hp_x = 0.0f, hp_y = 0.0f;
  const size_t burst = size_t(0.5f * kSr);
  for (size_t i = burst; i < burst + 2400; ++i) {
    const float v = rng.Bipolar();
    hp_y = 0.7f * (hp_y + v - hp_x);
    hp_x = v;
    x[i] = 0.5f * hp_y * sinf(float(M_PI) * (i - burst) / 2400.0f);
  }
  auto echo = [&](const std::vector<float>& y) {
    // First position after the direct sound where the energy envelope peaks.
    size_t best = 0;
    float best_e = 0.0f;
    for (size_t i = burst + 2400 + 2400; i < n - 480; i += 48) {
      const float e = Rms(y, i, i + 480);
      if (e > best_e) { best_e = e; best = i; }
    }
    return best;
  };
  {
    static SplitEngine e;
    e.Init(kSr, delay_buffer, reverb_buffer);
    Run(&e, x, &yl, &yr, [](size_t, SplitParams* p) { p->time = 0.5f; p->feedback = 0.0f; });
    const float expected_ms = 10.0f * powf(120.0f, 0.5f);
    // Burst envelope peak at burst + 1200; echo window centre at +240.
    const float measured_ms = (echo(yl) + 240.0f - burst - 1200.0f) / 48.0f -
        2 * SplitEngine::kLookahead / 48.0f;
    Check(fabsf(measured_ms - expected_ms) < 12.0f, "TIME 0.5: echo delay",
          "%.0f ms (%.0f expected)", measured_ms, expected_ms);
  }
  {
    static SplitEngine e;
    e.Init(kSr, delay_buffer, reverb_buffer);
    const size_t period = 9600;  // 200 ms clock
    Run(&e, x, &yl, &yr, [&](size_t s, SplitParams* p) {
      p->clock_patched = true;
      p->clock_edge = (s % period) < kBlock ? int(s % period) : -1;
      p->time = 4.5f / 7.0f;  // ratio 1
    });
    Check(fabsf(e.clock_period() - period / 2.0f) < 2.0f, "clock: period measured",
          "%.1f ms", e.clock_period() / 24.0f);
    const float measured_ms = (echo(yl) + 240.0f - burst - 1200.0f) / 48.0f -
        2 * SplitEngine::kLookahead / 48.0f;
    Check(fabsf(measured_ms - 200.0f) < 12.0f, "clock: echo = 1 clock period",
          "%.0f ms", measured_ms);
  }
}

static void TestClicks() {
  printf("transitions\n");
  GroundTruth gt;
  std::vector<float> x = Speech(10.0f, 63, &gt), yl, yr;
  float input_step = 0.0f;
  for (size_t i = 1; i < x.size(); ++i) input_step = std::max(input_step, fabsf(x[i] - x[i - 1]));
  for (float shape : { 0.0f, 1.0f }) {
    static SplitEngine e;
    e.Init(kSr, delay_buffer, reverb_buffer);
    Run(&e, x, &yl, &yr, [&](size_t, SplitParams* p) { p->shape = shape; p->time = 0.0f; });
    float step = 0.0f;
    for (size_t i = 1; i < yl.size(); ++i) step = std::max(step, fabsf(yl[i] - yl[i - 1]));
    char name[64];
    snprintf(name, sizeof(name), "no clicks with SHAPE at %s", shape > 0.5f ? "CW" : "CCW");
    Check(step <= input_step, name, "max step %.3f (input %.3f)", step, input_step);
  }
}

static void TestStability() {
  printf("stability (40 s random controls)\n");
  GroundTruth gt;
  std::vector<float> x = Speech(40.0f, 64, &gt), yl, yr;
  static SplitEngine e;
  e.Init(kSr, delay_buffer, reverb_buffer);
  TestRandom rng(10);
  SplitParams q = DefaultParams();
  Run(&e, x, &yl, &yr, [&](size_t s, SplitParams* p) {
    if (s % 4800 == 0) {
      q.shape = rng.Uniform(); q.size = rng.Uniform(); q.pitch = rng.Range(-24, 24);
      q.time = rng.Uniform(); q.feedback = rng.Uniform(); q.density = rng.Uniform();
      q.dry_wet = rng.Uniform(); q.reverb = rng.Uniform(); q.freeze = rng.Uniform() < 0.3f;
      q.clock_patched = rng.Uniform() < 0.4f; q.mute_voiced = rng.Uniform() < 0.2f;
      q.submode = uint8_t(rng.Int(4)); q.preset = uint8_t(rng.Int(3));
    }
    *p = q;
    p->capture = (s % 7200) < kBlock;
    p->clock_edge = (q.clock_patched && (s % 6000) < kBlock) ? 0 : -1;
  });
  Check(AllFinite(yl) && AllFinite(yr), "no NaN / Inf", "ok");
  Check(Peak(yl) < 1.5f && Peak(yr) < 1.5f, "output bounded", "peak %.2f",
        std::max(Peak(yl), Peak(yr)));
}

static void TestPerformance() {
  printf("performance (host, informational)\n");
  GroundTruth gt;
  std::vector<float> x = Speech(10.0f, 65, &gt), yl, yr;
  static SplitEngine e;
  e.Init(kSr, delay_buffer, reverb_buffer);
  auto t0 = std::chrono::high_resolution_clock::now();
  Run(&e, x, &yl, &yr, [](size_t, SplitParams* p) { p->size = 0.6f; p->feedback = 0.5f; p->reverb = 0.5f; });
  auto t1 = std::chrono::high_resolution_clock::now();
  const double s = std::chrono::duration<double>(t1 - t0).count();
  Info("speed", "%.1fx real time (%.0f ns/sample)", 10.0 / s, 1e9 * s / x.size());
}

static int ProcessFile(int argc, char** argv) {
  if (argc < 4) return 2;
  SplitParams base = DefaultParams();
  for (int i = 4; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    const float v = float(atof(argv[i + 1]));
    if (a == "--submode") base.submode = uint8_t(v);
    else if (a == "--shape") base.shape = v;
    else if (a == "--size") base.size = v;
    else if (a == "--pitch") base.pitch = v;
    else if (a == "--time") base.time = v;
    else if (a == "--feedback") base.feedback = v;
    else if (a == "--density") base.density = v;
    else if (a == "--mix") base.dry_wet = v;
    else if (a == "--reverb") base.reverb = v;
    else if (a == "--preset") base.preset = uint8_t(v);
    else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  Audio in;
  if (!ReadWav(argv[2], &in)) return 1;
  Resample(&in, int(kSr));
  std::vector<float> x(in.size());
  for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * (in.l[i] + in.r[i]);
  static SplitEngine e;
  e.Init(kSr, delay_buffer, reverb_buffer);
  Audio out;
  out.sample_rate = int(kSr);
  out.channels = 2;
  Run(&e, x, &out.l, &out.r, [&](size_t, SplitParams* p) { *p = base; });
  for (float& v : out.l) v = std::max(-1.0f, std::min(1.0f, v));
  for (float& v : out.r) v = std::max(-1.0f, std::min(1.0f, v));
  printf("%u tails spawned\n", e.tails().spawned());
  return WriteWav(argv[3], out) ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !strcmp(argv[1], "process")) return ProcessFile(argc, argv);
  TestSplitter();
  TestBalance();
  TestTails();
  TestDelay();
  TestClicks();
  TestStability();
  TestPerformance();
  return Summary();
}
