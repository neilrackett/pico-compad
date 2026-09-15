/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * Firmware encoder tests, and the round trip that matters.
 *
 * The Pico encodes and the ST decodes, and the two are written in
 * different repositories' worth of style with nothing but docs/protocol.md
 * between them. So this builds both halves on the host, feeds every
 * frame the firmware can emit through the real decoder byte by byte,
 * and checks what comes out the far end. A change to either that the
 * other does not expect fails here rather than on a bench.
 *
 * encode.h includes the real xpad.h, Bluepad32 and protocol.h headers
 * rather than copying their constants, so there is nothing here that
 * asserts copies still match: that is now a compile rather than a check
 * somebody has to remember to write.
 */

#include <stdio.h>
#include <string.h>

#include "../rp/src/encode.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

/*
 * Feed a frame through the real decoder one byte at a time, as a UART
 * delivers it, and copy out what the decoder assembled.
 *
 * The copy is the point. Asserting on the buffer the encoder wrote
 * would check the encoder against itself and let a decoder that framed
 * correctly but reassembled into the wrong offsets pass every field
 * check below. test/protocol_test.c's run() takes the same shape for
 * the same reason.
 */
static uint8_t round_trip(COMPAD_DECODER *d, const uint8_t *f, uint8_t len,
                          uint8_t *out)
{
    uint8_t i, got = 0;

    for (i = 0; i < len; i++)
    {
        got = compad_feed(d, f[i]);

        if (got)
            memcpy(out, d->buf, got);
    }

    return got;
}

/* Every mapping check differs in one argument; the other six are zero. */
static uint32_t mapped(uint8_t dpad, uint16_t buttons, uint8_t misc)
{
    COMPAD_STATE s;

    compad_map(dpad, buttons, misc, 0, 0, 0, 0, 0, 0, &s);

    return s.buttons;
}

int main(void)
{
    COMPAD_DECODER d;
    COMPAD_STATE s, zero;
    uint8_t f[16];
    uint8_t got[16];
    uint8_t len;
    unsigned i;
    int clean;

    printf("compad firmware encode\n\n");

    /* The frame lengths are local to encode.h because they size arrays,
     * which the decoder's function cannot do. They still have to agree
     * with it. */
    check(CE_STATE_LEN == compad_frame_len(COMPAD_TYPE_STATE) &&
              CE_DESC_LEN == compad_frame_len(COMPAD_TYPE_DESCRIPTOR),
          "frame lengths agree with the decoder");

    /* Mapping, by position. */
    check(mapped(DPAD_UP, 0, 0) == XPAD_UP, "dpad up maps to XPAD_UP");

    check(mapped(0, BUTTON_A, 0) == XPAD_SOUTH,
          "Bluepad32 A is the bottom button");
    check(mapped(0, BUTTON_B, 0) == XPAD_EAST,
          "Bluepad32 B is the right button");
    check(mapped(0, BUTTON_X, 0) == XPAD_WEST,
          "Bluepad32 X is the LEFT button");
    check(mapped(0, BUTTON_Y, 0) == XPAD_NORTH,
          "Bluepad32 Y is the TOP button");

    /* The trap, stated as a test so nobody "fixes" the mapping. XPAD_Y
     * is an alias for XPAD_WEST, so this is the same check said in the
     * vocabulary somebody would reach for when breaking it. */
    check(mapped(0, BUTTON_X, 0) == XPAD_Y,
          "and Bluepad32 X therefore reads as XPAD_Y");

    check(mapped(0, 0, MISC_BUTTON_SELECT) == XPAD_SELECT,
          "misc select maps to XPAD_SELECT");
    check(mapped(0, 0, MISC_BUTTON_START) == XPAD_START,
          "misc start maps to XPAD_START");
    check(mapped(0, 0, MISC_BUTTON_SYSTEM) == XPAD_MODE,
          "misc system maps to XPAD_MODE");
    check(mapped(0, 0, MISC_BUTTON_CAPTURE) == 0,
          "misc capture has no bit and is dropped");

    /* Axes: a 1024 range down to Xpad's, with the negative end clamped. */
    compad_map(0, 0, 0, 0, 0, 0, 0, 0, 0, &s);
    check(s.lx == 0 && s.ly == 0 && s.rx == 0 && s.ry == 0,
          "a centred stick is zero");
    compad_map(0, 0, 0, 511, -512, 0, 0, 0, 0, &s);
    check(s.lx == 127, "511 is full right");
    check(s.ly == -127, "-512 clamps to -127 rather than -128");
    compad_map(0, 0, 0, 0, 0, 0, 0, 1023, 1023, &s);
    check(s.lt == 255 && s.rt == 255, "1023 is a fully pulled trigger");

    clean = 1;
    for (i = 0; i <= 0xffff; i++)
    {
        compad_map(0xff, (uint16_t)i, 0xff, 0, 0, 0, 0, 0, 0, &s);
        if (s.buttons & ~CE_MAPPED)
            clean = 0;
    }
    check(clean, "no input sets a bit outside the mapped set");

    /* Round trip: encode, then decode with the ST's own decoder. */
    compad_init(&d);

    compad_map(DPAD_LEFT, BUTTON_A | BUTTON_SHOULDER_R, MISC_BUTTON_START,
               -200, 100, 60, -60, 512, 256, &s);
    compad_state_frame(1, &s, f);
    len = CE_STATE_LEN;
    check(round_trip(&d, f, len, got) == 12, "and the decoder accepts it");
    check(COMPAD_HDR_PAD(got[1]) == 1, "the pad index survives");
    check(COMPAD_HDR_TYPE(got[1]) == COMPAD_TYPE_STATE, "so does the type");
    check(COMPAD_HDR_VERSION(got[1]) == COMPAD_PROTO_VERSION,
          "and the version is the one the decoder expects");
    check(compad_state_buttons(got) == s.buttons, "the buttons survive");
    check(COMPAD_STATE_LX(got) == s.lx && COMPAD_STATE_LY(got) == s.ly &&
              COMPAD_STATE_RX(got) == s.rx && COMPAD_STATE_RY(got) == s.ry,
          "the axes survive, signs included");
    check(COMPAD_STATE_LT(got) == s.lt && COMPAD_STATE_RT(got) == s.rt,
          "and the triggers survive");

    /* Every button bit, one at a time, all the way to the far end. */
    clean = 1;
    for (i = 0; i < 24; i++)
    {
        uint32_t want = 1UL << i;

        if (!(want & CE_MAPPED))
            continue;

        memset(&s, 0, sizeof(s));
        s.buttons = want;
    compad_state_frame(0, &s, f);
    len = CE_STATE_LEN;

        if (round_trip(&d, f, len, got) != 12 || compad_state_buttons(got) != want)
            clean = 0;
    }
    check(clean, "every mapped bit round trips on its own");

    /* Extremes of every axis, since sign is the easy thing to lose. */
    clean = 1;
    {
        static const int8_t vals[] = {-127, -1, 0, 1, 127};
        unsigned v;

        for (v = 0; v < sizeof(vals) / sizeof(vals[0]); v++)
        {
            memset(&s, 0, sizeof(s));
            s.lx = s.ly = s.rx = s.ry = vals[v];
            s.lt = s.rt = (uint8_t)(v * 60);
    compad_state_frame(0, &s, f);
    len = CE_STATE_LEN;

            if (round_trip(&d, f, len, got) != 12 ||
                COMPAD_STATE_LX(got) != vals[v] ||
                COMPAD_STATE_RY(got) != vals[v] ||
                COMPAD_STATE_LT(got) != (uint8_t)(v * 60))
                clean = 0;
        }
    }
    check(clean, "axis and trigger extremes round trip");

    /* Descriptor. XPAD_TYPE_GAMEPAD is 2, as the harness sends. */
    compad_descriptor_frame(0, XPAD_TYPE_GAMEPAD, 0, 0, f);
    len = CE_DESC_LEN;
    check(round_trip(&d, f, len, got) == 7, "and the decoder accepts it");
    check(COMPAD_DESC_TYPE(got) == XPAD_TYPE_GAMEPAD, "the pad type survives");

    compad_descriptor_frame(0, XPAD_TYPE_GAMEPAD, 0, 0xBEEF, f);
    len = CE_DESC_LEN;
    check(round_trip(&d, f, len, got) == 7 && COMPAD_DESC_CAPS(got) == 0xBEEF,
          "caps survive, high byte first");

    /* A corrupted frame must be rejected, or the checksum is decoration. */
    memset(&s, 0, sizeof(s));
    s.buttons = XPAD_SOUTH;
    compad_state_frame(0, &s, f);
    len = CE_STATE_LEN;
    f[2] ^= 0x01;
    check(round_trip(&d, f, len, got) == 0, "a flipped bit fails the checksum");

    /* And the decoder must resynchronise afterwards. */
    compad_state_frame(0, &s, f);
    len = CE_STATE_LEN;
    check(round_trip(&d, f, len, got) == 12, "the next good frame still arrives");

    /* Change detection, which decides what goes on the wire at all. */
    memset(&zero, 0, sizeof(zero));
    memset(&s, 0, sizeof(s));
    check(!compad_state_differs(&s, &zero), "two rest states are the same");
    s.rt = 1;
    check(compad_state_differs(&s, &zero), "one trigger count is a change");
    s.rt = 0;
    s.buttons = XPAD_UP;
    check(compad_state_differs(&s, &zero), "so is one button");

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");

    return failures ? 1 : 0;
}
