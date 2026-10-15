// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Host tests for the vox_sequencer engine (and the shared WSOLA stretcher).
//
//   sequencer_test                    self tests
//   sequencer_test process in.wav out.wav [--submode N] [--bpm-density x]
//       [--time x] [--size x] [--shape x] [--repeat x] [--pitch st]
//       [--freeze-at s] [--clock-hz f]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "vox/sequencer/dsp/sequencer_engine.h"
#include "vox/test/test_signals.h"
#include "vox/test/test_util.h"
#include "vox/test/wav_io.h"

using namespace vox;
using namespace vox_test;
using beads::FloatFrame;

static const float kSr = 48000.0f;
static const size_t kBlock = 12;

static int16_t speech_storage[VoxSpeechBuffer::kCapacity];
static uint16_t reverb_buffer[32768];

static SequencerParams DefaultParams() {
  SequencerParams p;
  p.time = 0.0f;
  p.size = 7.0f / 15.0f;  // 8 steps
  p.shape = 0.0f;
  p.density = 0.5f;
  p.pitch = 0.0f;
  p.repeat = 0.0f;
  p.dry_wet = 1.0f;
  p.reverb = 0.0f;
  p.freeze = false;
  p.clock_edge = -1;
  p.clock_patched = false;
  p.tap = false;
  p.reset = false;
  p.lock = false;
  p.submode = SEQUENCER_SUBMODE_SYLLABLE_TRUNCATE;
  p.pause_removal = false;
  p.swing = 0;
  p.gate_mode = SEQUENCER_GATE_STEP_TRIGGER;
  return p;
}

struct Result {
  std::vector<float> y;
  std::vector<size_t> step_times;   // 48 kHz sample index of each step trigger
  std::vector<uint32_t> units;      // unit start played at each step
};

// Runs the engine; the callback can modify params per block.
static void Run(
    SequencerEngine* e,
    const std::vector<float>& x,
    Result* r,
    std::function<void(size_t, SequencerParams*)> params) {
  r->y.assign(x.size(), 0.0f);
  r->step_times.clear();
  r->units.clear();
  FloatFrame in[kBlock], out[kBlock];
  bool g[kBlock];
  bool previous = false;
  uint32_t steps = e->steps();
  for (size_t start = 0; start + kBlock <= x.size(); start += kBlock) {
    for (size_t i = 0; i < kBlock; ++i) in[i].l = in[i].r = x[start + i];
    SequencerParams p = DefaultParams();
    params(start, &p);
    e->Process(p, in, out, g, kBlock);
    for (size_t i = 0; i < kBlock; ++i) {
      r->y[start + i] = out[i].l;
      if (g[i] && !previous) r->step_times.push_back(start + i);
      previous = g[i];
    }
    if (e->steps() != steps) {
      steps = e->steps();
      r->units.push_back(e->last_unit_start());
    }
  }
}

static std::vector<float> Speech(float seconds, uint32_t seed) {
  GroundTruth gt;
  SpeechLikeOptions o = DefaultSpeechOptions(seconds, kSr);
  o.seed = seed;
  return SpeechLike(o, &gt);
}

// Speech followed by silence, so that a frozen pool can be sequenced.
static std::vector<float> SpeechThenSilence(float speech_s, float total_s, uint32_t seed) {
  std::vector<float> x = Speech(speech_s, seed);
  x.resize(size_t(total_s * kSr), 0.0f);
  return x;
}

// Marks a clock edge in the block when one falls in [start, start + kBlock).
static void Edges(size_t start, size_t period, SequencerParams* p, size_t first = 0) {
  p->clock_patched = true;
  p->clock_edge = -1;
  size_t next;
  if (start <= first) {
    next = first;
  } else {
    next = first + ((start - first + period - 1) / period) * period;
  }
  if (next < start + kBlock) p->clock_edge = int(next - start);
}

static float MeanInterval(const std::vector<size_t>& t, size_t from) {
  std::vector<size_t> v;
  for (size_t i = 1; i < t.size(); ++i) if (t[i - 1] >= from) v.push_back(t[i] - t[i - 1]);
  if (v.empty()) return 0.0f;
  double s = 0;
  for (size_t d : v) s += d;
  return float(s / v.size());
}

static void TestInternalClock() {
  printf("internal clock\n");
  std::vector<float> x = SpeechThenSilence(6.0f, 12.0f, 3);
  for (float density : { 0.0f, 0.5f, 1.0f }) {
    static SequencerEngine e;
    e.Init(kSr, speech_storage, reverb_buffer);
    Result r;
    Run(&e, x, &r, [&](size_t s, SequencerParams* p) {
      p->density = density;
      p->freeze = s > size_t(6.0f * kSr);
    });
    const float bpm = 40.0f * powf(6.0f, density);
    const float expected = kSr * 60.0f / (bpm * 4.0f);
    const float measured = MeanInterval(r.step_times, size_t(7.0f * kSr));
    char name[64];
    snprintf(name, sizeof(name), "DENSITY %.1f -> %.0f BPM 16ths", density, bpm);
    Check(fabsf(measured / expected - 1.0f) < 0.01f, name, "%.1f ms per step (%.1f)",
          measured / 48.0f, expected / 48.0f);
  }
}

static void TestExternalClock() {
  printf("external clock\n");
  std::vector<float> x = SpeechThenSilence(6.0f, 14.0f, 4);
  const size_t period = 6000;  // 125 ms (8 Hz)
  const size_t first = size_t(7.0f * kSr);
  // x1: steps on the edges.
  {
    static SequencerEngine e;
    e.Init(kSr, speech_storage, reverb_buffer);
    Result r;
    Run(&e, x, &r, [&](size_t s, SequencerParams* p) {
      p->freeze = s > size_t(6.0f * kSr);
      p->density = 0.5f;  // x1
      Edges(s, period, p, first);
    });
    size_t worst = 0, n = 0;
    for (size_t t : r.step_times) {
      if (t < first + 3 * period) continue;
      const size_t phase = (t - first) % period;
      const size_t err = std::min(phase, period - phase);
      worst = std::max(worst, err);
      ++n;
    }
    Check(n > 40 && worst <= 2, "x1: steps on the clock edges",
          "%zu steps, worst offset %zu samples (%.0f us)", n, worst, worst / 0.048f);
  }
  // Multiply / divide.
  const float densities[4] = { 0.05f, 0.3f, 0.65f, 0.95f };
  const float factors[4] = { 4.0f, 2.0f, 0.5f, 0.25f };  // step / clock period
  const char* names[4] = { "/4", "/2", "x2", "x4" };
  for (int k = 0; k < 4; ++k) {
    static SequencerEngine e;
    e.Init(kSr, speech_storage, reverb_buffer);
    Result r;
    Run(&e, x, &r, [&](size_t s, SequencerParams* p) {
      p->freeze = s > size_t(6.0f * kSr);
      p->density = densities[k];
      Edges(s, period, p, first);
    });
    const float measured = MeanInterval(r.step_times, first + 4 * period);
    const float expected = period * factors[k];
    char name[64];
    snprintf(name, sizeof(name), "ratio %s", names[k]);
    Check(fabsf(measured / expected - 1.0f) < 0.02f, name, "%.1f ms per step (%.1f)",
          measured / 48.0f, expected / 48.0f);
  }
  // Tempo change: 125 ms -> 166.7 ms; must re-lock within 3 pulses.
  {
    static SequencerEngine e;
    e.Init(kSr, speech_storage, reverb_buffer);
    Result r;
    const size_t change = size_t(10.0f * kSr);
    const size_t period_2 = 8000;
    Run(&e, x, &r, [&](size_t s, SequencerParams* p) {
      p->freeze = s > size_t(6.0f * kSr);
      p->density = 0.65f;  // x2: depends on the period estimate
      if (s < change) Edges(s, period, p, first);
      else Edges(s, period_2, p, change);
    });
    const float measured = MeanInterval(r.step_times, change + 3 * period_2);
    Check(fabsf(measured / (period_2 * 0.5f) - 1.0f) < 0.03f,
          "tempo change: re-locked within 3 pulses", "%.1f ms per step (%.1f)",
          measured / 48.0f, period_2 * 0.5f / 48.0f);
  }
}

static void TestOrder() {
  printf("pattern order\n");
  std::vector<float> x = SpeechThenSilence(8.0f, 16.0f, 5);
  auto run = [&](float shape, bool lock, Result* r) {
    static SequencerEngine e;
    e.Init(kSr, speech_storage, reverb_buffer);
    Run(&e, x, r, [&](size_t s, SequencerParams* p) {
      p->freeze = s > size_t(8.0f * kSr);
      p->shape = shape;
      p->time = 0.2f;
      p->density = 0.8f;
      p->lock = lock && s > size_t(9.0f * kSr);
    });
  };
  auto cycle = [](const Result& r, size_t from, size_t length) {
    std::vector<uint32_t> c(r.units.begin() + from, r.units.begin() + from + length);
    return c;
  };
  Result r;
  run(0.0f, false, &r);

  // Steps after freezing: find the first full cycle.
  const size_t skip = 120;  // well after the freeze (8 s)
  std::vector<uint32_t> c = cycle(r, skip - (skip % 8), 8);
  bool increasing = true;
  for (size_t i = 1; i < c.size(); ++i) increasing = increasing && c[i] > c[i - 1];
  Check(increasing, "in order: chronological", "%zu steps", r.units.size());
  std::vector<uint32_t> c2 = cycle(r, skip - (skip % 8) + 8, 8);
  Check(c == c2, "in order: cycles repeat", "ok");

  run(0.3f, false, &r);
  c = cycle(r, skip - (skip % 8), 8);
  bool decreasing = true;
  for (size_t i = 1; i < c.size(); ++i) decreasing = decreasing && c[i] < c[i - 1];
  Check(decreasing, "reverse: reverse chronological", "ok");

  run(0.5f, false, &r);
  c = cycle(r, skip - (skip % 8), 8);
  c2 = cycle(r, skip - (skip % 8) + 8, 8);
  std::vector<uint32_t> sorted = c;
  std::sort(sorted.begin(), sorted.end());
  bool distinct = std::unique(sorted.begin(), sorted.end()) == sorted.end();
  Check(c == c2 && distinct, "fixed shuffle: a repeating permutation", "ok");

  run(0.9f, false, &r);
  c = cycle(r, skip - (skip % 8), 8);
  c2 = cycle(r, skip - (skip % 8) + 8, 8);
  Check(c != c2, "random per step: cycles differ", "ok");

  run(0.9f, true, &r);
  c = cycle(r, r.units.size() - 16, 8);
  c2 = cycle(r, r.units.size() - 8, 8);
  Check(c == c2, "lock: random pattern frozen", "ok");
}

static void TestResetAndSwing() {
  printf("reset, swing, ratchets\n");
  std::vector<float> x = SpeechThenSilence(8.0f, 14.0f, 6);
  {
    static SequencerEngine e;
    e.Init(kSr, speech_storage, reverb_buffer);
    Result r;
    const size_t reset_at = size_t(10.03f * kSr);
    std::vector<uint32_t> after;
    uint32_t first_unit = 0;
    uint32_t steps_at_reset = 0;
    Run(&e, x, &r, [&](size_t s, SequencerParams* p) {
      p->freeze = s > size_t(8.0f * kSr);
      p->density = 0.8f;
      p->reset = s >= reset_at && s < reset_at + kBlock;
      if (p->reset) steps_at_reset = e.steps();
      if (steps_at_reset && e.steps() == steps_at_reset + 1 && after.empty()) {
        after.push_back(e.last_unit_start());
      }
    });
    // Step 1 of a cycle, from a run without reset (steps 0, 8, 16... are
    // pattern step 1 since the length is 8).
    static SequencerEngine e2;
    e2.Init(kSr, speech_storage, reverb_buffer);
    Result r2;
    Run(&e2, x, &r2, [&](size_t s, SequencerParams* p) {
      p->freeze = s > size_t(8.0f * kSr);
      p->density = 0.8f;
      if (e2.steps() == 129 && !first_unit) first_unit = e2.last_unit_start();
    });
    Check(!after.empty() && after[0] == first_unit, "reset: next step is step 1",
          "ok");
  }
  {
    static SequencerEngine e;
    e.Init(kSr, speech_storage, reverb_buffer);
    Result r;
    Run(&e, x, &r, [&](size_t s, SequencerParams* p) {
      p->freeze = s > size_t(8.0f * kSr);
      p->density = 0.6f;
      p->swing = 2;
    });
    // Alternating long/short intervals: 1.33 / 0.67 of the step period.
    std::vector<float> ratio;
    for (size_t i = 2; i < r.step_times.size(); ++i) {
      if (r.step_times[i] < size_t(9.0f * kSr)) continue;
      ratio.push_back(float(r.step_times[i] - r.step_times[i - 1]) / \
                      float(r.step_times[i - 1] - r.step_times[i - 2]));
    }
    float hi = 0.0f, lo = 10.0f;
    for (float v : ratio) { hi = std::max(hi, v); lo = std::min(lo, v); }
    Check(hi > 1.9f && hi < 2.1f && lo > 0.47f && lo < 0.53f,
          "swing 33%: long/short alternation", "ratios %.2f / %.2f", hi, lo);
  }
  {
    static SequencerEngine e;
    e.Init(kSr, speech_storage, reverb_buffer);
    Result r;
    Run(&e, x, &r, [&](size_t s, SequencerParams* p) {
      p->freeze = s > size_t(8.0f * kSr);
      p->density = 0.5f;
      p->repeat = 1.0f;
    });
    const float steps = float(e.steps());
    const float triggers = float(r.step_times.size());
    Check(triggers > steps * 1.5f, "REPEAT max: ratchets", "%.0f triggers for %.0f steps",
          triggers, steps);
  }
}

static void TestWsola() {
  printf("WSOLA stretcher\n");
  static VoxSpeechBuffer b;
  b.Init(speech_storage);
  // Harmonic tone, 200 Hz, 0.5 s at 24 kHz.
  const size_t n = 12000;
  for (size_t i = 0; i < n; ++i) {
    const float t = float(i) / 24000.0f;
    b.Write(0.3f * sinf(2 * M_PI * 200 * t) + 0.15f * sinf(2 * M_PI * 400 * t) + \
            0.1f * sinf(2 * M_PI * 600 * t));
  }
  for (float factor : { 0.5f, 2.0f, 3.0f }) {
    static Wsola<480> w;
    w.Init();
    const uint32_t out_length = uint32_t(n * factor);
    w.Start(0, n, out_length, 1.0f, 1.0f, 48);
    std::vector<float> y;
    while (w.active()) y.push_back(w.Render(b));
    // Frequency by zero crossings in the middle.
    size_t crossings = 0;
    const size_t a = y.size() / 4, z = 3 * y.size() / 4;
    for (size_t i = a + 1; i < z; ++i) crossings += (y[i - 1] < 0) != (y[i] < 0);
    // Fundamental: the composite wave crosses zero twice per 200 Hz period.
    const float f = crossings * 0.5f / ((z - a) / 24000.0f);
    // Steady-state level (Hann OLA must sum to 1): input RMS is 0.2475.
    const float level = Rms(y, a, z) / 0.2475f;
    char name[64];
    snprintf(name, sizeof(name), "x%.1f: duration, pitch, level", factor);
    Check(y.size() == out_length && fabsf(f / 200.0f - 1.0f) < 0.02f && \
          fabsf(level - 1.0f) < 0.1f, name, "%zu samples, %.1f Hz, level %.2f",
          y.size(), f, level);
  }
}

static void TestStretchMode() {
  printf("stretch-to-fit mode\n");
  std::vector<float> x = SpeechThenSilence(8.0f, 14.0f, 7);
  float silent_fraction[2];
  Result r;
  for (int k = 0; k < 2; ++k) {
    static SequencerEngine e;
    e.Init(kSr, speech_storage, reverb_buffer);
    Run(&e, x, &r, [&](size_t s, SequencerParams* p) {
      p->freeze = s > size_t(8.0f * kSr);
      p->density = 0.15f;  // slow steps (~290 ms): units get stretched
      p->submode = k ? SEQUENCER_SUBMODE_SYLLABLE_STRETCH
                     : SEQUENCER_SUBMODE_SYLLABLE_TRUNCATE;
    });
    size_t silent = 0, total = 0;
    for (size_t i = size_t(9.0f * kSr); i + 480 < r.y.size(); i += 480) {
      ++total;
      if (Rms(r.y, i, i + 480) < 1e-3f) ++silent;
    }
    silent_fraction[k] = float(silent) / float(total);
  }
  Check(silent_fraction[1] < 0.5f * silent_fraction[0],
        "stretch fills the steps (vs truncate)",
        "%.0f%% vs %.0f%% of 10 ms windows silent", 100 * silent_fraction[1],
        100 * silent_fraction[0]);
  Check(AllFinite(r.y) && Peak(r.y) < 1.2f, "output finite and bounded", "peak %.2f",
        Peak(r.y));
}

static void TestStability() {
  printf("stability (40 s random controls)\n");
  std::vector<float> x = Speech(40.0f, 8);
  static SequencerEngine e;
  e.Init(kSr, speech_storage, reverb_buffer);
  TestRandom rng(9);
  SequencerParams q = DefaultParams();
  Result r;
  Run(&e, x, &r, [&](size_t s, SequencerParams* p) {
    if (s % 4800 == 0) {
      q.time = rng.Uniform(); q.size = rng.Uniform(); q.shape = rng.Uniform();
      q.density = rng.Uniform(); q.pitch = rng.Range(-24, 24);
      q.repeat = rng.Uniform(); q.dry_wet = rng.Uniform(); q.reverb = rng.Uniform();
      q.freeze = rng.Uniform() < 0.3f; q.clock_patched = rng.Uniform() < 0.5f;
      q.lock = rng.Uniform() < 0.2f; q.submode = uint8_t(rng.Int(4));
      q.pause_removal = rng.Uniform() < 0.3f; q.swing = uint8_t(rng.Int(3));
      q.gate_mode = uint8_t(rng.Int(3));
    }
    *p = q;
    p->clock_edge = (q.clock_patched && (s % 4000) < kBlock) ? 0 : -1;
    p->tap = (s % 20000) < kBlock;
    p->reset = (s % 33000) < kBlock;
  });
  Check(AllFinite(r.y), "no NaN / Inf", "ok");
  Check(Peak(r.y) < 1.5f, "output bounded", "peak %.2f", Peak(r.y));
}

static void TestPerformance() {
  printf("performance (host, informational)\n");
  std::vector<float> x = Speech(10.0f, 10);
  static SequencerEngine e;
  e.Init(kSr, speech_storage, reverb_buffer);
  Result r;
  auto t0 = std::chrono::high_resolution_clock::now();
  Run(&e, x, &r, [&](size_t, SequencerParams* p) {
    p->density = 1.0f;
    p->submode = SEQUENCER_SUBMODE_SYLLABLE_STRETCH;
    p->reverb = 0.5f;
  });
  auto t1 = std::chrono::high_resolution_clock::now();
  const double s = std::chrono::duration<double>(t1 - t0).count();
  Info("speed", "%.1fx real time (%.0f ns/sample)", 10.0 / s, 1e9 * s / x.size());
}

static int ProcessFile(int argc, char** argv) {
  if (argc < 4) return 2;
  SequencerParams base = DefaultParams();
  float freeze_at = -1.0f, clock_hz = 0.0f;
  for (int i = 4; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    const float v = float(atof(argv[i + 1]));
    if (a == "--submode") base.submode = uint8_t(v);
    else if (a == "--density") base.density = v;
    else if (a == "--time") base.time = v;
    else if (a == "--size") base.size = v;
    else if (a == "--shape") base.shape = v;
    else if (a == "--repeat") base.repeat = v;
    else if (a == "--pitch") base.pitch = v;
    else if (a == "--freeze-at") freeze_at = v;
    else if (a == "--clock-hz") clock_hz = v;
    else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  Audio in;
  if (!ReadWav(argv[2], &in)) return 1;
  Resample(&in, int(kSr));
  std::vector<float> x(in.size());
  for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * (in.l[i] + in.r[i]);
  static SequencerEngine e;
  e.Init(kSr, speech_storage, reverb_buffer);
  Result r;
  Run(&e, x, &r, [&](size_t s, SequencerParams* p) {
    *p = base;
    p->freeze = freeze_at >= 0.0f && s >= size_t(freeze_at * kSr);
    if (clock_hz > 0.0f) Edges(s, size_t(kSr / clock_hz), p);
  });
  Audio out;
  out.sample_rate = int(kSr);
  out.channels = 2;
  out.l = r.y;
  for (float& v : out.l) v = std::max(-1.0f, std::min(1.0f, v));
  out.r = out.l;
  printf("%u steps\n", e.steps());
  return WriteWav(argv[3], out) ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !strcmp(argv[1], "process")) return ProcessFile(argc, argv);
  TestInternalClock();
  TestExternalClock();
  TestOrder();
  TestResetAndSwing();
  TestWsola();
  TestStretchMode();
  TestStability();
  TestPerformance();
  return Summary();
}
