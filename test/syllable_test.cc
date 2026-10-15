// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Host tests for the vox_syllable engine.
//
//   syllable_test                      self tests
//   syllable_test process in.wav out.wav [--submode N] [--time x] [--size x]
//       [--shape x] [--density x] [--pitch st] [--feedback x] [--mix x]
//       [--reverb x] [--freeze-at seconds]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "vox/syllable/dsp/syllable_engine.h"
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

static SyllableParams DefaultParams() {
  SyllableParams p;
  p.time = 0.0f;
  p.size = 0.5f;
  p.shape = 0.0f;
  p.density = 0.5f;
  p.pitch = 0.0f;
  p.feedback = 0.0f;
  p.dry_wet = 1.0f;
  p.reverb = 0.0f;
  p.freeze = false;
  p.trigger = false;
  p.seed_patched = false;
  p.latch = false;
  p.submode = SYLLABLE_SUBMODE_SYLLABLE;
  p.pause_removal = false;
  p.sensitivity = 1;
  return p;
}

// Runs the engine on a mono signal (fed to both inputs).
static void Run(
    SyllableEngine* e,
    const std::vector<float>& x,
    std::vector<float>* y,
    std::vector<bool>* gate,
    std::function<SyllableParams(size_t)> params) {
  y->assign(x.size(), 0.0f);
  if (gate) gate->assign(x.size(), false);
  FloatFrame in[kBlock], out[kBlock];
  bool g[kBlock];
  for (size_t start = 0; start + kBlock <= x.size(); start += kBlock) {
    for (size_t i = 0; i < kBlock; ++i) in[i].l = in[i].r = x[start + i];
    SyllableParams p = params(start);
    e->Process(p, in, out, g, kBlock);
    for (size_t i = 0; i < kBlock; ++i) {
      (*y)[start + i] = out[i].l;
      if (gate) (*gate)[start + i] = g[i];
    }
  }
}

static std::vector<float> Speech(float seconds, uint32_t seed, GroundTruth* gt) {
  SpeechLikeOptions o = DefaultSpeechOptions(seconds, kSr);
  o.seed = seed;
  return SpeechLike(o, gt);
}

static void TestSegmentationThroughEngine() {
  printf("segmentation through the engine (48 kHz in, 24 kHz analysis)\n");
  GroundTruth gt;
  std::vector<float> x = Speech(10.0f, 77, &gt), y;
  static SyllableEngine e;
  e.Init(kSr, speech_storage, reverb_buffer);
  Run(&e, x, &y, NULL, [](size_t) { return DefaultParams(); });
  const UnitTable<kMaxUnits>& t = e.segmenter().table(UNIT_LEVEL_SYLLABLE);
  // Buffer positions are 24 kHz sample indices: convert the reference.
  std::vector<size_t> ref, det;
  for (const Span& s : gt.syllables) ref.push_back(s.start / 2);
  for (int i = 0; i < t.size(); ++i) det.push_back(t.FromOldest(i).start);
  const float recall = BoundaryRecall(ref, det, 24 * 40);
  const float precision = BoundaryPrecision(ref, det, 24 * 40);
  Check(recall >= 0.8f && precision >= 0.8f, "syllables found / correct",
        "%.0f%% / %.0f%% (%zu ref, %zu det)", 100 * recall, 100 * precision,
        ref.size(), det.size());
}

static void TestAddressing() {
  printf("TIME addressing\n");
  GroundTruth gt;
  std::vector<float> x = Speech(10.0f, 5, &gt), y;
  static SyllableEngine e;
  e.Init(kSr, speech_storage, reverb_buffer);
  Run(&e, x, &y, NULL, [](size_t) { return DefaultParams(); });
  SyllableParams p = DefaultParams();
  p.freeze = true;
  uint32_t previous = 0, start, length;
  bool reverse, monotonic = true, all = true;
  int distinct = 0;
  for (int i = 0; i <= 100; ++i) {
    p.time = i / 100.0f;
    if (!e.SelectUnit(p, &start, &length, &reverse)) { all = false; continue; }
    if (i > 0 && start < previous) monotonic = false;
    if (i == 0 || start != previous) ++distinct;
    previous = start;
  }
  const int n = e.segmenter().table(UNIT_LEVEL_SYLLABLE).size();
  Check(all && monotonic, "frozen: TIME sweep is monotonic", "%d units, %d addressed",
        n, distinct);
  Check(distinct == n, "frozen: every unit reachable", "%d / %d", distinct, n);
  p.freeze = false;
  p.time = 0.0f;
  e.SelectUnit(p, &start, &length, &reverse);
  const Unit& newest = e.segmenter().table(UNIT_LEVEL_SYLLABLE).FromNewest(0);
  Check(start == newest.start, "live: TIME = 0 is the newest unit", "ok");
  // SIZE: noon = one unit, CW = up to 8 units, CCW = fraction.
  p.freeze = true;
  p.time = 0.5f;
  p.size = 0.5f;
  e.SelectUnit(p, &start, &length, &reverse);
  const uint32_t one = length;
  p.size = 1.0f;
  e.SelectUnit(p, &start, &length, &reverse);
  const uint32_t eight = length;
  p.size = 0.0f;
  e.SelectUnit(p, &start, &length, &reverse);
  const uint32_t fraction = length;
  Check(eight > 4 * one && fraction < one / 3, "SIZE: fraction < 1 unit < 8 units",
        "%u < %u < %u samples", fraction, one, eight);
}

static void TestGrainFidelity() {
  printf("grain playback\n");
  GroundTruth gt;
  std::vector<float> x = Speech(8.0f, 9, &gt);
  static SyllableEngine e;
  e.Init(kSr, speech_storage, reverb_buffer);
  std::vector<float> y;
  // Record 8 s, then freeze and trigger one syllable in silence.
  std::vector<float> input = x;
  input.resize(x.size() + size_t(2.0f * kSr), 0.0f);
  const size_t trigger_at = x.size() + size_t(0.5f * kSr);
  uint32_t unit_start = 0, unit_length = 0;
  Run(&e, input, &y, NULL, [&](size_t s) {
    SyllableParams p = DefaultParams();
    p.freeze = s >= x.size();
    p.time = 0.5f;
    if (s >= trigger_at && s < trigger_at + kBlock) {
      bool reverse;
      e.SelectUnit(p, &unit_start, &unit_length, &reverse);
      p.trigger = true;
    }
    return p;
  });
  // Locate the grain in the output.
  size_t first = 0, last = 0;
  for (size_t i = trigger_at; i < y.size(); ++i) {
    if (fabsf(y[i]) > 1e-3f) {
      if (!first) first = i;
      last = i;
    }
  }
  const float expected_ms = unit_length / 24.0f;
  const float measured_ms = (last - trigger_at) / 48.0f;
  Check(first && fabsf(measured_ms - expected_ms) < 0.15f * expected_ms + 5.0f,
        "grain duration = unit duration", "%.0f ms vs %.0f ms", measured_ms,
        expected_ms);
  // Compare with the source (24 kHz positions -> 48 kHz input indices).
  const size_t source = size_t(unit_start) * 2;
  float best = 0.0f;
  for (int lag = 0; lag <= 480; ++lag) {
    double xy = 0, xx = 0, yy = 0;
    for (size_t i = 0; i < size_t(unit_length) * 2; ++i) {
      const long a = long(source + i), b = long(trigger_at + i) + lag;
      if (b < 0 || size_t(b) >= y.size() || size_t(a) >= x.size()) continue;
      xy += x[a] * y[b]; xx += x[a] * x[a]; yy += y[b] * y[b];
    }
    const float c = float(xy / sqrt(xx * yy + 1e-20));
    best = std::max(best, c);
  }
  Check(best > 0.9f, "grain reproduces the source unit", "correlation %.3f", best);
}

static void TestDensity() {
  printf("trigger generator\n");
  GroundTruth gt;
  std::vector<float> x = Speech(6.0f, 3, &gt), y;
  const float settings[3] = { 0.5f, 1.0f, 0.0f };
  const char* names[3] = { "noon: off", "CW: regular 20 Hz", "CCW: random 20 Hz" };
  for (int k = 0; k < 3; ++k) {
    static SyllableEngine e;
    e.Init(kSr, speech_storage, reverb_buffer);
    Run(&e, x, &y, NULL, [&](size_t s) {
      SyllableParams p = DefaultParams();
      p.density = s > size_t(2.0f * kSr) ? settings[k] : 0.5f;
      return p;
    });
    const float rate = e.triggers() / 4.0f;
    const bool ok = k == 0 ? e.triggers() == 0 : fabsf(rate - 20.0f) < (k == 1 ? 1.0f : 4.0f);
    Check(ok, names[k], "%.1f triggers/s", rate);
  }
}

static void TestLatch() {
  printf("latch\n");
  GroundTruth gt;
  std::vector<float> x = Speech(6.0f, 11, &gt), y;
  std::vector<float> input = x;
  input.resize(x.size() + size_t(3.0f * kSr), 0.0f);
  static SyllableEngine e;
  e.Init(kSr, speech_storage, reverb_buffer);
  const size_t latch_at = x.size();
  Run(&e, input, &y, NULL, [&](size_t s) {
    SyllableParams p = DefaultParams();
    p.freeze = s >= latch_at;
    p.time = 0.3f;
    p.shape = 0.3f;
    p.latch = s >= latch_at + size_t(0.1f * kSr);
    return p;
  });
  // The output must stay sounding (no dropout longer than 30 ms).
  size_t longest = 0, run = 0;
  for (size_t i = latch_at + size_t(0.3f * kSr); i < y.size(); ++i) {
    run = fabsf(y[i]) < 1e-4f ? run + 1 : 0;
    longest = std::max(longest, run);
  }
  Check(longest < size_t(0.030f * kSr), "latched unit loops without gaps",
        "longest silence %.1f ms (%u triggers)", longest / 48.0f, e.triggers());
}

static void TestPauseRemoval() {
  printf("pause removal\n");
  GroundTruth gt;
  std::vector<float> x = Speech(12.0f, 21, &gt), y;
  static SyllableEngine e;
  e.Init(kSr, speech_storage, reverb_buffer);
  Run(&e, x, &y, NULL, [](size_t) {
    SyllableParams p = DefaultParams();
    p.submode = SYLLABLE_SUBMODE_SYLLABLE_PAUSE_REMOVAL;
    return p;
  });
  // Expected: total silence beyond the first 150 ms of each pause is dropped.
  size_t silence = 0, dropped = 0;
  for (size_t i = 0; i < gt.label.size(); ++i) {
    silence = gt.label[i] == LABEL_SILENCE ? silence + 1 : 0;
    if (silence > size_t(0.25f * kSr)) ++dropped;  // generous margin
  }
  const float kept_s = e.buffer().head() / 24000.0f;
  Check(kept_s < x.size() / kSr - dropped / kSr, "pauses are removed from the buffer",
        "%.1f s recorded of %.1f s input", kept_s, x.size() / kSr);
  // Seams: the recorded signal must not contain steps larger than the input's.
  float max_step = 0.0f, input_step = 0.0f;
  for (uint32_t i = 1; i < e.buffer().head(); ++i) {
    max_step = std::max(max_step, fabsf(e.buffer().Read(i) - e.buffer().Read(i - 1)));
  }
  for (size_t i = 2; i < x.size(); i += 2) {
    input_step = std::max(input_step, fabsf(x[i] - x[i - 2]));
  }
  Check(max_step <= input_step * 1.2f, "no steps at the seams",
        "max step %.3f (input %.3f)", max_step, input_step);
  const UnitTable<kMaxUnits>& w = e.segmenter().table(UNIT_LEVEL_WORD);
  Check(w.size() >= int(gt.words.size() * 8 / 10), "words still segmented",
        "%d of %zu", w.size(), gt.words.size());
}

static void TestGateOut() {
  printf("gate output\n");
  GroundTruth gt;
  std::vector<float> x = Speech(10.0f, 31, &gt), y;
  std::vector<bool> gate;
  static SyllableEngine e;
  e.Init(kSr, speech_storage, reverb_buffer);
  Run(&e, x, &y, &gate, [](size_t) { return DefaultParams(); });
  std::vector<size_t> edges;
  for (size_t i = 1; i < gate.size(); ++i) if (gate[i] && !gate[i - 1]) edges.push_back(i);
  // Gate edges come after the onset (analysis latency); accept 0..60 ms late.
  size_t hits = 0;
  for (size_t o : gt.onsets) {
    for (size_t g : edges) {
      if (g + size_t(0.005f * kSr) >= o && g <= o + size_t(0.060f * kSr)) {
        ++hits;
        break;
      }
    }
  }
  const float recall = float(hits) / gt.onsets.size();
  Check(recall >= 0.8f, "trigger at each syllable onset (<= 60 ms late)",
        "%.0f%% (%zu triggers, %zu onsets)", 100 * recall, edges.size(),
        gt.onsets.size());
}

static void TestStability() {
  printf("stability (40 s random controls)\n");
  GroundTruth gt;
  std::vector<float> x = Speech(40.0f, 41, &gt), y;
  static SyllableEngine e;
  e.Init(kSr, speech_storage, reverb_buffer);
  TestRandom rng(5);
  SyllableParams p = DefaultParams();
  Run(&e, x, &y, NULL, [&](size_t s) {
    if (s % 4800 == 0) {
      p.time = rng.Uniform(); p.size = rng.Uniform(); p.shape = rng.Uniform();
      p.density = rng.Uniform(); p.pitch = rng.Range(-24, 24);
      p.feedback = rng.Uniform(); p.dry_wet = rng.Uniform();
      p.reverb = rng.Uniform(); p.freeze = rng.Uniform() < 0.3f;
      p.trigger = rng.Uniform() < 0.3f; p.latch = rng.Uniform() < 0.2f;
      p.submode = uint8_t(rng.Int(4)); p.pause_removal = rng.Uniform() < 0.2f;
      p.sensitivity = uint8_t(rng.Int(3));
    } else {
      p.trigger = false;
    }
    return p;
  });
  Check(AllFinite(y), "no NaN / Inf", "ok");
  Check(Peak(y) < 1.5f, "output bounded", "peak %.2f", Peak(y));
}

static void TestPerformance() {
  printf("performance (host, informational)\n");
  GroundTruth gt;
  std::vector<float> x = Speech(10.0f, 51, &gt), y;
  static SyllableEngine e;
  e.Init(kSr, speech_storage, reverb_buffer);
  auto t0 = std::chrono::high_resolution_clock::now();
  Run(&e, x, &y, NULL, [](size_t) {
    SyllableParams p = DefaultParams();
    p.density = 1.0f;
    p.shape = 0.6f;
    p.reverb = 0.5f;
    return p;
  });
  auto t1 = std::chrono::high_resolution_clock::now();
  const double s = std::chrono::duration<double>(t1 - t0).count();
  Info("speed", "%.1fx real time (%.0f ns/sample)", 10.0 / s, 1e9 * s / x.size());
}

static int ProcessFile(int argc, char** argv) {
  if (argc < 4) return 2;
  SyllableParams p = DefaultParams();
  p.density = 0.8f;
  float freeze_at = -1.0f;
  for (int i = 4; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    float v = float(atof(argv[i + 1]));
    if (a == "--submode") p.submode = uint8_t(v);
    else if (a == "--time") p.time = v;
    else if (a == "--size") p.size = v;
    else if (a == "--shape") p.shape = v;
    else if (a == "--density") p.density = v;
    else if (a == "--pitch") p.pitch = v;
    else if (a == "--feedback") p.feedback = v;
    else if (a == "--mix") p.dry_wet = v;
    else if (a == "--reverb") p.reverb = v;
    else if (a == "--freeze-at") freeze_at = v;
    else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  Audio in;
  if (!ReadWav(argv[2], &in)) return 1;
  Resample(&in, int(kSr));
  std::vector<float> x(in.size()), y;
  for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * (in.l[i] + in.r[i]);
  static SyllableEngine e;
  e.Init(kSr, speech_storage, reverb_buffer);
  Run(&e, x, &y, NULL, [&](size_t s) {
    SyllableParams q = p;
    q.freeze = freeze_at >= 0.0f && s >= size_t(freeze_at * kSr);
    return q;
  });
  Audio out;
  out.sample_rate = int(kSr);
  out.channels = 2;
  out.l = y;
  out.r = y;
  for (float& v : out.l) v = std::max(-1.0f, std::min(1.0f, v));
  out.r = out.l;
  printf("%d syllables, %d words, %d slices, %u grains\n",
         e.segmenter().table(UNIT_LEVEL_SYLLABLE).size(),
         e.segmenter().table(UNIT_LEVEL_WORD).size(),
         e.segmenter().table(UNIT_LEVEL_ONSET_SLICE).size(), e.triggers());
  return WriteWav(argv[3], out) ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !strcmp(argv[1], "process")) return ProcessFile(argc, argv);
  TestSegmentationThroughEngine();
  TestAddressing();
  TestGrainFidelity();
  TestDensity();
  TestLatch();
  TestPauseRemoval();
  TestGateOut();
  TestStability();
  TestPerformance();
  return Summary();
}
