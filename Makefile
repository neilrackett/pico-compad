# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Neil Rackett

#
# Targets are named for what they prove, not for the roadmap phase that
# introduced them. docs/roadmap.md still numbers the phases; each script
# says which one it belongs to.
#
#   make                 the same as test-host: fast, run it constantly
#   make test-host       host tests only: decoder, encoder, mapping, socket
#   make test-decode     compad.c's own assertions, run on the ST
#   make test-serial     bytes cross the emulated link and frame up
#   make test-provider   a resident provider publishes a pad, and another
#                        program reads it back through the cookie jar
#   make test-live       the provider follows a sender that changes
#   make test-gamepad    a simulated controller: axes, triggers, pad type
#   make test-wire       a real UART, loopback: the only physical test
#   make test            all of the above, in order
#   make simulator       drive it yourself, in a window
#   make firmware        build the Pico W adapter firmware
#   make dist            collect what you flash or install into dist/
#   make tos             fetch EmuTOS into build/tos (the tests do this
#                        for you; the target is for priming a machine
#                        that is about to go offline)
#
#   STCMD_NO_TTY=1 stcmd make st       build the ST binaries
#
# st needs the atarist-toolkit-docker container; everything else needs
# Hatari and Node on the host, so the two never run under one command.
# A TOS image is fetched on first use, so no setup step is needed.
# That is the same split as atarist-xpad. Only test-host runs without
# the ST binaries: the rest boot them under Hatari and will tell you to
# build first.
#

BUILD = build

.DEFAULT_GOAL := test-host

.PHONY: test test-host test-decode test-serial test-provider \
        test-live test-gamepad test-wire simulator tos st firmware dist clean

# Not because of a shared port: no test target binds one, and each makes
# its own temporary FIFO. Because none of them fast-forward. Every
# sender here writes on a real-time clock, so several emulators
# contending for the host is exactly the condition that lets a sender
# outrun a slowed ST, which is the race these runs exist to detect.
.NOTPARALLEL:

test-host: | $(BUILD)
	cc -Wall -Wextra -Werror -std=c11 test/protocol_test.c -o $(BUILD)/protocol_test
	@$(BUILD)/protocol_test
	@echo
	cc -Wall -Wextra -Werror -std=c11 $(FW_INC) test/encode_test.c -o $(BUILD)/encode_test
	@$(BUILD)/encode_test
	@node test/xpadmap_test.js
	@node test/server_test.js

# rp/src/encode.h includes the real xpad, Bluepad32 and protocol headers
# rather than copying their constants, so the host build of the firmware
# encoder needs the same three include paths the firmware build uses.
FW_INC = -I lib/xpad/src \
         -I lib/bluepad32/src/components/bluepad32/include \
         -I target/atarist/src

# Everything you flash or install, in one place.
#
# This variable is the manifest. Nothing else in the repo should count
# or list these: the target prints what it made and where each piece
# goes, and prose that repeats the list has already disagreed with it
# three ways in one afternoon. Each entry is source, purpose, and the
# command that builds it if it is missing.
#
# Only the ones that go onto hardware. The remaining ST binaries in
# target/atarist/build are test programs that Hatari drives, and
# shipping them beside the ones you copy onto a real machine is how
# somebody ends up running CPDTEST.TOS on hardware and wondering why
# nothing happens.
#
# This copies rather than builds, because the two halves need different
# toolchains that must not run in the same place: the firmware wants
# CMake and arm-none-eabi-gcc on the host, the ST side wants the
# container.
DIST = dist
ST_BUILD = STCMD_NO_TTY=1 stcmd make st

define DIST_FILES
rp/build/compad.uf2|flash: hold BOOTSEL, plug in, copy it across|make firmware
target/atarist/build/COMPAD.PRG|install: into the ST's AUTO folder|$(ST_BUILD)
target/atarist/build/XPADVIEW.TOS|run it to watch the pad|$(ST_BUILD)
target/atarist/build/PIPECHK.TOS|run it first: prints raw bytes off the wire|$(ST_BUILD)
target/atarist/build/SENDTEST.TOS|the other direction: the ST transmitting|$(ST_BUILD)
endef
export DIST_FILES

# No order-only prerequisite on the directory: it would share its name
# with this target and make would call that circular.
dist:
	@mkdir -p $(DIST)
	@echo "$$DIST_FILES" | while IFS='|' read -r src what how; do \
		[ -n "$$src" ] || continue; \
		if [ ! -f "$$src" ]; then \
			echo "missing $$src, build it with: $$how"; \
			exit 1; \
		fi; \
		cp "$$src" $(DIST)/; \
		printf "%-24s %s\n" "$(DIST)/$${src##*/}" "$$what"; \
	done || exit 1

# The adapter firmware. Everything it needs is in lib/, so this wants
# nothing installed but CMake and arm-none-eabi-gcc, and it never runs
# as part of `test`: it is a cross build, not a check.
firmware:
	@cmake -B rp/build -S rp
	@cmake --build rp/build -j
	@echo
	@echo "flash rp/build/compad.uf2: hold BOOTSEL, plug in, copy it across"

test-decode:
	@test/run-decode.sh

test-serial:
	@test/run-serial.sh

test-provider:
	@test/run-provider.sh

test-gamepad:
	@test/run-gamepad.sh

test-live:
	@test/run-live.sh

test: test-host test-decode test-serial test-provider test-live \
      test-gamepad
	@echo "--- everything passed ---"

# Not in `test`: it needs hardware plugged in and a wire bridged, so it
# would fail on any machine that has neither. Run it deliberately.
test-wire:
	@test/run-wire.sh

simulator:
	@harness/simulator.sh

# The ROM is not committed: EmuTOS is GPLv2, so shipping the binary
# would oblige this repo to offer its source indefinitely, and it would
# sit in git history going stale. Fetched once into an ignored
# directory instead. $$TOS still overrides, which matters because
# EmuTOS always reports TOS 2.06.
tos:
	@. test/tos.sh && tos_find && echo "TOS: $$TOS"

st:
	$(MAKE) -C target/atarist

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(DIST) rp/build
	$(MAKE) -C target/atarist clean
