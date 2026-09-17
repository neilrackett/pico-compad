<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 Neil Rackett -->

# COMpad wire protocol

The contract between an adapter and a provider. Frame layouts here
are what `target/atarist/src/protocol.h` implements and what
`test/protocol_test.c` pins; change them together or not at all.

The ST family is the only consumer implemented. The protocol is still
specified as if the weakest plausible retro machine were listening,
because it is the one layer a future platform target inherits
unchanged, and widening a frozen wire format later is far harder than
starting dull.

## Principles

- **Full state every frame, never deltas.** Self-healing: a corrupt
  frame is dropped and correct state arrives on the next one, with no
  resync handshake to implement or debug.
- **Fixed length per frame type**, derivable from the header, so no byte
  stuffing and no length field.
- **Byte-wide everything**, so endianness never comes up, whether the
  consumer is today's 68000 or some future 6502.
- **Raw values.** Deadzone and d-pad folding are the provider's job:
  `xpad_fold_stick()` already does it. The adapter stays dumb.

## Header byte

```
bits 7-6  protocol version (currently 0)
bits 5-4  pad index (0-3, matching XPAD_MAX_PADS)
bits 3-0  frame type
```

Frame types:

| Value | Name    | Length | Use |
|-------|---------|--------|-----|
| `0x0` | State   | 12     | Normal per-pad state |
| `0x1` | Compact | 5      | Buttons only, for slow links |
| `0xE` | Request | 8      | Host to adapter: rumble, LED |
| `0xF` | Descriptor | 7   | Pad type, flags, caps on attach |

## State frame (type `0x0`)

```
0   sync        0xA5
1   header
2   buttons     bits 0-7
3   buttons     bits 8-15
4   buttons     bits 16-23
5   lx          int8, -127..127
6   ly          int8, -127..127
7   rx          int8, -127..127
8   ry          int8, -127..127
9   lt          uint8, 0..255
10  rt          uint8, 0..255
11  checksum    XOR of bytes 0..10
```

Three button bytes cover the 17 bits xpad currently defines with room to
reach bit 23 before the frame has to grow. Axis and trigger conventions
match xpad exactly: signed and screen oriented for axes (`+x` right,
`+y` down), unsigned for triggers.

XOR rather than CRC-8. One instruction per byte would keep even a
1 MHz 6502 happy, and a false sync self-corrects within a frame or two.

## Compact frame (type `0x1`)

```
0   sync        0xA5
1   header
2   buttons     bits 0-7
3   buttons     bits 8-15
4   checksum    XOR of bytes 0..3
```

For future consumers whose bit-banged UARTs cap out around 2400 baud.
Digital directions and face buttons only. Nothing on the ST needs it,
but it costs one switch case to keep in the decoder.

## Descriptor frame (type `0xF`)

```
0   sync        0xA5
1   header
2   type        XPAD_TYPE_*
3   flags       XPAD_PAD_*
4   caps        high byte
5   caps        low byte
6   checksum    XOR of bytes 0..5
```

Sent on pad attach and on any change, and then **repeated**, five
times a second in the harness. A consumer discards whatever is in its
serial buffer when it installs, so a descriptor sent while the machine
was still booting is simply gone, and an adapter has no way to know
when the machine came up or whether it has been reset since. Repeating
is 7 bytes at a time, under 4% of a 9600 link, though worth noting it
is then the larger half of what flows while nobody is touching the pad,
since state frames stop when nothing changes. It keeps the descriptor
as stateless as the state frames. A consumer asking for one
over the back channel would be the tidier answer and waits on phase 4.

**`caps` is adapter-scoped, not per-pad.** The descriptor's header
carries a pad index because `type` and `flags` are per-pad, but `caps`
describes what the adapter as a whole honours, and xpad has one `caps`
word per block to match. A receiver takes the newest value rather than
combining them, so an adapter must send the same `caps` in every pad's
descriptor. Per-pad capability belongs in `flags`, which xpad already
scopes per pad.

Until a descriptor arrives, a consumer that has seen state frames
should report a generic gamepad rather than nothing: `compad.c` does this, and it is
why a missing descriptor shows up as the wrong pad type rather than an
absent pad.

## Request frame (type `0xE`, host to adapter)

```
0   sync        0x5A    (distinct direction, distinct sync)
1   header
2   rumble low frequency motor
3   rumble high frequency motor
4   duration    units of 10ms, 0 = until replaced
5   led         index or 0
6   reserved    0
7   checksum    XOR of bytes 0..6
```

Xbox trigger impulse motors have no home in xpad v1, so only the two
main motors are driven.

## Timing budget

| Link | Bytes/sec | 12-byte frame | 1 pad @ 50Hz |
|------|-----------|---------------|--------------|
| 2400 | 240 | 50 ms | not viable, use compact |
| 9600 | 960 | 12.5 ms | 63% |
| 19200 | 1920 | 6.3 ms | 31% |
| 38400 | 3840 | 3.1 ms | 16% |

Four pads at 50 Hz needs 2400 bytes/sec, which does not fit at 19200.

**Send on change, plus a keepalive every 250 ms.** Still a full state
frame, so the self-healing property holds. Gamepads are idle most of
the time and four players rarely all move on the same frame.

This is not an optimisation to reach for later, it is the difference
between a link with headroom and one without. One pad streaming
unconditionally at 50 Hz is 600 bytes/sec of the 960 a 9600 link
carries: 63%, with a pad that is doing nothing. At that load any
moment the receiver runs slower than the sender, the shortfall queues,
and a queue on a link this slow is latency that never comes back.
Measured at the ST, the harness went from 710 bytes/sec to about 107 by
sending only what changed, and a press still goes out on the very next
tick. The steady-state floor is roughly 85: 50 for the keepalive and 35
for the descriptor repeat below. The browser and gamepad senders both
do this; `harness/frames.js` deliberately does not, because the phase 0
pipe check wants an unconditional stream. An adapter should.

**19200 is the default, and it is TOS's ceiling.** It is one constant,
`COMPAD_BAUD` in `protocol.h`, which both the firmware and the ST
programs compile: the firmware uses it directly and `stport.h` derives
the `Rsconf` speed code from it, so the two ends cannot disagree. To
drop to 9600 change that one line.

38400 is the one that needs more than a constant. It is not in
`Rsconf`'s table at all, so it means driving Timer D directly (prescale
/4, count 1: 2457600 / 4 / 16 = 38400 exactly) from a private MFP
handler. That is real work, and the timing table above says what it
buys: four pads at 50 Hz need 2400 bytes/sec, which fits at 38400 and
does not at 19200.
