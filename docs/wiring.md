<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 Neil Rackett -->

# Wiring

The minimum build, and what to add later. Only the MAX3232 is required:
the firmware runs, pairs a controller and sends frames with nothing else
attached.

Read the warnings at the end before powering anything.

## Minimum build

Four wires, if you have a MAX3232 module with the DE-9 already on it.
Modules differ in the order they put the pins, so go by the labels
rather than by position: the one drawn here reads VCC, RXD, TXD, GND
from the end furthest from the DE-9.

```
     MAX3232 module                     Pico W
   (pins as labelled,
    DE-9 to the left)
   ┌──────────────┐               ┌────────────────┐
   │              │               │                │
   │          VCC ├───────────────┤ 3V3   (pin 36) │
   │              │               │                │
   │          RXD ├──────────────►│ GP1   (pin 2)  │   UART0 RX
   │ DE-9         │               │                │
   │ female   TXD │◄──────────────┤ GP0   (pin 1)  │   UART0 TX
   │              │               │                │
   │          GND ├───────────────┤ GND   (pin 38) │
   │              │               │                │
   └──────┬───────┘               └────────────────┘
          │                         USB for power
          │ plugs straight in       and the console
   ┌──────┴──────┐
   │  Modem 1    │   Mega STE: the MFP port, measured
   │  or the     │   ST/STE: the DB25 modem port
   │  modem port │
   └─────────────┘
```

`GP0` is UART0 TX and `GP1` is UART0 RX, which is what `rp/src/config.h`
sets and what the firmware brings up before Bluetooth.

**`RXD` to `GP1` and `TXD` to `GP0`, which is not a crossover.** This
module names its pins for the device you attach: `TXD` is the pin you
feed from your MCU's transmit, `RXD` is the pin that feeds your MCU's
receive. A USB-to-TTL cable usually names them the other way, for
itself, and getting this backwards cost an entire evening: the module
still receives what the ST sends, because that path is separate, so the
link works perfectly in one direction and is silent in the other, which
reads like a broken wire rather than a swapped pair.

**So if one direction works and the other does not, swap these two
before suspecting anything else.** Both ends are 3.3V CMOS, so it costs
nothing but a minute, and `SENDTEST.TOS` will tell you when it is right:
with the two swapped you get bytes sent and none back.

Check the chip is a MAX**3232** and not a MAX232. The MAX232 wants 5V
and will not work from the Pico's 3V3 rail.

### If your MAX3232 is a bare chip

You also need five 100nF ceramic capacitors for the charge pump, and a
DE-9 female connector. The pinout is in
[hardware.md](hardware.md#wiring); the module simply has all of that
fitted already.

## What to put where

`make dist` collects everything that goes onto hardware into `dist/`
and prints, for each file, what it is and where it goes. That list is
kept in the Makefile and nowhere else, because prose copies of it have
already disagreed with it. In short: the UF2 goes on the Pico, the
`.PRG` goes in the ST's `AUTO` folder, and the `.TOS` programs are run
by hand.

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
   │                │                          4 pins? use DIAGONAL ones
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

**Button.** A 6x6mm tactile switch has four legs that are two pairs,
each pair joined inside the switch, so wire two **diagonally opposite**
legs: any other choice risks picking a joined pair, which is a permanent
short and reads as the button held down for ever, wiping your bonds two
seconds after every power-on.

Held for two seconds, it forgets every paired controller.
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
| Mega STE | Modem 1 | DE-9 male |
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
  transmit lands on the ST's transmit and nothing works. With a meter:
  power the module, leave its TTL `TXD` idle, and measure DE-9 pins 2
  and 3 against pin 5. Whichever sits at a solid negative voltage is
  the module's output, and it should be **pin 2**.

  Without a meter, just try it. **Getting this wrong cannot damage
  anything**: RS-232 line drivers are short-circuit protected by design,
  the MAX3232 explicitly so, and transmit meeting transmit gives you
  silence rather than smoke. Start straight through, since a module with
  a female DE-9 is built to plug into a PC's male COM port and so is
  almost certainly DCE already, then run `PIPECHK.TOS` on the ST. Raw
  bytes appearing at all means the orientation, the level shifter and
  the baud are right. Nothing at all, and you cross pins 2 and 3.

## When nothing arrives

Two things to know before suspecting the wiring.

**The adapter is silent with no controller connected.** It sends
nothing at all until a pad is paired and awake, so an idle adapter and
an absent one look identical from the ST. Get the LED solid first.

**A link that works in one direction only is almost always the module's
`TXD`/`RXD` swapped**, not a broken wire: the other path is separate and
keeps working. See the note under the diagram.

Then, in order:

- `node harness/listen.js /dev/cu.usbserial-XXXX` with a USB-to-TTL
  cable on the Pico's GP0, which decodes what the adapter is sending
  before any level shifter or ST is involved. Silence, bytes that do
  not frame, and frames mean three different things.
- `PIPECHK.TOS` on the ST, which says whether bytes reach it and, if
  not, hunts the other serial ports for them.
- `SENDTEST.TOS` on the ST, which goes the other way: the ST sends a
  byte naming each port while the adapter's USB console reports what
  arrived, so between them they name the socket.

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
