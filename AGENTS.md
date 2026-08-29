# AGENTS.md

Guidance for AI coding agents working in `pico-compad`.

## What this is

COMpad: modern gamepads into retro machines over RS-232. It is being
built for the Atari ST family, publishing to
[xpad](https://github.com/neilrackett/atarist-xpad); the wire protocol
is platform-neutral so other machines can gain targets later, but the
ST is the only one that exists and the only one being worked on. Keep
that line clear in anything you write: ST in the present tense, other
platforms in the future tense. README.md is the overview and docs/ holds
the detail: read docs/protocol.md before touching the wire format. This
file only covers working practices.

## Layout

See README's Layout section. The split that matters: the wire-protocol
decoder (`target/atarist/src/protocol.h`) is portable C with no
platform headers, tested on the host; TOS code stays out of it. It is
the one piece a future platform target would take verbatim, which is
why it stays clean even though only the ST uses it.

xpad is a **submodule** (`lib/xpad`), not a vendored copy, so the two
projects stay in sync while both evolve. Do not edit files under
`lib/`; change them upstream and bump.

## Building and testing

```
make                               same as test-host: fast, no toolchain
make test-host                     host tests only, no emulator
STCMD_NO_TTY=1 stcmd make st       build the ST binaries
make test-decode                   compad.c's own assertions, on the ST
make test-serial                   the link: Node -> FIFO -> Hatari
make test-provider                 residency and the cookie jar
make test-live                     updates actually follow the sender
make test-gamepad                  a simulated pad, full state
make test                          all six, in order
make simulator                     hands on, in a window
```

`test-host` is the default goal and needs nothing but a host compiler
and Node, so run it on every change. `test` runs the emulated ones too
and takes about half a minute; it does not build the ST binaries, so
run `st` first if they are stale.

Targets are named for what they prove. docs/roadmap.md still numbers
the phases, and a script names its phase where it has one, so the two
can be matched up without the numbers leaking into everyday commands.
`test-decode` and `test-live` belong to no phase: they cover things
that turned out to need covering.

The runners share `test/hatari.sh` the way they share `test/tos.sh`.
Booting, the Hatari flag lore, the cleanup trap and the verdict polling
live there, so each runner is only its own setup and assertions. Add a
new one by copying the shortest, `run-decode.sh`.

`test-gamepad` is the only target with a node_modules dependency
(`@mesmotronic/xpad`, via `npm install`). It is there because the test
drives the real library rather than a copy of its behaviour; anything
that only needed the behaviour would not have earned the dependency.
The version is pinned exactly, not a caret range: phase 2 asserts exact
wire bytes, so a silent minor bump would surface as an unexplained byte
mismatch. Bump it deliberately and re-run.

The `test-*` targets that boot Hatari run on the host, never under
stcmd: the container has no Hatari and no Node. Set `$HATARI` if Hatari
is somewhere unusual, and `$MACHINE` to change the emulated machine.

A TOS image needs no setup: `test/tos.sh` fetches EmuTOS into
`build/tos/` on first use, checks it against a known SHA-256, and every
runner sources it. The ROM is deliberately **not** committed, because
EmuTOS is GPLv2 and shipping the binary would oblige this repo to offer
its corresponding source indefinitely, for a 256K convenience file that
would also go stale in git history. `$TOS` still overrides, and has to:
EmuTOS always reports itself as TOS 2.06, so anything depending on a
real TOS version must be pointed at a real ROM.

The xpad conventions apply here unchanged: run the host tests on every
change, build with warnings fatal, and treat "compiles" as far short of
"works". Nothing here has run on real hardware yet.

The emulated targets must not run concurrently, and the Makefile is
marked .NOTPARALLEL rather than relying on nobody passing -j. Not
because of a shared port, which is the obvious guess and wrong: none of
them binds one. Because none of them fast-forward, so several emulators
contending for the host is exactly the condition that lets a real-time
sender outrun a slowed ST.

## Current status

Phases 0 to 2 pass under emulation, though phase 2 has only ever been
driven by a simulated pad. Phase 0: the Node harness writes
fixed state frames into a FIFO, Hatari presents them as RS-232, and
PIPECHK.TOS decodes them under EmuTOS, values verified byte for byte.
Phase 1: COMPAD.PRG installs resident from AUTO, drains the AUX iorec
ring from the etv_timer vector, publishes an xpad block, and xpad's own
viewer reads the held pattern back through the cookie jar
(`make test-provider`, browserless via the server's --test mode). The browser
keyboard page exists and follows README's mapping, but no automation
presses keys in it: a human at `node harness/server.js <fifo>` plus
XPADVIEW.TOS under Hatari is the manual half of the exit criteria.
Phase 2 is half done: `make test-gamepad` drives a synthetic gamepad through
the real @mesmotronic/xpad library and asserts buttons, signed axes,
analogue triggers, caps and pad type through the viewer. A real
controller works through `harness/pad.html`, but no automation presses
it, and no gamepad has been tried on real ST hardware. Phases 3 and 4
are not started.

Nothing has run on real hardware, and serial timing under Hatari is
meaningless: byte-level behaviour is proven, baud and latency are not.

## Hard rules

- **docs/protocol.md is the contract.** Change behaviour to match
  the doc, or change the doc deliberately in the same commit; never let
  them drift.
- **The decoder stays TOS-free** and byte-at-a-time. It must build for
  the host, the ST, and eventually much weaker consumers; keep it to
  `<stdint.h>`.
- **Full state per frame, never deltas.** The self-healing property
  depends on it. Send *on change* plus a 250 ms keepalive, though:
  every frame is still full state, but an idle pad must not fill the
  link. Streaming unconditionally at 50 Hz is 63% of a 9600 link, and
  at that load any receiver that falls behind never catches up.
- xpad's rules travel with it: single provider, chain any displaced
  handler, `Supexec` below `$800`, frozen button bits, and never claim
  a capability that is not honoured. See `xpad/AGENTS.md`.
- Serial timing observed under Hatari is meaningless; only framing,
  sync recovery and checksums are testable there. Do not tune timing
  against the emulator.
- **Test the harness, not just the ST.** The server's `--test` mode
  exists so the end-to-end runs need no browser, but it also means
  those runs never open a socket. A wrong magic GUID in the WebSocket
  handshake therefore sat there with every test green and every
  browser reconnecting forever. `test/server_test.js` covers the
  handshake and the frame path now, and it checks against RFC 6455's
  published test vector rather than against our own implementation, so
  a mistake cannot agree with itself. Anything hand-rolled from a spec
  gets checked against the spec's own vectors.
- **Test with a value that changes.** A held pattern proves a consumer
  can decode a frame; it cannot prove the consumer is reading new
  frames rather than re-reading old ones, because stale bytes decode to
  the same value and look perfect. `compad.c` had TOS's AUX ring head
  and tail the wrong way round, walked the ring backwards over stale
  bytes and never advanced the read index, and every end-to-end test
  passed because every one of them held a fixed pattern. `make
  test-live` exists so that class of bug cannot hide again.
- **Never leave a stream's `error` event unbound.** Node throws an
  unhandled `'error'` and takes the process with it, so dropping the
  listener does not make errors quieter, it makes them fatal. A browser
  that vanishes rudely, which is most of them, arrives as ECONNRESET
  and killed the whole server once because a tidy-up removed
  `socket.on('error')` to stop a duplicate log line. Deduplicate inside
  the handler instead; `test/server_test.js` resets a socket to hold
  this in place.
- **Automated runs must be hermetic.** `--test` opens no HTTP port, so
  a browser tab left open on 8232 cannot reach it. One did, and because
  a backgrounded tab sends "nothing held" on blur, it quietly replaced
  the fixed pattern and failed a run that had nothing wrong with it.
  The socket test uses its own port for the same reason.

## Style

As atarist-xpad: C89-friendly, Allman braces, 4 spaces, `-Wall -Wextra
-Werror` clean on gcc 4.6.4 for the target.

Harness JavaScript is plain Node with no dependencies until a phase
genuinely needs one, and it is **ES modules throughout**: `package.json`
sets `"type": "module"`, so every `.js` here is a module and there is no
`.mjs` to explain. Import Node builtins with the `node:` prefix, and
remember `__dirname` does not exist: use `import.meta.dirname`. Keeping
one module system is what lets the browser pages and the Node harness
share `harness/xpadmap.js` rather than keeping parallel copies of the
wire format.

The product is **COMpad**, styled that way in prose: capital C, O, M,
lowercase pad. Not Compad, COMPad or COMPAD. All-caps `COMPAD_` and
lowercase `compad_` are for C identifiers and filenames only, and
`pico-compad` is the repository, not the product. The name is settled;
it is not a working title.

Prose avoids em dashes.

## Licence

GPL-3.0-or-later throughout. This repo ships programs, not libraries:
consumers integrate via the xpad submodule, which is BSD-2-Clause and
must stay that way, so nothing here leaks copyleft into a game. Do not
copy code from this repo into `lib/xpad` or into anything a consumer
links. SPDX headers on new files:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */
```
