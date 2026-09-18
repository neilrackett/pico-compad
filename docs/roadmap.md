<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 Neil Rackett -->

# Roadmap

Phases 0 to 2 pass under emulation, phase 2 in both halves: a simulated
pad under `make test-gamepad`, and a real Bluetooth controller by hand
through `harness/pad.html`. The wire itself has been measured on real
hardware (`make test-wire`).

**Phase 3 is done, on hardware.** A Bluetooth controller paired to a
Pico W reaches an `XPAD` block on a real Mega STE over a real wire, and
`XPADVIEW.TOS` shows it moving. That is the exit criteria met, and it is
the first thing in this project to have run anywhere but an emulator.

**Phase 4 is written but unproven on hardware.** Rumble and the ping
that finds which serial port the adapter is on, both built on the
host-to-adapter direction that nothing implemented before. It passes
under emulation; no pad has buzzed yet.

Every phase here targets the Atari ST family. Other platforms are not
on this roadmap at all: if one happens, it arrives as a new directory
under `target/` reusing the decoder, with a bring-up of its own.

## Phase 0: prove the pipe

Before any interesting code exists.

- Node writes one fixed dummy state frame on a loop into a FIFO.
- Hatari reads it: `--rs232-in <fifo>`.
- ST-side throwaway program uses TOS `Rsconf` at 19200 and prints raw
  bytes.

If those bytes appear, wiring of the emulated path, baud and framing are
all proven and everything after this is software.

**Gotchas:**

- Opening a FIFO for writing blocks until a reader attaches. Start
  Hatari first, or have the harness retry the open.
- Hatari's **Patch Timer-D** option must stay **on** (its default).
  Timer D is the RS-232 baud generator, and with the patch off,
  reception under EmuTOS stalls after a single byte: measured, not
  theorised. Since Hatari's serial is not bit-accurate anyway, the
  patch costs nothing that was ever there to lose.
- Hatari needs `--rs232-out` as well, even for receive-only work: it
  defaults to `/dev/modem`, absent on modern systems, and one failed
  open disables RS-232 entirely, silently but for a line on stderr.
- Hatari's serial is byte-level plumbing, not bit-accurate. Framing,
  sync recovery and checksums are all testable. Throughput and latency
  numbers are meaningless until real hardware.

## Phase 1: keyboard harness

The first usable prototype. No gamepad, no Pico, no soldering.

**Host side:** a Node server that serves a single HTML page and holds a
WebSocket open. The page captures `keydown` and `keyup`, maintains held
state, and posts it up. Node encodes COMpad frames and writes them to
the Hatari FIFO at 50 Hz.

Browser rather than raw terminal stdin, because a terminal in raw mode
delivers key presses but not releases. Held state is exactly what a
gamepad harness needs, and faking it from key repeat feels wrong to use.

Initial mapping:

| Key | xpad bit |
|-----|----------|
| Arrows | `XPAD_UP` / `DOWN` / `LEFT` / `RIGHT` |
| Z | `XPAD_SOUTH` |
| X | `XPAD_EAST` |
| A | `XPAD_WEST` |
| S | `XPAD_NORTH` |
| Q / W | `XPAD_TL` / `XPAD_TR` |
| Enter | `XPAD_START` |
| Shift | `XPAD_SELECT` |

Axes send zero throughout. `XPAD_CAP_ANALOG` is not claimed, so nothing
downstream is lied to.

**ST side:** the real provider, built from `xpad_provider.c`.

- Frame decoder in its own file with no TOS headers, so it can be unit
  tested on the host. This is where the logic worth getting wrong lives.
- `Rsconf` at 19200, its fastest, handshake mode 0.
- Publish as `"COMpad <version>"`, from version.txt.
- Verify with `xpadview` before anything else consumes it.

**Exit criteria:** press a key in the browser, watch the corresponding
bit light up in `xpadview` running under Hatari.

## Phase 2: real gamepad

Change one thing: the input source. `navigator.getGamepads()` gives
standard mapping, so axes and buttons arrive already normalised, and
everything downstream is untouched.

Done, in two halves:

- **Simulated** (`make test-gamepad`). A synthetic Gamepad is installed where
  the Gamepad API would be and the real @mesmotronic/xpad package reads
  it, so the library is exercised rather than imitated. Asserts the
  positional button mapping, signed axes, analogue triggers, the caps
  word and the pad type, all read back through xpad's viewer. This is
  the first test to cover anything but buttons.
- **Real** (`harness/pad.html`, via `make simulator`). The same mapping
  module, the same socket, a controller in your hands. Not automated:
  a human has to hold the pad. Confirmed working with a real pad.

**Do not hardcode a gamepad index.** `navigator.getGamepads()` returns
a sparse array whose slots are assigned at connect time and never
compacted, so index 0 is routinely `null` while the only pad plugged in
sits at 1. Reconnect a pad, re-pair over Bluetooth, or run anything
that presents a virtual controller, and you are past 0. @mesmotronic/xpad
1.3.0 handles it: constructed with no index it binds to the first
connected pad, reports `-1` when there is none, and rebinds when one
arrives in a different slot. `pad.html` relies on that. `padsim.js`
still passes an explicit 0, deliberately, because it installs its own
synthetic pad there and must read that one rather than whatever is
plugged into the developer's machine.

Both share `harness/xpadmap.js`, so the thing a person tests and the
thing the suite tests cannot drift apart.

`@kmamal/sdl` is the alternative if the browser proves awkward for a
particular pad. Prebuilt binaries for Apple Silicon and Intel, so no
node-gyp. On macOS, if `sdl.controller.devices` comes back empty with a
pad plainly connected, grant Input Monitoring to the terminal app in
System Settings.

**The button naming trap.** Three conventions disagree:

- Linux kernel, which xpad follows: `BTN_X` is **north**, `BTN_Y` is
  **west**.
- W3C standard mapping: index 2 is **west**, index 3 is **north**.
- SDL, following the Xbox legend.

Map through positional names explicitly and never let a letter cross a
boundary. Otherwise the harness grows a bug that does not exist in the
real thing.

This is not hypothetical: @mesmotronic/xpad names its face buttons
after the Xbox legend, so its `X` is W3C index 2, the left one, which
atarist-xpad calls `XPAD_WEST` and aliases `XPAD_Y`. The letters cross
over while the positions line up. `harness/xpadmap.js` maps by
position and `test/xpadmap_test.js` fails if anyone changes it to map
by letter.

## Measuring the wire itself

Everything above runs through Hatari, whose serial is byte-level
plumbing rather than a bit-accurate UART. Framing, sync recovery and
checksums are testable there; throughput and latency are not.

`make test-wire` is the exception and the only test in the project that
touches physical hardware. Short TX to RX on a USB serial adapter and it
pushes real frames through a real UART at 9600, 19200 and 38400,
reporting achieved throughput, round-trip latency and whether every byte
survived. No ST, no Pico, no level shifter: both ends of a loopback are
the same 3.3V pin.

It is deliberately not part of `make test`, since it fails on any
machine without an adapter plugged in and a wire bridged.

Two things measured while building it, both worth knowing before they
look like bugs:

- **A USB serial port with nothing attached may never drain.** A
  blocking write of 2400 bytes to a Raspberry Pi Debug Probe never
  returned. The test writes non-blocking and probes with a single frame
  first, so a missing loopback is reported in under a second rather
  than wedging.
- **Opening one can be slow.** That same Debug Probe takes about 40
  seconds to open and close on macOS with an idle UART, on both the
  `tty.` and `cu.` nodes. A CH340G is usually instant. The test says it
  is opening before it does, so the wait does not look like a hang.

## Phase 3: hardware

Pico W or Pico 2 W, Bluepad32 over BTstack for the Bluetooth HID
side. Wiring, connectors and the move onto a private MFP handler
are in [hardware.md](hardware.md), and the build itself is in
[wiring.md](wiring.md).

The firmware lives in `rp/`. `rp/src/encode.h` holds everything with
logic worth getting wrong, free of both the Pico SDK and Bluepad32 so
the host build tests it, and `test/encode_test.c` feeds every frame it
can emit through the ST's own decoder so the two halves cannot drift
apart. `rp/src/compad_platform.c` is the parts that need a Pico.

**Exit criteria: met.** A controller paired to a real Pico W moves a bit
in `XPADVIEW.TOS` on a real Mega STE, through a MAX3232 into Modem 1.

What the bring-up actually cost, since none of it was the code: the
adapter is silent with no pad connected, so an idle adapter and an
absent one look identical; the Mega STE's MFP port is Modem 1, not
Serial 2 as hardware.md claimed; and the MAX3232 module names its TTL
pins for the device you attach rather than for itself, so `TXD` takes
the Pico's transmit. That last one is the nastiest, because the wrong
way round still works in one direction and reads like a broken wire.

**Pairing has no pairing mode, deliberately.** Scan while a pad slot is
empty, stop when they are full, and always allow incoming connections.
A new pad connects the first time it is put into pairing mode, and
after that it reconnects on its own because it is bonded, so the
ordinary case needs no interface at all. This follows Bluepad32 rather
than fighting it: `uni_bt_enable_new_connections_safe()` is deprecated
in 4.2.0 in favour of `uni_bt_start_scanning_and_autoconnect_safe()`
and `uni_bt_stop_scanning_safe()`, and bonded devices reconnect even
while scanning is off.

The one thing that cannot be expressed without hardware is forgetting a
bond, because the pad you would press buttons on is the one that is not
talking to you. That is a long press on a button to
`uni_bt_del_keys_safe()`. A button with no feedback cannot be told from
a crash, so the CYW43 onboard LED carries the state, following what an
Xbox controller does so that it needs no explaining:

| LED | State |
|-----|-------|
| Rapid flash | Looking for a new pad |
| Slow blink | Waiting for a known pad to come back |
| Solid | Connected |

Those three come from one fact rather than a mode the firmware has to
track. A slot is free either way, so the adapter is always scanning and
always accepting a bonded pad; what differs is whether any bond exists.
No stored keys means nothing can reconnect, so it is a new pad or
nothing: rapid flash. Keys stored means the pad is most likely just out
of range or asleep: slow pulse. The button's long press wipes the keys
and therefore moves the LED from pulse to flash by itself, which is the
confirmation that it worked, so no separate acknowledgement blink is
needed.

All three are plain on/off timings, not brightness fades. The rates
only have to be obviously different from each other: rapid while
looking, long while waiting, steady once connected. So no PWM is
wanted anywhere, which is just as well, because the CYW43 LED hangs
off the wireless chip rather than an RP2040 pin and cannot do it.

Pairing does **not** go through ST software. The adapter has to be
pairable before any ST software is installed and on a machine where the
AUTO folder driver did not load, which is when a pad is most needed; a
utility cannot open AUX behind the resident provider that owns it; and
xpad's `XPAD_REQ` is the wrong home, frozen at v1.0.0 with rumble and
LED only, for what is link management rather than pad state. The type
`0xE` request frame keeps a spare byte at offset 6 if a remote trigger
is ever wanted, as an addition rather than the mechanism.

## Phase 4: rumble, and finding the port

Both halves of the host-to-adapter direction, which nothing implemented
before: the firmware never read a byte and the provider never sent one.

**Rumble.** A consumer writes `req->rumble[n][0]` and `[1]`, bumps
`seq`, the provider notices and sends a type `0xE` frame, and the
adapter drives the motors. `XPAD_CAP_RUMBLE` is claimed only while a
pad that can actually rumble is connected, and the provider masks it
out entirely when it cannot transmit.

The provider sends from the `etv_timer` handler, so it writes the MFP's
data register directly rather than calling `Bconout`: the BIOS is not
reentrant and this file's design is that nothing calls it from an
interrupt. One byte per tick when the transmitter is free, which clears
an eight byte frame in 40 ms. That path is MFP only, which is why a
provider on one of a Mega STE's SCC ports stops claiming rumble.

**Finding the port.** `COMPAD.PRG` pings each serial port at install,
MFP first, and keeps the one that answers. Nothing answers, or the
machine has no `Bconmap`, and it falls back to the MFP, so forgetting
to plug the adapter in or switching it on later still works. The
install message says which port and whether that was by answer or by
default.

Probing is not read-only, so the settings of any port that does not
answer are put back, and stopping at the first answer means the common
case never touches the others at all.
