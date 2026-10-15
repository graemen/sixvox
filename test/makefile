# Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
#
# Host (macOS / Linux) builds of the VOX DSP test programs. From eurorack/:
#   make -f vox/test/makefile            builds everything in build/vox_test/
#   make -f vox/test/makefile check      builds and runs every self test
#   make -f vox/test/makefile core-check builds the host firmware cores
#                                        (vox/host/core.mk) and runs core_test

CXX ?= c++
CXXFLAGS = -O2 -g -std=c++11 -Wall -Wno-unused-local-typedefs -I. -DTEST
BUILD ?= build/vox_test

HEADERS = $(wildcard vox/common/*/*.h vox/test/*.h vox/*/dsp/*.h)
STMLIB = stmlib/dsp/units.cc

chain_test_SOURCES = vox/test/chain_test.cc vox/chain/dsp/chain_engine.cc
analysis_test_SOURCES = vox/test/analysis_test.cc $(STMLIB)
syllable_test_SOURCES = vox/test/syllable_test.cc \
		vox/syllable/dsp/syllable_engine.cc $(STMLIB)
sequencer_test_SOURCES = vox/test/sequencer_test.cc \
		vox/sequencer/dsp/sequencer_engine.cc $(STMLIB)
split_test_SOURCES = vox/test/split_test.cc \
		vox/split/dsp/split_engine.cc $(STMLIB)
psola_test_SOURCES = vox/test/psola_test.cc \
		vox/psola/dsp/psola_engine.cc $(STMLIB)
lpc_test_SOURCES = vox/test/lpc_test.cc \
		vox/lpc/dsp/lpc_engine.cc $(STMLIB)
multi_test_SOURCES = vox/test/multi_test.cc
make_speech_SOURCES = vox/test/make_speech.cc
qpsk_decode_test_SOURCES = vox/test/qpsk_decode_test.cc \
		stm_audio_bootloader/qpsk/demodulator.cc \
		stm_audio_bootloader/qpsk/packet_decoder.cc

TESTS = chain_test analysis_test syllable_test sequencer_test split_test psola_test lpc_test multi_test
PROGRAMS = $(TESTS) qpsk_decode_test make_speech

all: $(addprefix $(BUILD)/,$(PROGRAMS))

define PROGRAM_RULE
$(BUILD)/$(1): $$($(1)_SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $$@ $$($(1)_SOURCES) -lm
endef
$(foreach p,$(PROGRAMS),$(eval $(call PROGRAM_RULE,$(p))))

check: $(addprefix $(BUILD)/,$(TESTS))
	@set -e; for t in $(TESTS); do echo "=== $$t"; $(BUILD)/$$t selftest; done

CORE_BUILD ?= build/vox_core

$(BUILD)/core_test: vox/test/core_test.cc vox/host/core_loader.h vox/host/vox_core_api.h $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ vox/test/core_test.cc -ldl -lm

core-check: $(BUILD)/core_test
	$(MAKE) -f vox/host/core.mk all BUILD=$(CORE_BUILD)
	$(BUILD)/core_test selftest $(CORE_BUILD)

.PHONY: all check core-check
