// Copyright 2026 VOX contributors. MIT license.
//
// Minimal WAV reader/writer for the host test harness. Reads 16/24/32-bit PCM
// and 32-bit float, any channel count (channels beyond the first two are
// ignored). Writes 32-bit float stereo.

#ifndef VOX_TEST_WAV_IO_H_
#define VOX_TEST_WAV_IO_H_

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

namespace vox_test {

struct Audio {
  int sample_rate;
  int channels;
  std::vector<float> l;
  std::vector<float> r;
  size_t size() const { return l.size(); }
};

inline bool ReadWav(const std::string& path, Audio* audio) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path.c_str());
    return false;
  }
  char riff[12];
  if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) || \
      memcmp(riff + 8, "WAVE", 4)) {
    fprintf(stderr, "%s: not a WAV file\n", path.c_str());
    fclose(f);
    return false;
  }
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t sample_rate = 0;
  std::vector<uint8_t> data;
  while (true) {
    char id[4];
    uint32_t size;
    if (fread(id, 1, 4, f) != 4 || fread(&size, 4, 1, f) != 1) break;
    if (!memcmp(id, "fmt ", 4)) {
      std::vector<uint8_t> fmt(size);
      if (fread(fmt.data(), 1, size, f) != size) break;
      memcpy(&format, &fmt[0], 2);
      memcpy(&channels, &fmt[2], 2);
      memcpy(&sample_rate, &fmt[4], 4);
      memcpy(&bits, &fmt[14], 2);
      if (format == 0xfffe && size >= 26) {  // WAVE_FORMAT_EXTENSIBLE
        memcpy(&format, &fmt[24], 2);
      }
    } else if (!memcmp(id, "data", 4)) {
      data.resize(size);
      size_t n = fread(data.data(), 1, size, f);
      data.resize(n);
      break;
    } else {
      fseek(f, size + (size & 1), SEEK_CUR);
    }
  }
  fclose(f);
  if (!channels || !bits || data.empty()) {
    fprintf(stderr, "%s: missing fmt or data chunk\n", path.c_str());
    return false;
  }
  const size_t bytes = bits / 8;
  const size_t frames = data.size() / (bytes * channels);
  audio->sample_rate = sample_rate;
  audio->channels = channels;
  audio->l.resize(frames);
  audio->r.resize(frames);
  for (size_t i = 0; i < frames; ++i) {
    float v[2] = { 0.0f, 0.0f };
    for (size_t c = 0; c < 2 && c < channels; ++c) {
      const uint8_t* p = &data[(i * channels + c) * bytes];
      if (format == 3 && bits == 32) {
        memcpy(&v[c], p, 4);
      } else if (bits == 16) {
        int16_t s;
        memcpy(&s, p, 2);
        v[c] = s / 32768.0f;
      } else if (bits == 24) {
        int32_t s = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
        v[c] = s / 2147483648.0f;
      } else if (bits == 32) {
        int32_t s;
        memcpy(&s, p, 4);
        v[c] = s / 2147483648.0f;
      }
    }
    audio->l[i] = v[0];
    audio->r[i] = channels > 1 ? v[1] : v[0];
  }
  return true;
}

inline bool WriteWav(const std::string& path, const Audio& audio) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) {
    fprintf(stderr, "cannot write %s\n", path.c_str());
    return false;
  }
  const uint32_t frames = audio.size();
  const uint32_t data_size = frames * 2 * 4;
  const uint32_t riff_size = 4 + 8 + 16 + 8 + data_size;
  const uint16_t format = 3, channels = 2, bits = 32, block_align = 8;
  const uint32_t sr = audio.sample_rate, byte_rate = sr * 8, fmt_size = 16;
  fwrite("RIFF", 1, 4, f);
  fwrite(&riff_size, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f);
  fwrite(&fmt_size, 4, 1, f);
  fwrite(&format, 2, 1, f);
  fwrite(&channels, 2, 1, f);
  fwrite(&sr, 4, 1, f);
  fwrite(&byte_rate, 4, 1, f);
  fwrite(&block_align, 2, 1, f);
  fwrite(&bits, 2, 1, f);
  fwrite("data", 1, 4, f);
  fwrite(&data_size, 4, 1, f);
  for (uint32_t i = 0; i < frames; ++i) {
    fwrite(&audio.l[i], 4, 1, f);
    fwrite(&audio.r[i], 4, 1, f);
  }
  fclose(f);
  return true;
}

// Linear-interpolation resampler, good enough to bring test material to
// 48 kHz.
inline void Resample(Audio* audio, int target_rate) {
  if (audio->sample_rate == target_rate) return;
  const double ratio = double(audio->sample_rate) / target_rate;
  const size_t n = size_t(audio->size() / ratio);
  std::vector<float> l(n), r(n);
  for (size_t i = 0; i < n; ++i) {
    const double pos = i * ratio;
    const size_t j = size_t(pos);
    const float t = float(pos - j);
    const size_t k = j + 1 < audio->size() ? j + 1 : j;
    l[i] = audio->l[j] + (audio->l[k] - audio->l[j]) * t;
    r[i] = audio->r[j] + (audio->r[k] - audio->r[j]) * t;
  }
  audio->l.swap(l);
  audio->r.swap(r);
  audio->sample_rate = target_rate;
}

}  // namespace vox_test

#endif  // VOX_TEST_WAV_IO_H_
