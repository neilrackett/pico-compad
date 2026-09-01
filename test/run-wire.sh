#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Neil Rackett
#
# The wire, on real hardware.
#
# Every other test in this project runs through Hatari, whose serial is
# byte-level plumbing and not a bit-accurate UART. Framing and checksums
# are testable there; throughput and latency are not, and pretending
# otherwise is how a project convinces itself of a number it has never
# measured. This is the one test that touches a physical link.
#
# Needs a USB serial adapter with TX shorted to RX. Nothing else: no ST,
# no Pico, no level shifter, since both ends of the loopback are the
# same 3.3V pin. A Raspberry Pi Debug Probe works and is the safer
# choice, being 3.3V only with no voltage jumper to set wrong.
#
#   make test-wire                       auto-detect the port
#   SERIAL=/dev/cu.usbmodem1234 make test-wire
#   BAUDS="9600 19200" make test-wire

set -u

HERE=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$HERE/.." || exit 1

BAUDS=${BAUDS:-"9600 19200 38400"}

# Pick a port unless told. cu, not tty: the callout device does not
# block waiting for carrier. Bluetooth and the debug console are not
# serial adapters however much they look like one.
if [ -z "${SERIAL:-}" ]; then
    for p in /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.SLAB* /dev/cu.wchusb*; do
        [ -e "$p" ] || continue
        SERIAL=$p
        break
    done
fi

if [ -z "${SERIAL:-}" ] || [ ! -e "${SERIAL:-}" ]; then
    echo "No serial adapter found."
    echo
    echo "Plug one in, or name it: SERIAL=/dev/cu.something make test-wire"
    echo
    echo "Candidates on this machine right now:"
    ls /dev/cu.* 2>/dev/null | grep -viE 'bluetooth|debug-console' | sed 's/^/  /' \
        || echo "  (none)"
    exit 1
fi

echo "port: $SERIAL"
echo "Loopback test: this needs TX shorted to RX on that adapter."
echo

fail=0
for baud in $BAUDS; do
    echo "--- $baud baud ---"
    node harness/wire.js "$SERIAL" "$baud" 200
    rc=$?

    if [ $rc -eq 0 ]; then
        echo "    ok"
    elif [ $rc -eq 3 ]; then
        # No loopback at all: the other bauds will say the same thing,
        # and each one costs another slow open.
        echo "    no link, so not trying the remaining rates"
        fail=1
        break
    else
        echo "    FAILED at $baud"
        fail=1
    fi
    echo
done

if [ $fail -eq 0 ]; then
    echo "--- the wire carries the protocol at every baud tried ---"
    exit 0
fi
echo "--- the wire test FAILED ---"
exit 1
