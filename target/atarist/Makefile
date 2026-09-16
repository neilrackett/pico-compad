# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Neil Rackett

# Build under the cross toolchain:
#   STCMD_NO_TTY=1 stcmd make        (from this directory or via root)

CC     = m68k-atari-mint-gcc
# Passed in from the repo root, which reads version.txt. Defaulted the
# way md-net does, so building this directory alone still works.
COMPAD_VERSION ?= v0.0.0

CFLAGS = -Wall -Wextra -Werror -O2 -fomit-frame-pointer -m68000 \
         -DCOMPAD_VERSION='"$(COMPAD_VERSION)"'
BUILD  = build

# xpad comes in as a submodule; a provider links both halves.
XPAD     = ../../lib/xpad/src
XPADSRC  = $(XPAD)/xpad.c $(XPAD)/xpad_provider.c

COMPADSRC = src/compad.c src/etv.s $(XPADSRC)
COMPADDEP = $(COMPADSRC) src/protocol.h $(XPAD)/xpad.h

all: $(BUILD)/PIPECHK.TOS $(BUILD)/SENDTEST.TOS \
     $(BUILD)/COMPAD.PRG $(BUILD)/CPDTEST.TOS \
     $(BUILD)/VIEWTEST.TOS $(BUILD)/XPADVIEW.TOS $(BUILD)/LIVECHK.TOS \
     $(BUILD)/KEYECHO.TOS

# Bring-up only: the ST transmitting, which is the direction that is
# easy to read because the adapter's console can report what arrived.
$(BUILD)/SENDTEST.TOS: src/sendtest.c | $(BUILD)
	$(CC) $(CFLAGS) src/sendtest.c -o $@

$(BUILD)/PIPECHK.TOS: src/pipechk.c src/protocol.h | $(BUILD)
	$(CC) $(CFLAGS) src/pipechk.c -o $@

# Consumer side only: it reads the block a resident provider publishes.
$(BUILD)/LIVECHK.TOS: src/livechk.c $(XPAD)/xpad.c $(XPAD)/xpad.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(XPAD) src/livechk.c $(XPAD)/xpad.c -o $@

# The hands-on counterpart: prints when the ST sees a change, cheaply
# enough that its own output is not what you are measuring.
$(BUILD)/KEYECHO.TOS: src/keyecho.c $(XPAD)/xpad.c $(XPAD)/xpad.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(XPAD) src/keyecho.c $(XPAD)/xpad.c -o $@

$(BUILD)/COMPAD.PRG: $(COMPADDEP) | $(BUILD)
	$(CC) $(CFLAGS) -I$(XPAD) $(COMPADSRC) -o $@

# The same source built to run its self test and exit: Hatari's --auto
# takes a path and no arguments. It supplies argv and runs the ordinary
# main, per xpad's AGENTS rule.
$(BUILD)/CPDTEST.TOS: $(COMPADDEP) | $(BUILD)
	$(CC) $(CFLAGS) -I$(XPAD) -DXPAD_SELFTEST $(COMPADSRC) -o $@

# xpad's own viewer, harness build, straight from the submodule: it is
# the reference consumer, so it is what verifies the provider.
$(BUILD)/VIEWTEST.TOS: $(XPAD)/tools/xpadview.c $(XPADSRC) | $(BUILD)
	$(CC) $(CFLAGS) -DXPAD_SELFTEST $(XPAD)/tools/xpadview.c $(XPADSRC) -o $@

# And the interactive build, for pressing keys at: the phase 1 exit
# criteria in README are a human watching bits light up in this.
# .TOS, not .PRG: the viewer is a console program, no GEM calls at all,
# so TOS should give it a text screen rather than launch it over the
# desktop. xpad's own Makefile still calls it .PRG; fix that upstream
# and this comment goes away.
$(BUILD)/XPADVIEW.TOS: $(XPAD)/tools/xpadview.c $(XPADSRC) | $(BUILD)
	$(CC) $(CFLAGS) $(XPAD)/tools/xpadview.c $(XPADSRC) -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
