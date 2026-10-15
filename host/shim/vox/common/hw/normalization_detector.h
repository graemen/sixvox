// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Host shim of vox/common/hw/normalization_detector.h. On the module the
// patched state of each jack is detected by injecting a probe signal; a host
// knows it directly, so normalized() reads vox_host::jack_normalized.

#ifndef VOX_HW_NORMALIZATION_DETECTOR_H_
#define VOX_HW_NORMALIZATION_DETECTOR_H_

#include "stmlib/stmlib.h"

namespace beads {

enum NormalizationDetectorChannel {
  NORMALIZATION_DETECTOR_CHANNEL_SEED,
  NORMALIZATION_DETECTOR_CHANNEL_TIME_CV,
  NORMALIZATION_DETECTOR_CHANNEL_SIZE_CV,
  NORMALIZATION_DETECTOR_CHANNEL_SHAPE_CV,
  NORMALIZATION_DETECTOR_CHANNEL_PITCH_CV,
  NORMALIZATION_DETECTOR_CHANNEL_DENSITY_CV,
  NORMALIZATION_DETECTOR_CHANNEL_AUDIO_LEFT,
  NORMALIZATION_DETECTOR_CHANNEL_AUDIO_RIGHT,
  NORMALIZATION_DETECTOR_CHANNEL_LAST
};

}  // namespace beads

namespace vox_host {
extern bool jack_normalized[beads::NORMALIZATION_DETECTOR_CHANNEL_LAST];
}

namespace beads {

class NormalizationDetector {
 public:
  NormalizationDetector() { }
  ~NormalizationDetector() { }

  void Init(int) { }
  inline bool normalized(NormalizationDetectorChannel channel) const {
    return vox_host::jack_normalized[channel];
  }
  inline void Stop() { }
  inline void Start() { }

 private:
  DISALLOW_COPY_AND_ASSIGN(NormalizationDetector);
};

}  // namespace beads

#endif  // VOX_HW_NORMALIZATION_DETECTOR_H_
