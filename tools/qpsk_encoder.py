#!/usr/bin/env python3
#
# Python 3 port of stm_audio_bootloader/qpsk/encoder.py (Emilie Gillet, MIT
# license) restricted to the STM32H7 target used by Beads. The modulation,
# packet format, scrambler, block size, application start address and erase
# pauses are unchanged from the upstream H7 encoder, so the resulting .wav is
# decoded by the stock Beads bootloader.
#
# Usage: qpsk_encoder.py firmware.bin [-o firmware.wav]

import argparse
import struct
import zlib

import numpy as np

SAMPLE_RATE = 48000
CARRIER_FREQUENCY = 6000
BIT_RATE = 12000
PACKET_SIZE = 256

STM32H7_SECTOR_BASE_ADDRESS = [0x08000000 + i * 0x20000 for i in range(16)]
STM32H7_BLOCK_SIZE = 0x10000
STM32H7_APPLICATION_START = 0x08020000
ERASE_PAUSE = 3.0


class Scrambler:
  def __init__(self, seed=0):
    self._state = seed

  def scramble(self, data):
    out = bytearray(data)
    for i in range(len(out)):
      out[i] ^= (self._state >> 24)
      self._state = (self._state * 1664525 + 1013904223) & 0xffffffff
    return bytes(out)


class QpskEncoder:
  def __init__(self, sample_rate, carrier_frequency, bit_rate, packet_size,
               scramble):
    period = sample_rate // carrier_frequency
    symbol_time = sample_rate // (bit_rate // 2)
    assert (symbol_time % period == 0) or (period % symbol_time == 0)
    assert sample_rate % carrier_frequency == 0
    assert sample_rate % bit_rate == 0
    self._sr = sample_rate
    self._br = bit_rate
    self._carrier_frequency = carrier_frequency
    self._sample_index = 0
    self._packet_size = packet_size
    self._scrambler = Scrambler(0) if scramble else None

  @staticmethod
  def _upsample(x, factor):
    return np.tile(x.reshape(len(x), 1), (1, factor)).ravel()

  def _encode(self, symbols):
    ratio = self._sr // self._br * 2
    symbols = np.array(symbols, dtype=int)
    even = 2 * self._upsample(symbols % 2, ratio) - 1
    odd = 2 * self._upsample(symbols // 2, ratio) - 1
    q, i = even / np.sqrt(2.0), odd / np.sqrt(2.0)
    n = len(q)
    t = (np.arange(0.0, n) + self._sample_index) / self._sr
    self._sample_index += n
    phase = 2 * np.pi * self._carrier_frequency * t
    return q * np.sin(phase) + i * np.cos(phase)

  def code_blank(self, duration):
    num_zeros = int(duration * self._br / 8) * 4
    return self._encode(np.zeros(num_zeros, dtype=int))

  def code_packet(self, data):
    assert len(data) <= self._packet_size
    data = data + b'\xff' * (self._packet_size - len(data))
    if self._scrambler:
      data = self._scrambler.scramble(data)
    crc = zlib.crc32(data) & 0xffffffff
    preamble = [0] * 8 + [0x99] * 4 + [0xcc] * 4
    crc_bytes = [crc >> 24, (crc >> 16) & 0xff, (crc >> 8) & 0xff, crc & 0xff]
    symbols = []
    for byte in preamble + list(data) + crc_bytes:
      symbols += [(byte >> 6) & 3, (byte >> 4) & 3, (byte >> 2) & 3, byte & 3]
    return self._encode(symbols)

  def code_intro(self):
    yield np.zeros(self._sr)
    yield self.code_blank(1.0)

  def code(self, data, page_size, blank_duration):
    if len(data) % page_size:
      data = data + b'\xff' * (page_size - len(data) % page_size)
    written = 0
    for offset in range(0, len(data), self._packet_size):
      yield self.code_packet(data[offset:offset + self._packet_size])
      written += 1
      if written == page_size // self._packet_size:
        yield self.code_blank(blank_duration)
        written = 0


def write_wav(path, blocks, sample_rate):
  pcm = np.concatenate(blocks)
  pcm = np.clip(pcm, -1.0, 1.0)
  pcm = (pcm * 32767).astype('<i2')
  with open(path, 'wb') as f:
    f.write(b'RIFF')
    f.write(struct.pack('<L', 36 + pcm.nbytes))
    f.write(b'WAVEfmt ')
    f.write(struct.pack('<LHHLLHH', 16, 1, 1, sample_rate, sample_rate * 2,
                        2, 16))
    f.write(b'data')
    f.write(struct.pack('<L', pcm.nbytes))
    f.write(pcm.tobytes())


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('bin')
  parser.add_argument('-o', '--output')
  args = parser.parse_args()
  data = open(args.bin, 'rb').read()
  out = args.output or (args.bin[:-4] + '.wav' if args.bin.endswith('.bin')
                        else args.bin + '.wav')

  enc = QpskEncoder(SAMPLE_RATE, CARRIER_FREQUENCY, BIT_RATE, PACKET_SIZE,
                    scramble=True)
  blocks = list(enc.code_intro())
  for x in range(0, len(data), STM32H7_BLOCK_SIZE):
    address = STM32H7_APPLICATION_START + x
    pause = ERASE_PAUSE if address in STM32H7_SECTOR_BASE_ADDRESS else 0.2
    blocks += [b for b in enc.code(data[x:x + STM32H7_BLOCK_SIZE],
                                   STM32H7_BLOCK_SIZE, pause) if len(b)]
  blocks.append(enc.code_blank(5.0))
  write_wav(out, blocks, SAMPLE_RATE)
  print('wrote %s (%.1f s)' % (out, sum(len(b) for b in blocks) / SAMPLE_RATE))


if __name__ == '__main__':
  main()
