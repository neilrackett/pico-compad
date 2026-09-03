<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 Neil Rackett -->

# Hardware

Building the adapter, and which socket to plug it into. No adapter has
been built yet, and the ST side runs only under emulation. The one
exception is the wire: `make test-wire` pushes real frames through a
real UART with TX shorted to RX, so throughput and latency have been
measured on hardware even though nothing has been plugged into an ST.

## Pico side

Pico W or Pico 2 W, with Bluepad32 over BTstack handling the Bluetooth
HID side.

Conversions from Bluepad32 to xpad conventions:

```
axis:    clamp(bp_axis >> 2, -127, 127)     /* -512..511 to -127..127 */
trigger: bp_trigger >> 2                     /* 0..1023 to 0..255     */
```

## Wiring

Pico presents as DCE, so a straight-through cable works.

Pin numbers below are for the **DE-9** connector on a Mega STE's
Serial 2, and for the DB25 modem port on a plain ST or STE. Serial 2
uses the standard PC AT DE-9 pinout, confirmed on hardware: 1 DCD,
2 RxD, 3 TxD, 4 DTR, 5 GND, 6 DSR, 7 RTS, 8 CTS, 9 RI. The two
shells swap the data pins: on DE-9 pin 2 is RxD and pin 3 is TxD, on
DB25 it is the other way round. Miswiring that is the classic way to
get a silent link, so check the shell before the pin number.

| Pico | MAX3232 | Mega STE DE-9 | ST/STE DB25 |
|------|---------|---------------|-------------|
| GP0 (UART0 TX) | T1IN (11) to T1OUT (14) | pin 2, ST RxD | pin 3 |
| GP1 (UART0 RX) | R1OUT (12) from R1IN (13) | pin 3, ST TxD | pin 2 |
| 3V3 out (pin 36) | VCC (16) | | |
| GND (pin 38) | GND (15) | pin 5, signal ground | pin 7 |

Charge pump capacitors, all 100nF ceramic:

- C1+ (1) to C1- (3)
- C2+ (4) to C2- (5)
- V+ (2) to GND
- V- (6) to GND
- VCC (16) to GND, close to the chip

**Connector:** DE-9 **female** (sockets), since the machine's serial
ports are male. A screw terminal breakout block is easiest for
prototyping. An existing ST modem cable can be reused through a DE-9 to
DB25 adapter: the shell is the only physical difference.

**The RS-232 side swings to roughly ±5.5V from the charge pump. Never
let a DE-9 pin touch a GPIO.** Check that side twice before first
power-up.

**Handshake lines:** leave the rest unconnected and use handshake mode
0. If stock TOS terminal software should also work through the adapter,
add wire links inside the hood so it sees its own signals returned: on
DE-9 that is pin 7 to 8 (RTS to CTS) and pin 4 bridged to 6 and 1 (DTR
to DSR and DCD); on DB25 it is pin 4 to 5, and pin 20 bridged to 6 and
8. No components needed.

On the ST side the outgoing lines, RTS and DTR, are PSG port A bits 3
and 4, sharing that register with the floppy selects and the
Centronics strobe: anything asserting them must read-modify-write,
never store a whole byte. The incoming lines, DCD, CTS and RI, arrive
on the MFP GPIP at `$FFFA01` alongside Centronics busy. This code
touches neither, but a future flow-control phase would.

**Machine notes:** ST and STE have a single DB25 modem port, wired to
the MFP 68901.

On the Mega STE the equivalent is **Serial 2**, the 9-pin male D
connector on the rear panel below the VME slot. It is the same MFP at
`$FFFA00`, so it is register and interrupt compatible with ST/STE code
and needs no changes: only the connector shell differs.

The machine's other two serial ports are Mega STE additions driven by
the SCC 85C30 rather than the MFP, and neither is usable by this code
as written. Serial 1 is SCC channel B, and the LAN socket is SCC
channel A.

| Port | Chip | Bconmap device |
|------|------|----------------|
| Serial 2 (below the VME slot) | MFP 68901 | 6, the boot default |
| Serial 1 | SCC channel B | 7 |
| LAN | SCC channel A | 9 |

Device 6 is the default at boot, so `Bconin`/`Bconout` on device 1
(AUX) reach the MFP unless something has remapped the BIOS. Since
`Iorec(0)` follows that mapping, reading a ring belonging to an SCC
port would look exactly like a dead cable, so the provider calls
`Bconmap(6)` at install rather than trusting the default, and reports
it if the mapping had moved. Below TOS 2.00 it skips the call: Bconmap
arrived with the machines that have more than one serial port, and
anything older has only the MFP to offer anyway.

Beware one piece of misinformation while working on this: Hatari's own
manual lists the Mega STE's MFP port as a DB25 named "Modem 1". The
hardware says otherwise.

## Driving the port from the ST

Move off `Rsconf` onto a private MFP channel 12 handler once the
protocol is proven. IPL 6, and unlike the MIDI route the vector is not
shared with the IKBD, so no second status register to poll.
