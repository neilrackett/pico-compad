<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 Neil Rackett -->

# Wiring

The minimum build, and what to add later. Only the MAX3232 is required:
the firmware runs, pairs a controller and sends frames with nothing else
attached.

Read the warnings at the end before powering anything.

## Minimum build

Four wires, if you have a MAX3232 module with the DE-9 already on it.

```
        Pico W                     MAX3232 module              Atari ST
   ┌────────────────┐            ┌──────────────┐
   │                │            │              │
   │  GP0  (pin 1) ─┼────────────┤ TXD          │         ┌───────────┐
   │                │            │         DE-9 ├─────────┤ Serial 2  │
   │  GP1  (pin 2) ─┼────────────┤ RXD   female │  cable  │  (Mega    │
   │                │            │              │         │   STE)    │
   │  GND  (pin 38)─┼────────────┤ GND          │         └───────────┘
   │                │            │              │          or the DB25
   │  3V3  (pin 36)─┼────────────┤ VCC          │          modem port
   │                │            │              │          on an ST/STE
   └────────────────┘            └──────────────┘
        USB for power
        and the console
```

`GP0` is UART0 TX and `GP1` is UART0 RX, which is what `rp/src/config.h`
sets and what the firmware brings up before Bluetooth.

**The module's own labels are from its point of view, and makers differ
about that.** If nothing arrives, swap `TXD` and `RXD` at the module
header and try again. Both ends are 3.3V CMOS there, so getting it wrong
costs nothing.

Check the chip is a MAX**3232** and not a MAX232. The MAX232 wants 5V
and will not work from the Pico's 3V3 rail.

### If your MAX3232 is a bare chip

You also need five 100nF ceramic capacitors for the charge pump, and a
DE-9 female connector. The pinout is in
[hardware.md](hardware.md#wiring); the module simply has all of that
fitted already.

## Optional extras

Neither is needed to build, flash or use the adapter, and neither needs
a build flag to leave out. The firmware drives both unconditionally,
which is safe because an unconnected output does nothing and an input
with a pull-up reads as "not pressed" for ever.

```
        Pico W
   ┌────────────────┐
   │                │           ┌───┐
   │  GP16 (pin 21)─┼───────────┤ R ├────▶│───┐        R = 330Ω
   │                │           └───┘   LED   │
   │  GND  (pin 23)─┼─────────────────────────┘
   │                │
   │                │            ╭─────╮
   │  GP15 (pin 20)─┼────────────┤ o o ├───┐   momentary push button
   │                │            ╰─────╯   │
   │  GND  (pin 18)─┼──────────────────────┘   no resistor: the pin
   │                │                          uses its internal pull-up
   └────────────────┘
```

**LED.** Shows the same three states as the Pico's onboard LED, which
works from the moment you flash it. The external one is worth adding
because the onboard one is sealed inside the enclosure, and because it
is the only one that can report a radio failure: the onboard LED hangs
off the CYW43 chip and says nothing at all until `cyw43_arch_init()` has
succeeded, so a dead radio looks exactly like a dead board.

| LED | State |
| --- | ----- |
| Rapid flash | Looking for a new pad |
| Long blink | Waiting for a known pad to come back |
| Solid | A pad is connected |
| Solid, straight from power-on, onboard LED dark | The radio failed to start |

**Button.** Held for two seconds, it forgets every paired controller.
That is all it does: pairing itself needs no button, because the adapter
scans whenever a slot is free and a known pad reconnects on its own. See
[roadmap.md](roadmap.md) for why it works that way. Without the button
you simply cannot forget a pad, which matters only when you move one to
another machine.

You will see the LED change from the long blink to the rapid flash when
the keys are wiped. That is the confirmation.

## Which socket on the ST

| Machine | Socket | Shell |
| ------- | ------ | ----- |
| Mega STE | Serial 2, below the VME slot | DE-9 male |
| ST, STE, Mega ST | Modem port | DB25 male |

The two shells swap the data pins, so check which you have before
trusting a pin number. An existing ST modem cable works through a DE-9
to DB25 adapter, since the shell is the only physical difference.

## Before you power it

- **The RS-232 side swings to about ±5.5V** from the charge pump.
  Never let a DE-9 pin touch a Pico GPIO.
- **Check the shell before the pin number.** On DE-9 pin 2 is the ST's
  RxD; on DB25 it is pin 3. Miswiring that is the classic way to get a
  silent link with nothing to see on either end.
- **The module may be wired as DTE rather than DCE**, in which case its
  transmit lands on the ST's transmit and nothing works. Power the
  module, leave its TTL `TXD` idle, and measure DE-9 pins 2 and 3
  against pin 5: whichever sits at a solid negative voltage is the
  module's output. It should be **pin 2**. If it is pin 3, you need a
  null modem adapter or to cross 2 and 3.

## Proving it without an ST

Short DE-9 pin 2 to pin 3 at the shell and the module loops back through
its own transceiver, whichever way round it is wired. With a USB serial
adapter on the module's TTL header:

```
SERIAL=/dev/cu.usbserial-XXXX make test-wire
```

Clean frames at 9600, 19200 and 38400 prove the chip, the charge pump,
its capacitors, your TTL wiring and the RS-232 levels all at once,
before an ST is anywhere near it.

Keep that short at the D-shell and nowhere near the Pico.
