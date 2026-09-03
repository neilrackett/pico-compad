<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 Neil Rackett -->

# Design notes

Why the transport looks the way it does, and what must not be
quietly undone.

## Why serial rather than MIDI

MIDI was the first candidate and it works, but serial wins on balance:

- **8-bit clean.** MIDI data bytes are 7-bit, so signed axes would need
  packing or scaling. Serial carries `XPAD_PAD` values unchanged.
- **Full duplex for free.** One MAX3232 and one cable gives both
  directions, so the rumble back channel needs no extra hardware. MIDI
  would need a second circuit and socket.
- **Portable.** RS-232 is close to a universal retro interconnect.
  MIDI-on-a-gamepad is an Atari-shaped idea.

MIDI's advantages are opto-isolation and a tidier connector. Worth
revisiting only if ground loops become a real problem.

## Layering

The ST family is the platform being built for; the layering exists so
that a second platform is cheap when one arrives, not because one is
planned. Three pieces, kept separate so only the middle one is ever
rewritten per platform:

1. **Wire protocol** plus a reference decoder in portable C, no platform
   headers. Host-testable.
2. **Per-platform UART glue.** Init, baud, and getting at received
   bytes. On the ST today that is TOS: `Rsconf` for the line settings
   and draining the AUX iorec ring from `etv_timer`, which is what
   `compad.c` does. Taking MFP 68901 channel 12 directly, roughly 60
   lines, is phase 3 work and is not written; see hardware.md.
3. **Provider binding.** On the ST, `xpad_provider.c` publishing the
   cookie jar block. Unchanged whatever the transport.

The Pico firmware sits outside all three and never changes per
platform. Only the ST instantiation of layers 2 and 3 exists; layer 1
is the part a future platform takes verbatim.

## Constraints worth remembering

- **xpad is single provider.** Both example drivers refuse to install
  when an `XPAD` cookie is already present. Installing COMpad therefore
  takes MD/Sidepad's joyvec route off the table for that session, making
  AUTO folder ordering a real decision rather than something that sorts
  itself out.
- **`XPAD_PAD` is 12 bytes** and `buttons` is 32-bit with 17 defined.
  Bit values are frozen; new buttons go at bit 17 and up.
- **Always chain to any handler displaced.** One that swallows events
  breaks the desktop and every existing program.
- **Drop the IPL to 5 before doing real work in `etv_timer`.** Timer C
  arrives at level 6 with the keyboard ACIA masked, and the ACIA buffers
  one byte. A handler that holds level 6 too long loses IKBD bytes, and
  a lost byte desyncs the packet stream: mouse deltas replay as phantom
  keyclicks and the pointer crawls into the top-left corner. `etv.s`
  does the drop, with a `tas` guard against re-entry; the symptom only
  appears when someone moves a real mouse, so no headless test catches
  it going missing.
- **Reach system variables below `$800` through `Supexec`.**
- **TOS's IOREC uses queue names, not pointer names.** `ibufhd` is the
  head of the queue, the next byte to come out, and the reader advances
  it; `ibuftl` is the tail, where the RX interrupt appends. Reading
  them as "head = where TOS writes" gets the ring exactly backwards:
  the drain walks 244 stale bytes the wrong way round, never advances
  the read index, and clobbers TOS's write index. It still looks
  correct against a sender that never changes its value, which is what
  `make test-live` exists to catch. Use `_IOREC` from
  `<mint/ostruct.h>` rather than declaring the struct: it marks the two
  fields the RX interrupt writes `volatile`.
- **Do not claim capabilities not honoured.** `caps` is a promise.

## Open questions

- Whether the adapter should also expose a raw 3.3V TTL header
  alongside the RS-232 connector. Costs nothing, and the Spectrum Next,
  MSX and C64 user port all want TTL directly.
- Whether a future all-in-one board should host Bluetooth and populate
  the block locally, when the consumer runs on the RP2040 too, rather
  than routing out over serial and back. Probably yes, and it does not
  change this design: it is a different provider implementing the same
  interface.
- Whether descriptor frames should carry a name string for
  `XPAD.provider`, or whether the ST side should synthesise one.
