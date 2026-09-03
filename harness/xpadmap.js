// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Neil Rackett
//
// @mesmotronic/xpad state to COMpad wire values.
//
// Shared verbatim by the browser page (a real pad, via esm.sh) and the
// headless simulator (a synthetic pad, via node_modules), so the thing
// a human tests and the thing CI tests cannot drift. Plain ESM, no
// imports: it must load in both.
//
// THE X/Y TRAP. The two xpad libraries agree on every button position
// and disagree on two letters, so map by POSITION and never by letter:
//
//   js-xpad          W3C index   physical    atarist-xpad
//   XpadButton.A         0        bottom     XPAD_SOUTH (= XPAD_A)
//   XpadButton.B         1        right      XPAD_EAST  (= XPAD_B)
//   XpadButton.X         2        LEFT       XPAD_WEST  (= XPAD_Y)  <--
//   XpadButton.Y         3        TOP        XPAD_NORTH (= XPAD_X)  <--
//
// js-xpad names X and Y after the Xbox legend, which is what the W3C
// standard mapping indices give you. The Linux kernel, which
// atarist-xpad follows, puts BTN_X at north and BTN_Y at west. So the
// letters cross over while the positions line up exactly. Wiring
// X to X here would put every face button one quarter turn out, and it
// would look plausible on screen.
//
// Verified by hand against @mesmotronic/xpad 1.2.1: pressing the button
// the library calls X sets W3C index 2, the left one. The pin has since
// moved to 1.3.0 without that check being repeated by hand, which is
// tolerable only because `make test-gamepad` drives whatever version is
// pinned and asserts the face buttons through the viewer.

// atarist-xpad button bits (src/xpad.h). Mirrored rather than parsed:
// the harness stands in for an adapter, and adapters know the wire
// protocol, not C headers.
export const XPAD = {
  UP: 0x00000001, DOWN: 0x00000002, LEFT: 0x00000004, RIGHT: 0x00000008,
  SOUTH: 0x00000010, EAST: 0x00000020, NORTH: 0x00000040, WEST: 0x00000080,
  TL: 0x00000100, TR: 0x00000200, TL2: 0x00000400, TR2: 0x00000800,
  SELECT: 0x00001000, START: 0x00002000, MODE: 0x00004000,
  THUMBL: 0x00008000, THUMBR: 0x00010000,
};

export const XPAD_TYPE_XBOX = 3;
export const XPAD_CAP_ANALOG = 0x0001;

// W3C standard mapping index to xpad bit, by position throughout.
// Indices 6 and 7 are the analogue triggers: they also carry a
// digitised bit, matching XPAD_TL2 / XPAD_TR2.
const BUTTON_BITS = [
  XPAD.SOUTH,  //  0  bottom face
  XPAD.EAST,   //  1  right face
  XPAD.WEST,   //  2  left face   (js-xpad calls this X)
  XPAD.NORTH,  //  3  top face    (js-xpad calls this Y)
  XPAD.TL,     //  4  left shoulder
  XPAD.TR,     //  5  right shoulder
  XPAD.TL2,    //  6  left trigger, digitised
  XPAD.TR2,    //  7  right trigger, digitised
  XPAD.SELECT, //  8  view / back
  XPAD.START,  //  9  menu / start
  XPAD.THUMBL, // 10  left stick click
  XPAD.THUMBR, // 11  right stick click
  XPAD.UP,     // 12  d-pad
  XPAD.DOWN,   // 13
  XPAD.LEFT,   // 14
  XPAD.RIGHT,  // 15
  XPAD.MODE,   // 16  guide / home
];

// js-xpad reports buttons as 0..1 floats, already thresholded by its
// own inputThreshold. Anything above zero is pressed as far as a
// digital bit is concerned.
const isDown = (v) => typeof v === 'number' && v > 0;

// -1..1 float to xpad's signed byte. Axis sense already matches:
// both libraries are screen oriented, +x right and +y down.
const axis = (v) => {
  const n = Math.round((typeof v === 'number' ? v : 0) * 127);
  return Math.max(-127, Math.min(127, n));
};

// 0..1 float to xpad's unsigned byte.
const trigger = (v) => {
  const n = Math.round((typeof v === 'number' ? v : 0) * 255);
  return Math.max(0, Math.min(255, n));
};

/**
 * Convert an @mesmotronic/xpad XpadState into the fields of a COMpad
 * state frame. Pure: no DOM, no Node, no library import.
 */
export function toCompadState(state) {
  const b = state.buttons || [];
  let buttons = 0;

  for (let i = 0; i < BUTTON_BITS.length; i++) {
    if (isDown(b[i])) buttons |= BUTTON_BITS[i];
  }

  const left = state.leftStick || { x: 0, y: 0 };
  const right = state.rightStick || { x: 0, y: 0 };

  return {
    buttons,
    lx: axis(left.x),
    ly: axis(left.y),
    rx: axis(right.x),
    ry: axis(right.y),
    lt: trigger(b[6]),
    rt: trigger(b[7]),
  };
}

/** COMpad state frame (type 0x0), 12 bytes. See docs/protocol.md. */
export function stateFrame(pad, s) {
  const f = new Uint8Array(12);
  f[0] = 0xa5;
  f[1] = ((pad & 3) << 4) | 0x0;
  f[2] = s.buttons & 0xff;
  f[3] = (s.buttons >>> 8) & 0xff;
  f[4] = (s.buttons >>> 16) & 0xff;
  f[5] = s.lx & 0xff;
  f[6] = s.ly & 0xff;
  f[7] = s.rx & 0xff;
  f[8] = s.ry & 0xff;
  f[9] = s.lt & 0xff;
  f[10] = s.rt & 0xff;
  let x = 0;
  for (let i = 0; i <= 10; i++) x ^= f[i];
  f[11] = x;
  return f;
}

/** COMpad descriptor frame (type 0xF), 7 bytes. */
export function descriptorFrame(pad, type, flags, caps) {
  const f = new Uint8Array(7);
  f[0] = 0xa5;
  f[1] = ((pad & 3) << 4) | 0xf;
  f[2] = type;
  f[3] = flags;
  f[4] = (caps >>> 8) & 0xff;
  f[5] = caps & 0xff;
  let x = 0;
  for (let i = 0; i <= 5; i++) x ^= f[i];
  f[6] = x;
  return f;
}
