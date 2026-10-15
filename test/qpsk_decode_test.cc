// Copyright 2026 VOX contributors. MIT license.
//
// Decodes a firmware update .wav with the *stock* QPSK demodulator and packet
// decoder used by the Beads bootloader (stm_audio_bootloader/qpsk), using the
// same parameters and the same sample scaling as beads/bootloader/bootloader.cc,
// then compares the result with the original .bin file.
//
//   qpsk_decode_test firmware.wav firmware.bin

#include <stdio.h>
#include <string.h>

#include <vector>

#include "stm_audio_bootloader/qpsk/demodulator.h"
#include "stm_audio_bootloader/qpsk/packet_decoder.h"
#include "vox/test/wav_io.h"

using namespace stm_audio_bootloader;

const double kSampleRate = 48000.0;
const double kModulationRate = 6000.0;
const double kBitRate = 12000.0;
const uint32_t kBlockSize = 0x10000;

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: qpsk_decode_test firmware.wav firmware.bin\n");
    return 2;
  }
  vox_test::Audio audio;
  if (!vox_test::ReadWav(argv[1], &audio)) return 1;
  FILE* f = fopen(argv[2], "rb");
  if (!f) { perror(argv[2]); return 1; }
  std::vector<uint8_t> bin;
  int c;
  while ((c = fgetc(f)) != EOF) bin.push_back(uint8_t(c));
  fclose(f);

  static PacketDecoder decoder;
  static Demodulator demodulator;
  decoder.Init(20000, true);
  demodulator.Init(
      kModulationRate / kSampleRate * 4294967296.0,
      kSampleRate / kModulationRate,
      2.0 * kSampleRate / kBitRate);
  demodulator.SyncCarrier(true);
  decoder.Reset();

  std::vector<uint8_t> received;
  size_t packets = 0, errors = 0;
  bool done = false;
  for (size_t i = 0; i < audio.size() && !done; ++i) {
    // Same scaling as the bootloader: 16-bit sample (unity input gain, one
    // input patched), reduced to 12 bits around 2048.
    int32_t sample = int32_t(audio.l[i] * 32767.0f);
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
    demodulator.PushSample(2048 + (sample >> 4));
    if ((i & 31) != 31) continue;
    demodulator.ProcessAtLeast(32);
    while (demodulator.available()) {
      uint8_t symbol = demodulator.NextSymbol();
      PacketDecoderState state = decoder.ProcessSymbol(symbol);
      if (state == PACKET_DECODER_STATE_OK) {
        const uint8_t* p = decoder.packet_data();
        received.insert(received.end(), p, p + kPacketSize);
        ++packets;
        if ((packets % (kBlockSize / kPacketSize)) == 0) {
          decoder.Reset();
          demodulator.SyncCarrier(false);
        } else {
          decoder.Reset();
          demodulator.SyncDecision();
        }
      } else if (state == PACKET_DECODER_STATE_ERROR_SYNC ||
                 state == PACKET_DECODER_STATE_ERROR_CRC) {
        ++errors;
        fprintf(stderr, "decoder error %d after %zu packets\n", state, packets);
        done = true;
      } else if (state == PACKET_DECODER_STATE_END_OF_TRANSMISSION) {
        done = true;
      }
    }
  }
  bool match = received.size() >= bin.size() && \
      !memcmp(received.data(), bin.data(), bin.size());
  for (size_t i = bin.size(); match && i < received.size(); ++i) {
    match = received[i] == 0xff;  // padding
  }
  printf("%zu packets (%zu bytes) decoded, %zu errors, firmware %zu bytes: %s\n",
         packets, received.size(), errors, bin.size(),
         match ? "MATCH" : "MISMATCH");
  return match && !errors ? 0 : 1;
}
