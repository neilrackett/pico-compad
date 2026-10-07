# COMpad

<img src="./docs/hero.webp" width="640" alt="Connect modern Bluetooth gamepads to retro computers" />

Connect modern gamepads to retro computers using Bluetooth and RS-232, by [Neil Rackett](https://neilrackett.com)

## Introduction

COMpad lets you play games on your Atari ST with the same wireless controller you use on your console or PC. It's a little adapter you build from a Raspberry Pi Pico W and a couple of pounds' worth of parts, which plugs into your ST's serial ("Modem") port: pair almost any Bluetooth gamepad with it (Xbox One/Series, DualShock, DualSense, Switch Pro, 8BitDo, and more) and your ST sees every button, both analogue sticks and the triggers, with rumble too.

Up to four pads can be connected at once. Once a pad has been paired it reconnects by itself whenever you switch it on, and there's nothing to configure on the ST: just put `COMPAD.PRG` in your `AUTO` folder and it finds the adapter on whichever serial port it's plugged into.

Pads are published using [Xpad](https://downloads.neilrackett.com/atarist-xpad), the open standard for modern gamepads on the Atari ST, so any software written for Xpad can use them directly, and Xpad's `XPADEMU.PRG` turns your pad into a joystick and mouse for everything else.

COMpad is designed to work with any retro computer with a serial port, with the link between the adapter and the computer kept deliberately simple: fixed-length frames of raw controller state over a serial line. Anything with a UART and a few hundred bytes to spare could read it, so any machine can gain support using the appropriate driver (see [What's next?](#whats-next)).

**The first system to be supported is the Atari ST.**

## Making

To build your own COMpad, you will need:

- Raspberry Pi Pico W or Pico 2 W (a plain Pico has no radio, so it can't do Bluetooth)
- MAX3232 RS-232 module with a female DE-9 connector
- DE-9 to DB25 adapter, for anything other than a Mega STE
- USB power supply for the Pico
- Optionally
  - LED with a 330Ω resistor
  - Momentary push button

Check the chip really is a MAX**3232** and not a MAX232: the MAX232 needs 5V and won't work from the Pico's 3.3V supply.

All the information you need for wiring everything together is in [docs/wiring.md](docs/wiring.md).

## Currently supported systems are:

- Atari ST/STE, Mega ST/STE (TT and Falcon should work but currently untested)

If you'd like to add drivers for your favourite retro system, please don't hesitate to send a PR.

## Installation (Atari ST)

1. Download `COMPAD.PRG`, `XPADVIEW.TOS` and the firmware for your Pico, `compad-w.uf2` for a Pico W or `compad-2w.uf2` for a Pico 2 W, from the [latest release page](https://github.com/neilrackett/pico-compad/releases/tag/latest).
2. Hold the BOOTSEL button on your Pico while you plug it into your computer, then copy the firmware onto the drive that appears.
3. Copy `COMPAD.PRG` into the `AUTO` folder of your ST's boot disk.
4. Plug the adapter into your ST, power the Pico from any USB supply, and switch on your ST.

As your ST boots, you'll see `Xpad provider listening on Modem 1.`, or whichever port it found the adapter on, and you're ready to pair a controller.

## Usage

### Pairing a controller

Put your controller into pairing mode and it will connect. There's no pairing button to press: the adapter looks for new pads whenever it has a free slot, and once a pad has paired, it reconnects on its own every time you switch it on.

The Pico's onboard LED, and the external one if you fitted it, shows what the adapter is doing:

| LED         | Meaning                              |
| ----------- | ------------------------------------ |
| Rapid flash | Looking for a new pad                |
| Long blink  | Waiting for a known pad to come back |
| Solid       | A pad is connected                   |

If the external LED comes on solid as soon as the Pico is powered while the onboard one stays dark, the Bluetooth radio has failed to start. That's the main reason to fit the external LED: the onboard one can't light until the radio is running.

To forget every paired pad, for example to move a controller to another machine, hold the button for two seconds. The LED changes from a long blink to a rapid flash to confirm it.

### Checking your controller

Run `XPADVIEW.TOS` to see every button, stick and trigger on your pad live.

| Key     | Does                                    |
| ------- | --------------------------------------- |
| `1`-`4` | Choose which pad is shown               |
| `(`     | Rumble the left motor for half a second |
| `)`     | The same, right motor                   |
| `*`     | The same, both                          |
| `Q`     | Quit, and so does Escape                |

The three rumble keys are the top row of the numeric keypad.

### Playing games

Software written for Xpad reads your pads directly, analogue sticks and all.

For everything else, download `XPADEMU.PRG` from [Xpad's releases page](https://github.com/neilrackett/atarist-xpad/releases/tag/latest) and copy it into your `AUTO` folder **after** `COMPAD.PRG`, since it needs COMpad to be running first. Your first pad then works as a joystick in port 1, and its right stick as the mouse, with your real joystick and mouse still working alongside. Its settings, including which buttons fire, are described in the [Xpad README](https://github.com/neilrackett/atarist-xpad).

## How it works

```
Gamepad ──Bluetooth──► Pico W ──UART──► MAX3232 ──RS-232──► ST serial port
                      Bluepad32         level shifter              │
                                                                   ▼
Your game ◄──── XPAD block, in the cookie jar ◄──── COMPAD.PRG ◄───┘
```

The Pico runs [Bluepad32](https://github.com/ricardoquesada/bluepad32), which takes care of Bluetooth and the differences between controllers, and sends the full state of each pad down the serial line as short fixed-length frames. Every frame carries the whole state rather than just what changed, so a frame damaged in transit is simply dropped and the next one puts everything right, with no handshaking to go wrong.

On the ST, `COMPAD.PRG` stays resident and runs from the system timer, decoding frames from the serial port and publishing them as an Xpad block in the cookie jar, which is where games find it. Rumble travels the other way: a game asks Xpad for it, and COMpad sends a short frame back to the adapter, which drives the pad's motors.

When it installs, `COMPAD.PRG` pings each serial port in turn, the modem port first, and keeps the one that answers. If nothing answers, because the adapter is unplugged or not switched on yet, it falls back to the modem port, so switching the adapter on later still works.

The wire protocol is described in [docs/protocol.md](docs/protocol.md), and the reasons it looks the way it does are in [docs/design.md](docs/design.md).

## If nothing happens

- **The adapter sends nothing until a pad is connected**, so an idle adapter and an unplugged one look the same from the ST. Get the LED solid before looking anywhere else.
- **If one direction works but not the other**, the module's `TXD` and `RXD` are almost certainly swapped.
- The [releases page](https://github.com/neilrackett/pico-compad/releases/tag/latest) also has `PIPECHK.TOS`, which shows the raw bytes arriving at your ST and looks for them on the other serial ports, and `SENDTEST.TOS`, which checks the other direction.

The [wiring guide](docs/wiring.md#when-nothing-arrives) goes through it step by step.

## Known limitations

- Rumble only works through the modem port, which is Modem 1 on a Mega STE. If your adapter is on one of the Mega STE's other serial ports, COMpad will still find it, but rumble won't be available.
- Finding the adapter means trying each port in turn, and TOS has no way to read back a port's baud rate. So if your adapter isn't on the modem port, any port tried before it is left at 19200 baud with no handshaking, which matters only if you have a modem or serial printer on it.
- Only one Xpad provider can be installed at a time, so COMpad and [MD/Sidepad](https://github.com/neilrackett/md-sidepad) are alternatives rather than something to run together. Whichever is installed first is the one you get, and the other says so and steps aside.
- Software that starts before the `AUTO` folder runs, such as apps that boot from a cartridge, won't see COMpad, because it isn't running yet.
- `XPADEMU.PRG` reaches games through the system joystick and mouse handlers, so games that read the keyboard processor directly, which includes many original floppy games, won't see your pad as a joystick. Software written for Xpad is unaffected.
- Development has focussed on Xbox One gamepads, so if you have a different controller, please let us know how you get on.

## What's next?

The wire protocol was designed so that any machine with a serial port and a few hundred bytes of code can read the same frames, which means an Amiga, an X68000 or anything else with a UART could gain support with nothing more than a small provider of its own. None of those exist yet, but if you'd like to add your favourite retro platform, please don't hesitate to submit a PR: [docs/design.md](docs/design.md) explains how the pieces fit together.

Think you can help? Got an idea of your own? We'd love to hear from you, so why not let me know on [X](https://x.com/neilrackett) or submit a PR.

## Building

Clone with `--recursive`, or run `git submodule update --init --recursive` before your first build: the Pico SDK, Bluepad32 and Xpad all come from submodules in `lib/`.

```bash
# The adapter firmware: needs CMake and arm-none-eabi-gcc
make firmware

# The ST programs: needs atarist-toolkit-docker
STCMD_NO_TTY=1 stcmd make st

# Collect everything that goes onto hardware into dist/
make dist
```

`make dist` prints each file it collects, along with what it's for and where it goes.

`make firmware` builds for both boards: `rp/build/compad-w.uf2` for the Pico W and `rp/build-2w/compad-2w.uf2` for the Pico 2 W.

The ST programs are built with [atarist-toolkit-docker](https://github.com/sidecartridge/atarist-toolkit-docker), which provides `stcmd`.

### Layout

| Path              | Contents                                                                |
| ----------------- | ----------------------------------------------------------------------- |
| `rp/`             | Pico W adapter firmware: Bluepad32 in, COMpad frames out                |
| `target/atarist/` | `COMPAD.PRG`, the wire protocol decoder, and the ST tools               |
| `docs/`           | Wiring, hardware, protocol and design notes                             |
| `lib/`            | Submodules: `xpad` at v1.1.6, `pico-sdk`, `pico-extras` and `bluepad32` |

Working practices for contributors are in [AGENTS.md](AGENTS.md).

## License

Source code is licensed under the GNU General Public License v3.0 or later. See [LICENSE](LICENSE) for the full text.

COMpad is a standalone program at both ends of the wire, not a library games link against: software talks to it through [Xpad](https://github.com/neilrackett/atarist-xpad), included as the `lib/xpad` submodule, which is BSD-2-Clause. So a game under any licence can read a COMpad pad without inheriting anything from here.

Copyright (c) 2026 Neil Rackett.
