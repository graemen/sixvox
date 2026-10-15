// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Host shim of beads/drivers/codec.h: the frame and event types only.

#ifndef BEADS_DRIVERS_CODEC_H_
#define BEADS_DRIVERS_CODEC_H_

#include "stmlib/stmlib.h"

namespace beads {

const size_t kMaxCodecBlockSize = 24;

class Codec {
 public:
  struct Frame {
    int32_t l;
    int32_t r;
  };

  enum Event {
    EVENT_NONE,
    EVENT_SEED,
    EVENT_FREEZE,
  };
};

}  // namespace beads

#endif  // BEADS_DRIVERS_CODEC_H_
