// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Host shim of beads/drivers/leds.h: identical interface; Write() publishes
// the intensities to vox_host::led_intensity.

#ifndef BEADS_DRIVERS_LEDS_H_
#define BEADS_DRIVERS_LEDS_H_

#include <string.h>

#include "stmlib/stmlib.h"

namespace beads {

const uint8_t kMaxLedIntensity = 255;

enum Led {
  LED_MEDIA_R,
  LED_MEDIA_G,
  LED_MEDIA_B,
  LED_FREEZE,
  LED_SEED,
  LED_VU_METER,
  LED_ASSIGN_1,
  LED_ASSIGN_2,
  LED_ASSIGN_3,
  LED_LAST
};

}  // namespace beads

namespace vox_host {
extern uint8_t led_intensity[beads::LED_LAST];
}

namespace beads {

class Leds {
 public:
  Leds() { }
  ~Leds() { }

  struct Color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
  };

  bool Init() { Clear(); Write(); return true; }
  void Write() { memcpy(vox_host::led_intensity, intensity_, sizeof(intensity_)); }
  void Clear() { memset(intensity_, 0, sizeof(intensity_)); }

  inline void set(Led led, uint8_t intensity) {
    intensity_[led] = intensity;
  }

  inline void set_media(Color color) {
    intensity_[LED_MEDIA_R] = color.r;
    intensity_[LED_MEDIA_G] = color.g;
    intensity_[LED_MEDIA_B] = color.b;
  }

  inline void set_meter(float value) {
    Color c;
    if (value < 0.25f) {
      c.r = 0;
      c.g = uint8_t(value * 1024.0f);
      c.b = 0;
    } else if (value < 0.5f) {
      c.r = uint8_t((value - 0.25f) * 1024.0f);
      c.g = 255;
      c.b = 0;
    } else {
      c.r = 255;
      c.g = 255 - uint8_t((value - 0.5f) * 512.0f);
      c.b = 0;
    }
    set_media(c);
  }

  inline void set_media(Color color, float a) {
    if (a != 1.0f) {
      color.r = uint8_t(float(color.r) * a * a);
      color.g = uint8_t(float(color.g) * a);
      color.b = uint8_t(float(color.b) * a);
    }
    set_media(color);
  }

 private:
  uint8_t intensity_[LED_LAST];

  DISALLOW_COPY_AND_ASSIGN(Leds);
};

}  // namespace beads

#endif  // BEADS_DRIVERS_LEDS_H_
