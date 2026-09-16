# COMpad

Connect modern gamepads to retro computers using RS-232, by [Neil Rackett](https://neilrackett.com)

## Introduction

COMpad enables you to connect Bluetooth gamepads to retro computers using RS-232, via a Raspberry Pi Pico W or Pico 2 W microcontroller connected to the serial/modem/COM port.

The transport is deliberately simple, using fixed-length frames of raw controller state over a UART. That is what makes it portable.

The current version is built for the Atari ST family: the adapter connects to the modem/serial port and a resident provider publishes every connected gamepad as an `XPAD` block in the cookie jar, using the interface set out in the [Xpad](https://github.com/neilrackett/atarist-xpad) library. A modern gamepad on one end, an `XPAD` block on the other, RS-232 in between.

The wire protocol, though, is deliberately platform-neutral: any machine with a serial port and a few hundred bytes of code could consume the same frames, so an Amiga, X68000 or anything else with a UART can gain a target of its own later, each supplying its own thin provider. None of those exist yet, but if you'd like to add something for your favourite retro platform, please don't hesitate to submit a PR: see [docs/design.md](docs/design.md) for more information.

## Status

Phases 0 to 2 pass under emulation. A browser keyboard, a simulated
gamepad, or **a real Bluetooth controller** driven through
[@mesmotronic/xpad](https://www.npmjs.com/package/@mesmotronic/xpad)
reaches an `XPAD` block on the ST, and xpad's own viewer reads it back
through the cookie jar with axes, triggers and pad type intact.

Phase 3, the Pico W firmware, is now **written but unproven**. It builds
with `make firmware`, and `test/encode_test.c` pushes every frame it can
emit through the ST's own decoder on the host, so the two halves are
known to agree. No Pico has been flashed and no adapter has been built,
though, so nothing has reached a real ST: see
[docs/roadmap.md](docs/roadmap.md). The wire itself has been measured on
real hardware through `make test-wire`, which is still the only thing
here that has.

To build one, see [docs/wiring.md](docs/wiring.md). A MAX3232 is the
only part you need; the status LED and the forget button are optional
and need no build flag to leave out.

```
make                 the default goal, same as test-host
make test-host       host tests: decoder, encoder, mapping, harness socket
make test-decode     compad.c's own assertions, run on the ST
make test-serial     bytes cross the emulated link and frame up
make test-provider   a resident provider publishes a pad, and another
                     program reads it back through the cookie jar
make test-live       the provider follows a sender that changes
make test-gamepad    a simulated controller: axes, triggers, pad type
make test            all six, in order
make test-wire       a real UART, loopback: needs hardware
make simulator       drive it yourself, in a window
make firmware        build the Pico W adapter firmware
make dist            collect what you flash or install into dist/
make tos             fetch or locate EmuTOS, and print where it landed
make clean           both halves

STCMD_NO_TTY=1 stcmd make st      build the ST binaries
```

`test-gamepad` drives the real [@mesmotronic/xpad](https://www.npmjs.com/package/@mesmotronic/xpad)
library from a synthetic gamepad, so it covers what a keyboard cannot:
signed axes, analogue triggers, and the positional face-button mapping.
It needs `npm install` once.

`st` needs the [atarist-toolkit-docker](https://github.com/sidecartridge/atarist-toolkit-docker)
container; the test targets need Hatari and Node on the host, so the
two never run under the same command.

A TOS image is fetched on first use, so there is no setup step: EmuTOS
lands in `build/tos/` and is reused after that. Set `$TOS` to point at
a ROM of your own, which is the only way to test anything that depends
on a real TOS version, since EmuTOS always reports itself as 2.06.

To drive it by hand, run `make simulator`, then open
<http://localhost:8232> for the keyboard or
<http://localhost:8232/pad.html> for a real controller, and watch the
bits move in `XPADVIEW.TOS`.

## Layout

| Path              | Contents                                                           |
| ----------------- | ------------------------------------------------------------------ |
| `rp/`             | Pico W adapter firmware: Bluepad32 in, COMpad frames out           |
| `target/atarist/` | ST provider, the wire-protocol decoder, the test programs, and `XPADVIEW.TOS` built from the submodule |
| `harness/`        | dev rig: frame writer, server, keyboard and gamepad pages, and `wire.js`, the loopback measurement |
| `test/`           | host tests for the decoder, Hatari end-to-end runners              |
| `docs/`           | protocol, hardware, roadmap, design notes                          |
| `dist/`           | `make dist` output: the UF2 to flash and the two ST programs       |
| `lib/`            | submodules: `xpad` at v1.1.0, plus `pico-sdk`, `pico-extras` and `bluepad32` |

Two different things are called xpad, and it is worth keeping them
apart. [atarist-xpad](https://github.com/neilrackett/atarist-xpad) is
the C library in `lib/xpad`, BSD-2-Clause, and it defines the `XPAD`
block this publishes. [@mesmotronic/xpad](https://www.npmjs.com/package/@mesmotronic/xpad)
is an unrelated npm package that reads browser gamepads, used only by
the development harness and never by anything that ships.

## Documentation

| Document                             | Contents                                                  |
| ------------------------------------ | --------------------------------------------------------- |
| [docs/protocol.md](docs/protocol.md) | the wire contract: frame types, layouts, timing budget    |
| [docs/wiring.md](docs/wiring.md)     | how to build one: four wires minimum, extras marked optional |
| [docs/hardware.md](docs/hardware.md) | building the adapter, connectors, which socket is the MFP |
| [docs/roadmap.md](docs/roadmap.md)   | phases 0 to 4, what each one proves                       |
| [docs/design.md](docs/design.md)     | why serial rather than MIDI, layering, constraints        |

Working practices for contributors and agents are in
[AGENTS.md](AGENTS.md).

## Licence

GPL-3.0-or-later. COMpad is a standalone program on both ends of the
wire, not a library games link against: consumers talk to it through
[xpad](https://github.com/neilrackett/atarist-xpad), which stays
BSD-2-Clause, so a game of any licence can read a COMpad-published pad
without inheriting anything from here.

Copyright (c) 2026 Neil Rackett.
