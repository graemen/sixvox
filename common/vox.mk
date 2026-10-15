# Copyright 2026 VOX contributors. MIT license.
#
# Shared build rules for VOX firmwares. Included by vox/<name>/makefile after
# it has set TARGET, PACKAGES, etc.

TOOLCHAIN_PATH ?= /usr/local/arm/
OPENOCD_BIN_PATH ?= /usr/local/bin/
OPENOCD_SCRIPT_PATH ?= /usr/local/share/openocd/scripts/

# Must be set before stmlib/makefile.inc (used in a prerequisite list).
UPLOAD_COMMAND = upload_application_jtag

include stmlib/makefile.inc

# The STM32H7 HAL headers use the 'register' keyword, which C++17 (the default
# of GCC >= 11) rejects under -Werror. Overrides the value set by
# stmlib/makefile.inc (recipes expand it lazily).
CPPFLAGS = -fno-exceptions -fno-rtti -std=gnu++11

# Python 3 port of the stock QPSK encoder (same signal as beads' "make wav").
wav: $(TARGET_BIN)
	python3 vox/tools/qpsk_encoder.py $(TARGET_BIN) -o $(BUILD_DIR)$(TARGET).wav

# SWD upload of the application only (the Beads bootloader stays in place).
upload_application_jtag: $(TARGET_BIN)
	$(OPENOCD_BIN_PATH)openocd \
		-s $(OPENOCD_SCRIPT_PATH) \
		-f $(OPENOCD_INTERFACE_SCRIPT) \
		-f $(OPENOCD_TARGET_SCRIPT) \
		-c "init" -c "halt" -c "sleep 200" \
		-c "flash write_image erase $(TARGET_BIN) 0x08020000" \
		-c "verify_image $(TARGET_BIN) 0x08020000" \
		-c "sleep 200" -c "reset run" -c "shutdown"

# Extra preprocessor definitions for a firmware (vox_multi: -DVOX_MULTI).
# Kept out of CPPFLAGS so that overriding CPPFLAGS on the command line keeps
# them.
CCFLAGS += $(VOX_DEFS)
