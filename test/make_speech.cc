// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Writes synthetic speech-like test material (and a label track) as WAV:
//   make_speech out.wav [seconds] [seed]
// Left channel: speech; right channel: ground-truth class (0 silence,
// 0.33 voiced, 0.66 unvoiced, 1 burst) for inspection in an editor.

#include <stdlib.h>

#include "vox/test/test_signals.h"
#include "vox/test/wav_io.h"

using namespace vox_test;

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: make_speech out.wav [seconds] [seed]\n");
    return 2;
  }
  SpeechLikeOptions o = DefaultSpeechOptions(argc > 2 ? float(atof(argv[2])) : 20.0f, 48000.0f);
  if (argc > 3) o.seed = uint32_t(atoi(argv[3]));
  GroundTruth gt;
  Audio a;
  a.sample_rate = 48000;
  a.channels = 2;
  a.l = SpeechLike(o, &gt);
  a.r.resize(a.l.size());
  for (size_t i = 0; i < a.r.size(); ++i) a.r[i] = gt.label[i] / 3.0f;
  return WriteWav(argv[1], a) ? 0 : 1;
}
