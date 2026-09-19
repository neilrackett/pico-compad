/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * COMpad provider for the Atari ST.
 *
 * Reads COMpad frames from the serial port and publishes them as an
 * xpad block, so anything xpad-aware sees whatever sits at the far end
 * of the wire. Stays resident; the serial ring is drained from the
 * etv_timer vector (~200 Hz), which xpad's AGENTS prefers over VBL as
 * far less likely to be clobbered by a game.
 *
 * TOS keeps doing the serial receive: its RX interrupt fills the AUX
 * iorec ring and this driver drains the ring, reader-side only, which
 * is exactly what Bconin would do. No BIOS calls happen in interrupt
 * context and nothing TOS owns is displaced except the etv vector,
 * which is chained.
 *
 *   COMPAD.PRG        install and stay resident (AUTO folder friendly)
 *   COMPAD.PRG -t     run the self test and exit, installing nothing
 *
 * Limits:
 *   - rumble goes back over the request frame direction, but only from
 *     the MFP port: the transmit path writes the USART directly rather
 *     than calling the BIOS from an interrupt, and XPAD_CAP_RUMBLE is
 *     masked out when this provider cannot send
 *   - caps and pad types arrive from descriptor frames when the
 *     adapter sends them; until then a pad that has produced a state
 *     frame reports XPAD_TYPE_GAMEPAD
 *   - Rsconf at 19200, which is its fastest. 38400 needs a private MFP
 *     handler, because it is not in Rsconf's table at all
 *   - the MFP port only. The Mega STE's two SCC ports are a different
 *     chip with different initialisation, and nothing here drives them
 */

#include <mint/basepage.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "stport.h"
#include "xpad.h"

/* COMPAD_VERSION comes from version.txt via the Makefile, so the
 * banner, the provider string a consumer reads out of the cookie jar
 * and the release all say the same thing. */
#define PROVIDER "COMpad " COMPAD_VERSION

/*
 * Banner and messages follow md-net's shape: a version and licence
 * line, a blank line, then what happened. Lines stay inside 40 columns
 * so they are readable in ST low resolution, which is where a game
 * machine usually sits.
 */
#define BANNER "\r\n" PROVIDER " (c)2026 Neil Rackett\r\n" \
               "GPLv3 neilrackett.com/atarist\r\n\r\n"

/*
 * Deadzone for folding the left stick into the d-pad bits, in the same
 * -127..127 units as the axes. 40 is what xpad's own viewer uses in the
 * demo provider it labels "exactly as a real provider should", so it is
 * the reference value rather than a guess. Radial, so the diagonals a
 * box test would miss are reached: xpad_fold_stick does that part.
 */
#define STICK_DEADZONE 40

/*
 * Iorec(0) follows the Bconmap mapping, so on a Mega STE reading a ring
 * that belongs to one of the SCC ports looks exactly like a dead cable.
 * The mapping is forced to the MFP rather than assumed. The device
 * numbers and what they correspond to on the rear panel are in
 * stport.h, shared with the bring-up tools.
 */

#define ETV_TIMER_VEC 0x100 /* vector number: address $400 */

/*
 * The AUX input ring, as Iorec() hands it out. _IOREC comes from
 * <mint/ostruct.h>, which <mint/osbind.h> already includes, and it
 * marks ibufhd and ibuftl volatile: those are the two fields TOS's RX
 * interrupt writes behind our back, so the declaration is worth having
 * rather than repeating here without it.
 *
 * Queue names, not pointer names, and the way round that matters:
 * ibufhd is the HEAD of the queue, the next byte to come out, and the
 * reader advances it. ibuftl is the TAIL, where the RX interrupt
 * appends. Getting these the wrong way round still "works" against a
 * sender that never changes its mind, because the stale bytes being
 * re-read decode to the same values; it shows up the moment a button
 * changes.
 */

/*
 * The MFP's USART, written directly, which is the one place here that
 * goes round TOS.
 *
 * Requests are sent from the etv handler, in interrupt context, and
 * this file's whole design is that nothing calls the BIOS from there:
 * Bconout is not reentrant and the timer can land inside it. A byte
 * into the data register when the transmitter says it is empty is
 * safe, and it is all we need.
 *
 * It only works on the MFP, so a provider that ended up on one of a
 * Mega STE's SCC ports cannot send, and says so by masking
 * XPAD_CAP_RUMBLE out of what it publishes. That is the honest way
 * round: never claim a capability that is not honoured.
 */
#define MFP_TSR (*(volatile uint8_t *)0xFFFFFA2DL)
#define MFP_UDR (*(volatile uint8_t *)0xFFFFFA2FL)
#define MFP_TSR_EMPTY 0x80

#define TXBUF 32 /* four request frames, which is more than enough */

static uint8_t txbuf[TXBUF];
static uint8_t txhead, txtail;
static int can_transmit;

static XPAD block;
static XPAD_REQ req;
static COMPAD_DECODER dec;
static _IOREC *aux;

/* What was last sent, so an unchanged pad is not re-sent. */
static uint8_t sent_rumble[XPAD_MAX_PADS][2];
static uint8_t seq_seen;

/*
 * Whole frames or nothing.
 *
 * Dropping a frame costs one request, which the consumer will make
 * again; leaving half of one in the ring puts bytes on the wire that
 * the adapter has to resynchronise past. The ring holds 31 usable
 * bytes and four pads asking at once is 32, so this is reachable
 * rather than theoretical.
 */
static int tx_push(const uint8_t *b, uint8_t n)
{
    uint8_t used = (uint8_t)((txtail + TXBUF - txhead) % TXBUF);
    uint8_t i;

    if (TXBUF - 1 - used < n)
        return 0;

    for (i = 0; i < n; i++)
    {
        txbuf[txtail] = b[i];
        txtail = (uint8_t)((txtail + 1) % TXBUF);
    }

    return 1;
}

/*
 * One byte per tick, when the transmitter is free. At ~200 Hz that is
 * 200 bytes a second, which an eight byte request frame clears in
 * 40 ms: far inside what a rumble needs, and it cannot stall the
 * interrupt the way waiting for the register would.
 */
static void tx_tick(void)
{
    if (txhead == txtail || !(MFP_TSR & MFP_TSR_EMPTY))
        return;

    MFP_UDR = txbuf[txhead];
    txhead = (uint8_t)((txhead + 1) % TXBUF);
}

/*
 * Has a consumer asked for something? xpad says to bump seq after
 * writing the request area, so this is a byte compare on the common
 * path and only walks the pads when something actually changed.
 */
static void requests_tick(void)
{
    int i, dropped = 0;

    if (!can_transmit || req.seq == seq_seen)
        return;

    for (i = 0; i < XPAD_MAX_PADS; i++)
    {
        uint8_t lo = req.rumble[i][0];
        uint8_t hi = req.rumble[i][1];
        uint8_t f[COMPAD_REQUEST_LEN];

        if (lo == sent_rumble[i][0] && hi == sent_rumble[i][1])
            continue;

        /* Duration 0: hold it until asked for something else, which is
         * what a request area with no duration field means. */
        compad_request_frame((uint8_t)i, lo, hi, 0, req.led[i], f);

        /* Recorded only once it is queued. Marking it sent first would
         * make a dropped frame look delivered, and this filter would
         * then suppress the retry that dropping it relies on. */
        if (!tx_push(f, COMPAD_REQUEST_LEN))
        {
            dropped = 1;
            continue;
        }

        sent_rumble[i][0] = lo;
        sent_rumble[i][1] = hi;
    }

    /*
     * Only once every pad's request is queued. Marking the sequence
     * seen while one was dropped would stop this function running
     * again until the consumer happened to bump seq, which is the
     * retry the drop is counting on. Leaving it behind costs a walk of
     * four pads per tick until the ring drains, and the comparison
     * above skips the ones already sent.
     */
    if (!dropped)
        seq_seen = req.seq;
}

/* Latest complete state per pad, applied to the whole back buffer on
 * every commit so no slot ever exposes two-commits-old data. */
static XPAD_PAD shadow[XPAD_MAX_PADS];
static uint16_t caps_shadow;
static int dirty;

/* Read by the trampoline in etv.s. */
void (*compad_etv_chain)(void);

extern void compad_etv_entry(void); /* etv.s calls in  */
void compad_tick(void);             /* ...and calls this */

static void apply_frame(const uint8_t *f)
{
    uint8_t pad = COMPAD_HDR_PAD(f[1]);
    XPAD_PAD *p = &shadow[pad];

    switch (COMPAD_HDR_TYPE(f[1]))
    {
    case COMPAD_TYPE_STATE:
        p->buttons = compad_state_buttons(f);
        p->lx = COMPAD_STATE_LX(f);
        p->ly = COMPAD_STATE_LY(f);
        p->rx = COMPAD_STATE_RX(f);
        p->ry = COMPAD_STATE_RY(f);
        p->lt = COMPAD_STATE_LT(f);
        p->rt = COMPAD_STATE_RT(f);

        /*
         * Fold the left stick into the d-pad bits. xpad.h makes this
         * the provider's job precisely so a consumer that wants only
         * digital directions can ignore the analogue fields, and STDL
         * is such a consumer: it reads the d-pad bits alone. Without
         * this the stick is invisible to every digital consumer while
         * the d-pad works, which is exactly how it presented.
         *
         * After the axes are stored and the buttons assigned, because
         * it ORs into buttons and both are replaced from this frame.
         * The right stick is left alone: it is a look or mouse axis,
         * and folding it would fight the left.
         */
        xpad_fold_stick(p, p->lx, p->ly, STICK_DEADZONE);

        if (p->type == XPAD_TYPE_NONE)
            p->type = XPAD_TYPE_GAMEPAD;
        break;

    case COMPAD_TYPE_COMPACT:
        p->buttons = compad_compact_buttons(f);

        /*
         * And fold again, from the axes this frame did not carry.
         *
         * A compact frame is only sent when the sticks have not moved,
         * so p->lx and p->ly still hold the right values: what the
         * assignment above wiped is the d-pad bits the last fold put
         * there. Without this, a held direction drops out of every
         * digital consumer's view for as long as compact frames keep
         * arriving, which is up to a keepalive interval, and only when
         * three or four pads are busy enough to be over budget. That
         * is a fault nobody would find on a bench.
         */
        xpad_fold_stick(p, p->lx, p->ly, STICK_DEADZONE);

        if (p->type == XPAD_TYPE_NONE)
            p->type = XPAD_TYPE_GAMEPAD;
        break;

    case COMPAD_TYPE_DESCRIPTOR:
        p->type = COMPAD_DESC_TYPE(f);
        p->flags = COMPAD_DESC_FLAGS(f);
        caps_shadow = COMPAD_DESC_CAPS(f);

        /* The adapter may honour rumble, but if this provider cannot
         * transmit there is no way to ask it to. */
        if (!can_transmit)
            caps_shadow &= (uint16_t)~XPAD_CAP_RUMBLE;
        break;

    default:
        return;
    }

    dirty = 1;
}

/* Feed a run of received bytes through the decoder into the shadow. */
static void compad_bytes(const uint8_t *b, int n)
{
    int i;

    for (i = 0; i < n; i++)
    {
        uint8_t got = compad_feed(&dec, b[i]);

        if (got)
            apply_frame(dec.buf);
    }
}

static void publish_shadow(void)
{
    XPAD_PAD *back = xpad_back(&block);
    int i;

    for (i = 0; i < XPAD_MAX_PADS; i++)
        back[i] = shadow[i];

    block.caps = caps_shadow;
    xpad_commit(&block);
    dirty = 0;
}

/*
 * Called from the trampoline at ~200 Hz, in interrupt context. Drains
 * the AUX ring the way Bconin would: TOS's RX interrupt appends at the
 * tail (`ibuftl`) and this consumes from the head (`ibufhd`), so a
 * single-producer single-consumer ring needs no masking. Reading those
 * two the other way round is the exact bug design.md warns about, and
 * it passes every test that holds a fixed pattern. Nothing here calls
 * the BIOS or allocates.
 */
void compad_tick(void)
{
    /* Hoisted: install() sets these once and TOS never moves them, but
     * the compiler cannot know that across compad_feed(), so left in
     * place it reloads the buffer address and the size on every byte. */
    _IOREC *rec = aux;
    const char *buf = rec->ibuf;
    int16_t size = rec->ibufsiz;

    int16_t wr = rec->ibuftl; /* TOS has appended up to here */
    int16_t rd = rec->ibufhd; /* we have consumed up to here */

    while (rd != wr)
    {
        uint8_t got;

        rd++;
        if (rd >= size)
            rd = 0;

        got = compad_feed(&dec, (uint8_t)buf[rd]);

        if (got)
            apply_frame(dec.buf);
    }

    rec->ibufhd = rd;

    requests_tick();
    tx_tick();

    if (dirty)
        publish_shadow();
}

static void init_block(void)
{
    int i;

    memset(shadow, 0, sizeof(shadow));
    caps_shadow = 0;
    dirty = 0;
    compad_init(&dec);

    /* Four slots from the start: the wire protocol addresses four pads
     * and a slot that has seen no frames honestly reports NONE. */
    xpad_init(&block, XPAD_MAX_PADS, 0, PROVIDER, &req);

    /* Both buffers start as the (empty) shadow. */
    for (i = 0; i < 2; i++)
        publish_shadow();
}

/* ------------------------------------------------------------------ */
/* Self test                                                           */
/* ------------------------------------------------------------------ */

static int failures;

static void check(int ok, const char *what)
{
    printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

static uint8_t mkframe(uint8_t *out, uint8_t type, uint8_t pad,
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

static int selftest(void)
{
    uint8_t f[COMPAD_MAX_FRAME];
    uint8_t body[9];
    XPAD_PAD out;
    uint16_t seq0;
    uint8_t n;

    init_block();

    check(xpad_valid(&block), "the block passes xpad_valid");
    check(xpad_connected(&block) == 0, "no pads before any frame");

    /* A state frame for pad 2 lands in slot 2 and nowhere else. */
    memset(body, 0, sizeof(body));
    body[0] = 0x18; /* RIGHT | SOUTH  */
    body[1] = 0x02; /* TR             */
    body[3] = 100;  /* lx             */
    body[7] = 255;  /* lt             */
    n = mkframe(f, COMPAD_TYPE_STATE, 2, body, 9);
    compad_bytes(f, n);
    publish_shadow();

    check(xpad_read(&block, 2, &out) &&
              out.buttons == (XPAD_RIGHT | XPAD_SOUTH | XPAD_TR),
          "state frame buttons reach pad 2");
    check(out.lx == 100 && out.lt == 255, "axes and triggers reach pad 2");
    check(out.type == XPAD_TYPE_GAMEPAD, "a stateful pad reports gamepad");
    check(xpad_read(&block, 0, &out) && out.type == XPAD_TYPE_NONE,
          "pad 0 stays empty");
    check(xpad_connected(&block) == 1, "one pad connected");

    /*
     * The left stick folds into the d-pad bits, with no buttons held.
     *
     * This is the check that was missing when a game saw a working
     * d-pad and a dead stick: STDL, and any consumer that wants only
     * digital directions, reads the direction bits and never looks at
     * the axes, exactly as xpad.h invites it to. Asserting the axes
     * arrive is not the same test.
     */
    memset(body, 0, sizeof(body));
    body[4] = (uint8_t)(int8_t)-120; /* ly hard up, +y being down */
    n = mkframe(f, COMPAD_TYPE_STATE, 2, body, 9);
    compad_bytes(f, n);
    publish_shadow();

    check(xpad_read(&block, 2, &out) && (out.buttons & XPAD_UP),
          "the left stick folds into the d-pad");
    check(out.ly == -120, "and the analogue value survives the fold");

    /*
     * And it survives a compact frame, which carries buttons and no
     * axes at all.
     *
     * The adapter sends these when a tick is over budget, which is
     * three or four busy pads, and the receiver keeps the axes it
     * already had. Assigning the sixteen button bits therefore wipes
     * the d-pad bits the last fold put there unless the fold is
     * applied again from the axes still in the pad. The symptom is a
     * held direction dropping out of every digital consumer's view for
     * up to a keepalive interval, only under load, which is not a
     * fault anybody would find on a bench.
     *
     * The stick is still hard up from the frame above.
     */
    body[0] = XPAD_SOUTH;
    body[1] = 0;
    n = mkframe(f, COMPAD_TYPE_COMPACT, 2, body, 2);
    compad_bytes(f, n);
    publish_shadow();

    check(xpad_read(&block, 2, &out) &&
              out.buttons == (XPAD_UP | XPAD_SOUTH),
          "a compact frame keeps the folded direction");
    check(out.ly == -120, "and does not disturb the axes it omits");

    /* Inside the deadzone nothing folds, or a resting pad would walk. */
    memset(body, 0, sizeof(body));
    body[3] = 20; /* lx, well under STICK_DEADZONE */
    n = mkframe(f, COMPAD_TYPE_STATE, 2, body, 9);
    compad_bytes(f, n);
    publish_shadow();

    check(xpad_read(&block, 2, &out) && !(out.buttons & XPAD_DPAD),
          "a stick inside the deadzone folds nothing");

    /* A descriptor upgrades the type, flags and caps. */
    body[0] = XPAD_TYPE_XBOX;
    body[1] = XPAD_PAD_ANALOG | XPAD_PAD_WIRELESS;
    body[2] = 0x00;
    body[3] = XPAD_CAP_ANALOG | XPAD_CAP_HOTPLUG;
    n = mkframe(f, COMPAD_TYPE_DESCRIPTOR, 2, body, 4);
    compad_bytes(f, n);
    publish_shadow();

    check(xpad_read(&block, 2, &out) && out.type == XPAD_TYPE_XBOX,
          "descriptor sets the pad type");
    check(block.caps == (XPAD_CAP_ANALOG | XPAD_CAP_HOTPLUG),
          "descriptor sets the caps");

    /* A corrupted frame changes nothing. */
    seq0 = block.seq;
    n = mkframe(f, COMPAD_TYPE_STATE, 2, body, 9);
    f[5] ^= 0xff;
    compad_bytes(f, n);
    check(!dirty && block.seq == seq0, "a corrupt frame is ignored");

    /* Release: a fresh state frame replaces, never ORs. */
    memset(body, 0, sizeof(body));
    n = mkframe(f, COMPAD_TYPE_STATE, 2, body, 9);
    compad_bytes(f, n);
    publish_shadow();
    check(xpad_read(&block, 2, &out) && out.buttons == 0,
          "full state replaces rather than accumulates");

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    printf("COMPAD-DONE %d\r\n", failures ? 1 : 0);
    fflush(stdout);

    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Install                                                             */
/* ------------------------------------------------------------------ */

/*
 * Ping a port and wait briefly for any valid frame back.
 *
 * The adapter answers a ping with a descriptor whether or not a pad is
 * connected, so this finds it even with the controller switched off,
 * which passive listening cannot: the adapter is silent until a pad
 * is paired.
 *
 * Deliberately short. It runs at boot from AUTO, and a machine with
 * something else on a port should not have its morning held up.
 */
#define PING_WAIT_TICKS 8 /* ~160 ms at 50 Hz, several adapter ticks */

static int port_answers(void)
{
    COMPAD_DECODER probe;
    uint8_t f[COMPAD_PING_LEN];
    long t;

    compad_init(&probe);

    /* Drop anything already waiting, so a previous port's traffic
     * cannot be mistaken for this one answering. */
    while (Bconstat(1))
        (void)Bconin(1);

    compad_ping_frame(f);

    for (t = 0; t < COMPAD_PING_LEN; t++)
        Bconout(1, f[t]);

    for (t = 0; t < PING_WAIT_TICKS; t++)
    {
        while (Bconstat(1))
        {
            if (compad_feed(&probe, (uint8_t)Bconin(1)))
                return 1; /* a whole frame, checksum and all */
        }

        Vsync();
    }

    return 0;
}

/*
 * Find the adapter, and leave device 1 pointed at it.
 *
 * Ping each port in turn, MFP first because that is where the adapter
 * is meant to be and because stopping there means the other ports are
 * never touched at all. That matters: probing is not read only. It
 * remaps device 1 and reconfigures the port's line settings, so a
 * modem mid-call or a serial printer would notice. Every port that
 * does not answer has its settings put back as far as Rsconf permits,
 * which is UCR, RSR, TSR and SCR: the line rate and flow control
 * cannot be read back through TOS at all, so a port probed and left
 * keeps 19200 with no handshake. The MFP is probed first, so a port
 * is only ever reached when the adapter is somewhere else.
 *
 * Nothing answers, or this TOS has no Bconmap: fall back to the MFP,
 * which is the recommended and often only port. So forgetting to plug
 * the adapter in, or switching it on later, still works.
 *
 * Returns the device it settled on, and sets *answered to whether
 * that was because something replied or because nothing did.
 */

/* What is printed on the case, not which chip is behind it. */
static const char *port_name(int dev)
{
    unsigned i;

    for (i = 0; i < STPORT_COUNT; i++)
    {
        if (stports[i].dev == dev)
            return stports[i].label;
    }

    return "an unknown port";
}

static int claim_port(int *answered)
{
    unsigned i;

    *answered = 0;

    if (stport_has_bconmap())
    {
        for (i = 0; i < STPORT_COUNT; i++)
        {
            long previous, settings;

            /* A machine without this port leaves the mapping alone,
             * and probing it would really be probing the last one. */
            previous = Bconmap(stports[i].dev);

            if (previous <= 0)
                continue;

            settings = Rsconf(-1, -1, -1, -1, -1, -1);
            stport_configure();

            if (port_answers())
            {
                *answered = 1;
                return stports[i].dev;
            }

            /*
             * Put this port back, as far as TOS will allow.
             *
             * Rsconf's query form reports UCR, RSR, TSR and SCR and
             * nothing else: the line rate and the flow control setting
             * cannot be read back at all, so a port probed and left
             * keeps this provider's 19200 with no handshake rather
             * than whatever it had. Nothing in TOS can do better, so
             * the honest thing is to say so rather than to imply the
             * probe was invisible.
             *
             * It reaches a port at all only when the adapter is not on
             * the MFP, since that one is probed first and answers
             * immediately.
             */
            Rsconf(-1, -1, (int)((settings >> 24) & 0xff),
                   (int)((settings >> 16) & 0xff),
                   (int)((settings >> 8) & 0xff), (int)(settings & 0xff));
            Bconmap(previous);
        }

        Bconmap(BCONMAP_MFP);
    }

    stport_configure();

    return BCONMAP_MFP;
}

static int install(void)
{
    int port, answered;

    printf(BANNER);

    if (xpad_find())
    {
        printf("An Xpad provider is already installed,\r\n");
        printf("so this one has left it alone.\r\n");
        return 0;
    }

    init_block();

    port = claim_port(&answered);
    can_transmit = (port == BCONMAP_MFP);
    aux = (_IOREC *)Iorec(0);

    if (!aux)
    {
        printf("No AUX input record, so the serial\r\n");
        printf("port cannot be read.\r\n");
        return 0;
    }

    /* Catch the read index up to the write index, so bytes that
     * arrived before install do not turn up as a burst of half
     * frames. */
    aux->ibufhd = aux->ibuftl;

    if (!xpad_publish(&block))
    {
        printf("Could not add the XPAD cookie.\r\n");
        printf("The cookie jar may be full.\r\n");
        return 0;
    }

    /* Read the old vector first, so the trampoline never runs with a
     * garbage chain even for a tick. */
    compad_etv_chain = (void (*)(void))Setexc(ETV_TIMER_VEC, (void (*)())-1L);
    (void)Setexc(ETV_TIMER_VEC, compad_etv_entry);

    /* Say which port and how it was chosen. A ping that finds nothing
     * falls back silently otherwise, and "it is on Modem 1 because I
     * looked" is a different fact from "because I gave up". */
    printf("Xpad provider listening on %s.\r\n", port_name(port));

    if (!answered)
        printf("No adapter answered, so this is the default.\r\n");

    return 1;
}

int main(int argc, char **argv)
{
#ifdef XPAD_SELFTEST
    /*
     * Hatari's --auto takes a path and no arguments, so the harness has
     * no way to ask for a mode. This build supplies the command line it
     * would have passed and then runs the ordinary parsing below; the
     * two binaries differ only in where argv came from. See xpad's
     * AGENTS for why it must never branch around the real logic.
     */
    static char *test_args[] = {"COMPAD", "-t"};

    argc = 2;
    argv = test_args;
#endif

    if (argc > 1 && strcmp(argv[1], "-t") == 0)
        return selftest();

    if (!install())
        return 1;

    Ptermres(_base->p_tlen + _base->p_dlen + _base->p_blen + 256, 0);

    return 0; /* not reached */
}
