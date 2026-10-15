# Copyright 2026 VOX contributors. MIT license.
#
# vox_lpc: LPC analysis/resynthesis firmware for Mutable Instruments Beads.
#
# Build from the eurorack/ root:
#   make -f vox/lpc/makefile            (ELF/BIN, needs arm-none-eabi-gcc)
#   make -f vox/lpc/makefile wav        (audio update file, needs python3 + numpy)
#   make -f vox/lpc/makefile upload     (SWD via OpenOCD, application only)
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

TARGET         = vox_lpc
PACKAGES       = vox/lpc \
		vox/lpc/dsp \
		vox/common/app \
		vox/common/hw \
		vox/common/ui \
		beads/drivers \
		stmlib/dsp \
		stmlib/utils \
		stmlib/system

include vox/common/vox.mk
