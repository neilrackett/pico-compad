// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Neil Rackett
//
// Host tests for the harness server's WebSocket path. No emulator, no
// ST: start the server on a FIFO, talk to it the way a browser does,
// and read the frames back off the pipe.
//
// This exists because the socket had no coverage at all and a wrong
// magic GUID in the handshake went unnoticed: every browser rejected
// the handshake and reconnected forever, while the end-to-end tests
// stayed green because they drive the server's --test mode, which
// never opens a socket. The handshake is checked against RFC 6455's
// own test vector rather than against our implementation of it, so a
// mistake cannot agree with itself.

import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const PORT = 8233; // not 8232: a stray browser tab may still hold that
let failures = 0;

/*
 * The client half of this test is Node's own global WebSocket, which
 * only became available without a flag in Node 22.4. Say so plainly:
 * on an older Node this file dies with "WebSocket is not defined" a
 * hundred lines further down, which reads like a broken test rather
 * than a Node too old to run it. The first CI run hit exactly that.
 */
if (typeof WebSocket === 'undefined') {
  console.error(
    `This test needs Node's global WebSocket client, which is available\n` +
      `from Node 22.4. This is ${process.version}. Upgrade Node, or run it\n` +
      `with --experimental-websocket on 21 and 22.0 to 22.3.`,
  );
  process.exit(1);
}

function check(ok, what) {
  console.log(`${ok ? '   ' : '!! '}${what}${ok ? '' : '   FAILED'}`);
  if (!ok) failures++;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const buttonsOf = (f) => f[2] | (f[3] << 8) | (f[4] << 16);

/** The most recent checksum-valid state frame on the wire, if any. */
function lastStateFrame(bytes) {
  for (let i = bytes.length - 12; i >= 0; i--) {
    if (bytes[i] !== 0xa5 || bytes[i + 1] !== 0x00) continue;
    const f = bytes.slice(i, i + 12);
    let x = 0;
    for (let j = 0; j <= 10; j++) x ^= f[j];
    if (x === f[11]) return f;
  }
  return null;
}

/** Poll the wire until a frame satisfies `want`, rather than sleeping
 *  a fixed budget and hoping it was long enough. */
async function waitForFrame(want, timeout = 3000) {
  const until = Date.now() + timeout;
  while (Date.now() < until) {
    const f = lastStateFrame(received);
    if (f && want(f)) return f;
    await sleep(20);
  }
  return null;
}

// ------------------------------------------------------------------
console.log('harness server, WebSocket path');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'compad-srv-'));
const fifo = path.join(dir, 'serial.in');
spawn('mkfifo', [fifo]).unref();
await sleep(200);

// Hold the FIFO open for reading, and collect everything written.
const received = [];
const reader = fs.createReadStream(fifo);
reader.on('data', (b) => received.push(...b));
reader.on('error', () => {});

const server = spawn('node', ['harness/server.js', fifo], {
  env: { ...process.env, COMPAD_PORT: String(PORT) },
  stdio: ['ignore', 'pipe', 'pipe'],
});

// Wait for the server to say it is up rather than guessing at a delay:
// this is the default `make` target, so its seconds are the ones a
// person actually spends.
const ready = new Promise((resolve) => {
  const done = () => resolve(true);
  server.stdout.on('data', (d) => {
    if (d.toString().includes('reader attached')) done();
  });
  setTimeout(done, 4000);
});
server.stderr.on('data', () => {});

async function shutdown() {
  server.kill('SIGKILL');
  reader.destroy();
  await sleep(100);
  fs.rmSync(dir, { recursive: true, force: true });
}

await ready;

// ------------------------------------------------------------------
console.log('\nhandshake, against the RFC 6455 test vector');

// RFC 6455 section 1.3 gives this key and the accept value it must
// produce. Any browser computes the same thing and hangs up if the
// server disagrees.
const RFC_KEY = 'dGhlIHNhbXBsZSBub25jZQ==';
const RFC_ACCEPT = 's3pPLMBiTxaQ9kYGzzhZRbK+xOo=';

const response = await new Promise((resolve) => {
  const sock = net.connect(PORT, '127.0.0.1', () => {
    sock.write(
      'GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n' +
      `Connection: Upgrade\r\nSec-WebSocket-Key: ${RFC_KEY}\r\n` +
      'Sec-WebSocket-Version: 13\r\n\r\n');
  });
  let buf = '';
  sock.on('data', (d) => {
    buf += d.toString('latin1');
    if (buf.includes('\r\n\r\n')) { sock.destroy(); resolve(buf); }
  });
  sock.on('error', () => resolve(buf));
  setTimeout(() => { sock.destroy(); resolve(buf); }, 2000);
});

check(response.startsWith('HTTP/1.1 101'), 'the server switches protocols');
check(/upgrade:\s*websocket/i.test(response), 'Upgrade: websocket');
check(response.includes(`Sec-WebSocket-Accept: ${RFC_ACCEPT}`),
      'the accept hash matches the RFC test vector');

// Independently: the value must be sha1(key + GUID), base64. Written
// out here so a wrong GUID in server.js cannot be self-consistent.
const expected = createHash('sha1')
  .update(RFC_KEY + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
  .digest('base64');
check(expected === RFC_ACCEPT, 'and that vector is sha1(key + GUID) as spec');

// ------------------------------------------------------------------
console.log('\na browser drives the wire');

const PATTERN = 0x218; // XPAD_RIGHT | XPAD_SOUTH | XPAD_TR

const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
const opened = await new Promise((resolve) => {
  ws.onopen = () => resolve(true);
  ws.onerror = () => resolve(false);
  setTimeout(() => resolve(false), 3000);
});
check(opened, 'a real WebSocket client stays connected');

if (opened) {
  received.length = 0;
  ws.send(JSON.stringify({ b: PATTERN }));

  const found = await waitForFrame((f) => buttonsOf(f) === PATTERN);

  check(found !== null, 'the pressed buttons reach the FIFO as 0x218');

  // A vanished browser must not leave keys held down.
  received.length = 0;
  ws.close();

  const released = await waitForFrame((f) => buttonsOf(f) === 0);
  check(released !== null, 'closing the socket releases everything held');
}

// ------------------------------------------------------------------
console.log('\na browser that vanishes rudely');

// A clean close is the easy case. Real browsers also disappear without
// one: a reload mid-frame, a crashed tab, a closed lid, all of which
// reach the server as ECONNRESET. A socket with no 'error' listener
// makes Node throw the event and take the server down with it, which
// is exactly what happened once, so this holds the listener in place.
{
  const raw = net.connect(PORT, '127.0.0.1');
  await new Promise((resolve) => {
    raw.on('connect', () => {
      raw.write(
        'GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n' +
        `Connection: Upgrade\r\nSec-WebSocket-Key: ${RFC_KEY}\r\n` +
        'Sec-WebSocket-Version: 13\r\n\r\n');
    });
    raw.on('data', () => resolve());
    raw.on('error', () => resolve());
    setTimeout(resolve, 2000);
  });

  // setLinger(0) makes close() send RST instead of FIN, which is the
  // ECONNRESET a rudely-vanished browser produces.
  raw.setNoDelay(true);
  raw.resetAndDestroy();
  await sleep(400);

  check(server.exitCode === null && server.signalCode === null,
        'the server survives a socket reset');

  // And is still serving, not merely un-crashed.
  const alive = await new Promise((resolve) => {
    const probe = net.connect(PORT, '127.0.0.1', () => {
      probe.destroy();
      resolve(true);
    });
    probe.on('error', () => resolve(false));
    setTimeout(() => resolve(false), 2000);
  });
  check(alive, 'and still accepts connections afterwards');
}

await shutdown();

console.log(failures ? `\n${failures} check(s) failed` : '\nall checks passed');
process.exit(failures ? 1 : 0);
