// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_multi settings: every algorithm keeps its own sub-mode and flags byte.
//
// The State chunk must keep the stock 24-byte layout, so there is room for
// the live pair (State::vox_submode / vox_flags, used by the active
// algorithm exactly as in its single firmware) plus 10 bytes of slots: the
// pairs of the other (up to 5) algorithms, in algorithm order, skipping the
// active one. Switching algorithm rotates the active pair into the slots and
// the new algorithm's pair out of them.
//
// Also: the PITCH-knob zone selector and the MEDIA confirmation logic used by
// the boot-time algorithm selection.

#ifndef VOX_COMMON_APP_ALGORITHM_SLOTS_H_
#define VOX_COMMON_APP_ALGORITHM_SLOTS_H_

#include <stdint.h>

namespace vox {

const int kMaxAlgorithms = 6;

// submode / flags: the live pair. slots: 2 * (kMaxAlgorithms - 1) bytes.
inline void SwitchAlgorithmSlots(
    uint8_t* submode, uint8_t* flags, uint8_t* slots,
    int from, int to, int num_algorithms) {
  if (from == to) return;
  uint8_t table[kMaxAlgorithms][2];
  int j = 0;
  for (int i = 0; i < num_algorithms; ++i) {
    if (i == from) {
      table[i][0] = *submode;
      table[i][1] = *flags;
    } else {
      table[i][0] = slots[2 * j];
      table[i][1] = slots[2 * j + 1];
      ++j;
    }
  }
  *submode = table[to][0] & 3;
  *flags = table[to][1];
  j = 0;
  for (int i = 0; i < num_algorithms; ++i) {
    if (i == to) continue;
    slots[2 * j] = table[i][0];
    slots[2 * j + 1] = table[i][1];
    ++j;
  }
}

// Knob position (0..1) to one of n equal zones, with hysteresis: the current
// zone is kept until the knob is 1/4 of a zone past its edge.
inline int SelectZone(float value, int current, int n) {
  if (current >= 0 && current < n) {
    const float lo = (float(current) - 0.25f) / float(n);
    const float hi = (float(current) + 1.25f) / float(n);
    if (value >= lo && value <= hi) return current;
  }
  int zone = int(value * float(n));
  if (zone < 0) zone = 0;
  if (zone > n - 1) zone = n - 1;
  return zone;
}

// Confirmation of the boot-time selection by a press of MEDIA. MEDIA is held
// at power-on to enter the selection, and the debouncer starts in the
// "released" state, so that hold would otherwise look like a fresh press:
// MEDIA must first read released (raw pin level) for 20 consecutive ticks,
// counted from 10 ticks after the start. Ticks are 1 ms (UI poll rate).
class SelectionConfirm {
 public:
  void Start() {
    ticks_ = 0;
    up_count_ = 0;
    released_ = false;
    confirmed_ = false;
  }

  // raw_pressed: undebounced pin level. just_pressed: debounced press edge.
  bool Tick(bool raw_pressed, bool just_pressed) {
    ++ticks_;
    if (!released_) {
      if (ticks_ > 10) {
        up_count_ = raw_pressed ? 0 : up_count_ + 1;
        released_ = up_count_ >= 20;
      }
    } else if (just_pressed) {
      confirmed_ = true;
    }
    return confirmed_;
  }

  inline bool released() const { return released_; }
  inline bool confirmed() const { return confirmed_; }

 private:
  uint32_t ticks_;
  int up_count_;
  bool released_;
  volatile bool confirmed_;  // written from the SysTick interrupt
};

}  // namespace vox

#endif  // VOX_COMMON_APP_ALGORITHM_SLOTS_H_
