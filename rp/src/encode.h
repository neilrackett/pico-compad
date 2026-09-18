/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * Bluepad32 controller state to COMpad frames.
 *
 * Free of Pico SDK and BTstack headers, deliberately: this is the only
 * part of the firmware with logic that can be wrong, so the host build
 * tests it without a Pico. The platform file passes plain integers in
 * and this decides what goes on the wire.
 *
 * The three headers below are all stdint-only and host-portable, so
 * they are included rather than copied. Mirroring their constants here
 * would need a test to prove the copies still matched, and the copy of
 * Bluepad32's half is the one that would rot: its bits are BIT(enum),
 * so an upstream reorder moves every button silently. Including them
 * makes that a compile, not an assertion somebody has to remember to
 * write.
 *
 * docs/protocol.md is the contract. protocol.h is the decoder that has
 * to agree with this, and test/encode_test.c round trips one against
 * the other so they cannot drift.
 */

#ifndef COMPAD_ENCODE_H
#define COMPAD_ENCODE_H

#include <stdint.h>

#include <controller/uni_gamepad.h> /* DPAD_*, BUTTON_*, MISC_BUTTON_* */
#include <protocol.h>               /* the wire contract, and the decoder */
#include <xpad.h>                   /* XPAD_* button bits, XPAD_TYPE_*   */

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
static COMPAD_UNUSED int8_t ce_axis(int32_t v)
{
    v >>= 2;

    if (v > 127)
        v = 127;
    if (v < -127)
        v = -127;

    return (int8_t)v;
}

static COMPAD_UNUSED uint8_t ce_trigger(int32_t v)
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
static COMPAD_UNUSED void compad_map(uint8_t dpad, uint16_t buttons, uint8_t misc,
                       int32_t ax, int32_t ay, int32_t arx, int32_t ary,
                       int32_t brake, int32_t throttle, COMPAD_STATE *out)
{
    uint32_t b = 0;

    if (dpad & DPAD_UP)
        b |= XPAD_UP;
    if (dpad & DPAD_DOWN)
        b |= XPAD_DOWN;
    if (dpad & DPAD_LEFT)
        b |= XPAD_LEFT;
    if (dpad & DPAD_RIGHT)
        b |= XPAD_RIGHT;

    if (buttons & BUTTON_A)
        b |= XPAD_SOUTH; /* bottom */
    if (buttons & BUTTON_B)
        b |= XPAD_EAST; /* right  */
    if (buttons & BUTTON_X)
        b |= XPAD_WEST; /* left   */
    if (buttons & BUTTON_Y)
        b |= XPAD_NORTH; /* top    */

    if (buttons & BUTTON_SHOULDER_L)
        b |= XPAD_TL;
    if (buttons & BUTTON_SHOULDER_R)
        b |= XPAD_TR;
    if (buttons & BUTTON_TRIGGER_L)
        b |= XPAD_TL2;
    if (buttons & BUTTON_TRIGGER_R)
        b |= XPAD_TR2;
    if (buttons & BUTTON_THUMB_L)
        b |= XPAD_THUMBL;
    if (buttons & BUTTON_THUMB_R)
        b |= XPAD_THUMBR;

    if (misc & MISC_BUTTON_SELECT)
        b |= XPAD_SELECT;
    if (misc & MISC_BUTTON_START)
        b |= XPAD_START;
    if (misc & MISC_BUTTON_SYSTEM)
        b |= XPAD_MODE;

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
    (XPAD_UP | XPAD_DOWN | XPAD_LEFT | XPAD_RIGHT | XPAD_SOUTH | XPAD_EAST | XPAD_NORTH |  \
     XPAD_WEST | XPAD_TL | XPAD_TR | XPAD_TL2 | XPAD_TR2 | XPAD_SELECT | XPAD_START |      \
     XPAD_MODE | XPAD_THUMBL | XPAD_THUMBR)

/* ------------------------------------------------------------------ */
/* Frames                                                              */
/* ------------------------------------------------------------------ */

/* Lengths stay local because they size arrays, which compad_frame_len()
 * cannot do. encode_test.c checks them against it. */
#define CE_STATE_LEN 12
#define CE_DESC_LEN 7
#define CE_COMPACT_LEN 5

static COMPAD_UNUSED uint8_t ce_checksum(const uint8_t *f, uint8_t len)
{
    uint8_t x = 0;
    uint8_t i;

    for (i = 0; i + 1 < len; i++)
        x ^= f[i];

    return x;
}

/* Writes CE_STATE_LEN bytes. */
static COMPAD_UNUSED void compad_state_frame(uint8_t pad,
                                             const COMPAD_STATE *s,
                                             uint8_t *f)
{
    f[0] = COMPAD_SYNC;
    f[1] = (uint8_t)(((pad & 3) << 4) | COMPAD_TYPE_STATE);
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
}

/* Caps go high byte first, which is not the order the buttons use.
 * That is the protocol, not a slip: docs/protocol.md pins it and
 * test/protocol_test.c has held it since phase 0. */
static COMPAD_UNUSED void compad_descriptor_frame(uint8_t pad, uint8_t type,
                                                  uint8_t flags,
                                                  uint16_t caps, uint8_t *f)
{
    f[0] = COMPAD_SYNC;
    f[1] = (uint8_t)(((pad & 3) << 4) | COMPAD_TYPE_DESCRIPTOR);
    f[2] = type;
    f[3] = flags;
    f[4] = (uint8_t)((caps >> 8) & 0xff);
    f[5] = (uint8_t)(caps & 0xff);
    f[6] = ce_checksum(f, CE_DESC_LEN);
}

/*
 * Writes CE_COMPACT_LEN bytes: buttons and nothing else.
 *
 * Only ever valid when the axes have not moved, because the receiver
 * leaves its copy of them alone, and only when no button above bit 15
 * is held, because sixteen bits is all this carries and the receiver
 * assigns rather than merges. XPAD_THUMBR is bit 16, so a held right
 * stick click would be cleared by every compact frame that passed.
 * ce_compact_ok() is that test and the only supported way to ask it.
 */
static COMPAD_UNUSED void compad_compact_frame(uint8_t pad, uint32_t buttons,
                                               uint8_t *f)
{
    f[0] = COMPAD_SYNC;
    f[1] = (uint8_t)(((pad & 3) << 4) | COMPAD_TYPE_COMPACT);
    f[2] = (uint8_t)(buttons & 0xff);
    f[3] = (uint8_t)((buttons >> 8) & 0xff);
    f[4] = ce_checksum(f, CE_COMPACT_LEN);
}

/* Whether a compact frame would carry the whole difference. */
static COMPAD_UNUSED int ce_compact_ok(const COMPAD_STATE *now,
                                       const COMPAD_STATE *sent)
{
    if (now->buttons & ~(uint32_t)0xffff)
        return 0;

    return now->lx == sent->lx && now->ly == sent->ly &&
           now->rx == sent->rx && now->ry == sent->ry &&
           now->lt == sent->lt && now->rt == sent->rt;
}

static COMPAD_UNUSED int compad_state_differs(const COMPAD_STATE *a, const COMPAD_STATE *b)
{
    return a->buttons != b->buttons || a->lx != b->lx || a->ly != b->ly ||
           a->rx != b->rx || a->ry != b->ry || a->lt != b->lt ||
           a->rt != b->rt;
}

/* ------------------------------------------------------------------ */
/* What fits in a tick                                                  */
/* ------------------------------------------------------------------ */

/*
 * The link is finite and four pads can want more of it than it has.
 *
 * A 12 byte state frame is 6.25 ms at 19200, so a 20 ms tick carries
 * three of them and not four: four pads all moving at once want 125%
 * of the link. Sending anyway does not make it faster, it makes the
 * run loop late, which costs Bluetooth and the receive drain as well.
 *
 * So the tick spends a budget. What does not fit is not dropped, it is
 * deferred, and the pad it happened to goes first next time, which is
 * what stops a busy neighbour starving a quiet one. Every frame is
 * full state, so a deferred pad is a tick behind rather than wrong:
 * the same property that lets the decoder resynchronise after noise is
 * what makes this safe.
 *
 * Here rather than in the platform file because it is logic, and logic
 * in this header is tested on the host. Nothing about it needs a Pico,
 * and starvation is precisely the kind of bug a bench cannot show you.
 */
#define CE_SEND_DESCRIPTOR 1
#define CE_SEND_STATE 2
#define CE_SEND_COMPACT 4 /* set alongside STATE when one would do */

/*
 * want[] is what each pad would send on an unlimited link, as a mask
 * of the two bits above. plan[] comes back as what the budget allows,
 * and *next carries the rotation from one tick to the next.
 */
static COMPAD_UNUSED void ce_schedule(const uint8_t *want, uint8_t pads,
                                      int budget, uint8_t *next,
                                      uint8_t *plan)
{
    uint8_t start = *next % pads;
    uint8_t n;
    int demand = 0;
    int squeeze;
    int cut = 0;

    for (n = 0; n < pads; n++)
    {
        plan[n] = 0;

        if (want[n] & CE_SEND_DESCRIPTOR)
            demand += CE_DESC_LEN;
        if (want[n] & CE_SEND_STATE)
            demand += CE_STATE_LEN;
    }

    /*
     * Compact frames are a property of the tick, not of a pad. Deciding
     * per pad as the budget ran down would let the first pads spend it
     * on full state and leave the last one deferred anyway, which is
     * the pad the shortfall was never about. So: if everything fits,
     * nobody goes compact, and one or two pads therefore never see a
     * compact frame at all. If it does not fit, everyone who can goes
     * compact, and usually nothing has to be deferred.
     */
    squeeze = demand > budget;

    for (n = 0; n < pads; n++)
    {
        uint8_t i = (uint8_t)((start + n) % pads);

        /* A descriptor that does not fit is simply not sent: the
         * counter behind it keeps climbing and brings it back on the
         * next tick, so it needs no place in the rotation. */
        if ((want[i] & CE_SEND_DESCRIPTOR) && budget >= CE_DESC_LEN)
        {
            plan[i] |= CE_SEND_DESCRIPTOR;
            budget -= CE_DESC_LEN;
        }

        if (want[i] & CE_SEND_STATE)
        {
            int compact = squeeze && (want[i] & CE_SEND_COMPACT);
            int len = compact ? CE_COMPACT_LEN : CE_STATE_LEN;

            if (budget < len)
            {
                *next = i;
                cut = 1;
                break;
            }

            plan[i] |= compact ? CE_SEND_COMPACT : CE_SEND_STATE;
            budget -= len;
        }
    }

    /* Everyone who wanted the link got it, so the rotation goes back
     * to its natural order rather than drifting for no reason. */
    if (!cut)
        *next = 0;
}

#endif /* COMPAD_ENCODE_H */
