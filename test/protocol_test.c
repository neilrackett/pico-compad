/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * COMpad decoder tests, on the host. The decoder is the logic worth
 * getting wrong, and it is TOS-free precisely so this file needs no
 * emulator: every framing, checksum and resync behaviour is pinned
 * here, and the ST only has to prove the bytes arrive.
 */

#include <stdio.h>
#include <string.h>

#include "../target/atarist/src/protocol.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

/* Build a valid frame of the given type into out, returns length. */
/*
 * Build a frame the way docs/protocol.md describes one, not the way
 * protocol.h builds one.
 *
 * The XOR below is deliberately a second implementation rather than a
 * call to compad_xor(): a test that computed the checksum with the
 * code under test would let a wrong checksum rule agree with itself
 * and pass. Same reason the harness checks its WebSocket handshake
 * against RFC 6455's published vector rather than against its own
 * output. Do not deduplicate this.
 */
static uint8_t build(uint8_t *out, uint8_t type, uint8_t pad,
                     const uint8_t *body, uint8_t body_len)
{
    uint8_t len = compad_frame_len(type);
    uint8_t x = 0;
    uint8_t i;

    out[0] = COMPAD_SYNC;
    out[1] = (uint8_t)((pad << 4) | type);

    for (i = 0; i < body_len; i++)
        out[2 + i] = body[i];

    for (i = 0; i + 1 < len; i++)
        x ^= out[i];

    out[len - 1] = x;

    return len;
}

/* Feed a byte string; count delivered frames, remember the last one. */
static int run(COMPAD_DECODER *d, const uint8_t *bytes, int n,
               uint8_t *last, uint8_t *last_len)
{
    int frames = 0;
    int i;

    for (i = 0; i < n; i++)
    {
        uint8_t got = compad_feed(d, bytes[i]);

        if (got)
        {
            frames++;
            if (last)
                memcpy(last, d->buf, got);
            if (last_len)
                *last_len = got;
        }
    }

    return frames;
}

int main(void)
{
    COMPAD_DECODER d;
    uint8_t frame[COMPAD_MAX_FRAME];
    uint8_t stream[64];
    uint8_t last[COMPAD_MAX_FRAME];
    uint8_t last_len = 0;
    uint8_t state_body[9] = {0x18, 0x00, 0x01, /* buttons          */
                             0x64, 0xce, 0x19, 0x00, /* lx ly rx ry */
                             0xff, 0x00}; /* lt rt; ry folded above  */
    int n;

    printf("COMpad decoder\n\n");

    compad_init(&d);

    /* A clean state frame, byte at a time. */
    n = build(frame, COMPAD_TYPE_STATE, 2, state_body, 9);
    check(n == 12, "state frames are 12 bytes");
    check(run(&d, frame, n, last, &last_len) == 1, "clean frame decodes");
    check(last_len == 12, "with the right length");
    check(COMPAD_HDR_PAD(last[1]) == 2, "pad index survives");
    check(compad_state_buttons(last) == 0x00010018UL,
          "buttons reassemble across three bytes");
    check(COMPAD_STATE_LX(last) == 100 && COMPAD_STATE_LY(last) == -50,
          "axes read signed");
    check(COMPAD_STATE_LT(last) == 255 && COMPAD_STATE_RT(last) == 0,
          "triggers read unsigned");

    /* Garbage before the sync is skipped. */
    memset(stream, 0x42, 7);
    memcpy(stream + 7, frame, 12);
    check(run(&d, stream, 19, 0, 0) == 1, "leading garbage is skipped");

    /* A corrupted frame is dropped, and the stream recovers. */
    memcpy(stream, frame, 12);
    stream[5] ^= 0xff; /* damage an axis, checksum now wrong */
    memcpy(stream + 12, frame, 12);
    check(run(&d, stream, 24, 0, 0) == 1,
          "bad checksum drops one frame, next one lands");

    /* A false sync inside data self-corrects. */
    memset(stream, 0, 24);
    stream[0] = COMPAD_SYNC; /* looks like sync            */
    stream[1] = 0x00;        /* looks like a state header  */
    /* ...ten bytes of zeros make a wrong checksum for it   */
    memcpy(stream + 12, frame, 12);
    check(run(&d, stream, 24, 0, 0) == 1,
          "false sync costs at most one frame");

    /* Compact and descriptor frames. */
    {
        uint8_t body[2] = {0x0f, 0x00};

        n = build(frame, COMPAD_TYPE_COMPACT, 0, body, 2);
        check(n == 5, "compact frames are 5 bytes");
        check(run(&d, frame, n, last, &last_len) == 1 && last_len == 5,
              "compact frame decodes");
        check(compad_compact_buttons(last) == 0x0f, "compact buttons read");
    }
    {
        uint8_t body[4] = {3, 0x03, 0x00, 0x09}; /* type flags caps */

        n = build(frame, COMPAD_TYPE_DESCRIPTOR, 1, body, 4);
        check(n == 7, "descriptor frames are 7 bytes");
        check(run(&d, frame, n, last, &last_len) == 1 && last_len == 7,
              "descriptor frame decodes");
        check(COMPAD_DESC_TYPE(last) == 3 && COMPAD_DESC_FLAGS(last) == 0x03,
              "descriptor fields read");
        check(COMPAD_DESC_CAPS(last) == 0x0009, "caps reassemble high first");
    }

    /* Frames this side must never accept. */
    n = build(frame, COMPAD_TYPE_STATE, 0, state_body, 9);
    frame[0] = COMPAD_SYNC_REQ; /* request-direction sync */
    check(run(&d, frame, 12, 0, 0) == 0, "request-direction sync is ignored");

    n = build(frame, COMPAD_TYPE_STATE, 0, state_body, 9);
    frame[1] |= 0x40; /* version 1 */
    check(run(&d, frame, 12, 0, 0) == 0, "future protocol version refused");

    /* An unknown type whose header byte is itself a sync restarts
     * cleanly rather than being thrown away. */
    memset(stream, 0, 32);
    stream[0] = COMPAD_SYNC;
    stream[1] = COMPAD_SYNC; /* type 0x5: unknown, but also a sync */
    n = build(frame, COMPAD_TYPE_STATE, 0, state_body, 9);
    memcpy(stream + 2, frame + 1, 11); /* rest of a real frame */
    /* stream[1] serves as the real frame's sync */
    check(run(&d, stream, 13, 0, 0) == 1,
          "a sync byte in the header slot restarts the frame");

    /* ---------------------------------------------------------- */
    /* Host to adapter: the other direction, and its own sync byte  */
    /* ---------------------------------------------------------- */

    {
        COMPAD_DECODER r;
        uint8_t q[COMPAD_REQUEST_LEN];
        uint8_t got, i;

        compad_init(&r);

        /* A ping round trips through the decoder the adapter uses. */
        compad_ping_frame(q);
        check(q[0] == COMPAD_SYNC_REQ, "a ping carries the request sync");
        check(COMPAD_HDR_TYPE(q[1]) == COMPAD_TYPE_PING, "and the ping type");

        got = 0;
        for (i = 0; i < COMPAD_PING_LEN; i++)
            got = compad_feed_req(&r, q[i]);
        check(got == COMPAD_PING_LEN, "the adapter's decoder accepts it");

        /* A request carries the magnitudes and survives. */
        compad_request_frame(2, 200, 100, 0, 3, q);
        got = 0;
        for (i = 0; i < COMPAD_REQUEST_LEN; i++)
            got = compad_feed_req(&r, q[i]);
        check(got == COMPAD_REQUEST_LEN, "a request frame decodes");
        check(COMPAD_HDR_PAD(r.buf[1]) == 2, "the pad index survives");
        check(COMPAD_REQ_RUMBLE_LO(r.buf) == 200 &&
                  COMPAD_REQ_RUMBLE_HI(r.buf) == 100,
              "both motors survive");
        check(COMPAD_REQ_LED(r.buf) == 3, "and the led index");

        /* A flipped bit is caught here as it is the other way. */
        compad_request_frame(0, 1, 2, 0, 0, q);
        q[2] ^= 0x01;
        got = 0;
        for (i = 0; i < COMPAD_REQUEST_LEN; i++)
            got = compad_feed_req(&r, q[i]);
        check(got == 0, "a corrupted request fails its checksum");

        /*
         * The two directions must not read each other's frames. An
         * adapter that accepted 0xA5 would hear its own echo, and a
         * provider that accepted 0x5A would hear its own requests on
         * a wire that loops back.
         */
        compad_ping_frame(q);
        compad_init(&r);
        got = 0;
        for (i = 0; i < COMPAD_PING_LEN; i++)
            got = compad_feed(&r, q[i]);
        check(got == 0, "the host direction ignores request frames");

        check(compad_frame_len(COMPAD_TYPE_PING) == 0,
              "and does not know the ping length");
        check(compad_req_len(COMPAD_TYPE_STATE) == 0,
              "nor the adapter direction a state frame");
    }

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");

    return failures ? 1 : 0;
}
