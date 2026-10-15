// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Host shim of beads/drivers/switches.h: the stock 8-sample debouncer, fed by
// vox_host::switch_pressed (true while the button is held).

#ifndef BEADS_DRIVERS_SWITCHES_H_
#define BEADS_DRIVERS_SWITCHES_H_

#include <algorithm>

#include "stmlib/stmlib.h"

namespace beads {

enum Switch {
  SWITCH_FREEZE,
  SWITCH_SEED,
  SWITCH_MEDIA,
  SWITCH_ASSIGN,
  SWITCH_LAST
};

}  // namespace beads

namespace vox_host {
extern volatile bool switch_pressed[beads::SWITCH_LAST];
}

namespace beads {

class Switches {
 public:
  Switches() { }
  ~Switches() { }

  bool Init() {
    std::fill(&switch_state_[0], &switch_state_[SWITCH_LAST], 0xff);
    return true;
  }

  // As stock: the pins read 1 when released (pull-up), 0 when pressed.
  void Debounce() {
    for (int i = 0; i < SWITCH_LAST; ++i) {
      switch_state_[i] = (switch_state_[i] << 1) | \
          (vox_host::switch_pressed[i] ? 0 : 1);
    }
  }

  inline bool released(Switch s) const { return switch_state_[s] == 0x7f; }
  inline bool just_pressed(Switch s) const { return switch_state_[s] == 0x80; }
  inline bool pressed(Switch s) const { return switch_state_[s] == 0x00; }
  inline bool pressed_immediate(Switch s) const {
    return vox_host::switch_pressed[s];
  }

 private:
  uint8_t switch_state_[SWITCH_LAST];

  DISALLOW_COPY_AND_ASSIGN(Switches);
};

}  // namespace beads

#endif  // BEADS_DRIVERS_SWITCHES_H_
