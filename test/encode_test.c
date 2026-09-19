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

/*
 * What fits in a tick.
 *
 * Four pads at 50 Hz is the one load this link cannot carry, and the
 * scheduler is what makes it degrade rather than break. None of it can
 * be seen on a bench: a starved pad looks like a pad nobody pressed.
 * So it is checked here, where the budget is a number and the rotation
 * is observable.
 */

#define TEST_PADS 4
#define TEST_BUDGET 38 /* 19200 8N1, 20 ms: (19200/10)/50 */

static int plan_count(const uint8_t *plan, uint8_t bit)
{
    int i, n = 0;

    for (i = 0; i < TEST_PADS; i++)
        if (plan[i] & bit)
            n++;

    return n;
}

static void schedule_checks(void)
{
    uint8_t want[TEST_PADS], plan[TEST_PADS];
    uint8_t next = 0;
    int served[TEST_PADS];
    int gap[TEST_PADS];
    int worst = 0;
    int i, t;

    /* The budget has to be the real one, or every number below is
     * about a link this project does not have. */
    check(TEST_BUDGET == (COMPAD_BAUD / 10) / (1000 / 20),
          "the tick budget is the link's own byte rate");
    check(TEST_BUDGET / CE_STATE_LEN == 3,
          "three state frames fit in a tick, and four do not");

    /* Nothing wanted, nothing sent, and the rotation stays put. */
    memset(want, 0, sizeof(want));
    ce_schedule(want, TEST_PADS, TEST_BUDGET, &next, plan);
    check(plan_count(plan, CE_SEND_STATE) == 0, "an idle link sends nothing");
    check(next == 0, "and leaves the rotation alone");

    /* Two pads is the ordinary case and must be untouched by any of
     * this: both go, in order, every tick. */
    want[0] = want[1] = CE_SEND_STATE;
    want[2] = want[3] = 0;
    ce_schedule(want, TEST_PADS, TEST_BUDGET, &next, plan);
    check(plan[0] == CE_SEND_STATE && plan[1] == CE_SEND_STATE,
          "two pads both send in the same tick");
    check(next == 0, "with the rotation still at its natural order");

    /* Four is the case that does not fit: three go, the fourth is
     * deferred, and it is the one the next tick starts from. */
    for (i = 0; i < TEST_PADS; i++)
        want[i] = CE_SEND_STATE;
    next = 0;
    ce_schedule(want, TEST_PADS, TEST_BUDGET, &next, plan);
    check(plan_count(plan, CE_SEND_STATE) == 3, "four pads send three");
    check(plan[3] == 0, "and the fourth is the one held back");
    check(next == 3, "which is where the next tick starts");

    ce_schedule(want, TEST_PADS, TEST_BUDGET, &next, plan);
    check(plan[3] == CE_SEND_STATE, "so the held-back pad goes first");

    /*
     * The property that matters, and the only one a bench could not
     * tell you: under permanent overload nobody starves. Four pads all
     * changing every tick for four seconds, and no pad may wait more
     * than one tick longer than its turn.
     */
    for (i = 0; i < TEST_PADS; i++)
    {
        served[i] = 0;
        gap[i] = 0;
    }

    next = 0;
    for (t = 0; t < 200; t++)
    {
        for (i = 0; i < TEST_PADS; i++)
            want[i] = CE_SEND_STATE;

        ce_schedule(want, TEST_PADS, TEST_BUDGET, &next, plan);

        for (i = 0; i < TEST_PADS; i++)
        {
            if (plan[i] & CE_SEND_STATE)
            {
                served[i]++;
                gap[i] = 0;
            }
            else if (++gap[i] > worst)
                worst = gap[i];
        }
    }

    check(worst <= 1, "no pad ever waits more than one tick for its turn");

    for (i = 0; i < TEST_PADS; i++)
        if (served[i] < 140)
            break;

    check(i == TEST_PADS, "and every pad still gets at least 35 Hz");

    /*
     * A descriptor is 7 bytes and comes out of the same budget, so a
     * tick carrying one has room for two state frames, not three.
     */
    for (i = 0; i < TEST_PADS; i++)
        want[i] = CE_SEND_STATE;
    want[0] |= CE_SEND_DESCRIPTOR;
    next = 0;
    ce_schedule(want, TEST_PADS, TEST_BUDGET, &next, plan);
    check(plan[0] == (CE_SEND_DESCRIPTOR | CE_SEND_STATE),
          "a descriptor and a state frame share a tick");
    check(plan_count(plan, CE_SEND_STATE) == 2,
          "and the descriptor costs the tick a state frame");

    /* A pad that wants only a descriptor never takes the rotation's
     * place: it is not what the budget runs out on. */
    memset(want, 0, sizeof(want));
    want[1] = CE_SEND_DESCRIPTOR;
    next = 0;
    ce_schedule(want, TEST_PADS, TEST_BUDGET, &next, plan);
    check(plan[1] == CE_SEND_DESCRIPTOR && next == 0,
          "a descriptor alone does not move the rotation");

    /*
     * Compact frames, which exist only to relieve a tick that is over
     * budget. The promise made in docs/protocol.md is that one and two
     * pads never see one, so that is the first thing checked.
     */
    memset(want, 0, sizeof(want));
    want[0] = CE_SEND_STATE | CE_SEND_COMPACT;
    next = 0;
    ce_schedule(want, TEST_PADS, TEST_BUDGET, &next, plan);
    check(plan[0] == CE_SEND_STATE, "one pad sends full state, never compact");

    want[1] = CE_SEND_STATE | CE_SEND_COMPACT;
    ce_schedule(want, TEST_PADS, TEST_BUDGET, &next, plan);
    check(plan[0] == CE_SEND_STATE && plan[1] == CE_SEND_STATE,
          "and so do two");

    /* Four pads pressing buttons is the case it was built for: all
     * four fit, and nothing is deferred. */
    for (i = 0; i < TEST_PADS; i++)
        want[i] = CE_SEND_STATE | CE_SEND_COMPACT;
    next = 0;
    ce_schedule(want, TEST_PADS, TEST_BUDGET, &next, plan);
    check(plan_count(plan, CE_SEND_COMPACT) == 4, "four pads all go compact");
    check(plan_count(plan, CE_SEND_STATE) == 0, "instead of full state");
    check(next == 0, "and none of them is deferred");

    /* A pad whose sticks moved cannot go compact, so it keeps its
     * twelve bytes while the others give theirs up. */
    want[2] = CE_SEND_STATE;
    next = 0;
    ce_schedule(want, TEST_PADS, TEST_BUDGET, &next, plan);
    check(plan[2] == CE_SEND_STATE, "a pad that moved a stick sends state");
    check(plan_count(plan, CE_SEND_COMPACT) == 3, "while the rest go compact");
    check(next == 0, "and the tick still fits");

    /* Nothing eligible is nothing gained: this is the old behaviour,
     * unchanged, which is what four sticks in motion still gets. */
    for (i = 0; i < TEST_PADS; i++)
        want[i] = CE_SEND_STATE;
    next = 0;
    ce_schedule(want, TEST_PADS, TEST_BUDGET, &next, plan);
    check(plan_count(plan, CE_SEND_STATE) == 3 && next == 3,
          "four moving sticks still defer one, as before");
}

/*
 * What may travel in a compact frame, and what may not.
 *
 * The receiver assigns the sixteen bits it carries over the whole
 * button mask rather than merging them, so a held XPAD_THUMBR, bit 16,
 * would be cleared by every compact frame that passed. That is not a
 * dropped update, it is a button that releases itself while you are
 * holding it, and it is the one thing about this frame type that could
 * be silently wrong.
 */
static void compact_checks(void)
{
    COMPAD_DECODER d;
    COMPAD_STATE now, sent;
    uint8_t f[CE_COMPACT_LEN];
    uint8_t got[16];

    memset(&now, 0, sizeof(now));
    memset(&sent, 0, sizeof(sent));

    now.buttons = XPAD_SOUTH;
    check(ce_compact_ok(&now, &sent), "a button press alone may go compact");

    now.lx = 40;
    check(!ce_compact_ok(&now, &sent), "a stick that moved may not");

    now.lx = 0;
    now.rt = 9;
    check(!ce_compact_ok(&now, &sent), "nor a trigger");

    now.rt = 0;
    now.buttons = XPAD_THUMBR;
    check(!ce_compact_ok(&now, &sent),
          "nor a button above the sixteen bits it carries");

    now.buttons = XPAD_SOUTH | XPAD_THUMBR;
    check(!ce_compact_ok(&now, &sent),
          "even alongside one that would have fitted");

    /* And the frame itself, through the ST's own decoder. */
    compad_init(&d);
    compad_compact_frame(2, XPAD_SOUTH | XPAD_START, f);
    check(round_trip(&d, f, CE_COMPACT_LEN, got) == CE_COMPACT_LEN,
          "a compact frame decodes whole");
    check(COMPAD_HDR_PAD(got[1]) == 2, "with its pad index intact");
    check(COMPAD_HDR_TYPE(got[1]) == COMPAD_TYPE_COMPACT, "typed compact");
    check(compad_compact_buttons(got) == (XPAD_SOUTH | XPAD_START),
          "and the buttons the encoder put in it");
    check(compad_frame_len(COMPAD_TYPE_COMPACT) == CE_COMPACT_LEN,
          "the decoder and the encoder agree on its length");
}

/*
 * Battery, and the flags it reaches the ST in.
 *
 * uni_controller.h contradicts itself here: an enum says 0 means the
 * pad does not report a battery, and a comment three lines later says
 * 0 is empty and 255 is unavailable. Every parser in the tree follows
 * the enum, and getting it backwards would light LOWBATT on every pad
 * that never reports one, which is the opposite of a warning: it would
 * teach people to ignore the flag.
 */
static void battery_checks(void)
{
    uint8_t flags = ce_pad_flags(255);

    check((flags & (XPAD_PAD_ANALOG | XPAD_PAD_WIRELESS)) ==
              (XPAD_PAD_ANALOG | XPAD_PAD_WIRELESS),
          "every pad here is analogue and wireless");
    check(!(flags & XPAD_PAD_LOWBATT), "a full battery is not low");

    check(!(ce_pad_flags(0) & XPAD_PAD_LOWBATT),
          "a pad that reports no battery is not low either");
    check(ce_pad_flags(1) & XPAD_PAD_LOWBATT, "an empty one is");
    check(ce_pad_flags(51) & XPAD_PAD_LOWBATT, "and so is a fifth left");
    check(!(ce_pad_flags(52) & XPAD_PAD_LOWBATT), "just above it is not");

    /* The DS5 and DS4 formula is capacity * 25 + 1, so these are the
     * values those pads actually produce, not points between them. */
    check(ce_pad_flags(2 * 25 + 1) & XPAD_PAD_LOWBATT,
          "two bars of ten reads low on a DualSense");
    check(!(ce_pad_flags(3 * 25 + 1) & XPAD_PAD_LOWBATT),
          "three does not");
}

/*
 * The LED and the button, which are the two optional components and
 * therefore the two nobody's bench necessarily has.
 */
#define TEST_FAST 5
#define TEST_SLOW 40

static int led_edges(int bonded, int ticks)
{
    int was = -1, edges = 0, t;

    for (t = 0; t < ticks; t++)
    {
        int on = ce_led_on(0, bonded, (uint16_t)t, TEST_FAST, TEST_SLOW);

        if (was >= 0 && on != was)
            edges++;

        was = on;
    }

    return edges;
}

static void led_checks(void)
{
    int fast = led_edges(0, 800);
    int slow = led_edges(1, 800);

    check(ce_led_on(1, 0, 0, TEST_FAST, TEST_SLOW) &&
              ce_led_on(1, 0, 7, TEST_FAST, TEST_SLOW) &&
              ce_led_on(1, 1, 123, TEST_FAST, TEST_SLOW),
          "a connected pad is a solid light, whatever the phase");

    check(fast > 0 && slow > 0, "and no pad is a blink either way");

    /* The whole requirement, from docs/roadmap.md: the two phases only
     * have to be obviously different from each other. */
    check(fast >= slow * 4, "looking for a new pad blinks far faster");

    /* A blink is a blink: it has to spend time off as well as on.
     * The first period is the off half, the second the on half. */
    check(!ce_led_on(0, 0, 0, TEST_FAST, TEST_SLOW) &&
              ce_led_on(0, 0, TEST_FAST, TEST_FAST, TEST_SLOW),
          "the fast blink is off then on");
    check(!ce_led_on(0, 1, 0, TEST_FAST, TEST_SLOW) &&
              ce_led_on(0, 1, TEST_SLOW, TEST_FAST, TEST_SLOW),
          "and so is the slow one");
}

static void button_checks(void)
{
    const uint16_t hold = 100;
    uint16_t held = 0;
    int fired = 0;
    int t;

    /* An unwired button reads not-pressed for ever and must never
     * fire: this is the default build, with nothing soldered on. */
    for (t = 0; t < 1000; t++)
        fired += ce_forget_due(0, hold, &held);

    check(fired == 0, "an unwired button never forgets anything");

    /* A hold fires once, on the tick it completes, and not again
     * however long the finger stays. */
    held = 0;
    fired = 0;
    for (t = 0; t < 10000; t++)
        fired += ce_forget_due(1, hold, &held);

    check(fired == 1, "a hold fires exactly once, however long it lasts");

    /* A tap short of the threshold does nothing at all. */
    held = 0;
    fired = 0;
    for (t = 0; t < (int)hold - 1; t++)
        fired += ce_forget_due(1, hold, &held);
    fired += ce_forget_due(0, hold, &held);

    check(fired == 0, "a tap short of the hold does nothing");
    check(held == 0, "and releasing resets the count");

    /* Released and held again, it fires again: once per hold, not
     * once per lifetime. */
    fired = 0;
    for (t = 0; t < (int)hold; t++)
        fired += ce_forget_due(1, hold, &held);

    check(fired == 1, "a second hold fires again");
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

    schedule_checks();
    compact_checks();
    battery_checks();
    led_checks();
    button_checks();

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");

    return failures ? 1 : 0;
}
