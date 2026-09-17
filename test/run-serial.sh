#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Neil Rackett
#
# The serial link, end to end (roadmap phase 0). The harness writes
# fixed state frames into a FIFO, Hatari presents them as RS-232, and
# PIPECHK.TOS decodes them under EmuTOS. If the bytes arrive and frame
# up, the emulated path, the baud rate and the framing are all proven
# and everything after this is software.
#
# Runs on the host, never under stcmd: the container has no Hatari and
# no Node. Overrides: $HATARI, $TOS, $MACHINE.

set -u

HERE=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$HERE/.." || exit 1

# shellcheck source=tos.sh
. "$HERE/tos.sh"
# shellcheck source=hatari.sh
. "$HERE/hatari.sh"

hatari_require PIPECHK.TOS || exit 1
tos_find || exit 1
hatari_workdir || exit 1

cp "$BUILD/PIPECHK.TOS" "$WORK/"

node harness/frames.js "$FIFO" &
hatari_own $!
hatari_boot PIPECHK.TOS

hatari_wait_verdict "COMPAD-DONE" 60 "the ST framed up the bytes it received"
hatari_verdict "the serial link works" "serial link FAILED"
