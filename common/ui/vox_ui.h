// Copyright 2019 Emilie Gillet (original Beads UI).
// VOX modifications 2026.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// VOX user interface: a fork of beads/ui.cc that keeps every stock gesture
// users rely on (ASSIGN routing and amounts, manual gain, auto-gain reset,
// gate output toggle, autosave toggle, calibration) and turns the MEDIA
// button into a 4-way sub-mode selector. Firmware-specific gestures are
// exposed as event counters that the firmware polls from its main loop.

#ifndef VOX_COMMON_UI_VOX_UI_H_
#define VOX_COMMON_UI_VOX_UI_H_

#include "stmlib/ui/event_queue.h"

#include "beads/drivers/leds.h"
#include "beads/drivers/switches.h"

#include "vox/common/app/algorithm_slots.h"
#include "vox/common/hw/pot_controller.h"

namespace beads {

class Calibrator;
class GainControl;
class Settings;

}  // namespace beads

namespace vox {

struct VoxUiConfig {
  // 4 = stock destinations (Feedback, Dry/Wet, Reverb, all-with-amounts).
  // 5 = stock + one firmware-specific destination (index 4).
  uint8_t num_assign_destinations;
  // Number of states cycled by ASSIGN + SEED (2 = off/on, as stock).
  uint8_t num_gate_output_modes;
  beads::Leds::Color submode_palette[4];
};

class VoxUi {
 public:
  VoxUi() { }
  ~VoxUi() { }

  enum Mode {
    MODE_NORMAL,
    MODE_CALIBRATION_1,
    MODE_CALIBRATION_2,
    MODE_CALIBRATION_3,
    MODE_CALIBRATION_4,
    MODE_DISPLAY_AUTOSAVE,
    MODE_PANIC,
    MODE_SELECT_ALGORITHM
  };

  enum MixParameter {
    MIX_PARAMETER_FEEDBACK,
    MIX_PARAMETER_DRY_WET,
    MIX_PARAMETER_REVERB,
    MIX_PARAMETER_LAST,
  };

  enum Combo {
    COMBO_SEED_FREEZE,
    COMBO_MEDIA_SEED,
    COMBO_MEDIA_FREEZE,
    COMBO_SEED_PRESS,  // Plain short press of SEED (not part of a combo).
    COMBO_LAST
  };

  void Init(
      const VoxUiConfig* config,
      beads::Settings* settings,
      beads::GainControl* gain_control,
      beads::Calibrator* calibrator);
  void Poll();
  void DoEvents();

  // Replaces the configuration given to Init (vox_multi: once the algorithm
  // has been chosen).
  inline void set_config(const VoxUiConfig* config) { config_ = config; }

  // Boot-time algorithm selection (vox_multi). While active, the switches
  // generate no events; the LEDs show `index` (ASSIGN LEDs: index + 1 in
  // binary, MEDIA LED: `color`) and FREEZE / SEED blink alternately. The
  // selection is confirmed by a fresh press of MEDIA (the press held at
  // power-on does not count).
  void StartAlgorithmSelection();
  void SetAlgorithmSelection(int index, beads::Leds::Color color);
  inline bool algorithm_selection_confirmed() const {
    return selection_confirm_.confirmed();
  }
  // Back to normal operation; pending events are dropped and the switches
  // still held are ignored until released.
  void EndAlgorithmSelection();

  inline bool calibrating() const {
    return mode_ >= MODE_CALIBRATION_1 && mode_ <= MODE_CALIBRATION_4;
  }

  inline bool pressed_immediate(beads::Switch s) const {
    return switches_.pressed_immediate(s);
  }

  inline bool freeze_button_state() const { return freeze_button_state_; }
  inline bool seed_button_state() const { return seed_button_state_; }

  inline float mix_parameter(MixParameter parameter) {
    return assignable_cv_pot_[parameter].main_parameter();
  }

  void ProcessPot(MixParameter parameter, float value) {
    assignable_cv_pot_[parameter].ProcessControlRate(value);
  }

  // Number of times a combo has been performed since the last call.
  inline int TakeComboCount(Combo c) {
    int n = combo_count_[c];
    combo_count_[c] = 0;
    return n;
  }

  // LED values supplied by the firmware (written from the audio interrupt).
  inline void set_freeze_led(uint8_t v) { freeze_led_ = v; }
  inline void set_seed_led(uint8_t v) { seed_led_ = v; }
  inline void set_vu_led(uint8_t v) { vu_led_ = v; }
  inline void set_media_brightness(float b) { media_brightness_ = b; }

  // Temporarily light the ASSIGN LEDs with a bit mask (bit 0 = LED 1).
  void FlashAssignLeds(uint8_t mask, uint32_t duration_ms);

 private:
  void OnSwitchPressed(const stmlib::Event& e);
  void OnSwitchReleased(const stmlib::Event& e);

  void UpdateLeds();
  void NextCalibrationStep();
  void UnlockAllPots();

  const VoxUiConfig* config_;

  stmlib::EventQueue<16> queue_;

  beads::Leds leds_;
  beads::Switches switches_;
  uint32_t press_time_[beads::SWITCH_LAST];
  static const uint32_t long_press_duration[beads::SWITCH_LAST];
  bool ignore_release_[beads::SWITCH_LAST];
  Mode mode_;

  bool freeze_button_state_;
  bool seed_button_state_;

  volatile int combo_count_[COMBO_LAST];

  volatile uint8_t freeze_led_;
  volatile uint8_t seed_led_;
  volatile uint8_t vu_led_;
  volatile float media_brightness_;

  uint8_t assign_flash_mask_;
  uint32_t assign_flash_end_;

  volatile int selection_index_;
  beads::Leds::Color selection_color_;
  SelectionConfirm selection_confirm_;

  beads::GainControl* gain_control_;
  beads::Settings* settings_;
  beads::Calibrator* calibrator_;

  beads::PotController assignable_cv_pot_[MIX_PARAMETER_LAST];

  DISALLOW_COPY_AND_ASSIGN(VoxUi);
};

}  // namespace vox

#endif  // VOX_COMMON_UI_VOX_UI_H_
