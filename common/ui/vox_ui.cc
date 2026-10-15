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
// VOX user interface.

#include "vox/common/ui/vox_ui.h"

#include <algorithm>

#include "stmlib/system/system_clock.h"

#include "vox/common/hw/calibrator.h"
#include "vox/common/hw/gain_control.h"
#include "vox/common/hw/settings.h"

namespace vox {

using namespace beads;
using namespace std;
using namespace stmlib;

// FREEZE, SEED, MEDIA, ASSIGN (same as stock).
/* static */
const uint32_t VoxUi::long_press_duration[SWITCH_LAST] = {
  4000, 4000, 1500, 0x7fffffff
};

void VoxUi::Init(
    const VoxUiConfig* config,
    Settings* settings,
    GainControl* gain_control,
    Calibrator* calibrator) {
  config_ = config;
  leds_.Init();
  switches_.Init();

  system_clock.Init();

  settings_ = settings;
  gain_control_ = gain_control;
  calibrator_ = calibrator;

  mode_ = MODE_NORMAL;
  queue_.Init();

  fill(&press_time_[0], &press_time_[SWITCH_LAST], 0);
  fill(&ignore_release_[0], &ignore_release_[SWITCH_LAST], false);
  for (int i = 0; i < COMBO_LAST; ++i) {
    combo_count_[i] = 0;
  }

  freeze_button_state_ = false;
  seed_button_state_ = false;

  freeze_led_ = 0;
  seed_led_ = 0;
  vu_led_ = 0;
  media_brightness_ = 1.0f;
  assign_flash_mask_ = 0;
  assign_flash_end_ = 0;

  selection_index_ = 0;
  selection_color_.r = selection_color_.g = selection_color_.b = 0;
  selection_confirm_.Start();

  for (int i = 0; i < MIX_PARAMETER_LAST; ++i) {
    assignable_cv_pot_[i].Init(1.0f, 0.0f);
  }
}

void VoxUi::StartAlgorithmSelection() {
  selection_confirm_.Start();
  mode_ = MODE_SELECT_ALGORITHM;
}

void VoxUi::SetAlgorithmSelection(int index, Leds::Color color) {
  selection_color_ = color;
  selection_index_ = index;
}

void VoxUi::EndAlgorithmSelection() {
  mode_ = MODE_NORMAL;
  queue_.Init();
  for (int i = 0; i < SWITCH_LAST; ++i) {
    ignore_release_[i] = true;
  }
}

void VoxUi::FlashAssignLeds(uint8_t mask, uint32_t duration_ms) {
  assign_flash_mask_ = mask;
  assign_flash_end_ = system_clock.milliseconds() + duration_ms;
}

void VoxUi::UnlockAllPots() {
  for (int i = 0; i < MIX_PARAMETER_LAST; ++i) {
    assignable_cv_pot_[i].Unlock();
  }
}

void VoxUi::Poll() {
  system_clock.Tick();
  switches_.Debounce();

  if (mode_ == MODE_SELECT_ALGORITHM) {
    // MEDIA must be released (after being held at power-on) and pressed
    // again to confirm. No events are queued meanwhile.
    selection_confirm_.Tick(
        switches_.pressed_immediate(SWITCH_MEDIA),
        switches_.just_pressed(SWITCH_MEDIA));
    UpdateLeds();
    return;
  }

  for (int i = 0; i < SWITCH_LAST; ++i) {
    Switch s = Switch(i);

    if (switches_.just_pressed(s)) {
      queue_.AddEvent(CONTROL_SWITCH, i, 0);
      press_time_[i] = system_clock.milliseconds();
      ignore_release_[i] = false;
    }

    if (switches_.pressed(s) && !ignore_release_[i]) {
      uint32_t pressed_time = system_clock.milliseconds() - press_time_[i];
      if (pressed_time >= long_press_duration[i]) {
        queue_.AddEvent(CONTROL_SWITCH, i, pressed_time);
        ignore_release_[i] = true;
      }
    }

    if (switches_.released(s) && !ignore_release_[i]) {
      queue_.AddEvent(
          CONTROL_SWITCH,
          i,
          system_clock.milliseconds() - press_time_[i] + 1);
      ignore_release_[i] = true;
    }
  }

  for (int i = 0; i < MIX_PARAMETER_LAST; ++i) {
    PotController* p = &assignable_cv_pot_[i];
    p->ProcessUIRate();
    if (p->editing_hidden_parameter()) {
      State* state = settings_->mutable_state();
      if (switches_.pressed(SWITCH_MEDIA)) {
        // Same hack as stock: prevent the MEDIA long press from firing while
        // the manual gain is being adjusted with the FEEDBACK pot.
        press_time_[SWITCH_MEDIA] = system_clock.milliseconds();
        state->gain = 1.0f + p->hidden_parameter() * 255.0f;
      } else if (switches_.pressed(SWITCH_ASSIGN)) {
        state->assignable_cv_destination = 3;
        state->assignable_cv_amount[i] = p->hidden_parameter() * 256.0f;
      }
    }
  }

  UpdateLeds();
}

void VoxUi::UpdateLeds() {
  leds_.Clear();

  const uint32_t now = system_clock.milliseconds();
  int triangle = (now & 2047) >> 2;
  triangle = triangle >= 256 ? 511 - triangle : triangle;

  bool adjusting_manual_gain = switches_.pressed(SWITCH_MEDIA) && \
      assignable_cv_pot_[MIX_PARAMETER_FEEDBACK].editing_hidden_parameter();

  switch (mode_) {
    case MODE_SELECT_ALGORITHM:
      {
        const int n = selection_index_ + 1;
        leds_.set_media(selection_color_);
        for (int i = 0; i < 3; ++i) {
          leds_.set(Led(LED_ASSIGN_1 + i), (n & (1 << i)) ? 255 : 0);
        }
        const bool phase = now & 256;
        leds_.set(LED_FREEZE, phase ? 255 : 0);
        leds_.set(LED_SEED, phase ? 0 : 255);
      }
      break;

    case MODE_PANIC:
      {
        bool blink = now & 64;
        for (int i = 0; i < LED_LAST; ++i) {
          leds_.set(Led(i), blink ? 255 : 0);
        }
      }
      break;

    case MODE_CALIBRATION_1:
    case MODE_CALIBRATION_2:
    case MODE_CALIBRATION_3:
    case MODE_CALIBRATION_4:
      {
        // Identical to stock, so the stock calibration procedure applies.
        int channel = (int(mode_) - MODE_CALIBRATION_1) >> 1;
        int step = (int(mode_) - MODE_CALIBRATION_1) & 1;
        leds_.set(channel == 0 ? LED_FREEZE : LED_SEED, 255);
        leds_.set(LED_MEDIA_R, step == 1 ? 255 : 0);
        leds_.set(LED_MEDIA_G, 255);
        leds_.set(LED_MEDIA_B, 0);
        leds_.set(LED_ASSIGN_1, triangle >> 2);
        leds_.set(LED_ASSIGN_2, triangle >> 2);
        leds_.set(LED_ASSIGN_3, triangle >> 2);
      }
      break;

    case MODE_DISPLAY_AUTOSAVE:
    case MODE_NORMAL:
      {
        const State& state = settings_->state();

        if (adjusting_manual_gain) {
          leds_.set_meter(0.05f + 0.95f * float(state.gain) / 256.0f);
        } else {
          leds_.set_media(
              config_->submode_palette[state.vox_submode & 3],
              media_brightness_);
        }

        if (mode_ == MODE_DISPLAY_AUTOSAVE) {
          leds_.set(LED_FREEZE, state.disable_autosave ? 16 : 64);
        } else {
          leds_.set(LED_FREEZE, freeze_led_);
        }
        leds_.set(LED_SEED, seed_button_state_ ? 255 : seed_led_);

        if (int32_t(assign_flash_end_ - now) > 0) {
          for (int i = 0; i < 3; ++i) {
            leds_.set(
                Led(LED_ASSIGN_1 + i),
                (assign_flash_mask_ & (1 << i)) ? 255 : 0);
          }
        } else if (state.assignable_cv_destination == 3) {
          for (int i = 0; i < MIX_PARAMETER_LAST; ++i) {
            leds_.set(
                Led(LED_ASSIGN_1 + i),
                state.assignable_cv_amount[i] >> 1);
          }
        } else if (state.assignable_cv_destination == 4) {
          // Firmware-specific destination: all three LEDs blink together.
          uint8_t v = (now & 256) ? 160 : 0;
          leds_.set(LED_ASSIGN_1, v);
          leds_.set(LED_ASSIGN_2, v);
          leds_.set(LED_ASSIGN_3, v);
        } else {
          leds_.set(Led(LED_ASSIGN_1 + state.assignable_cv_destination), 128);
        }
      }
      break;
  }

  if (gain_control_->adjusting()) {
    leds_.set(LED_VU_METER, (now & 128) ? 128 : 0);
  } else {
    leds_.set(LED_VU_METER, vu_led_);
  }
  leds_.Write();
}

void VoxUi::OnSwitchPressed(const Event& e) {
  // SEED + FREEZE: firmware-specific "latch" gesture.
  if (switches_.pressed(SWITCH_SEED) && e.control_id == SWITCH_FREEZE) {
    ++combo_count_[COMBO_SEED_FREEZE];
    seed_button_state_ = false;
    ignore_release_[SWITCH_SEED] = ignore_release_[SWITCH_FREEZE] = true;
    return;
  }

  // ASSIGN + SEED: cycle gate output modes (stock: toggle).
  if (switches_.pressed(SWITCH_ASSIGN) && e.control_id == SWITCH_SEED) {
    State* state = settings_->mutable_state();
    uint8_t n = config_->num_gate_output_modes < 2
        ? 2 : config_->num_gate_output_modes;
    state->gate_output_mode = (state->gate_output_mode + 1) % n;
    settings_->SaveState();
    UnlockAllPots();
    ignore_release_[SWITCH_ASSIGN] = ignore_release_[SWITCH_SEED] = true;
    return;
  }

  // ASSIGN + FREEZE: toggle autosave (as stock).
  if (switches_.pressed(SWITCH_ASSIGN) && e.control_id == SWITCH_FREEZE) {
    State* state = settings_->mutable_state();
    state->disable_autosave = !state->disable_autosave;
    settings_->SaveState();
    UnlockAllPots();
    mode_ = MODE_DISPLAY_AUTOSAVE;
    ignore_release_[SWITCH_ASSIGN] = ignore_release_[SWITCH_FREEZE] = true;
    return;
  }

  // MEDIA + SEED / MEDIA + FREEZE: firmware-specific.
  if (switches_.pressed(SWITCH_MEDIA) && e.control_id == SWITCH_SEED) {
    ++combo_count_[COMBO_MEDIA_SEED];
    assignable_cv_pot_[0].Unlock();
    ignore_release_[SWITCH_MEDIA] = ignore_release_[SWITCH_SEED] = true;
    return;
  }
  if (switches_.pressed(SWITCH_MEDIA) && e.control_id == SWITCH_FREEZE) {
    ++combo_count_[COMBO_MEDIA_FREEZE];
    assignable_cv_pot_[0].Unlock();
    ignore_release_[SWITCH_MEDIA] = ignore_release_[SWITCH_FREEZE] = true;
    return;
  }

  // ASSIGN + MEDIA: calibration (unchanged from stock).
  if ((e.control_id == SWITCH_ASSIGN && switches_.pressed(SWITCH_MEDIA)) || \
      (e.control_id == SWITCH_MEDIA && switches_.pressed(SWITCH_ASSIGN))) {
    mode_ = MODE_CALIBRATION_1;
    calibrator_->Start();
    UnlockAllPots();
    ignore_release_[SWITCH_MEDIA] = ignore_release_[SWITCH_ASSIGN] = true;
    return;
  }

  switch (e.control_id) {
    case SWITCH_FREEZE:
      if (!calibrating()) {
        freeze_button_state_ = !freeze_button_state_;
      }
      break;

    case SWITCH_SEED:
      seed_button_state_ = true;
      break;

    case SWITCH_MEDIA:
      assignable_cv_pot_[0].Lock();
      break;

    case SWITCH_ASSIGN:
      for (int i = 0; i < MIX_PARAMETER_LAST; ++i) {
        assignable_cv_pot_[i].Lock();
      }
      break;

    default:
      break;
  }
}

void VoxUi::NextCalibrationStep() {
  switch (mode_) {
    case MODE_CALIBRATION_1:
      calibrator_->CalibrateOffsets();
      calibrator_->Calibrate(CV_ADC_CHANNEL_PITCH, 0, 12.0f);
      mode_ = MODE_CALIBRATION_2;
      break;

    case MODE_CALIBRATION_2:
      mode_ = calibrator_->Calibrate(CV_ADC_CHANNEL_PITCH, 1, 36.0f)
          ? MODE_CALIBRATION_3
          : MODE_PANIC;
      break;

    case MODE_CALIBRATION_3:
      calibrator_->Calibrate(CV_ADC_CHANNEL_DENSITY, 0, 0.2f);
      mode_ = MODE_CALIBRATION_4;
      break;

    case MODE_CALIBRATION_4:
      mode_ = calibrator_->Calibrate(CV_ADC_CHANNEL_DENSITY, 1, 0.6f)
          ? MODE_NORMAL
          : MODE_PANIC;
      if (mode_ == MODE_NORMAL) {
        settings_->SavePersistentData();
      }
      break;

    default:
      break;
  }
}

void VoxUi::OnSwitchReleased(const Event& e) {
  switch (e.control_id) {
    case SWITCH_SEED:
      if (seed_button_state_ && !calibrating()) {
        ++combo_count_[COMBO_SEED_PRESS];
      }
      seed_button_state_ = false;
      break;

    case SWITCH_MEDIA:
      {
        if (!assignable_cv_pot_[0].editing_hidden_parameter()) {
          if (calibrating()) {
            NextCalibrationStep();
          } else {
            State* state = settings_->mutable_state();
            if (uint32_t(e.data) >= long_press_duration[SWITCH_MEDIA]) {
              // Long press: back to automatic gain, and re-learn it.
              if (state->gain != 0) {
                state->gain = 0;
                settings_->SaveState();
              }
              gain_control_->Restart();
            } else {
              // Short press: next sub-mode.
              state->vox_submode = (state->vox_submode + 1) & 3;
              settings_->SaveState();
            }
          }
        } else {
          // Save the manual gain settings.
          settings_->SaveState();
        }
        assignable_cv_pot_[0].Unlock();
      }
      break;

    case SWITCH_ASSIGN:
      {
        bool pot_touched = false;
        for (int i = 0; i < MIX_PARAMETER_LAST; ++i) {
          pot_touched = pot_touched || \
              assignable_cv_pot_[i].editing_hidden_parameter();
          assignable_cv_pot_[i].Unlock();
        }
        if (!pot_touched) {
          State* state = settings_->mutable_state();
          uint8_t d = state->assignable_cv_destination;
          if (d == 3) {
            d = 0;
          } else {
            ++d;
            if (d == 3) {
              d = config_->num_assign_destinations > 4 ? 4 : 0;
            } else if (d > 4) {
              d = 0;
            }
          }
          state->assignable_cv_destination = d;
        }
        settings_->SaveState();
      }
      break;

    default:
      break;
  }
}

void VoxUi::DoEvents() {
  while (queue_.available()) {
    Event e = queue_.PullEvent();
    if (e.control_type == CONTROL_SWITCH) {
      if (e.data == 0) {
        OnSwitchPressed(e);
      } else {
        OnSwitchReleased(e);
      }
    }
  }

  if (queue_.idle_time() > 1000 && \
      (mode_ == MODE_PANIC || mode_ == MODE_DISPLAY_AUTOSAVE)) {
    mode_ = MODE_NORMAL;
  }

  if (queue_.idle_time() > 3000) {
    queue_.Touch();
  }
}

}  // namespace vox
