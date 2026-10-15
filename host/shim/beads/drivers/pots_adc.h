// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Host shim of beads/drivers/pots_adc.h. Raw codes (0 = fully CCW, 65535 =
// fully CW) come from vox_host::pots_adc_values.

#ifndef BEADS_DRIVERS_POTS_ADC_H_
#define BEADS_DRIVERS_POTS_ADC_H_

#include <algorithm>
#include <cmath>

#include "stmlib/stmlib.h"

namespace beads {

enum PotsAdcChannel {
  POTS_ADC_CHANNEL_FEEDBACK,
  POTS_ADC_CHANNEL_DRY_WET,
  POTS_ADC_CHANNEL_REVERB,
  POTS_ADC_CHANNEL_TIME_AMOUNT,
  POTS_ADC_CHANNEL_SIZE_AMOUNT,
  POTS_ADC_CHANNEL_SHAPE_AMOUNT,
  POTS_ADC_CHANNEL_PITCH_AMOUNT,

  POTS_ADC_CHANNEL_FIRST_DIRECT_CHANNEL,
  POTS_ADC_CHANNEL_TIME = POTS_ADC_CHANNEL_FIRST_DIRECT_CHANNEL,
  POTS_ADC_CHANNEL_DENSITY,
  POTS_ADC_CHANNEL_PITCH,
  POTS_ADC_CHANNEL_SIZE,
  POTS_ADC_CHANNEL_SHAPE,
  POTS_ADC_CHANNEL_LAST
};

}  // namespace beads

namespace vox_host {
extern uint16_t pots_adc_values[beads::POTS_ADC_CHANNEL_LAST];
}

namespace beads {

class PotsAdc {
 public:
  PotsAdc() { }
  ~PotsAdc() { }

  bool Init() { return true; }
  void DeInit() { }
  void Convert() { }
  void StartMuxedPotsConversion() { }
  void Cycle() { }

  inline uint16_t value(PotsAdcChannel channel) const {
    return vox_host::pots_adc_values[channel];
  }
  inline float float_value(PotsAdcChannel channel) const {
    return static_cast<float>(value(channel)) / 65536.0f;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(PotsAdc);
};

}  // namespace beads

#endif  // BEADS_DRIVERS_POTS_ADC_H_
