/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * COMpad wire protocol: reference decoder.
 *
 * Byte-at-a-time, so it sits behind any UART regardless of how bytes
 * arrive, and free of platform headers so the host build tests it.
 * docs/protocol.md is the contract this implements;
 * change them together or not at all.
 *
 * Recovery model, per docs/protocol.md's principles: every frame carries full
 * state, so nothing here retries or negotiates. A checksum failure
 * drops the frame and the decoder hunts for the next sync byte; a false
 * sync therefore costs at most a frame or two before real traffic lines
 * back up.
 */

#ifndef COMPAD_PROTOCOL_H
#define COMPAD_PROTOCOL_H

#include <stdint.h>

#define COMPAD_SYNC 0xA5     /* adapter to host frames     */
#define COMPAD_SYNC_REQ 0x5A /* host to adapter, not ours  */

#define COMPAD_PROTO_VERSION 0

/*
 * The line rate, here because this is the one header both ends
 * compile: rp/src/config.h and target/atarist/src/stport.h each derive
 * their own form from it, so the two ends cannot disagree. 19200 is
 * TOS's ceiling through Rsconf; 38400 needs a private MFP handler.
 */
#define COMPAD_BAUD 19200

#define COMPAD_TYPE_STATE 0x0
#define COMPAD_TYPE_COMPACT 0x1
#define COMPAD_TYPE_REQUEST 0xE
#define COMPAD_TYPE_DESCRIPTOR 0xF

#define COMPAD_MAX_FRAME 12

/* Every function here is static in a shared header, so any one includer
 * uses a subset and the rest must not trip -Werror. */
#ifdef __GNUC__
#define COMPAD_UNUSED __attribute__((unused))
#else
#define COMPAD_UNUSED
#endif

#define COMPAD_HDR_VERSION(h) ((uint8_t)((h) >> 6))
#define COMPAD_HDR_PAD(h) ((uint8_t)(((h) >> 4) & 3))
#define COMPAD_HDR_TYPE(h) ((uint8_t)((h) & 0x0f))

/* Frame length for a type, or 0 for one this side never receives:
 * requests travel the other way under their own sync byte. */
static COMPAD_UNUSED uint8_t compad_frame_len(uint8_t type)
{
    switch (type)
    {
    case COMPAD_TYPE_STATE:
        return 12;
    case COMPAD_TYPE_COMPACT:
        return 5;
    case COMPAD_TYPE_DESCRIPTOR:
        return 7;
    default:
        return 0;
    }
}

typedef struct
{
    uint8_t buf[COMPAD_MAX_FRAME];
    uint8_t have; /* bytes collected; 0 means hunting for sync */
    uint8_t need; /* full frame length once the header is in   */
} COMPAD_DECODER;

static COMPAD_UNUSED void compad_init(COMPAD_DECODER *d)
{
    d->have = 0;
    d->need = 0;
}

/*
 * Feed one received byte. Returns the frame's length when a complete,
 * checksum-verified frame sits in d->buf, otherwise 0. The frame is
 * valid until the next call.
 */
static COMPAD_UNUSED uint8_t compad_feed(COMPAD_DECODER *d, uint8_t b)
{
    if (d->have == 0)
    {
        if (b != COMPAD_SYNC)
            return 0;

        d->buf[0] = b;
        d->have = 1;
        return 0;
    }

    if (d->have == 1)
    {
        d->need = compad_frame_len(COMPAD_HDR_TYPE(b));

        if (d->need == 0 || COMPAD_HDR_VERSION(b) != COMPAD_PROTO_VERSION)
        {
            /* Unknown type or a future version: drop it and hunt. The
             * byte may itself be the real sync of the next frame, so
             * treat it freshly rather than discarding it. */
            d->have = (b == COMPAD_SYNC) ? 1 : 0;
            return 0;
        }

        d->buf[1] = b;
        d->have = 2;
        return 0;
    }

    d->buf[d->have++] = b;

    if (d->have < d->need)
        return 0;

    {
        uint8_t len = d->need;
        uint8_t x = 0;
        uint8_t i;

        d->have = 0;
        d->need = 0;

        for (i = 0; i + 1 < len; i++)
            x ^= d->buf[i];

        if (x == d->buf[len - 1])
            return len;
    }

    /* Bad checksum: the frame is lost, which full-state-per-frame makes
     * survivable, and the hunt resumes with the next byte. */
    return 0;
}

/* Field accessors for a delivered frame. Byte-wide reassembly, so the
 * consumer's endianness never enters into it. */

static COMPAD_UNUSED uint32_t compad_state_buttons(const uint8_t *f)
{
    return (uint32_t)f[2] | ((uint32_t)f[3] << 8) | ((uint32_t)f[4] << 16);
}

#define COMPAD_STATE_LX(f) ((int8_t)(f)[5])
#define COMPAD_STATE_LY(f) ((int8_t)(f)[6])
#define COMPAD_STATE_RX(f) ((int8_t)(f)[7])
#define COMPAD_STATE_RY(f) ((int8_t)(f)[8])
#define COMPAD_STATE_LT(f) ((f)[9])
#define COMPAD_STATE_RT(f) ((f)[10])

static COMPAD_UNUSED uint32_t compad_compact_buttons(const uint8_t *f)
{
    return (uint32_t)f[2] | ((uint32_t)f[3] << 8);
}

#define COMPAD_DESC_TYPE(f) ((f)[2])
#define COMPAD_DESC_FLAGS(f) ((f)[3])
#define COMPAD_DESC_CAPS(f) ((uint16_t)(((uint16_t)(f)[4] << 8) | (f)[5]))

#endif /* COMPAD_PROTOCOL_H */
