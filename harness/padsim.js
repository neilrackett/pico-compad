#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Neil Rackett
//
// A synthetic gamepad driven through the real @mesmotronic/xpad
// library, out as COMpad frames. Drives make test-gamepad.
//
// The point is that nothing here fakes the library. A stub Gamepad is
// installed where the Gamepad API would be, and the actual npm package
// reads it, thresholds it and builds its XpadState exactly as it would
// for a pad in someone's hands. What this proves that the keyboard
// harness cannot:
//
//   - the positional button mapping, X/Y trap included (xpadmap.js)
//   - signed axes surviving the wire and the 68000's byte order
//   - unsigned triggers, and their digitised TL2/TR2 bits
//   - a descriptor claiming XPAD_CAP_ANALOG, honestly this time
//
//   node harness/padsim.js <fifo> [--hold]
//
// --hold freezes the pose the end-to-end test asserts, instead of
// cycling, so the run is not a race against the sequence.

import fs from 'node:fs';
import { Xpad } from '@mesmotronic/xpad';
import {
  toCompadState, stateFrame, descriptorFrame,
  XPAD_TYPE_XBOX, XPAD_CAP_ANALOG,
} from './xpadmap.js';

const fifoPath = process.argv[2];
const hold = process.argv.includes('--hold');

if (!fifoPath) {
  console.error('usage: padsim.js <fifo> [--hold]');
  process.exit(2);
}

// ------------------------------------------------------------------
// A synthetic pad, in W3C standard mapping order

const pressed = new Set();
let axes = [0, 0, 0, 0];
let analogue = new Map(); // index -> 0..1, for the triggers

const gamepad = {
  index: 0,
  id: 'COMpad simulated pad (standard)',
  connected: true,
  mapping: 'standard',
  timestamp: 0,
  get axes() { return axes; },
  get buttons() {
    return Array.from({ length: 17 }, (_, i) => {
      const value = analogue.has(i) ? analogue.get(i) : (pressed.has(i) ? 1 : 0);
      return { pressed: value > 0, touched: value > 0, value };
    });
  },
};

// The library reads navigator.getGamepads() and listens for connect
// events on window. Node has neither, so they are stubbed here: this
// is how the synthetic pad gets injected at all, not a workaround for
// anything the package could fix. navigator is getter-only on modern
// Node, so define rather than assign.
Object.defineProperty(globalThis, 'navigator', {
  value: { getGamepads: () => [gamepad] },
  configurable: true,
  writable: true,
});
globalThis.window = globalThis.window || {
  addEventListener() {}, removeEventListener() {},
};
globalThis.addEventListener = globalThis.addEventListener || (() => {});
globalThis.removeEventListener = globalThis.removeEventListener || (() => {});

const xpad = new Xpad(0);

// ------------------------------------------------------------------
// The pose the end-to-end test asserts
//
// Deliberately asymmetric, so a swapped or duplicated field shows up as
// a wrong number rather than a plausible one: every axis differs, the
// two triggers differ, and the face button is the one that catches an
// X/Y mix-up. W3C index 2 is the LEFT face button, which must arrive as
// XPAD_WEST (0x80). Wire it by letter and it lands on 0x40 instead.

const POSE = {
  buttons: [2, 5, 9],            // left face, right shoulder, menu
  axes: [0.5, -0.25, -1, 0.75],  // lx ly rx ry
  triggers: { 6: 0.25, 7: 1 },   // lt rt
};

function applyPose(p) {
  pressed.clear();
  analogue = new Map();
  for (const b of p.buttons) pressed.add(b);
  for (const [i, v] of Object.entries(p.triggers || {})) {
    analogue.set(Number(i), v);
    if (v > 0) pressed.add(Number(i));
  }
  axes = p.axes.slice();
}

// A short cycle for the hands-off case, so a human watching the viewer
// sees movement rather than a still frame.
const SEQUENCE = [
  POSE,
  { buttons: [12], axes: [0, -1, 0, 0], triggers: {} },        // d-pad up
  { buttons: [0], axes: [1, 0, 0, 0], triggers: { 7: 0.5 } },  // A, stick right
  { buttons: [], axes: [0, 0, 0, 0], triggers: {} },           // released
];

applyPose(POSE);

if (hold) {
  console.log('simulated pad: holding the test pose');
} else {
  let step = 0;
  setInterval(() => {
    step = (step + 1) % SEQUENCE.length;
    applyPose(SEQUENCE[step]);
  }, 1000);
  console.log('simulated pad: cycling a short sequence');
}

// ------------------------------------------------------------------
// 50 Hz out

// Re-sent periodically rather than once. The provider discards the
// serial ring when it installs, so a descriptor sent before the ST
// finished booting is gone: on real hardware the adapter has no idea
// when the machine came up, or whether it was reset since. Cheap
// insurance, and it keeps the transport stateless the way the state
// frames already are.
//
// Five a second rather than one, because a consumer that has just
// started should not spend up to a second reporting the wrong pad
// type. At 7 bytes it costs 35 bytes/sec, under 4% of a 9600 link.
// The alternative, having the ST ask for one over the back channel,
// waits on phase 4.
const DESCRIPTOR_EVERY = 10; // ticks, so five times a second at 50 Hz
const KEEPALIVE_EVERY = 12;  // ticks, so about every 250 ms
let tick = 0;

// Send on change, plus a keepalive, exactly as server.js does and as
// docs/protocol.md specifies. Full state at 50 Hz is 63% of a 9600
// link with a pad that is idle almost all the time; the shortfall on
// any slower receiver queues up as latency.
let lastSent = null;
let sinceSend = 0;

const changed = (a, b) => !b ||
  a.buttons !== b.buttons || a.lx !== b.lx || a.ly !== b.ly ||
  a.rx !== b.rx || a.ry !== b.ry || a.lt !== b.lt || a.rt !== b.rt;

(function openFifo(attempt) {
  fs.open(fifoPath, fs.constants.O_WRONLY | fs.constants.O_NONBLOCK, (err, fd) => {
    if (err) {
      if (attempt === 0) console.log(`waiting for a reader on ${fifoPath}`);
      setTimeout(() => openFifo(attempt + 1), 200);
      return;
    }

    console.log('reader attached; streaming at 50 Hz');

    setInterval(() => {
      // Drive the real library, then map its state positionally.
      gamepad.timestamp = (gamepad.timestamp + 1) & 0xffff;
      xpad.update();
      const s = toCompadState(xpad.state);

      try {
        if (tick % DESCRIPTOR_EVERY === 0) {
          // Analogue is claimed here because it is now true: axes and
          // triggers carry real values, unlike the keyboard harness.
          fs.writeSync(fd, descriptorFrame(0, XPAD_TYPE_XBOX, 0, XPAD_CAP_ANALOG));
        }
        tick++;

        if (changed(s, lastSent) || ++sinceSend >= KEEPALIVE_EVERY) {
          fs.writeSync(fd, stateFrame(0, s));
          lastSent = s;
          sinceSend = 0;
        }
      } catch (e) {
        // Reader gone: full state per frame means just keep trying.
      }
    }, 20);
  });
})(0);
