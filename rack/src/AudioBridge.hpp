// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Runs a fixed-rate, fixed-block processor (the VOX core: 48 kHz, 12 frames)
// from VCV Rack's per-sample process(), at any engine sample rate:
//
//   engine rate -> [in_engine] -> SRC -> [in_core] -> blocks -> [out_core]
//               -> SRC -> [out_engine] -> engine rate
//
// The core runs in bursts, so the output starts once `kPrime` frames are
// queued (about 1.3 ms at 48 kHz); the queue level then stays above zero.
// Uses only header-only parts of the Rack SDK (dsp::SampleRateConverter,
// dsp::DoubleRingBuffer), so it is tested outside Rack (vox/rack/test).

#pragma once

#include <dsp/resampler.hpp>
#include <dsp/ringbuffer.hpp>

namespace vox_rack {

template<int kBlock, int kCoreRate>
class AudioBridge {
 public:
  typedef rack::dsp::Frame<2> Frame;

  enum {
    kChunk = 16,   // engine frames converted at a time
    kPrime = 64    // engine frames queued before the output starts
  };

  AudioBridge() {
    src_in_.setQuality(6);
    src_out_.setQuality(6);
    Reset();
  }

  void Reset() {
    in_engine_.clear();
    in_core_.clear();
    out_core_.clear();
    out_engine_.clear();
    primed_ = false;
    underruns_ = 0;
  }

  // One engine sample. run_block(const Frame* in, Frame* out) processes
  // kBlock frames at the core rate. Returns the output frame (silence until
  // primed). Returns whether at least one block ran in `ran`.
  template<typename F>
  Frame Process(float engine_rate, const Frame& in, F run_block, bool* ran) {
    *ran = false;
    if (!in_engine_.full()) in_engine_.push(in);

    if (in_engine_.size() >= kChunk) {
      src_in_.setRates(int(engine_rate), kCoreRate);
      int in_len = int(in_engine_.size());
      int out_len = int(in_core_.capacity());
      src_in_.process(in_engine_.startData(), &in_len, in_core_.endData(), &out_len);
      in_engine_.startIncr(in_len);
      in_core_.endIncr(out_len);
    }

    while (in_core_.size() >= size_t(kBlock) && out_core_.capacity() >= size_t(kBlock)) {
      run_block(in_core_.startData(), out_core_.endData());
      in_core_.startIncr(kBlock);
      out_core_.endIncr(kBlock);
      *ran = true;
    }

    if (!out_core_.empty() && out_engine_.capacity() > 0) {
      src_out_.setRates(kCoreRate, int(engine_rate));
      int in_len = int(out_core_.size());
      int out_len = int(out_engine_.capacity());
      src_out_.process(out_core_.startData(), &in_len, out_engine_.endData(), &out_len);
      out_core_.startIncr(in_len);
      out_engine_.endIncr(out_len);
    }

    Frame out = {};
    if (!primed_ && out_engine_.size() >= kPrime) primed_ = true;
    if (primed_) {
      if (out_engine_.empty()) {
        primed_ = false;
        ++underruns_;
      } else {
        out = out_engine_.shift();
      }
    }
    return out;
  }

  inline int underruns() const { return underruns_; }
  inline size_t queued() const { return out_engine_.size(); }

 private:
  rack::dsp::SampleRateConverter<2> src_in_, src_out_;
  rack::dsp::DoubleRingBuffer<Frame, 256> in_engine_, in_core_, out_core_, out_engine_;
  bool primed_;
  int underruns_;
};

}  // namespace vox_rack
