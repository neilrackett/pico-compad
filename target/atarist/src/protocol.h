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

#define COMPAD_SYNC 0xA5     /* adapter to host            */
#define COMPAD_SYNC_REQ 0x5A /* host to adapter            */

#define COMPAD_PROTO_VERSION 0

/*
 * The line rate, here because this is the one header both ends
 * compile: rp/src/config.h and target/atarist/src/stport.h each derive
 * their own form from it, so the two ends cannot disagree. 19200 is
 * TOS's ceiling through Rsconf; 38400 needs a private MFP handler.
 */
#define COMPAD_BAUD 19200

/* Adapter to host, under COMPAD_SYNC. */
#define COMPAD_TYPE_STATE 0x0
#define COMPAD_TYPE_COMPACT 0x1
#define COMPAD_TYPE_DESCRIPTOR 0xF

/* Host to adapter, under COMPAD_SYNC_REQ. */
#define COMPAD_TYPE_PING 0x2
#define COMPAD_TYPE_REQUEST 0xE

#define COMPAD_PING_LEN 3
#define COMPAD_REQUEST_LEN 8

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

/* Length of an adapter-to-host frame, or 0 for a type that direction
 * does not carry. */
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

/* Length of a host-to-adapter frame. The adapter decodes these; the
 * ST only sends them, so nothing on that side needs this. */
static COMPAD_UNUSED uint8_t compad_req_len(uint8_t type)
{
    switch (type)
    {
    case COMPAD_TYPE_PING:
        return COMPAD_PING_LEN;
    case COMPAD_TYPE_REQUEST:
        return COMPAD_REQUEST_LEN;
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

/* ------------------------------------------------------------------ */

static COMPAD_UNUSED uint8_t compad_xor(const uint8_t *f, uint8_t len)
{
    uint8_t x = 0;
    uint8_t i;

    for (i = 0; i + 1 < len; i++)
        x ^= f[i];

    return x;
}

/*
 * Feed one received byte. Returns the frame's length when a complete,
 * checksum-verified frame sits in d->buf, otherwise 0. The frame is
 * valid until the next call.
 */
static COMPAD_UNUSED uint8_t compad_feed_sync(COMPAD_DECODER *d, uint8_t b,
                                              uint8_t sync)
{
    if (d->have == 0)
    {
        if (b != sync)
            return 0;

        d->buf[0] = b;
        d->have = 1;
        return 0;
    }

    if (d->have == 1)
    {
        d->need = (sync == COMPAD_SYNC) ? compad_frame_len(COMPAD_HDR_TYPE(b))
                                        : compad_req_len(COMPAD_HDR_TYPE(b));

        if (d->need == 0 || COMPAD_HDR_VERSION(b) != COMPAD_PROTO_VERSION)
        {
            /* Unknown type or a future version: drop it and hunt. The
             * byte may itself be the real sync of the next frame, so
             * treat it freshly rather than discarding it. */
            d->have = (b == sync) ? 1 : 0;
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

        d->have = 0;
        d->need = 0;

        if (compad_xor(d->buf, len) == d->buf[len - 1])
            return len;
    }

    /* Bad checksum: the frame is lost, which full-state-per-frame makes
     * survivable, and the hunt resumes with the next byte. */
    return 0;
}

/*
 * Feed one byte of the adapter-to-host direction. The sync byte is a
 * constant here so the compiler folds the branch above away, which
 * matters: the ST runs this per byte from a timer interrupt.
 */
static COMPAD_UNUSED uint8_t compad_feed(COMPAD_DECODER *d, uint8_t b)
{
    return compad_feed_sync(d, b, COMPAD_SYNC);
}

/* Feed one byte of the host-to-adapter direction. */
static COMPAD_UNUSED uint8_t compad_feed_req(COMPAD_DECODER *d, uint8_t b)
{
    return compad_feed_sync(d, b, COMPAD_SYNC_REQ);
}

/* ------------------------------------------------------------------ */
/* Building host-to-adapter frames. The ST sends these; the harness    */
/* and the tests build them too, so they live with the contract.       */

/* Writes COMPAD_PING_LEN bytes: "are you there?" */
static COMPAD_UNUSED void compad_ping_frame(uint8_t *f)
{
    f[0] = COMPAD_SYNC_REQ;
    f[1] = COMPAD_TYPE_PING;
    f[2] = compad_xor(f, COMPAD_PING_LEN);
}

/*
 * Writes COMPAD_REQUEST_LEN bytes. duration is in units of 10 ms, and
 * 0 means "until replaced", which is what xpad's request area implies:
 * it carries magnitudes and no duration, so a consumer that sets them
 * expects them to hold until it sets something else.
 */
static COMPAD_UNUSED void compad_request_frame(uint8_t pad, uint8_t lo,
                                               uint8_t hi, uint8_t duration,
                                               uint8_t led, uint8_t *f)
{
    f[0] = COMPAD_SYNC_REQ;
    f[1] = (uint8_t)(((pad & 3) << 4) | COMPAD_TYPE_REQUEST);
    f[2] = lo;
    f[3] = hi;
    f[4] = duration;
    f[5] = led;
    f[6] = 0; /* reserved */
    f[7] = compad_xor(f, COMPAD_REQUEST_LEN);
}

#define COMPAD_REQ_RUMBLE_LO(f) ((f)[2])
#define COMPAD_REQ_RUMBLE_HI(f) ((f)[3])
#define COMPAD_REQ_DURATION(f) ((f)[4])
#define COMPAD_REQ_LED(f) ((f)[5])

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
