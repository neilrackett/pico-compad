// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Neil Rackett
//
// Watch an adapter's output on a host, for bring-up.
//
// The ST shows you nothing when the wire is wrong, and nothing when the
// firmware is silent, which are very different faults. This listens on
// the adapter's TTL side with a USB-to-TTL cable and says which: raw
// bytes arriving at all means the Pico is transmitting, frames decoding
// means it is transmitting correctly, and silence means it is not
// transmitting and the ST was never the problem.
//
//   node harness/listen.js /dev/cu.usbserial-XXXX [baud]
//
// Two wires: adapter GND to a Pico GND, adapter RXD to Pico GP0. Do not
// connect VCC or 5V while the Pico has its own USB power, and never put
// the DE-9 side of a MAX3232 anywhere near a TTL adapter: RS-232 swings
// to about +/-5.5V and TTL parts do not survive it.

import fs from 'node:fs';

import { openSerial } from './serial.js';
import { buttonNames, checksum, SYNC, FRAME_LEN } from './xpadmap.js';

const port = process.argv[2];
const baud = Number(process.argv[3] ?? 19200);

if (!port) {
  console.error('usage: node harness/listen.js <device> [baud]');
  process.exit(2);
}

const fd = openSerial(port, baud);

let buf = [];
let bytes = 0, frames = 0, bad = 0;
let lastLine = '';

function describe(f) {
  const type = f[1] & 0x0f;
  const pad = (f[1] >> 4) & 3;

  if (type === 0xf) {
    return `pad ${pad}  descriptor: type ${f[2]} flags ${f[3]} ` +
           `caps ${((f[4] << 8) | f[5]).toString(16).padStart(4, '0')}`;
  }

  const bits = type === 0x0
    ? f[2] | (f[3] << 8) | (f[4] << 16)
    : f[2] | (f[3] << 8);

  const held = buttonNames(bits).join(' ') || '-';

  if (type === 0x1) return `pad ${pad}  compact  ${held}`;

  const s = (v) => String(v > 127 ? v - 256 : v).padStart(4);

  return `pad ${pad}  L${s(f[5])},${s(f[6])}  R${s(f[7])},${s(f[8])}  ` +
         `lt ${String(f[9]).padStart(3)} rt ${String(f[10]).padStart(3)}  ${held}`;
}

function feed(b) {
  bytes++;

  if (buf.length === 0) {
    if (b === SYNC) buf.push(b);
    return;
  }

  if (buf.length === 1) {
    const len = FRAME_LEN[b & 0x0f];
    if (!len || (b >> 6) !== 0) {           // unknown type, or a version we do not speak
      buf = b === SYNC ? [b] : [];
      return;
    }
    buf.push(b);
    return;
  }

  buf.push(b);

  const want = FRAME_LEN[buf[1] & 0x0f];
  if (buf.length < want) return;

  const frame = buf;
  buf = [];

  if (checksum(frame, want) !== frame[want - 1]) { bad++; return; }

  frames++;
  const line = describe(frame);
  if (line !== lastLine) { console.log(line); lastLine = line; }
}

console.log(`listening on ${port} at ${baud} baud, ctrl-C to stop`);
console.log('move the stick: frames go out on change, plus a keepalive\n');

// Polled rather than streamed. The port is opened non-blocking so the
// open itself cannot hang waiting for carrier, and a non-blocking read
// with nothing available raises EAGAIN, which a ReadStream treats as
// fatal. On a link that is idle most of the time, and where silence is
// one of the answers we are looking for, that is exactly wrong.
const chunk = Buffer.alloc(256);

setInterval(() => {
  for (;;) {
    let n;

    try {
      n = fs.readSync(fd, chunk, 0, chunk.length, null);
    } catch (e) {
      if (e.code === 'EAGAIN') return;      // nothing waiting, try later
      console.error(`\nread failed: ${e.message}`);
      process.exit(1);
    }

    if (!n) return;
    for (let i = 0; i < n; i++) feed(chunk[i]);
  }
}, 20);

setInterval(() => {
  process.stderr.write(
    `\r${bytes} bytes, ${frames} frames, ${bad} bad checksum   `);
}, 1000);

process.on('SIGINT', () => {
  console.log(`\n\n${bytes} bytes, ${frames} frames, ${bad} bad checksum`);
  process.exit(frames > 0 ? 0 : 1);
});
