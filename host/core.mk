# Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
#
# Builds a VOX core: a VOX firmware compiled for this computer as a shared
# library (vox/host/vox_core_api.h), for the VCV Rack module and host tests.
#
# From eurorack/:
#   make -f vox/host/core.mk FIRMWARE=multi        (or psola, lpc, syllable,
#                                                   split, sequencer, chain)
#   make -f vox/host/core.mk all                   (all seven)
# Output: build/vox_core/vox_<firmware>.<so|dylib|dll>
# CORE_FLAGS: extra compiler flags (the Rack plugin passes its target, so the
# cores match the plugin's architecture).
#
# The firmware sources are the module's (panel glue, engines, vox_ui,
# settings, cv_reader, gain_control); vox/host/shim replaces the hardware
# headers and vox/host/vox_host_platform.cc replaces vox_platform.cc.

FIRMWARE ?= multi
BUILD ?= build/vox_core
CXX ?= c++

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  EXT = dylib
  LDFLAGS_SHARED = -dynamiclib -undefined error
else ifneq (,$(findstring MINGW,$(UNAME))$(findstring MSYS,$(UNAME)))
  EXT = dll
  LDFLAGS_SHARED = -shared -static-libstdc++ -static-libgcc
else
  EXT = so
  LDFLAGS_SHARED = -shared -Wl,-Bsymbolic -Wl,--no-undefined
endif

ALGORITHMS = psola lpc syllable split sequencer chain

ifeq ($(FIRMWARE),multi)
  GLUE_DIRS = $(ALGORITHMS)
  FIRMWARE_DEFS = -DVOX_MULTI
  FIRMWARE_SOURCES = vox/multi/multi.cc
else
  GLUE_DIRS = $(FIRMWARE)
  FIRMWARE_DEFS =
  FIRMWARE_SOURCES =
endif

SOURCES = \
	$(FIRMWARE_SOURCES) \
	$(foreach d,$(GLUE_DIRS),vox/$(d)/$(d).cc $(wildcard vox/$(d)/dsp/*.cc)) \
	vox/host/vox_host_platform.cc \
	vox/common/hw/cv_reader.cc \
	vox/common/hw/gain_control.cc \
	vox/common/hw/settings.cc \
	vox/common/ui/vox_ui.cc \
	stmlib/dsp/units.cc \
	stmlib/utils/random.cc

CXXFLAGS = -O2 -std=gnu++11 -fPIC -fvisibility=hidden \
	-Wall -Wno-unused-local-typedefs -Wno-unused-variable \
	-DTEST -DVOX_HOST $(FIRMWARE_DEFS) -DVOX_CORE_NAME='"vox_$(FIRMWARE)"' \
	-Ivox/host/shim -I. $(CORE_FLAGS)

HEADERS = $(wildcard vox/host/*.h vox/host/shim/*/*.h vox/host/shim/*/*/*.h \
	vox/host/shim/*/*/*/*.h vox/common/*/*.h vox/*/dsp/*.h)

TARGET = $(BUILD)/vox_$(FIRMWARE).$(EXT)

$(TARGET): $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(LDFLAGS_SHARED) -o $@ $(SOURCES) -lm

all:
	@for f in multi $(ALGORITHMS); do $(MAKE) -f vox/host/core.mk FIRMWARE=$$f || exit 1; done

.PHONY: all
