#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Neil Rackett
#
# Does the provider follow a sender that changes its mind?
#
# The other end-to-end tests hold one pattern throughout, which a
# consumer can pass while re-reading stale bytes forever, because stale
# bytes decode to the same value. compad.c did exactly that with every
# test green: it had TOS's AUX ring head and tail indices the wrong way
# round, so it walked the ring backwards over old data and never
# advanced the read index. Nothing showed until a button changed.
#
# So this one changes the button pattern on a loop and insists the ST
# sees it move.

set -u

HERE=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$HERE/.." || exit 1

# shellcheck source=tos.sh
. "$HERE/tos.sh"
# shellcheck source=hatari.sh
. "$HERE/hatari.sh"

hatari_require COMPAD.PRG LIVECHK.TOS || exit 1
tos_find || exit 1
hatari_workdir || exit 1

cp "$BUILD/COMPAD.PRG" "$WORK/AUTO/"
cp "$BUILD/LIVECHK.TOS" "$WORK/"

node harness/server.js "$FIFO" --cycle &
hatari_own $!
hatari_boot LIVECHK.TOS

hatari_wait "LIVE-DONE" 90

hatari_expect "LIVE-DONE 0" "the published value followed the sender"
hatari_verdict "the provider follows a changing sender" "live updates FAILED"
