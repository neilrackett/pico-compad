<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 Neil Rackett -->

# Roadmap

Phases 0 to 2 pass under emulation, though phase 2 has only been driven
by a simulated pad; 3 and 4 are not started. See AGENTS.md for what
that means in practice.

Every phase here targets the Atari ST family. Other platforms are not
on this roadmap at all: if one happens, it arrives as a new directory
under `target/` reusing the decoder, with a bring-up of its own.

## Phase 0: prove the pipe

Before any interesting code exists.

- Node writes one fixed dummy state frame on a loop into a FIFO.
- Hatari reads it: `--rs232-in <fifo>`.
- ST-side throwaway program uses TOS `Rsconf` at 9600 and prints raw
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
- `Rsconf` at 9600 for bring-up, handshake mode 0.
- Publish as `"COMpad 0.1"`.
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
  a human has to hold the pad.

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

## Phase 3: hardware

Pico W or Pico 2 W, Bluepad32 over BTstack for the Bluetooth HID
side. Wiring, connectors and the move onto a private MFP handler
are in [hardware.md](hardware.md).

## Phase 4: rumble

Request frames from ST to adapter. Consumer writes `req->rumble[0][0]`
and `[0][1]`, bumps `seq`, provider notices and sends a type `0xE`
frame. Claim `XPAD_CAP_RUMBLE` only once this actually works.
