# SPDX-License-Identifier: MIT
# Copyright Kebag-Logic, 2026
#
# Tests of the Linux platform (the network stack driver and flexptpd).
#
#   make -C tests/linux check     the unit test, then the netns smoke test
#   make -C tests/linux unit      the transmit timestamp matcher alone
#   make -C tests/linux netns     the two-daemon smoke test alone
#
# The netns test runs build/flexptpd at the top of the tree, built first with
# cmake -S linux -B build (it does so itself when missing). BUILD=... points
# elsewhere.

TOP := $(abspath ../..)
BUILD ?= $(TOP)/build
O ?= $(BUILD)/tests-linux
CC ?= cc

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -I$(TOP)/src

TXTS := $(TOP)/src/flexptp/port/example_netstack_drivers/nsd_linux_txts.c

all: check

check: unit netns

unit: $(O)/test_txts
	$(O)/test_txts

$(O)/test_txts: test_txts.c $(TXTS) $(TOP)/src/flexptp/port/example_netstack_drivers/nsd_linux_txts.h
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -o $@ test_txts.c $(TXTS)

netns: $(BUILD)/flexptpd
	./run-netns-smoke.sh $(BUILD)

$(BUILD)/flexptpd:
	cmake -S $(TOP)/linux -B $(BUILD)
	cmake --build $(BUILD)

.PHONY: all check unit netns
