#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Neil Rackett
//
// Push COMpad frames through a real UART and see what comes back.
//
// Everything measured so far went through Hatari, whose serial is
// byte-level plumbing rather than a bit-accurate UART: framing and
// checksums are testable there, throughput and latency are not, and
// docs/roadmap.md says so. This is the first thing in the project that
// measures something physical.
//
// Wants TX shorted to RX on a USB serial adapter. Nothing else: no ST,
// no Pico, no level shifter, since both ends are the same 3.3V pin.
//
//   node harness/wire.js <port> <baud> [frames]
//
// Reports achieved throughput, round-trip latency for one frame, and
// whether every byte came back the way it left.

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';

import { XPAD, stateFrame } from './xpadmap.js';

const port = process.argv[2];
const baud = Number(process.argv[3] || 19200);
const wanted = Number(process.argv[4] || 200);

if (!port) {
  console.error('usage: wire.js <port> <baud> [frames]');
  process.exit(2);
}

// A pattern with every field distinct, so a byte landing in the wrong
// place shows up as a wrong number rather than a plausible one.
const POSE = {
  buttons: XPAD.WEST | XPAD.TR | XPAD.START,
  lx: 100, ly: -50, rx: -127, ry: 95, lt: 64, rt: 255,
};

const frame = Buffer.from(stateFrame(0, POSE));
const expected = Buffer.concat(Array.from({ length: wanted }, () => frame));

// ------------------------------------------------------------------

function configure() {
  // macOS resets a port when the last fd closes, so this has to happen
  // with ours already open, and a later `stty -f` from a shell will
  // read 9600 back. That is the port being reopened, not the setting
  // failing to take.
  execFileSync('stty', [
    '-f', port, String(baud),
    'cs8', '-cstopb', '-parenb', '-crtscts',
    'raw', '-echo', '-hupcl',
  ]);
}

const summary = (label, value) => console.log(`  ${label.padEnd(28)}${value}`);

// ------------------------------------------------------------------

// Opening a USB serial port is not always quick. A Raspberry Pi Debug
// Probe with nothing on its UART takes about 40 seconds to open and
// close on macOS, measured; a CH340G is usually instant. Say so before
// it happens, or the test looks wedged when it is only waiting.
console.log(`opening ${port} (some adapters take a while)...`);

let fd;

try {
  // cu, not tty: the callout device does not block waiting for carrier.
  //
  // O_NONBLOCK matters more than it looks. A USB serial adapter with
  // nothing on the other end may not drain at all: measured on a
  // Raspberry Pi Debug Probe, a blocking write of 2400 bytes never
  // returns. Without this the test wedges instead of reporting that
  // the loopback is missing, which is the very thing it exists to say.
  fd = fs.openSync(port,
    fs.constants.O_RDWR | fs.constants.O_NOCTTY | fs.constants.O_NONBLOCK);
} catch (e) {
  console.error(`cannot open ${port}: ${e.code}`);
  console.error('is the adapter plugged in, and is the path right?');
  process.exit(1);
}

try {
  configure();
} catch (e) {
  console.error(`cannot configure ${port}: ${e.message}`);
  process.exit(1);
}

console.log(`${port} at ${baud} baud, ${wanted} frames of ${frame.length} bytes`);
console.log(`  ${expected.length} bytes out, expecting the same back\n`);

const received = [];
let firstByteAt = 0;
let measuring = false;

const reader = fs.createReadStream(null, { fd, autoClose: false });
reader.on('data', (chunk) => {
  // Anything arriving before we start is left over from an earlier
  // session; counting it would make an unshorted port look alive.
  if (!measuring) return;
  if (!firstByteAt) firstByteAt = process.hrtime.bigint();
  received.push(chunk);
});
reader.on('error', () => {});

// Let the flush window pass, then measure.
await new Promise((r) => setTimeout(r, 250));
measuring = true;

// Feed the port as fast as it will take it, never faster. EAGAIN is
// the adapter saying its buffer is full, not an error.
async function push(buf) {
  const deadline = Date.now() + 4000;
  let sent = 0;

  while (sent < buf.length) {
    let n = 0;

    try {
      n = fs.writeSync(fd, buf, sent, Math.min(64, buf.length - sent));
    } catch (e) {
      if (e.code !== 'EAGAIN') throw e;
    }

    sent += n;

    if (Date.now() > deadline) return sent;
    if (n === 0) await new Promise((r) => setTimeout(r, 2));
  }
  return sent;
}

// Probe with a single frame before committing to a measurement.
//
// A port with nothing attached still accepts a kilobyte or two into the
// kernel's buffer and only then stops, and those bytes never drain, so
// closing the fd afterwards waits on them: a full-size write to a dead
// link took 102 seconds to fail. Twelve bytes fail in under one, and
// leave nothing queued to hold the close up.
await push(frame);
await new Promise((r) => setTimeout(r, 400));

if (received.length === 0) {
  console.error('No loopback: the frame sent did not come back.\n');
  console.error('This test needs TX shorted to RX on the adapter, so it');
  console.error('can talk to itself. Bridge those two pins.\n');
  console.error('If they are bridged, check this is the UART and not a');
  console.error('second interface on the same device: a Debug Probe');
  console.error('presents CMSIS-DAP alongside its UART, and only one of');
  console.error('them carries your bytes.');
  process.exit(3);
}

received.length = 0; // the probe is not part of the measurement
firstByteAt = 0;

const startedAt = process.hrtime.bigint();
const sent = await push(expected);
const writtenAt = process.hrtime.bigint();

if (sent < expected.length) {
  console.error(`The port took ${sent} of ${expected.length} bytes and ` +
                'then stopped draining.');
  console.error('The link came up but did not stay up.');
  process.exit(1);
}

// 10 bits a byte at 8N1, plus a generous margin for the adapter's own
// buffering, then a floor so a fast link still gets a fair chance.
const expectMs = (expected.length * 10 * 1000) / baud;
const waitMs = Math.max(1500, expectMs * 3);

setTimeout(() => {
  reader.destroy();

  const got = Buffer.concat(received);
  const ms = firstByteAt
    ? Number(process.hrtime.bigint() - startedAt) / 1e6
    : 0;

  if (got.length < expected.length / 2) {
    console.error(got.length === 0
      ? 'Nothing came back.\n'
      : `Only ${got.length} of ${expected.length} bytes came back.\n`);
    console.error('The likely cause is that TX and RX are not shorted.');
    console.error('This test is a loopback: it needs the adapter talking');
    console.error('to itself. Bridge the TX and RX pins and try again.\n');
    console.error('If they ARE bridged, check the port is the UART and');
    console.error('not a second interface the same device exposes: a');
    console.error('Debug Probe presents CMSIS-DAP as well as its UART.');
    process.exit(1);
  }

  const rate = Math.round(got.length / (ms / 1000));
  const intact = got.length === expected.length && got.equals(expected);

  console.log('Results');
  summary('bytes sent', expected.length);
  summary('bytes returned', got.length);
  summary('elapsed', `${ms.toFixed(1)} ms`);
  summary('achieved', `${rate} bytes/sec`);
  summary('theoretical at this baud', `${Math.round(baud / 10)} bytes/sec`);
  summary('efficiency', `${Math.round((rate / (baud / 10)) * 100)}%`);
  summary('first byte back after', firstByteAt
    ? `${(Number(firstByteAt - writtenAt) / 1e6).toFixed(1)} ms`
    : 'n/a');
  summary('every byte identical', intact ? 'yes' : 'NO');

  if (!intact) {
    const at = [...expected].findIndex((b, i) => got[i] !== b);
    console.log(`\n  first difference at byte ${at}` +
                `, sent 0x${expected[at]?.toString(16)}` +
                ` got 0x${got[at]?.toString(16) ?? '--'}`);
  }

  // What this means for the protocol: one pad of full state at 50 Hz.
  const perPad = 12 * 50;
  console.log('\nAgainst the protocol');
  summary('one pad, full state, 50 Hz', `${perPad} bytes/sec`);
  summary('that is', `${Math.round((perPad / rate) * 100)}% of this link`);
  summary('send-on-change idle', '~85 bytes/sec, ' +
          `${Math.round((85 / rate) * 100)}% of this link`);

  process.exit(intact ? 0 : 1);
}, waitMs);
