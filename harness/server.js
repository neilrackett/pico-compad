#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Neil Rackett
//
// Browser input to COMpad frames.
//
// Serves index.html, holds a WebSocket open (hand-rolled RFC 6455
// subset: our messages are tiny, masked, single-frame text), and
// writes state frames to the Hatari FIFO at 50 Hz. Plain Node, no
// dependencies.
//
//   node server.js <fifo>            serve http://localhost:8232
//   node server.js <fifo> --verbose  timestamp every key and every
//                                    frame written, to see where a
//                                    delay is coming from
//   node server.js <fifo> --test     no browser: hold a fixed pattern
//   node server.js <fifo> --cycle    no browser: change the pattern on
//                                    a loop, so a consumer that latches
//                                    onto stale state is caught
//
// Two pages are served: / is the keyboard harness, /pad.html drives a
// real gamepad through @mesmotronic/xpad. Both post state up the same
// socket; the pad page fills in the analogue fields the keyboard
// leaves at rest.

import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';

// One definition of the wire format, shared with the browser pages and
// the simulator. fs.writeSync takes the Uint8Array it returns unchanged.
import { XPAD, stateFrame, descriptorFrame, buttonNames } from './xpadmap.js';

// Overridable so the socket test can run on its own port, and so a
// stray browser tab still reconnecting to 8232 cannot wander into it.
const PORT = Number(process.env.COMPAD_PORT) || 8232;
const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'; // RFC 6455 section 1.3

const fifoPath = process.argv[2];
// Three modes. Both headless ones open no HTTP port, which is what
// keeps an automated run out of reach of a stray browser tab.
const mode = process.argv.includes('--cycle') ? 'cycle'
           : process.argv.includes('--test') ? 'test'
           : 'browser';
const headless = mode !== 'browser';
const verbose = process.argv.includes('--verbose');

// Milliseconds matter here: the whole point is comparing when a key was
// pressed against when the ST reacts.
const stamp = () => {
  const d = new Date();
  return `${d.toTimeString().slice(0, 8)}.${String(d.getMilliseconds()).padStart(3, '0')}`;
};

// Named bits, so the log reads as keys rather than as hex.
const describe = (mask) => {
  const on = buttonNames(mask);
  return on.length ? on.join('+') : '(none)';
};

if (!fifoPath) {
  console.error('usage: server.js <fifo> [--test|--cycle] [--verbose]');
  process.exit(2);
}

// ------------------------------------------------------------------
// Held state, and the 50 Hz writer

// XPAD_TYPE_GAMEPAD = 2; no flags, no caps until a page says otherwise.
const REST = { buttons: 0, lx: 0, ly: 0, rx: 0, ry: 0, lt: 0, rt: 0 };
let pad = { ...REST };
let descriptor = descriptorFrame(0, 2, 0, 0);
// Re-sent periodically, not once: the provider discards the serial
// ring when it installs, so a descriptor that arrived while the ST was
// still booting is gone. A changed descriptor sets this back to 0 so
// it goes out on the very next frame.
const DESCRIPTOR_EVERY = 10; // ticks, so five times a second at 50 Hz
const KEEPALIVE_EVERY = 12;  // ticks, so about every 250 ms
let tick = 0;

// Send on change, plus a keepalive: the mitigation docs/protocol.md
// already specifies, and the reason latency stayed low.
//
// Writing full state 50 times a second is 600 bytes/sec on a link that
// carries 960, and a pad is idle almost all the time. At 63% there is
// no headroom: any moment the receiver runs slower than the sender,
// the shortfall queues, and a queue on a link this slow is latency you
// never get back. Idle now costs about 85 bytes/sec, and a press still
// goes out on the very next tick, so nothing is traded away for it.
let lastSent = null;
let sinceSend = 0;
let bytesOut = 0;

function changed(a, b) {
  if (!b) return true;
  return a.buttons !== b.buttons || a.lx !== b.lx || a.ly !== b.ly ||
         a.rx !== b.rx || a.ry !== b.ry || a.lt !== b.lt || a.rt !== b.rt;
}

if (mode === 'test') {
  // The same asymmetric pattern the pipe check used: RIGHT|SOUTH|TR.
  pad.buttons = 0x08 | 0x10 | 0x200;
  console.log('test mode: holding buttons 0x218, no browser needed');
}

// A held pattern proves a consumer can decode a frame. It cannot prove
// the consumer is reading new frames rather than re-reading old ones:
// stale bytes decode to the same value and look perfect. This walks a
// short cycle so that mistake has to show itself.
const CYCLE = [0x01, 0x02, 0x04, 0x08, 0x218];

if (mode === 'cycle') {
  let step = 0;
  pad.buttons = CYCLE[0];
  setInterval(() => { pad.buttons = CYCLE[++step % CYCLE.length]; }, 600);
  console.log('cycle mode: walking a button pattern, no browser needed');
}

(function openFifo(attempt) {
  fs.open(fifoPath, fs.constants.O_WRONLY | fs.constants.O_NONBLOCK, (err, fd) => {
    if (err) {
      if (attempt === 0) console.log(`waiting for a reader on ${fifoPath}`);
      setTimeout(() => openFifo(attempt + 1), 200);
      return;
    }
    console.log('reader attached; streaming at 50 Hz');
    setInterval(() => {
      try {
        if (tick % DESCRIPTOR_EVERY === 0) {
          fs.writeSync(fd, descriptor);
        }
        tick++;

        const moved = changed(pad, lastSent);

        if (moved || ++sinceSend >= KEEPALIVE_EVERY) {
          fs.writeSync(fd, stateFrame(0, pad));
          if (verbose && moved) {
            console.log(`${stamp()}  wire:    ${describe(pad.buttons)}` +
                        `  (${bytesOut += 12} bytes sent)`);
          }
          lastSent = { ...pad };
          sinceSend = 0;
        }
      } catch (e) {
        // Reader gone: full state per frame means just keep trying.
      }
    }, 20);
  });
})(0);

// ------------------------------------------------------------------
// HTTP + WebSocket

const STATIC = {
  '/': ['index.html', 'text/html; charset=utf-8'],
  '/index.html': ['index.html', 'text/html; charset=utf-8'],
  '/pad.html': ['pad.html', 'text/html; charset=utf-8'],
  // Served so the browser and the simulator share one mapping file
  // rather than two that can drift.
  '/xpadmap.js': ['xpadmap.js', 'text/javascript; charset=utf-8'],
};

const server = http.createServer((req, res) => {
  const url = req.url.split('?')[0];
  const entry = STATIC[url];

  if (!entry) {
    res.writeHead(404);
    res.end();
    return;
  }

  fs.readFile(path.join(import.meta.dirname, entry[0]), (err, body) => {
    if (err) {
      res.writeHead(500);
      res.end(`${entry[0]} missing`);
      return;
    }
    res.writeHead(200, { 'Content-Type': entry[1] });
    res.end(body);
  });
});

// Newest connection wins. A page sends its held state on open and on
// blur, so a second tab left over from an earlier run will happily
// clear what the tab you are actually typing in just pressed. For a
// single-user rig the last socket to arrive is the one you mean.
let activeSocket = null;

server.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'];
  if (!key) {
    socket.destroy();
    return;
  }
  const accept = crypto.createHash('sha1').update(key + WS_GUID).digest('base64');
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);

  if (activeSocket && activeSocket !== socket) {
    console.log('another browser connected; the earlier one is now ignored');
  } else {
    console.log('browser connected');
  }
  activeSocket = socket;

  let stash = Buffer.alloc(0);

  socket.on('data', (chunk) => {
    stash = Buffer.concat([stash, chunk]);
    // Minimal parser: masked client frames, payload <= 125 bytes,
    // which {"b":16777215} fits with room to spare.
    while (stash.length >= 6) {
      const op = stash[0] & 0x0f;
      const len = stash[1] & 0x7f;
      if (len > 125) { socket.destroy(); return; }
      if (stash.length < 6 + len) break;
      const mask = stash.subarray(2, 6);
      const payload = Buffer.from(stash.subarray(6, 6 + len));
      for (let i = 0; i < len; i++) payload[i] ^= mask[i & 3];
      stash = stash.subarray(6 + len);

      if (op === 0x8) { // close
        socket.end(Buffer.from([0x88, 0x00]));
        return;
      }
      if (op === 0x9) { // ping -> pong, unmasked from server
        socket.write(Buffer.concat([Buffer.from([0x8a, len]), payload]));
        continue;
      }
      if (op === 0x1) { // text: {"b":<mask>} plus optional axes
        if (socket !== activeSocket) continue; // a superseded tab
        try {
          const msg = JSON.parse(payload.toString('utf8'));
          if (typeof msg.b === 'number') {
            const was = pad.buttons;
            pad.buttons = msg.b >>> 0;
            if (verbose && pad.buttons !== was) {
              console.log(`${stamp()}  browser: ${describe(pad.buttons)}`);
            }
          }
          for (const k of ['lx', 'ly', 'rx', 'ry', 'lt', 'rt']) {
            if (typeof msg[k] === 'number') pad[k] = msg[k] | 0;
          }
          // The pad page announces itself so the descriptor can claim
          // analogue truthfully; the keyboard page never sends this.
          if (msg.desc) {
            descriptor = descriptorFrame(0, msg.desc.type | 0, 0, msg.desc.caps | 0);
            tick = 0; // send the new one immediately
            console.log(`descriptor: type ${msg.desc.type}, caps ${msg.desc.caps}`);
          }
        } catch (e) { /* ignore malformed */ }
      }
    }
  });

  let released = false;
  const release = () => {
    // Both 'close' and 'error' can fire for one disconnect, so this
    // guards itself rather than reporting twice.
    if (released) return;
    released = true;
    // Only the tab in charge may release: a stale one going away must
    // not clear the keys you are holding in the live one.
    if (socket !== activeSocket) {
      console.log('a superseded browser went away');
      return;
    }
    activeSocket = null;
    // A vanished browser must not leave anything held down.
    if (!headless) pad = { ...REST };
    console.log('browser gone; inputs released');
  };

  // 'error' MUST stay bound. A socket with no error listener makes Node
  // throw the event, which kills the whole server the moment a browser
  // goes away rudely: closing the laptop lid, a reload mid-frame, a
  // crashed tab all surface as ECONNRESET. Deduplicate in the handler,
  // never by leaving the event unhandled.
  socket.on('close', release);
  socket.on('error', release);
});

// --test needs no browser, so it opens no port. That is not just tidy:
// a browser tab left open on this port reconnects to whatever appears
// there, and a backgrounded one sends "nothing held" on blur, which
// silently overwrote the fixed pattern and failed the run. An
// automated test should not be reachable from a stray window.
if (headless) {
  console.log(`${mode} mode: no HTTP server, nothing can override the pattern`);
} else {
  server.listen(PORT, () => {
    console.log(`open http://localhost:${PORT} and press keys`);
  });
}
