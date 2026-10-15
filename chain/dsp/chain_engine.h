// Copyright 2026 VOX contributors.
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
// vox_chain DSP engine: voice channel strip + music bed ducker.
//
//  voice (IN L) -> expander -> HPF -> de-esser -> compressor (parallel mix)
//               -> tilt EQ -> character -> mute -> reverb ---------+
//  bed (IN R) -> ducker (keyed by the processed voice) ------------+-> sum
//                                       -> output gain -> limiter -> bypass
//
// The engine is hardware independent (it also builds on the host for tests).

#ifndef VOX_CHAIN_DSP_CHAIN_ENGINE_H_
#define VOX_CHAIN_DSP_CHAIN_ENGINE_H_

#include "stmlib/stmlib.h"
#include "stmlib/dsp/filter.h"

#include "beads/dsp/frame.h"
#include "beads/dsp/fx/reverb.h"

#include "vox/chain/dsp/character.h"
#include "vox/chain/dsp/dynamics.h"
#include "vox/chain/dsp/level_tools.h"
#include "vox/chain/dsp/tone.h"

namespace vox {

enum ChainPresetId {
  CHAIN_PRESET_PODCAST,    // green
  CHAIN_PRESET_BROADCAST,  // yellow
  CHAIN_PRESET_ANNOUNCER,  // red
  CHAIN_PRESET_RAW,        // blue
  CHAIN_PRESET_LAST
};

// A preset sets the ranges the knobs sweep; the knobs act within them.
struct ChainPreset {
  float hpf_hz;
  float threshold_min_db;    // compressor threshold at DENSITY = 0+
  float threshold_range_db;  // ... minus this at DENSITY = 1
  float ratio_range;         // ratio = 1 + ratio_range * DENSITY
  float time_bias;           // added to TIME
  float tilt_bias;           // added to the SIZE tilt (-1..1 domain)
  float deess_bias;          // added to PITCH (de-esser) when it is not off
  float character_scale;     // SHAPE sensitivity
  float duck_depth_db;       // default bed ducking depth
  float output_gain_db;
};

extern const ChainPreset kChainPresets[CHAIN_PRESET_LAST];

// Normalized controls (0..1 unless noted), filled by the firmware or by the
// host test harness.
struct ChainParams {
  float amount;      // DENSITY: compression amount
  float time;        // TIME: attack/release macro
  float tilt;        // SIZE: 0.5 = flat
  float character;   // SHAPE: 0.5 = clean
  float deess;       // PITCH: de-esser depth
  float gate;        // FEEDBACK: expander threshold/ratio
  float dry_wet;     // DRY/WET: parallel compression blend
  float reverb;      // REVERB
  float duck_cv;     // -1..1, ASSIGN CV when routed to "duck depth"
  bool bypass;       // FREEZE button
  bool push;         // SEED gate: +6 dB into the compressor detector
  bool mute;         // FREEZE gate: mute the voice (20 ms fades)
  bool bed_present;  // IN R patched
  uint8_t preset;
};

class ChainEngine {
 public:
  ChainEngine() { }
  ~ChainEngine() { }

  enum Meter {
    METER_INPUT,
    METER_GAIN_REDUCTION,
    METER_OUTPUT,
    METER_BED,
    METER_LAST
  };

  void Init(float sample_rate, uint16_t* reverb_buffer);

  // in[i].l = voice, in[i].r = music bed (ignored unless bed_present).
  // vad[i] receives the voice activity flag.
  void Process(
      const ChainParams& p,
      const beads::FloatFrame* in,
      beads::FloatFrame* out,
      bool* vad,
      size_t size);

  // 0..1 meter values for the VU LED.
  float meter(Meter m) const;
  inline float gain_reduction_db() const { return compressor_.gain_reduction_db(); }
  inline float deess_reduction_db() const { return deesser_.reduction_db(); }
  inline float bed_gain_db() const { return ducker_.gain_db(); }
  inline float expander_gain_db() const { return expander_.gain_db(); }

 private:
  void Configure(const ChainParams& p);

  float sample_rate_;
  uint8_t preset_;

  Expander expander_;
  stmlib::Svf hpf_;
  DeEsser deesser_;
  Compressor compressor_;
  TiltEq tilt_eq_;
  Character character_;
  Vad vad_;
  Ducker ducker_;
  beads::Reverb reverb_;
  PeakLimiter limiter_;

  float push_db_;
  float dry_wet_;
  float mute_gain_;
  float mute_coefficient_;
  float bypass_amount_;
  float bypass_coefficient_;
  float output_gain_;

  EnvelopeFollower input_meter_;
  EnvelopeFollower output_meter_;
  EnvelopeFollower bed_meter_;

  DISALLOW_COPY_AND_ASSIGN(ChainEngine);
};

}  // namespace vox

#endif  // VOX_CHAIN_DSP_CHAIN_ENGINE_H_
