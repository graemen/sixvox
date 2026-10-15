// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_multi host tests: per-algorithm settings slots and the PITCH-knob
// algorithm selector.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vox/common/app/algorithm_slots.h"
#include "vox/test/test_util.h"

using namespace vox;
using namespace vox_test;

static int SelfTest() {
  const int n = 6;

  // Each algorithm's (sub-mode, flags) pair survives any sequence of switches.
  {
    uint8_t submode = 0, flags = 0, slots[10] = { 0 };
    uint8_t expected_submode[n], expected_flags[n];
    int active = 0;
    // Give each algorithm its own values, visiting them in order.
    for (int a = 0; a < n; ++a) {
      SwitchAlgorithmSlots(&submode, &flags, slots, active, a, n);
      active = a;
      expected_submode[a] = uint8_t(a & 3);
      expected_flags[a] = uint8_t(0x11 * (a + 1));
      submode = expected_submode[a];
      flags = expected_flags[a];
    }
    srand(1);
    int errors = 0;
    for (int k = 0; k < 2000; ++k) {
      const int to = rand() % n;
      SwitchAlgorithmSlots(&submode, &flags, slots, active, to, n);
      active = to;
      if (submode != expected_submode[to] || flags != expected_flags[to]) {
        ++errors;
      }
      // The user changes the settings of the active algorithm now and then.
      if (rand() % 4 == 0) {
        submode = expected_submode[to] = uint8_t(rand() & 3);
        flags = expected_flags[to] = uint8_t(rand() & 0xff);
      }
    }
    Check(errors == 0, "settings kept per algorithm",
          "%d mismatches in 2000 random switches", errors);
  }

  // Fresh (zeroed) settings give every algorithm sub-mode 0, no flags.
  {
    uint8_t submode = 0, flags = 0, slots[10] = { 0 };
    bool all_zero = true;
    int active = 0;
    for (int a = 1; a < n; ++a) {
      SwitchAlgorithmSlots(&submode, &flags, slots, active, a, n);
      active = a;
      all_zero = all_zero && submode == 0 && flags == 0;
    }
    Check(all_zero, "fresh settings: defaults for every algorithm", "ok");
  }

  // Selecting the same algorithm changes nothing.
  {
    uint8_t submode = 2, flags = 0x5a, slots[10];
    for (int i = 0; i < 10; ++i) slots[i] = uint8_t(i * 7);
    uint8_t before[10];
    memcpy(before, slots, 10);
    SwitchAlgorithmSlots(&submode, &flags, slots, 3, 3, n);
    Check(submode == 2 && flags == 0x5a && !memcmp(before, slots, 10),
          "same algorithm: settings untouched", "ok");
  }

  // PITCH sweep: six zones in order, of equal width.
  {
    int zone = 0, transitions = 0, last = 0;
    float edges[n] = { 0.0f };
    bool ordered = true;
    for (int i = 0; i <= 1000; ++i) {
      const float v = float(i) / 1000.0f;
      zone = SelectZone(v, zone, n);
      if (zone != last) {
        ordered = ordered && zone == last + 1;
        edges[zone] = v;
        ++transitions;
        last = zone;
      }
    }
    Check(ordered && transitions == n - 1 && zone == n - 1,
          "PITCH CCW -> CW selects 1 .. 6 in order", "%d transitions", transitions);
    float worst = 0.0f;
    for (int z = 1; z < n; ++z) {
      // Rising edges sit 1/4 zone past the nominal boundary (hysteresis).
      const float expected = (float(z) + 0.25f) / float(n);
      const float e = edges[z] - expected;
      worst = e > worst ? e : (-e > worst ? -e : worst);
    }
    Check(worst < 0.002f, "zone edges where expected", "max error %.4f", worst);
    Check(SelectZone(0.0f, -1, n) == 0 && SelectZone(1.0f, -1, n) == n - 1,
          "knob ends select algorithms 1 and 6", "ok");
  }

  // Noise around a boundary does not make the selection flicker.
  {
    int zone = SelectZone(2.0f / 6.0f - 0.01f, -1, n);  // just inside zone 1
    int changes = 0;
    srand(2);
    for (int i = 0; i < 10000; ++i) {
      const float noise = (float(rand()) / RAND_MAX - 0.5f) * 0.06f;  // +-3 %
      const int z = SelectZone(2.0f / 6.0f + noise, zone, n);
      if (z != zone) ++changes;
      zone = z;
    }
    Check(changes == 0, "no flicker with +-3 % knob noise at a boundary",
          "%d changes", changes);
  }

  // MEDIA confirmation, driven through a copy of the stock debouncer
  // (beads/drivers/switches.cc: 8-sample shift register starting at 0xff).
  {
    struct Run {
      // pin[t]: raw MEDIA pin pressed at ms t. Returns the confirm time or -1.
      static int Confirm(const bool* pin, int length) {
        uint8_t state = 0xff;
        SelectionConfirm c;
        c.Start();
        for (int t = 0; t < length; ++t) {
          state = uint8_t((state << 1) | (pin[t] ? 0 : 1));
          if (c.Tick(pin[t], state == 0x80)) return t;
        }
        return -1;
      }
    };
    const int L = 6000;
    static bool pin[L];

    // Held 2 s at power-on, released, pressed again at 4 s.
    for (int t = 0; t < L; ++t) pin[t] = t < 2000 || (t >= 4000 && t < 4200);
    const int a = Run::Confirm(pin, L);
    Check(a >= 4000 && a < 4020, "boot hold, release, press: confirms at the press",
          "confirmed at %d ms (press at 4000 ms)", a);

    // Held at power-on and never released: never confirms.
    for (int t = 0; t < L; ++t) pin[t] = true;
    Check(Run::Confirm(pin, L) == -1, "boot hold without release: no confirm", "ok");

    // Held, then released for only 10 ms between two presses: too short.
    for (int t = 0; t < L; ++t) pin[t] = !(t >= 1000 && t < 1010);
    Check(Run::Confirm(pin, L) == -1, "10 ms gap is not a release", "ok");

    // Contact bounce on release and on the confirming press.
    for (int t = 0; t < L; ++t) {
      bool p = t < 1500;
      if (t >= 1500 && t < 1506) p = (t & 1);        // release bounce
      if (t >= 3000 && t < 3300) p = true;           // confirming press
      if (t >= 3000 && t < 3006) p = !(t & 1);       // press bounce
      pin[t] = p;
    }
    const int b = Run::Confirm(pin, L);
    Check(b >= 3000 && b < 3030, "bouncing contacts: one confirm at the press",
          "confirmed at %d ms (press at 3000 ms)", b);

    // Very short tap at power-on (released before the debouncer saw it).
    for (int t = 0; t < L; ++t) pin[t] = t < 3 || (t >= 2000 && t < 2100);
    const int c = Run::Confirm(pin, L);
    Check(c >= 2000 && c < 2020, "short tap at power-on, then press",
          "confirmed at %d ms (press at 2000 ms)", c);
  }

  // A stored index past the end (e.g. settings from a firmware with more
  // algorithms) is brought back into range by the selector.
  Check(SelectZone(0.5f, 9, n) == 3, "out-of-range stored index", "ok");

  return Summary();
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  return SelfTest();
}
