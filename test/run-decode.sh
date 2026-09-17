#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Neil Rackett
#
# compad.c's own assertions, on the ST.
#
# CPDTEST.TOS is the -DXPAD_SELFTEST build of the provider: it feeds
# frames straight into apply_frame and checks what lands in the shadow,
# so it needs no serial link and no sender. It covers the pieces the
# end-to-end runs cannot see from outside: that a corrupt frame is
# dropped, that full state replaces rather than accumulates, that a
# descriptor upgrades the pad type, and that the block passes
# xpad_valid before it is ever published.
#
# It was built by every `make st` and run by nothing for a while. If it
# is cheap enough to build it is cheap enough to run: this is the
# fastest of the emulated targets, because nothing has to be waited for.

set -u

HERE=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$HERE/.." || exit 1

# shellcheck source=tos.sh
. "$HERE/tos.sh"
# shellcheck source=hatari.sh
. "$HERE/hatari.sh"

hatari_require CPDTEST.TOS || exit 1
tos_find || exit 1
hatari_workdir --no-serial || exit 1

cp "$BUILD/CPDTEST.TOS" "$WORK/"

hatari_boot CPDTEST.TOS

hatari_wait_verdict "COMPAD-DONE" 60 "the provider's own assertions all passed"
hatari_verdict "compad.c is sound on the ST" "compad.c self test FAILED"
