#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Neil Rackett
//
// Write one fixed COMpad state frame to a FIFO at 50 Hz, so the ST side
// has something to decode before any real input exists. Used by
// test-serial (roadmap phase 0). Plain Node, no dependencies.
//
// Usage: node frames.js <fifo>

import fs from 'node:fs';

// One definition of the wire format and the button bits, shared with
// the browser pages and the simulator. See harness/xpadmap.js.
import { XPAD, stateFrame } from './xpadmap.js';

// The same deliberately asymmetric values the xpad test stub uses, so a
// wrong byte shows up as a wrong number rather than a plausible one.
const frame = stateFrame(0, {
  buttons: XPAD.RIGHT | XPAD.SOUTH | XPAD.TR,
  lx: 100, ly: -50, rx: 25, ry: 0, lt: 255, rt: 0,
});

const path = process.argv[2];
if (!path) {
  console.error('usage: frames.js <fifo>');
  process.exit(2);
}

// Opening a FIFO for writing blocks (or, with O_NONBLOCK, fails with
// ENXIO) until a reader attaches, so retry: the harness then works
// whether it starts before or after Hatari.
(function open(attempt) {
  fs.open(path, fs.constants.O_WRONLY | fs.constants.O_NONBLOCK, (err, fd) => {
    if (err) {
      if (attempt === 0) console.log(`waiting for a reader on ${path}`);
      setTimeout(() => open(attempt + 1), 200);
      return;
    }
    console.log('reader attached; writing state frames at 50 Hz');
    setInterval(() => {
      try {
        fs.writeSync(fd, frame);
      } catch (e) {
        // Reader gone mid-run (Hatari quit): nothing useful to do but
        // keep trying, matching the transport's stateless design.
      }
    }, 20);
  });
})(0);
