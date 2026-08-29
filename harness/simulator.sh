#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Neil Rackett
#
# The hands-on rig: a browser keyboard driving a real xpad block on an
# emulated ST, with a window you can watch.
#
# The human half of the exit criteria. `make test-provider` and
# `make test-gamepad` are the same chain with the browser replaced by a
# fixed pattern or a synthetic pad, so they run unattended; this one
# you drive yourself.
#
# Ctrl-C to stop. Overrides: $HATARI, $TOS, $MACHINE.

set -u

HERE=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$HERE/.." || exit 1

BUILD=target/atarist/build
HATARI=${HATARI:-/Applications/Hatari.app/Contents/MacOS/Hatari}
MACHINE=${MACHINE:-st}
URL=http://localhost:8232

for f in COMPAD.PRG XPADVIEW.TOS KEYECHO.TOS; do
    if [ ! -f "$BUILD/$f" ]; then
        echo "build it first: STCMD_NO_TTY=1 stcmd make st"
        exit 1
    fi
done
if [ ! -x "$HATARI" ]; then
    echo "Hatari not found; set \$HATARI"
    exit 1
fi
# shellcheck source=../test/tos.sh
. "$HERE/../test/tos.sh"
tos_find || exit 1

WORK=$(mktemp -d)
FIFO=$WORK/serial.in
mkfifo "$FIFO"
mkdir "$WORK/AUTO"
cp "$BUILD/COMPAD.PRG" "$WORK/AUTO/"     # installs at boot, stays resident
cp "$BUILD/XPADVIEW.TOS" "$WORK/"        # xpad's viewer, the full picture
cp "$BUILD/KEYECHO.TOS" "$WORK/"         # one line per change, for timing

cleanup() {
    kill "$NPID" 2>/dev/null
    sleep 1
    kill -9 "$NPID" 2>/dev/null
    rm -rf "$WORK"
    echo
    echo "stopped."
}
trap cleanup EXIT INT TERM

node harness/server.js "$FIFO" --verbose &
NPID=$!

cat <<BANNER

  COMpad, hands on
  ----------------
  1. a browser window is opening at $URL
     click the page first, so it has keyboard focus
     for a real controller, open $URL/pad.html instead
  2. in the ST window, double-click one of:
       XPADVIEW.TOS  the full viewer, redraws the whole screen
       KEYECHO.TOS   one timestamped line per change, Esc quits
  3. hold keys in the browser and watch the bits move

  Timing a delay? Run KEYECHO.TOS. The ST's console is echoed into
  this terminal, so both timelines land here together: the harness
  timestamps a key arriving and the frame going on the wire, KEYECHO
  prints t= when the ST sees it. Compare the two.

  Reading it: if the ST's line follows the wire line closely, the
  link is fine and what is slow is the picture, not the data. That
  points at the emulator window rather than at COMpad, and XPADVIEW
  especially, since it redraws the whole screen through the TOS
  console every frame.

  keys: arrows, Z X A S (south east west north),
        Q W (shoulders), Enter (start), Shift (select)

  COMPAD.PRG installs itself from AUTO at boot, so the provider is
  already resident by the time the desktop appears.

  Ctrl-C here when you are done.

BANNER

command -v open >/dev/null && (sleep 2; open "$URL") &

# Windowed on purpose: the whole point is watching it. Timer-D patching
# stays on and --rs232-out is given for the reasons in docs/roadmap.md.
#
# Never --fast-forward here either: you are the sender, on a wall clock,
# and the ST has to run at a comparable pace to feel responsive.
"$HATARI" --tos "$TOS" --machine "$MACHINE" --memsize 4 \
    --timer-d true --rs232-in "$FIFO" --rs232-out "$WORK/serial.out" \
    --conout 2 \
    "$WORK"
