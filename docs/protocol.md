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
| `0x1` | Compact | 5      | Buttons only, for an over-budget tick or a slow link |
| `0x2` | Ping    | 3      | Host to adapter: are you there? |
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

Digital directions and face buttons only, and the receiver leaves its
copy of the axes alone. Sent when a tick is over budget, which is what
lets four pads pressing buttons all be served: see "When it does not
fit". Also what a future consumer whose bit-banged UART caps out around
2400 baud would use for everything.

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
as stateless as the state frames. A consumer can also ask for one: that
is what a ping is, below.

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

**A pad leaving is a descriptor too**, carrying `XPAD_TYPE_NONE` and
zero flags for that slot. Without one, a slot keeps the type of
whatever last occupied it for the rest of the session and
`xpad_connected()` goes on counting a pad that walked out of the room.
It is repeated like any other descriptor, because one frame is a single
point of failure on a link that can lose bytes, but a bounded number of
times: an empty slot has nothing further to say, so the notices stop
after about two seconds.

**`flags` carries the battery.** `XPAD_PAD_LOWBATT` is set at a fifth
of the range or below, and a pad that does not report a battery at all
is not low. That distinction matters more than the threshold does: a
flag that lights for every pad that simply has no sensor is one people
learn to ignore.

An adapter that announces arrivals and departures this way may claim
`XPAD_CAP_HOTPLUG`, which says pad type changes at runtime and a
consumer should re-read it rather than cache it at startup.

## Ping frame (type `0x2`, host to adapter)

```
0   sync        0x5A
1   header
2   checksum    XOR of bytes 0..1
```

"Is a COMpad on this port?" The adapter answers with a **descriptor**
frame per connected pad, or a single descriptor for pad 0 with
`XPAD_TYPE_NONE` when nothing is paired. No reply type of its own: the
host already decodes descriptors, and they carry the pad type and caps
it wants anyway. A reply has to pass its checksum, so noise cannot
fake one.

It answers **whether or not a pad is connected**, which is the whole
point. An adapter with nothing paired sends nothing at all, so passive
listening cannot tell it from an absent one. That is what makes a ping
usable for finding which serial port the adapter is on: three ports at
a few hundred milliseconds each, against seconds of listening that
still cannot distinguish silence from absence.

A host that pings is transmitting onto a port it has not identified
yet, which is not a read-only act: it must claim the port and set its
line rate, and something else may be using it. Probe the recommended
port first and stop on the first answer, and put the line settings back
on any port that does not reply.

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

**What the reference adapter honours today**: the two rumble bytes, and
nothing else. `duration` is treated as 0 whatever it holds, because
xpad's request area carries magnitudes and no duration, so a consumer
has no way to ask for anything else. `led` is read off the wire and
discarded: Bluepad32's Xbox parser implements neither `set_player_leds`
nor `set_lightbar_color`, so there is nothing on that pad to light, and
`XPAD_CAP_LED` is correspondingly never claimed. Both fields stay in
the frame so that honouring them later needs no wire change. A sender
should keep filling them in; a receiver may ignore what it cannot do,
but must not claim the capability.

`duration` of 0 means "until replaced", which is what xpad's request
area implies: it carries magnitudes and no duration, so a consumer that
sets them expects them to hold until it sets something else. An adapter
whose own rumble call wants a duration should arm a burst slightly
longer than the interval it re-arms at, so the motor never gaps and a
host that stops asking, or is switched off mid rumble, leaves the pad
falling silent on its own rather than buzzing indefinitely.

## Timing budget

| Link | Bytes/sec | 12-byte frame | 1 pad @ 50Hz |
|------|-----------|---------------|--------------|
| 2400 | 240 | 50 ms | not viable, use compact |
| 9600 | 960 | 12.5 ms | 63% |
| 19200 | 1920 | 6.3 ms | 31% |
| 38400 | 3840 | 3.1 ms | 16% |

Four pads at 50 Hz needs 2400 bytes/sec, which does not fit at 19200.
See "When it does not fit" below for what the adapter does about it.

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
handler. That is real work, machine-specific, and untestable under
emulation, and the round-robin below buys most of what it would.

## When it does not fit

An adapter MUST NOT send more in a tick than the link carries in one.
Sending anyway does not make the wire faster: it blocks the sender for
longer than its own tick, which costs it whatever else the tick was
for. On the Pico that is Bluetooth and the receive drain, and the RX
FIFO holds less than one tick of 19200, so the cost is dropped input
rather than merely late output.

The budget is the link's byte rate divided by the tick rate: 38 bytes
at 19200 on a 20 ms tick. Three state frames fit; four do not.

What does not fit is **deferred, not dropped**, and the pad it happened
to is the one the next tick starts from. So four pads under permanent
load settle at three served per tick, no pad waiting more than one tick
past its turn, and each one updating at about 37 Hz rather than 50.
Nothing accumulates, because every frame is full state: a deferred pad
sends a complete, correct frame when its turn comes, and the frame it
skipped was never needed. This is the self-healing property doing a
second job.

Descriptors come out of the same budget and are simply skipped when
they do not fit, because the repeat interval brings them back. They
never take the rotation's place.

**Compact frames relieve a tick that is over budget**, and only then.
Whether to use them is a property of the tick, not of a pad: if
everything fits, nobody sends one, so one and two pads never see a
compact frame at all. If it does not fit, every pad that can goes
compact, which is usually enough that nothing needs deferring. Four
pads pressing buttons cost 20 bytes instead of 48 and all four are
served. Deciding per pad as the budget ran down would let the early
ones spend it on full state and defer the last one anyway, which is
the pad the shortfall was never about.

A compact frame is only valid when the axes have not moved since the
last frame this pad sent, because the receiver leaves its copy of them
alone, **and** when no button above bit 15 is held. Sixteen bits is all
it carries and the receiver assigns rather than merges, so a held
`XPAD_THUMBR`, bit 16, would be cleared by every compact frame that
passed: a button that releases itself while you hold it, rather than an
update that arrives late.

A compact frame does not restart the keepalive interval. The keepalive's
second job is self healing, and a receiver that lost bytes to noise gets
its axes back from a full state frame and from nothing else, so
sustained button pressing must not be able to hold one off.

A frame that cannot be queued at all is dropped whole, never
truncated. A truncated frame is bytes the receiver has to resynchronise
past; a dropped one costs a single update.

None of this changes anything below three pads, which is every session
anyone has actually run: one or two frames fit the UART's FIFO outright
and the rotation never defers anything.

**Two paths are outside the budget**, deliberately. A ping is answered
the moment it arrives rather than waiting for the next tick's
allowance, because the sender is timing the reply and a probe that
found nothing moves on. A departing pad's last state frame and first
departure notice go out the same way, because by the time the tick runs
the slot is empty and has nothing left to say. Both are one-off and
bounded, the queue absorbs them, and anything it cannot hold is dropped
whole and counted rather than truncated. An adapter may do the same; it
may not put a periodic frame on that path.
