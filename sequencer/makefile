# Copyright 2026 VOX contributors. MIT license.
#
# vox_sequencer: clocked word/syllable re-sequencer firmware for Mutable Instruments Beads.
#
# Build from the eurorack/ root:
#   make -f vox/sequencer/makefile            (ELF/BIN, needs arm-none-eabi-gcc)
#   make -f vox/sequencer/makefile wav        (audio update file, needs python3 + numpy)
#   make -f vox/sequencer/makefile upload     (SWD via OpenOCD, application only)
#
# Recent GCC versions default to C++17, in which the STM32H7 HAL headers do not
# compile with -Werror; vox/common/vox.mk pins gnu++11 like the original
# toolchain.

F_CRYSTAL      = 8000000L
F_CPU          = 400000000L
SYSCLOCK       = SYSCLK_FREQ_400MHz
FAMILY         = h7xx

APPLICATION_LARGE = TRUE
BOOTLOADER        = beads_bootloader

TARGET         = vox_sequencer
PACKAGES       = vox/sequencer \
		vox/sequencer/dsp \
		vox/common/app \
		vox/common/hw \
		vox/common/ui \
		beads/drivers \
		stmlib/dsp \
		stmlib/utils \
		stmlib/system

include vox/common/vox.mk
