# Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
#
# VCV Rack 2 plugin running the VOX firmwares.
#
#   make RACK_DIR=/path/to/Rack-SDK            plugin + firmware cores
#   make RACK_DIR=/path/to/Rack-SDK install    into the Rack user folder
#
# The firmware cores (cores/vox_<firmware>.so|dylib|dll) are built from the
# firmware sources in this repository by vox/host/core.mk.

RACK_DIR ?= ../../../Rack-SDK

EURORACK := $(abspath ../..)

FLAGS += -I$(EURORACK)
SOURCES += $(wildcard src/*.cpp)
DISTRIBUTABLES += res cores

include $(RACK_DIR)/plugin.mk

ifdef ARCH_LIN
	LDFLAGS += -ldl
endif

CORES := multi psola lpc syllable split sequencer chain

# The cores are built with the plugin's compiler and, on macOS, its target
# (architecture and minimum system version).
CORE_FLAGS :=
ifdef ARCH_MAC
	CORE_FLAGS += --target=$(MACHINE) $(MAC_SDK_FLAGS)
endif

cores:
	@for f in $(CORES); do \
		$(MAKE) -C $(EURORACK) -f vox/host/core.mk FIRMWARE=$$f BUILD=$(CURDIR)/cores \
			CXX="$(CXX)" CORE_FLAGS="$(CORE_FLAGS)" || exit 1; \
	done

all: cores

clean-cores:
	rm -rf cores

.PHONY: cores clean-cores
