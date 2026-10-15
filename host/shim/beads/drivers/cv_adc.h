// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Host shim of beads/drivers/cv_adc.h. The raw 16-bit ADC codes come from
// vox_host::cv_adc_values (written by the host, as the codes the module's
// ADC would read for the jack voltages).

#ifndef BEADS_DRIVERS_CV_ADC_H_
#define BEADS_DRIVERS_CV_ADC_H_

#include <algorithm>
#include <cmath>

#include "stmlib/stmlib.h"

namespace beads {

enum CvAdcChannel {
  CV_ADC_CHANNEL_ASSIGN,
  CV_ADC_CHANNEL_DENSITY,
  CV_ADC_CHANNEL_PITCH,
  CV_ADC_CHANNEL_TIME,
  CV_ADC_CHANNEL_SIZE,
  CV_ADC_CHANNEL_SHAPE,
  CV_ADC_CHANNEL_LAST
};

}  // namespace beads

namespace vox_host {
extern uint16_t cv_adc_values[beads::CV_ADC_CHANNEL_LAST];
}

namespace beads {

class CvAdc {
 public:
  CvAdc() { }
  ~CvAdc() { }

  bool Init() { return true; }
  void DeInit() { }
  void Convert() { }

  inline uint16_t value(CvAdcChannel channel) const {
    return vox_host::cv_adc_values[channel];
  }
  inline float float_value(CvAdcChannel channel) const {
    return static_cast<float>(value(channel)) / 65536.0f;
  }
  inline const uint16_t* values() { return vox_host::cv_adc_values; }

 private:
  DISALLOW_COPY_AND_ASSIGN(CvAdc);
};

}  // namespace beads

#endif  // BEADS_DRIVERS_CV_ADC_H_
