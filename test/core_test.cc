// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// End-to-end tests of the VOX cores (vox/host): the firmwares compiled for a
// computer and driven through the same C interface as the VCV Rack module,
// with button presses, knob positions and jack voltages.
//
//   make -f vox/host/core.mk all
//   build/vox_test/core_test selftest [build/vox_core]

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "vox/host/core_loader.h"
#include "vox/test/test_signals.h"
#include "vox/test/test_util.h"

using namespace vox_test;

namespace {

std::string core_dir = "build/vox_core";
const float kSr = 48000.0f;
const int kBlock = VOX_CORE_BLOCK_SIZE;

// State chunk offsets (vox/common/hw/settings.h).
enum {
  STATE_GATE_OUTPUT_MODE = 6,
  STATE_VOX_MAGIC = 10,
  STATE_VOX_FIRMWARE = 11,
  STATE_VOX_SUBMODE = 12,
  STATE_VOX_FLAGS = 13
};

std::string CorePath(const std::string& name) {
#if defined(__APPLE__)
  return core_dir + "/vox_" + name + ".dylib";
#elif defined(_WIN32)
  return core_dir + "/vox_" + name + ".dll";
#else
  return core_dir + "/vox_" + name + ".so";
#endif
}

// One emulated module.
struct Module {
  vox_host::Core core;
  VoxCoreControls c;
  std::vector<float> out_l, out_r;
  const std::vector<float>* input;
  size_t input_pos;

  bool Load(const std::string& name) {
    memset(&c, 0, sizeof(c));
    for (int i = 0; i < VOX_POT_LAST; ++i) c.pot[i] = 0.5f;
    c.pot[VOX_POT_TIME] = 0.0f;
    c.pot[VOX_POT_FEEDBACK] = 0.0f;
    c.pot[VOX_POT_DRY_WET] = 1.0f;
    c.pot[VOX_POT_REVERB] = 0.0f;
    input = NULL;
    input_pos = 0;
    return core.Load(CorePath(name), "/tmp");
  }

  void PowerOn() {
    core.set_controls(&c);
    core.power_on();
  }

  // Runs `ms` milliseconds (rounded to blocks), recording the outputs.
  void Run(float ms) {
    const int blocks = int(ms * kSr / 1000.0f / kBlock + 0.5f);
    float in_l[kBlock], in_r[kBlock], ol[kBlock], or_[kBlock];
    uint8_t gates[kBlock] = { 0 };
    for (int b = 0; b < blocks; ++b) {
      for (int i = 0; i < kBlock; ++i) {
        float x = 0.0f;
        if (input && !input->empty()) {
          x = (*input)[input_pos] * 5.0f;
          input_pos = (input_pos + 1) % input->size();
        }
        in_l[i] = x;
        in_r[i] = 0.0f;
      }
      core.set_controls(&c);
      core.process(in_l, in_r, gates, ol, or_);
      out_l.insert(out_l.end(), ol, ol + kBlock);
      out_r.insert(out_r.end(), or_, or_ + kBlock);
    }
  }

  void Press(int button, float ms = 60.0f) {
    c.button[button] = 1;
    Run(ms);
    c.button[button] = 0;
    Run(60.0f);
  }

  VoxCoreStatus Status() {
    VoxCoreStatus s;
    core.get_status(&s);
    return s;
  }

  std::vector<uint8_t> State() {
    std::vector<uint8_t> s(64);
    s.resize(core.get_state(s.data(), s.size()));
    return s;
  }

  // Power-on with MEDIA held, PITCH in the middle of the algorithm's zone,
  // release, then a fresh press of MEDIA to confirm.
  void BootInto(int algorithm, int n) {
    const float pitch = c.pot[VOX_POT_PITCH];
    c.button[VOX_BUTTON_MEDIA] = 1;
    PowerOn();
    Run(500.0f);
    c.button[VOX_BUTTON_MEDIA] = 0;
    c.pot[VOX_POT_PITCH] = (float(algorithm) + 0.5f) / float(n);
    Run(600.0f);
    Press(VOX_BUTTON_MEDIA, 80.0f);
    c.pot[VOX_POT_PITCH] = pitch;
  }

  void ClearOutput() { out_l.clear(); out_r.clear(); }
};

float RmsRange(const std::vector<float>& x, size_t a, size_t b) {
  if (b > x.size()) b = x.size();
  if (a >= b) return 0.0f;
  return Rms(x, a, b);
}

std::vector<float> Speech(float seconds) {
  GroundTruth gt;
  return SpeechLike(DefaultSpeechOptions(seconds, kSr), &gt);
}

int SelfTest() {
  const char* names[] = { "psola", "lpc", "syllable", "split", "sequencer", "chain" };
  const int n = 6;
  const std::vector<float> speech = Speech(12.0f);

  // Identification.
  {
    Module m;
    const bool ok = m.Load("multi");
    Check(ok, "vox_multi core loads", "%s", ok ? CorePath("multi").c_str() : m.core.error().c_str());
    if (!ok) return Summary();
    bool names_ok = m.core.num_algorithms() == n &&
        std::string(m.core.firmware_name()) == "vox_multi";
    for (int i = 0; i < n && names_ok; ++i) {
      names_ok = std::string(m.core.algorithm_name(i)) == names[i];
    }
    Check(names_ok, "identification: vox_multi, 6 algorithms in selection order",
          "%s, %d algorithms", m.core.firmware_name(), m.core.num_algorithms());
  }

  // Blank settings: boots into algorithm 1 and writes its settings.
  {
    Module m;
    m.Load("multi");
    m.PowerOn();
    m.Run(400.0f);
    VoxCoreStatus s = m.Status();
    std::vector<uint8_t> st = m.State();
    Check(s.phase == VOX_PHASE_RUNNING && s.algorithm == 0,
          "blank settings: runs algorithm 1 (psola)", "phase %d, algorithm %d", s.phase, s.algorithm);
    Check(st.empty(), "nothing saved until a setting changes (as on the module)",
          "%zu bytes", st.size());
    const VoxCoreStatus before = m.Status();
    m.Press(VOX_BUTTON_MEDIA);
    st = m.State();
    Check(st.size() == 24 && st[STATE_VOX_MAGIC] == 0xA5 && st[STATE_VOX_FIRMWARE] == 0x80 &&
          st[STATE_VOX_SUBMODE] == 1,
          "MEDIA press: sub-mode 1 saved (stock 24-byte layout)", "%zu bytes, firmware 0x%02x",
          st.size(), st.empty() ? 0 : st[STATE_VOX_FIRMWARE]);
    Check(before.led[VOX_LED_MEDIA_G] > 0.9f && before.led[VOX_LED_MEDIA_R] < 0.1f,
          "MEDIA LED green (psola, first sub-mode)", "r %.2f g %.2f b %.2f",
          before.led[VOX_LED_MEDIA_R], before.led[VOX_LED_MEDIA_G], before.led[VOX_LED_MEDIA_B]);
  }

  // Boot-time selection, step by step.
  {
    Module m;
    m.Load("multi");
    m.c.button[VOX_BUTTON_MEDIA] = 1;
    m.PowerOn();
    m.Run(500.0f);
    VoxCoreStatus s = m.Status();
    Check(s.phase == VOX_PHASE_SELECTING, "MEDIA held at power-on: selection", "phase %d", s.phase);
    // FREEZE and SEED alternate.
    int alternate = 0, samples = 0;
    for (int k = 0; k < 20; ++k) {
      m.Run(50.0f);
      VoxCoreStatus t = m.Status();
      ++samples;
      if ((t.led[VOX_LED_FREEZE] > 0.5f) != (t.led[VOX_LED_SEED] > 0.5f)) ++alternate;
    }
    Check(alternate == samples, "FREEZE and SEED LEDs blink alternately", "%d of %d", alternate, samples);
    m.Run(1000.0f);  // still holding MEDIA: no confirmation
    Check(m.Status().phase == VOX_PHASE_SELECTING, "holding MEDIA does not confirm", "ok");
    m.c.button[VOX_BUTTON_MEDIA] = 0;

    bool zones_ok = true;
    for (int a = 0; a < n; ++a) {
      m.c.pot[VOX_POT_PITCH] = (float(a) + 0.5f) / float(n);
      m.Run(200.0f);
      VoxCoreStatus t = m.Status();
      const int code = (t.led[VOX_LED_ASSIGN_1] > 0.5f ? 1 : 0) |
          (t.led[VOX_LED_ASSIGN_2] > 0.5f ? 2 : 0) | (t.led[VOX_LED_ASSIGN_3] > 0.5f ? 4 : 0);
      if (t.algorithm != a || code != a + 1) {
        zones_ok = false;
        Info("zone", "PITCH zone %d: algorithm %d, ASSIGN LEDs %d", a, t.algorithm, code);
      }
    }
    Check(zones_ok, "PITCH zones 1..6, ASSIGN LEDs count in binary", "ok");
    m.c.pot[VOX_POT_PITCH] = 2.5f / 6.0f;
    m.Run(200.0f);
    VoxCoreStatus t = m.Status();
    Check(t.led[VOX_LED_MEDIA_R] > 0.9f && fabsf(t.led[VOX_LED_MEDIA_G] - 160.0f / 255.0f) < 0.02f &&
          t.led[VOX_LED_MEDIA_B] < 0.05f, "zone 3: MEDIA LED yellow", "r %.2f g %.2f b %.2f",
          t.led[VOX_LED_MEDIA_R], t.led[VOX_LED_MEDIA_G], t.led[VOX_LED_MEDIA_B]);
    m.Press(VOX_BUTTON_MEDIA, 80.0f);
    m.Run(300.0f);
    t = m.Status();
    std::vector<uint8_t> st = m.State();
    Check(t.phase == VOX_PHASE_RUNNING && t.algorithm == 2 && st[STATE_VOX_FIRMWARE] == 0x82,
          "MEDIA press confirms: syllable runs, choice saved", "phase %d, algorithm %d, firmware 0x%02x",
          t.phase, t.algorithm, st[STATE_VOX_FIRMWARE]);
    Check(st[STATE_VOX_SUBMODE] == 0, "confirming press does not change the sub-mode",
          "sub-mode %d", st[STATE_VOX_SUBMODE]);

    // Next power-on without MEDIA: same algorithm, from the saved settings.
    Module m2;
    m2.Load("multi");
    std::vector<uint8_t> persistent(512);
    persistent.resize(m.core.get_persistent(persistent.data(), persistent.size()));
    m2.core.set_storage(persistent.data(), persistent.size(), st.data(), st.size());
    m2.PowerOn();
    m2.Run(300.0f);
    Check(m2.Status().algorithm == 2, "next power-on: last algorithm", "algorithm %d", m2.Status().algorithm);
  }

  // Two emulated modules are independent (separate copies of the core).
  {
    Module a, b;
    a.Load("multi");
    b.Load("multi");
    a.PowerOn();
    b.PowerOn();
    a.Run(300.0f);
    b.Run(300.0f);
    a.Press(VOX_BUTTON_MEDIA);
    a.Press(VOX_BUTTON_MEDIA);
    const int sa = a.State()[STATE_VOX_SUBMODE], sb = b.State()[STATE_VOX_SUBMODE];
    Check(sa == 2 && sb == 0, "two modules keep separate state", "sub-modes %d and %d", sa, sb);
  }

  // Each algorithm keeps its sub-mode across switches.
  {
    Module m;
    m.Load("multi");
    m.BootInto(2, n);
    m.Run(200.0f);
    m.Press(VOX_BUTTON_MEDIA);                 // syllable: sub-mode 1
    std::vector<uint8_t> st = m.State();
    m.core.set_storage(NULL, 0, st.data(), st.size());
    m.BootInto(4, n);                          // sequencer
    m.Run(200.0f);
    const int seq_before = m.State()[STATE_VOX_SUBMODE];
    m.Press(VOX_BUTTON_MEDIA);
    m.Press(VOX_BUTTON_MEDIA);
    m.Press(VOX_BUTTON_MEDIA);                 // sequencer: sub-mode 3
    st = m.State();
    m.core.set_storage(NULL, 0, st.data(), st.size());
    m.BootInto(2, n);
    m.Run(200.0f);
    const int syl = m.State()[STATE_VOX_SUBMODE];
    st = m.State();
    m.core.set_storage(NULL, 0, st.data(), st.size());
    m.BootInto(4, n);
    m.Run(200.0f);
    const int seq = m.State()[STATE_VOX_SUBMODE];
    Check(seq_before == 0 && syl == 1 && seq == 3, "sub-mode kept per algorithm",
          "syllable %d, sequencer %d (fresh %d)", syl, seq, seq_before);
  }

  // Every algorithm processes speech: output finite, bounded, not silent.
  for (int a = 0; a < n; ++a) {
    Module m;
    m.Load("multi");
    m.BootInto(a, n);
    m.c.patched[VOX_JACK_IN_L] = 1;
    // DENSITY: noon holds the read position in psola and lpc; 0.765 = 1x.
    if (a == 0 || a == 1) m.c.pot[VOX_POT_DENSITY] = 0.765f;
    if (a == 2) m.c.pot[VOX_POT_DENSITY] = 0.85f;   // syllable: grain generator on
    if (a == 4) m.c.pot[VOX_POT_DENSITY] = 0.6f;    // sequencer: internal tempo
    m.input = &speech;
    m.ClearOutput();
    m.Run(8000.0f);
    const size_t skip = size_t(2.0f * kSr);
    const float rms = RmsRange(m.out_l, skip, m.out_l.size());
    const float in_rms = Rms(speech, 0, speech.size()) * 5.0f;
    const bool finite = AllFinite(m.out_l) && AllFinite(m.out_r);
    const float peak = std::max(Peak(m.out_l), Peak(m.out_r));
    Check(finite && peak <= 5.0f && Db(rms / in_rms) > -30.0f,
          (std::string(names[a]) + ": processes speech").c_str(),
          "%.1f dB re input, peak %.2f V, load %.1f%%", Db(rms / in_rms), peak,
          100.0f * m.Status().cpu_load);
  }

  // Gate output (ASSIGN + SEED) in LPC: OUT R = voiced gate.
  {
    Module m;
    m.Load("multi");
    m.BootInto(1, n);
    m.c.patched[VOX_JACK_IN_L] = 1;
    m.c.pot[VOX_POT_DENSITY] = 0.765f;
    m.input = &speech;
    m.Run(1000.0f);
    m.c.button[VOX_BUTTON_ASSIGN] = 1;
    m.Run(60.0f);
    m.c.button[VOX_BUTTON_SEED] = 1;
    m.Run(60.0f);
    m.c.button[VOX_BUTTON_SEED] = 0;
    m.c.button[VOX_BUTTON_ASSIGN] = 0;
    m.Run(100.0f);
    m.ClearOutput();
    m.Run(4000.0f);
    size_t high = 0, other = 0;
    for (float v : m.out_r) {
      if (fabsf(v - 4.0f) < 0.01f) ++high; else if (fabsf(v) > 0.01f) ++other;
    }
    const std::vector<uint8_t> st = m.State();
    Check(st[STATE_GATE_OUTPUT_MODE] == 1 && other == 0 && high > m.out_r.size() / 10,
          "ASSIGN + SEED: OUT R becomes the voiced gate (+4 V)", "mode %d, high %.0f%%, other %zu",
          st[STATE_GATE_OUTPUT_MODE], 100.0f * high / m.out_r.size(), other);
  }

  // Single firmwares load and run too.
  for (int a = 0; a < n; ++a) {
    Module m;
    const bool ok = m.Load(names[a]);
    bool runs = false;
    if (ok) {
      m.c.button[VOX_BUTTON_MEDIA] = 1;  // no selection step in single firmwares
      m.PowerOn();
      m.Run(300.0f);
      runs = m.core.num_algorithms() == 1 && m.Status().phase == VOX_PHASE_RUNNING;
    }
    Check(ok && runs, (std::string("vox_") + names[a] + " core: loads, no selection step").c_str(),
          "%s", ok ? "ok" : m.core.error().c_str());
  }

  // Long run of Syllable (2 minutes of speech): the output never goes silent
  // for long.
  {
    Module m;
    m.Load("multi");
    m.BootInto(2, n);
    m.c.patched[VOX_JACK_IN_L] = 1;
    m.c.pot[VOX_POT_DENSITY] = 0.85f;
    m.input = &speech;
    m.ClearOutput();
    m.Run(120000.0f);
    const size_t w = size_t(kSr);
    int silent = 0, worst_at = -1;
    for (size_t i = 2 * w; i + w <= m.out_l.size(); i += w) {
      if (RmsRange(m.out_l, i, i + w) < 1e-3f) {
        ++silent;
        if (worst_at < 0) worst_at = int(i / w);
      }
    }
    Check(silent == 0, "syllable: 2 minutes without a silent second", "%d silent seconds (first at %d s)",
          silent, worst_at);
  }

  return Summary();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3) core_dir = argv[2];
  if (argc >= 2 && !strcmp(argv[1], "selftest")) return SelfTest();
  fprintf(stderr, "usage: core_test selftest [core directory]\n");
  return 2;
}
