/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * Bluepad32 controller state to COMpad frames.
 *
 * Free of both Pico SDK and Bluepad32 headers, deliberately: this is
 * the only part of the firmware with logic that can be wrong, so the
 * host build tests it without a Pico. The platform file passes plain
 * integers in and this decides what goes on the wire.
 *
 * docs/protocol.md is the contract. target/atarist/src/protocol.h is
 * the decoder that has to agree with this, and test/encode_test.c
 * round trips one against the other so they cannot drift.
 */

#ifndef COMPAD_ENCODE_H
#define COMPAD_ENCODE_H

#include <stdint.h>

/* Xpad button bits, from lib/xpad/src/xpad.h. Mirrored rather than
 * included so this header stays dependency free; test/encode_test.c
 * includes the real one and asserts they still agree. */
#define CE_UP 0x00000001UL
#define CE_DOWN 0x00000002UL
#define CE_LEFT 0x00000004UL
#define CE_RIGHT 0x00000008UL
#define CE_SOUTH 0x00000010UL
#define CE_EAST 0x00000020UL
#define CE_NORTH 0x00000040UL
#define CE_WEST 0x00000080UL
#define CE_TL 0x00000100UL
#define CE_TR 0x00000200UL
#define CE_TL2 0x00000400UL
#define CE_TR2 0x00000800UL
#define CE_SELECT 0x00001000UL
#define CE_START 0x00002000UL
#define CE_MODE 0x00004000UL
#define CE_THUMBL 0x00008000UL
#define CE_THUMBR 0x00010000UL

/* Bluepad32's own bits, from lib/bluepad32 uni_gamepad.h. Same reason. */
#define BP_DPAD_UP 0x01
#define BP_DPAD_DOWN 0x02
#define BP_DPAD_RIGHT 0x04
#define BP_DPAD_LEFT 0x08

#define BP_BUTTON_A 0x0001
#define BP_BUTTON_B 0x0002
#define BP_BUTTON_X 0x0004
#define BP_BUTTON_Y 0x0008
#define BP_BUTTON_SHOULDER_L 0x0010
#define BP_BUTTON_SHOULDER_R 0x0020
#define BP_BUTTON_TRIGGER_L 0x0040
#define BP_BUTTON_TRIGGER_R 0x0080
#define BP_BUTTON_THUMB_L 0x0100
#define BP_BUTTON_THUMB_R 0x0200

#define BP_MISC_SYSTEM 0x01
#define BP_MISC_SELECT 0x02
#define BP_MISC_START 0x04
#define BP_MISC_CAPTURE 0x08

/* One pad, in the form a state frame wants. */
typedef struct
{
    uint32_t buttons;
    int8_t lx, ly, rx, ry;
    uint8_t lt, rt;
} COMPAD_STATE;

/*
 * Bluepad32 normalises axes to a 1024 range, so -512..511 for sticks
 * and 0..1023 for the analogue triggers. Xpad wants -127..127 and
 * 0..255, which is a shift of two either way. The clamp matters on the
 * negative end only: -512 >> 2 is -128, one past what Xpad allows.
 */
static int8_t ce_axis(int32_t v)
{
    v >>= 2;

    if (v > 127)
        v = 127;
    if (v < -127)
        v = -127;

    return (int8_t)v;
}

static uint8_t ce_trigger(int32_t v)
{
    v >>= 2;

    if (v > 255)
        v = 255;
    if (v < 0)
        v = 0;

    return (uint8_t)v;
}

/*
 * Map by position, never by the letter on the button. Bluepad32 names
 * its face buttons after the Xbox legend, so BUTTON_X is the *left* one
 * and BUTTON_Y the *top* one, while Xpad's XPAD_X aliases north and
 * XPAD_Y aliases west, following the Linux kernel. The letters cross
 * over and the positions do not, which is why nothing below mentions a
 * letter twice. See README's "The button naming trap".
 */
static void compad_map(uint8_t dpad, uint16_t buttons, uint8_t misc,
                       int32_t ax, int32_t ay, int32_t arx, int32_t ary,
                       int32_t brake, int32_t throttle, COMPAD_STATE *out)
{
    uint32_t b = 0;

    if (dpad & BP_DPAD_UP)
        b |= CE_UP;
    if (dpad & BP_DPAD_DOWN)
        b |= CE_DOWN;
    if (dpad & BP_DPAD_LEFT)
        b |= CE_LEFT;
    if (dpad & BP_DPAD_RIGHT)
        b |= CE_RIGHT;

    if (buttons & BP_BUTTON_A)
        b |= CE_SOUTH; /* bottom */
    if (buttons & BP_BUTTON_B)
        b |= CE_EAST; /* right  */
    if (buttons & BP_BUTTON_X)
        b |= CE_WEST; /* left   */
    if (buttons & BP_BUTTON_Y)
        b |= CE_NORTH; /* top    */

    if (buttons & BP_BUTTON_SHOULDER_L)
        b |= CE_TL;
    if (buttons & BP_BUTTON_SHOULDER_R)
        b |= CE_TR;
    if (buttons & BP_BUTTON_TRIGGER_L)
        b |= CE_TL2;
    if (buttons & BP_BUTTON_TRIGGER_R)
        b |= CE_TR2;
    if (buttons & BP_BUTTON_THUMB_L)
        b |= CE_THUMBL;
    if (buttons & BP_BUTTON_THUMB_R)
        b |= CE_THUMBR;

    if (misc & BP_MISC_SELECT)
        b |= CE_SELECT;
    if (misc & BP_MISC_START)
        b |= CE_START;
    if (misc & BP_MISC_SYSTEM)
        b |= CE_MODE;

    /* MISC_CAPTURE has no Xpad bit. Xpad v1 defines 17 and new ones
     * start at bit 17, which is a decision for that repo rather than
     * something this firmware should invent. It is dropped. */

    out->buttons = b;
    out->lx = ce_axis(ax);
    out->ly = ce_axis(ay);
    out->rx = ce_axis(arx);
    out->ry = ce_axis(ary);
    out->lt = ce_trigger(brake);
    out->rt = ce_trigger(throttle);
}

/* Everything this firmware can ever set, and nothing else. */
#define CE_MAPPED                                                            \
    (CE_UP | CE_DOWN | CE_LEFT | CE_RIGHT | CE_SOUTH | CE_EAST | CE_NORTH |  \
     CE_WEST | CE_TL | CE_TR | CE_TL2 | CE_TR2 | CE_SELECT | CE_START |      \
     CE_MODE | CE_THUMBL | CE_THUMBR)

/* ------------------------------------------------------------------ */
/* Frames                                                              */
/* ------------------------------------------------------------------ */

#define CE_SYNC 0xA5
#define CE_TYPE_STATE 0x0
#define CE_TYPE_DESCRIPTOR 0xF

#define CE_STATE_LEN 12
#define CE_DESC_LEN 7

static uint8_t ce_checksum(const uint8_t *f, uint8_t len)
{
    uint8_t x = 0;
    uint8_t i;

    for (i = 0; i + 1 < len; i++)
        x ^= f[i];

    return x;
}

/* Writes CE_STATE_LEN bytes and returns the length. */
static uint8_t compad_state_frame(uint8_t pad, const COMPAD_STATE *s,
                                  uint8_t *f)
{
    f[0] = CE_SYNC;
    f[1] = (uint8_t)(((pad & 3) << 4) | CE_TYPE_STATE);
    f[2] = (uint8_t)(s->buttons & 0xff);
    f[3] = (uint8_t)((s->buttons >> 8) & 0xff);
    f[4] = (uint8_t)((s->buttons >> 16) & 0xff);
    f[5] = (uint8_t)s->lx;
    f[6] = (uint8_t)s->ly;
    f[7] = (uint8_t)s->rx;
    f[8] = (uint8_t)s->ry;
    f[9] = s->lt;
    f[10] = s->rt;
    f[11] = ce_checksum(f, CE_STATE_LEN);

    return CE_STATE_LEN;
}

/* Caps go high byte first, which is not the order the buttons use.
 * That is the protocol, not a slip: docs/protocol.md pins it and
 * test/protocol_test.c has held it since phase 0. */
static uint8_t compad_descriptor_frame(uint8_t pad, uint8_t type,
                                       uint8_t flags, uint16_t caps,
                                       uint8_t *f)
{
    f[0] = CE_SYNC;
    f[1] = (uint8_t)(((pad & 3) << 4) | CE_TYPE_DESCRIPTOR);
    f[2] = type;
    f[3] = flags;
    f[4] = (uint8_t)((caps >> 8) & 0xff);
    f[5] = (uint8_t)(caps & 0xff);
    f[6] = ce_checksum(f, CE_DESC_LEN);

    return CE_DESC_LEN;
}

static int compad_state_differs(const COMPAD_STATE *a, const COMPAD_STATE *b)
{
    return a->buttons != b->buttons || a->lx != b->lx || a->ly != b->ly ||
           a->rx != b->rx || a->ry != b->ry || a->lt != b->lt ||
           a->rt != b->rt;
}

#endif /* COMPAD_ENCODE_H */
