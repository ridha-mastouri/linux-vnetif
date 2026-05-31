# SPDX-License-Identifier: GPL-2.0-only
# Build system for the vnetif kernel module.
#
# All build artefacts are isolated in build/ — src/ is never written to.
#
# Targets:
#   make          — build vnetif.ko into build/
#   make clean    — remove the build/ directory
#   make test     — build the module, compile and run integration tests

KDIR      := /lib/modules/$(shell uname -r)/build
PWD       := $(CURDIR)
BUILD_DIR := $(PWD)/build
MODULE    := vnetif

# Explicit source list — vnetif.c is a legacy stub and is intentionally excluded.
MODULE_SRCS := src/vnetif_core.c src/vnetif_netdev.c src/vnetif_proc.c
MODULE_HDRS := src/vnetif.h

.PHONY: all clean test

all:
	mkdir -p $(BUILD_DIR)
	cp $(MODULE_SRCS) $(MODULE_HDRS) $(BUILD_DIR)/
	{ \
	  printf 'obj-m    := vnetif.o\n'; \
	  printf 'vnetif-y := vnetif_core.o vnetif_netdev.o vnetif_proc.o\n'; \
	} > $(BUILD_DIR)/Kbuild
	$(MAKE) -C $(KDIR) M=$(BUILD_DIR) modules
	@echo ""
	@echo "  Built: $(BUILD_DIR)/$(MODULE).ko"

clean:
	rm -rf $(BUILD_DIR)

test: all
	cmake -S tests -B $(BUILD_DIR)/tests -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR)/tests --parallel
	sudo $(BUILD_DIR)/tests/vnetif_tests