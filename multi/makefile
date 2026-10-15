# Copyright 2026 VOX contributors. MIT license.
#
# vox_multi: all six VOX algorithms in one firmware for Mutable Instruments
# Beads. Hold MEDIA at power-on to choose the algorithm with PITCH.
#
# Build from the eurorack/ root:
#   make -f vox/multi/makefile            (ELF/BIN, needs arm-none-eabi-gcc)
#   make -f vox/multi/makefile wav        (audio update file, needs python3 + numpy)
#   make -f vox/multi/makefile upload     (SWD via OpenOCD, application only)

F_CRYSTAL      = 8000000L
F_CPU          = 400000000L
SYSCLOCK       = SYSCLK_FREQ_400MHz
FAMILY         = h7xx

APPLICATION_LARGE = TRUE
BOOTLOADER        = beads_bootloader

TARGET         = vox_multi
VOX_DEFS       = -DVOX_MULTI
PACKAGES       = vox/multi \
		vox/psola \
		vox/psola/dsp \
		vox/lpc \
		vox/lpc/dsp \
		vox/syllable \
		vox/syllable/dsp \
		vox/split \
		vox/split/dsp \
		vox/sequencer \
		vox/sequencer/dsp \
		vox/chain \
		vox/chain/dsp \
		vox/common/app \
		vox/common/hw \
		vox/common/ui \
		beads/drivers \
		stmlib/dsp \
		stmlib/utils \
		stmlib/system

include vox/common/vox.mk
