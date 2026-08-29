// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Neil Rackett
//
// Host tests for the js-xpad to COMpad mapping. No emulator, no ST, no
// gamepad: pure functions in, bytes out. Run by `make test`.
//
// The cases that matter are the positional ones. A letter-for-letter
// mapping passes everything else in this file and fails here.

import { XPAD, toCompadState, stateFrame, descriptorFrame } from '../harness/xpadmap.js';

let failures = 0;

function check(ok, what) {
  console.log(`${ok ? '   ' : '!! '}${what}${ok ? '' : '   FAILED'}`);
  if (!ok) failures++;
}

function eq(got, want, what) {
  check(got === want, `${what}  (got ${got}, want ${want})`);
}

/** An XpadState as the library would build it, from pressed indices. */
function state(pressed = [], sticks = {}, analogue = {}) {
  return {
    buttons: Array.from({ length: 17 }, (_, i) =>
      i in analogue ? analogue[i] : (pressed.includes(i) ? 1 : 0)),
    leftStick: { x: sticks.lx || 0, y: sticks.ly || 0 },
    rightStick: { x: sticks.rx || 0, y: sticks.ry || 0 },
  };
}

console.log('js-xpad to COMpad mapping');

// ------------------------------------------------------------------
console.log('\nface buttons map by position, not by letter');

// The whole point. js-xpad calls W3C index 2 "X"; it is the LEFT face
// button, and atarist-xpad calls left WEST (which it aliases XPAD_Y).
eq(toCompadState(state([2])).buttons, XPAD.WEST,
   'index 2, the one js-xpad calls X, is WEST');
eq(toCompadState(state([3])).buttons, XPAD.NORTH,
   'index 3, the one js-xpad calls Y, is NORTH');
check(toCompadState(state([2])).buttons !== XPAD.NORTH,
      'index 2 is not NORTH (would be a letter-for-letter mapping)');
check(toCompadState(state([3])).buttons !== XPAD.WEST,
      'index 3 is not WEST (would be a letter-for-letter mapping)');

eq(toCompadState(state([0])).buttons, XPAD.SOUTH, 'index 0 is SOUTH');
eq(toCompadState(state([1])).buttons, XPAD.EAST, 'index 1 is EAST');

// ------------------------------------------------------------------
console.log('\nremaining buttons');

eq(toCompadState(state([4])).buttons, XPAD.TL, 'index 4 is TL');
eq(toCompadState(state([5])).buttons, XPAD.TR, 'index 5 is TR');
eq(toCompadState(state([8])).buttons, XPAD.SELECT, 'index 8 is SELECT');
eq(toCompadState(state([9])).buttons, XPAD.START, 'index 9 is START');
eq(toCompadState(state([10])).buttons, XPAD.THUMBL, 'index 10 is THUMBL');
eq(toCompadState(state([11])).buttons, XPAD.THUMBR, 'index 11 is THUMBR');
eq(toCompadState(state([12])).buttons, XPAD.UP, 'index 12 is UP');
eq(toCompadState(state([13])).buttons, XPAD.DOWN, 'index 13 is DOWN');
eq(toCompadState(state([14])).buttons, XPAD.LEFT, 'index 14 is LEFT');
eq(toCompadState(state([15])).buttons, XPAD.RIGHT, 'index 15 is RIGHT');
eq(toCompadState(state([16])).buttons, XPAD.MODE, 'index 16 is MODE');

eq(toCompadState(state([0, 15])).buttons, XPAD.SOUTH | XPAD.RIGHT,
   'buttons combine');

// ------------------------------------------------------------------
console.log('\naxes: -1..1 float to signed byte, +x right and +y down');

eq(toCompadState(state([], { lx: 1 })).lx, 127, 'lx +1 is +127');
eq(toCompadState(state([], { lx: -1 })).lx, -127, 'lx -1 is -127');
eq(toCompadState(state([], { lx: 0 })).lx, 0, 'lx centred is 0');
eq(toCompadState(state([], { ly: -0.25 })).ly, -32, 'ly -0.25 is -32');
eq(toCompadState(state([], { ry: 0.75 })).ry, 95, 'ry 0.75 is +95');

// Out of range input must not wrap into the wrong sign on the ST.
eq(toCompadState(state([], { lx: 2 })).lx, 127, 'lx clamps high');
eq(toCompadState(state([], { lx: -2 })).lx, -127, 'lx clamps low');

// Axes must not cross over each other.
const four = toCompadState(state([], { lx: 0.5, ly: -0.25, rx: -1, ry: 0.75 }));
check(four.lx === 64 && four.ly === -32 && four.rx === -127 && four.ry === 95,
      'the four axes stay in their own fields');

// ------------------------------------------------------------------
console.log('\ntriggers: 0..1 float to unsigned byte, plus a digitised bit');

eq(toCompadState(state([], {}, { 6: 1 })).lt, 255, 'lt full is 255');
eq(toCompadState(state([], {}, { 7: 0.25 })).rt, 64, 'rt quarter is 64');
eq(toCompadState(state([], {}, { 6: 0 })).lt, 0, 'lt released is 0');
eq(toCompadState(state([], {}, { 6: 0.25 })).buttons, XPAD.TL2,
   'a part-pulled left trigger still sets TL2');
eq(toCompadState(state([], {}, { 7: 0.25 })).buttons, XPAD.TR2,
   'a part-pulled right trigger still sets TR2');
eq(toCompadState(state([], {}, { 6: 0 })).buttons, 0,
   'a released trigger sets no bit');

// ------------------------------------------------------------------
console.log('\nframe encoding');

const s = toCompadState(state([2, 5, 9], { lx: 0.5, ly: -0.25, rx: -1, ry: 0.75 },
                              { 6: 0.25, 7: 1 }));
eq(s.buttons, 0x2e80, 'the test pose is 0x2e80');

const f = stateFrame(0, s);
eq(f.length, 12, 'a state frame is 12 bytes');
eq(f[0], 0xa5, 'sync');
eq(f[1], 0x00, 'header: version 0, pad 0, type state');
eq(f[2], 0x80, 'buttons low byte');
eq(f[3], 0x2e, 'buttons middle byte');
eq(f[4], 0x00, 'buttons high byte');
eq(f[6], 0xe0, 'ly -32 goes on the wire as twos complement 0xe0');
eq(f[9], 64, 'lt');
eq(f[10], 255, 'rt');

let x = 0;
for (let i = 0; i <= 10; i++) x ^= f[i];
eq(f[11], x, 'checksum is the XOR of bytes 0 to 10');

const d = descriptorFrame(0, 3, 0, 1);
eq(d.length, 7, 'a descriptor frame is 7 bytes');
eq(d[1], 0x0f, 'header: type descriptor');
eq(d[2], 3, 'type XBOX');
eq(d[5], 1, 'caps ANALOG in the low byte');

let dx = 0;
for (let i = 0; i <= 5; i++) dx ^= d[i];
eq(d[6], dx, 'descriptor checksum');

// ------------------------------------------------------------------
console.log(failures ? `\n${failures} check(s) failed` : '\nall checks passed');
process.exit(failures ? 1 : 0);
