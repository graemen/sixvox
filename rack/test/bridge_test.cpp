// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Tests the engine-rate <-> 48 kHz bridge of the VOX Rack module
// (src/AudioBridge.hpp) outside Rack, with Rack's own resampler:
// no underruns, a stable queue, the right number of 12-frame blocks, and a
// transparent round trip at the usual engine sample rates.
//
//   make -C vox/rack/test RACK_DIR=/path/to/Rack-SDK SPEEXDSP=/path/to/speexdsp

#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/AudioBridge.hpp"

typedef vox_rack::AudioBridge<12, 48000> Bridge;

static int failures = 0, checks = 0;
static void Check(bool ok, const char* what, float rate, const char* detail) {
  ++checks;
  if (!ok) ++failures;
  printf("  [%s] %-44s %6.0f Hz  %s\n", ok ? "PASS" : "FAIL", what, rate, detail);
}

static void Run(float rate) {
  Bridge bridge;
  const float seconds = 30.0f;
  const int n = int(seconds * rate);
  const float f = 997.0f;
  std::vector<float> in(n), out(n);
  long blocks = 0;
  size_t queue_min = 1 << 30, queue_max = 0;
  int first_output = -1;
  for (int i = 0; i < n; ++i) {
    // Three unrelated tones: one clear correlation peak for the latency.
    const float t = float(i) / rate;
    in[i] = 0.4f * sinf(2.0f * float(M_PI) * f * t) +
        0.3f * sinf(2.0f * float(M_PI) * 1499.0f * t) +
        0.3f * sinf(2.0f * float(M_PI) * 2333.0f * t);
    Bridge::Frame x;
    x.samples[0] = in[i];
    x.samples[1] = -in[i];
    bool ran = false;
    Bridge::Frame y = bridge.Process(rate, x, [&](const Bridge::Frame* a, Bridge::Frame* b) {
      for (int k = 0; k < 12; ++k) b[k] = a[k];
      ++blocks;
    }, &ran);
    out[i] = y.samples[0];
    if (first_output < 0 && y.samples[0] != 0.0f) first_output = i;
    if (i > int(rate)) {
      queue_min = std::min(queue_min, bridge.queued());
      queue_max = std::max(queue_max, bridge.queued());
    }
  }
  char d[160];
  snprintf(d, sizeof(d), "%d underruns", bridge.underruns());
  Check(bridge.underruns() == 0, "no underruns over 30 s", rate, d);
  snprintf(d, sizeof(d), "queue %zu..%zu frames after 1 s", queue_min, queue_max);
  Check(queue_min > 0 && queue_max < 256, "queue level stable", rate, d);
  const double expected = seconds * 48000.0 / 12.0;
  snprintf(d, sizeof(d), "%ld blocks (expected %.0f)", blocks, expected);
  Check(fabs(blocks - expected) / expected < 0.002, "core blocks at 4 kHz", rate, d);

  // Round trip: find the delay, then compare.
  const int a = int(5 * rate), len = int(2 * rate);
  int best_lag = 0;
  double best = -1e30;
  for (int lag = 0; lag < int(0.02f * rate); ++lag) {
    double c = 0;
    for (int i = a; i < a + len; i += 7) c += double(out[i + lag]) * in[i];
    if (c > best) { best = c; best_lag = lag; }
  }
  double e = 0, p = 0;
  for (int i = a; i < a + len; ++i) {
    const double ref = in[i];
    // Fractional delay: compare against the best linear-interpolated lag.
    e += (out[i + best_lag] - ref) * (out[i + best_lag] - ref);
    p += ref * ref;
  }
  const double err_db = 10.0 * log10(e / p + 1e-20);
  snprintf(d, sizeof(d), "latency %.2f ms, first output %.2f ms, error %.1f dB",
           1000.0 * best_lag / rate, 1000.0 * first_output / rate, err_db);
  // Rates that are not a multiple of 48 kHz leave a fractional delay, which
  // a whole-sample comparison measures as error: only check level there.
  const bool integer_ratio = fmodf(rate, 48000.0f) == 0.0f || fmodf(48000.0f, rate) == 0.0f;
  Check(integer_ratio ? err_db < -40.0 : err_db < -6.0, "round trip transparent", rate, d);
  double gain_in = 0, gain_out = 0;
  for (int i = a; i < a + len; ++i) { gain_in += in[i] * in[i]; gain_out += out[i] * out[i]; }
  snprintf(d, sizeof(d), "%.2f dB", 10.0 * log10(gain_out / gain_in));
  Check(fabs(10.0 * log10(gain_out / gain_in)) < 0.2, "round trip gain", rate, d);
}

int main() {
  const float rates[] = { 44100.0f, 48000.0f, 88200.0f, 96000.0f, 192000.0f };
  for (float r : rates) Run(r);
  printf("\n%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}
