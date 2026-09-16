#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Neil Rackett
#
# The provider, end to end (roadmap phase 1). Browserless: the server's
# --test mode holds a fixed button pattern, COMPAD.PRG installs
# resident from AUTO and publishes it as an xpad block, and xpad's own
# viewer reads it back through the cookie jar.
#
# This is the only test where provider and consumer are separate
# programs, so it is the one that proves residency, the cookie jar and
# the consumer path together. Buttons only; test-gamepad covers the
# rest of the state, and test-live covers updates actually arriving.
# make simulator is the human version: this proves the machinery,
# pressing keys proves the feel.

set -u

HERE=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$HERE/.." || exit 1

# shellcheck source=tos.sh
. "$HERE/tos.sh"
# shellcheck source=hatari.sh
. "$HERE/hatari.sh"

hatari_require COMPAD.PRG VIEWTEST.TOS || exit 1
tos_find || exit 1
hatari_workdir || exit 1

cp "$BUILD/COMPAD.PRG" "$WORK/AUTO/"
cp "$BUILD/VIEWTEST.TOS" "$WORK/"

node harness/server.js "$FIFO" --test &
hatari_own $!
hatari_boot VIEWTEST.TOS

hatari_wait "XPAD-DONE" 60

hatari_expect "provider COMpad v" "a resident provider owns the cookie"
hatari_expect "buttons  00000218" "the held pattern reached the consumer"
hatari_verdict "a resident provider is on the cookie jar" "provider FAILED"
