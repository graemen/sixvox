// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// The VOX algorithms. Each firmware directory defines one descriptor; a
// single firmware lists its own in kAlgorithms, vox_multi lists all of them.

#ifndef VOX_COMMON_APP_ALGORITHMS_H_
#define VOX_COMMON_APP_ALGORITHMS_H_

#include "vox/common/app/vox_platform.h"

namespace vox {

extern const VoxAlgorithm kPsolaAlgorithm;      // vox/psola
extern const VoxAlgorithm kLpcAlgorithm;        // vox/lpc
extern const VoxAlgorithm kSyllableAlgorithm;   // vox/syllable
extern const VoxAlgorithm kSplitAlgorithm;      // vox/split
extern const VoxAlgorithm kSequencerAlgorithm;  // vox/sequencer
extern const VoxAlgorithm kChainAlgorithm;      // vox/chain

}  // namespace vox

#endif  // VOX_COMMON_APP_ALGORITHMS_H_
