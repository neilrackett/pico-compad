#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Neil Rackett
#
# A simulated gamepad, end to end (roadmap phase 2): a synthetic pad
# driven through the real @mesmotronic/xpad library, encoded as COMpad
# frames, decoded by COMPAD.PRG and read back by xpad's viewer through
# the cookie jar.
#
# This is the analogue counterpart to test-provider, which proves
# buttons; this proves the whole state frame: positional face buttons
# (the X/Y trap), signed axes across the 68000's byte order, unsigned
# triggers, and a descriptor that claims XPAD_CAP_ANALOG and means it.
#
# The pose is held rather than cycled, so the run is an assertion and
# not a race. Expected values come from harness/xpadmap.js and are
# pinned by test/xpadmap_test.js on the host first.

set -u

HERE=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$HERE/.." || exit 1

# shellcheck source=tos.sh
. "$HERE/tos.sh"
# shellcheck source=hatari.sh
. "$HERE/hatari.sh"

hatari_require COMPAD.PRG VIEWTEST.TOS || exit 1
if [ ! -d node_modules/@mesmotronic/xpad ]; then
    echo "the simulated pad needs its library: npm install"
    exit 1
fi
tos_find || exit 1
hatari_workdir || exit 1

cp "$BUILD/COMPAD.PRG" "$WORK/AUTO/"
cp "$BUILD/VIEWTEST.TOS" "$WORK/"

node harness/padsim.js "$FIFO" --hold &
hatari_own $!
hatari_boot VIEWTEST.TOS

hatari_wait_verdict "XPAD-DONE" 60 "the viewer ran to completion"

hatari_expect "provider COMpad $(cat version.txt)"     "the provider is COMpad"
hatari_expect "caps     0001"           "XPAD_CAP_ANALOG is claimed"
hatari_expect "type Xbox"               "the descriptor set the pad type"
# 0x2e80 = WEST | TR | TL2 | TR2 | START. WEST rather than NORTH is the
# X/Y trap: js-xpad calls that button X, and it must not land on XPAD_X.
# 2e88, not 2e80: the extra 0x08 is XPAD_RIGHT, folded from the stick.
# The simulated pad holds lx 64, which clears the provider's deadzone, so
# this line is also the end to end check that the provider folds stick
# direction into the d-pad bits the way xpad.h requires. A digital
# consumer such as STDL reads those bits and never looks at the axes.
hatari_expect "buttons  00002e88"       "buttons, with WEST and the folded stick"
hatari_expect "sticks   64,-32 -127,95" "signed axes survived the wire"
hatari_expect "triggers 64 255"         "unsigned triggers survived the wire"
hatari_verdict "a simulated gamepad reads correctly on the ST" \
               "simulated gamepad FAILED"
