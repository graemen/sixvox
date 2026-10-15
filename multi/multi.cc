// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// vox_multi: the six VOX algorithms in one firmware.
//
// Hold MEDIA while powering the module on to enter the algorithm selection:
// the FREEZE and SEED LEDs blink alternately, PITCH chooses (fully CCW = 1,
// fully CW = 6), the ASSIGN LEDs show the number in binary and the MEDIA LED
// its colour. Press MEDIA to confirm. The choice is remembered; without
// MEDIA held, the module starts with the last algorithm used.
//
//   1  psola      green     pitch-synchronous voice (pitch, formants, intonation)
//   2  lpc        red       LPC analysis / resynthesis (whisper, robot, talk box)
//   3  syllable   yellow    syllable-aware granular
//   4  split      cyan      voiced / unvoiced splitter
//   5  sequencer  blue      clocked syllable / word sequencer
//   6  chain      magenta   voice processing chain
//
// Each algorithm keeps its own MEDIA sub-mode and option flags.

#include "vox/common/app/vox_platform.h"
#include "vox/common/app/algorithms.h"
#include "vox/common/app/algorithm_slots.h"

namespace vox {

const VoxAlgorithm* const kAlgorithms[] = {
  &kPsolaAlgorithm,
  &kLpcAlgorithm,
  &kSyllableAlgorithm,
  &kSplitAlgorithm,
  &kSequencerAlgorithm,
  &kChainAlgorithm,
};

const int kNumAlgorithms = sizeof(kAlgorithms) / sizeof(kAlgorithms[0]);

static_assert(sizeof(kAlgorithms) / sizeof(kAlgorithms[0]) <= kMaxAlgorithms,
              "too many algorithms for the settings slots");

}  // namespace vox
