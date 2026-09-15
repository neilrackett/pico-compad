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
 * It also asserts the mirrored constants in rp/src/encode.h still match
 * the real xpad.h and Bluepad32 headers, since mirroring them is what
 * keeps the firmware's logic free of both dependencies.
 */

#include <stdio.h>
#include <string.h>

#include "../rp/src/encode.h"
#include "../target/atarist/src/protocol.h"
#include "../lib/xpad/src/xpad.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

/* Feed a frame through the real decoder one byte at a time, as a UART
 * delivers it, and return the length it reports. */
static uint8_t round_trip(COMPAD_DECODER *d, const uint8_t *f, uint8_t len)
{
    uint8_t i, got = 0;

    for (i = 0; i < len; i++)
        got = compad_feed(d, f[i]);

    return got;
}

int main(void)
{
    COMPAD_DECODER d;
    COMPAD_STATE s, zero;
    uint8_t f[16];
    uint8_t len;
    unsigned i;
    int clean;

    printf("compad firmware encode\n\n");

    /* The mirrored constants have to still be the real ones. */
    check(CE_UP == XPAD_UP && CE_DOWN == XPAD_DOWN && CE_LEFT == XPAD_LEFT &&
              CE_RIGHT == XPAD_RIGHT,
          "mirrored d-pad bits match xpad.h");
    check(CE_SOUTH == XPAD_SOUTH && CE_EAST == XPAD_EAST &&
              CE_NORTH == XPAD_NORTH && CE_WEST == XPAD_WEST,
          "mirrored face bits match xpad.h");
    check(CE_TL == XPAD_TL && CE_TR == XPAD_TR && CE_TL2 == XPAD_TL2 &&
              CE_TR2 == XPAD_TR2,
          "mirrored shoulder bits match xpad.h");
    check(CE_SELECT == XPAD_SELECT && CE_START == XPAD_START &&
              CE_MODE == XPAD_MODE && CE_THUMBL == XPAD_THUMBL &&
              CE_THUMBR == XPAD_THUMBR,
          "mirrored menu and thumb bits match xpad.h");
    check(CE_SYNC == COMPAD_SYNC && CE_TYPE_STATE == COMPAD_TYPE_STATE &&
              CE_TYPE_DESCRIPTOR == COMPAD_TYPE_DESCRIPTOR,
          "frame constants match the decoder");
    check(CE_STATE_LEN == compad_frame_len(COMPAD_TYPE_STATE) &&
              CE_DESC_LEN == compad_frame_len(COMPAD_TYPE_DESCRIPTOR),
          "frame lengths match the decoder");

    /* Mapping, by position. */
    memset(&s, 0, sizeof(s));
    compad_map(BP_DPAD_UP, 0, 0, 0, 0, 0, 0, 0, 0, &s);
    check(s.buttons == XPAD_UP, "dpad up maps to XPAD_UP");

    compad_map(0, BP_BUTTON_A, 0, 0, 0, 0, 0, 0, 0, &s);
    check(s.buttons == XPAD_SOUTH, "Bluepad32 A is the bottom button");
    compad_map(0, BP_BUTTON_B, 0, 0, 0, 0, 0, 0, 0, &s);
    check(s.buttons == XPAD_EAST, "Bluepad32 B is the right button");
    compad_map(0, BP_BUTTON_X, 0, 0, 0, 0, 0, 0, 0, &s);
    check(s.buttons == XPAD_WEST, "Bluepad32 X is the LEFT button");
    compad_map(0, BP_BUTTON_Y, 0, 0, 0, 0, 0, 0, 0, &s);
    check(s.buttons == XPAD_NORTH, "Bluepad32 Y is the TOP button");

    /* The trap, stated as a test so nobody "fixes" the mapping. */
    compad_map(0, BP_BUTTON_X, 0, 0, 0, 0, 0, 0, 0, &s);
    check(s.buttons == XPAD_Y, "and Bluepad32 X therefore reads as XPAD_Y");

    compad_map(0, 0, BP_MISC_SELECT, 0, 0, 0, 0, 0, 0, &s);
    check(s.buttons == XPAD_SELECT, "misc select maps to XPAD_SELECT");
    compad_map(0, 0, BP_MISC_START, 0, 0, 0, 0, 0, 0, &s);
    check(s.buttons == XPAD_START, "misc start maps to XPAD_START");
    compad_map(0, 0, BP_MISC_SYSTEM, 0, 0, 0, 0, 0, 0, &s);
    check(s.buttons == XPAD_MODE, "misc system maps to XPAD_MODE");
    compad_map(0, 0, BP_MISC_CAPTURE, 0, 0, 0, 0, 0, 0, &s);
    check(s.buttons == 0, "misc capture has no bit and is dropped");

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

    compad_map(BP_DPAD_LEFT, BP_BUTTON_A | BP_BUTTON_SHOULDER_R,
               BP_MISC_START, -200, 100, 60, -60, 512, 256, &s);
    len = compad_state_frame(1, &s, f);
    check(len == 12, "a state frame is 12 bytes");
    check(round_trip(&d, f, len) == 12, "and the decoder accepts it");
    check(COMPAD_HDR_PAD(f[1]) == 1, "the pad index survives");
    check(COMPAD_HDR_TYPE(f[1]) == COMPAD_TYPE_STATE, "so does the type");
    check(COMPAD_HDR_VERSION(f[1]) == COMPAD_PROTO_VERSION,
          "and the version is the one the decoder expects");
    check(compad_state_buttons(f) == s.buttons, "the buttons survive");
    check(COMPAD_STATE_LX(f) == s.lx && COMPAD_STATE_LY(f) == s.ly &&
              COMPAD_STATE_RX(f) == s.rx && COMPAD_STATE_RY(f) == s.ry,
          "the axes survive, signs included");
    check(COMPAD_STATE_LT(f) == s.lt && COMPAD_STATE_RT(f) == s.rt,
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
        len = compad_state_frame(0, &s, f);

        if (round_trip(&d, f, len) != 12 || compad_state_buttons(f) != want)
            clean = 0;
    }
    check(clean, "every mapped bit round trips on its own");

    /* Extremes of every axis, since sign is the easy thing to lose. */
    clean = 1;
    for (i = 0; i < 4; i++)
    {
        static const int8_t vals[] = {-127, -1, 0, 1, 127};
        unsigned v;

        for (v = 0; v < sizeof(vals) / sizeof(vals[0]); v++)
        {
            memset(&s, 0, sizeof(s));
            s.lx = s.ly = s.rx = s.ry = vals[v];
            s.lt = s.rt = (uint8_t)(v * 60);
            len = compad_state_frame(0, &s, f);

            if (round_trip(&d, f, len) != 12 ||
                COMPAD_STATE_LX(f) != vals[v] ||
                COMPAD_STATE_RY(f) != vals[v] ||
                COMPAD_STATE_LT(f) != (uint8_t)(v * 60))
                clean = 0;
        }
    }
    check(clean, "axis and trigger extremes round trip");

    /* Descriptor. XPAD_TYPE_GAMEPAD is 2, as the harness sends. */
    len = compad_descriptor_frame(0, XPAD_TYPE_GAMEPAD, 0, 0, f);
    check(len == 7, "a descriptor frame is 7 bytes");
    check(round_trip(&d, f, len) == 7, "and the decoder accepts it");
    check(COMPAD_DESC_TYPE(f) == XPAD_TYPE_GAMEPAD, "the pad type survives");

    len = compad_descriptor_frame(0, XPAD_TYPE_GAMEPAD, 0, 0xBEEF, f);
    check(round_trip(&d, f, len) == 7 && COMPAD_DESC_CAPS(f) == 0xBEEF,
          "caps survive, high byte first");

    /* A corrupted frame must be rejected, or the checksum is decoration. */
    memset(&s, 0, sizeof(s));
    s.buttons = XPAD_SOUTH;
    len = compad_state_frame(0, &s, f);
    f[2] ^= 0x01;
    check(round_trip(&d, f, len) == 0, "a flipped bit fails the checksum");

    /* And the decoder must resynchronise afterwards. */
    len = compad_state_frame(0, &s, f);
    check(round_trip(&d, f, len) == 12, "the next good frame still arrives");

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
