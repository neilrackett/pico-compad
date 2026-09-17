// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Neil Rackett
//
// Open a serial port for raw bytes. One place, because the rules are
// easy to get wrong and were: listen.js copied wire.js's comment about
// the ordering and still did it the wrong way round.
//
// Open first, then configure. macOS resets a port when the last fd
// closes, so an stty that runs before the open is undone the moment it
// exits and the reader gets the default 9600. The symptom is a stream
// of plausible garbage rather than an error.

import fs from 'node:fs';
import { execFileSync } from 'node:child_process';

// BSD stty takes the device as -f, GNU stty as -F, and each rejects the
// other's spelling outright.
const DEVICE_FLAG = process.platform === 'darwin' ? '-f' : '-F';

/**
 * Returns an fd on `port` at `baud`, 8N1, no flow control, raw.
 * Non-blocking, so the open itself cannot hang waiting for carrier and
 * a read with nothing waiting raises EAGAIN rather than blocking.
 */
export function openSerial(port, baud, { write = false } = {}) {
  const mode = (write ? fs.constants.O_RDWR : fs.constants.O_RDONLY) |
    fs.constants.O_NOCTTY | fs.constants.O_NONBLOCK;
  const fd = fs.openSync(port, mode);

  execFileSync('stty', [
    DEVICE_FLAG, port, String(baud),
    'cs8', '-cstopb', '-parenb', '-crtscts',
    'raw', '-echo', '-hupcl',
  ]);

  return fd;
}
