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
 * Limits, phase 1:
 *   - receive only: no request frames back yet, so no rumble and no
 *     XPAD_CAP_RUMBLE claimed
 *   - caps and pad types arrive from descriptor frames when the
 *     adapter sends them; until then a pad that has produced a state
 *     frame reports XPAD_TYPE_GAMEPAD
 *   - Rsconf at 9600; the private MFP handler and higher rates are
 *     phase 3
 *   - the MFP port only. The Mega STE's two SCC ports are a different
 *     chip with different initialisation, and nothing here drives them
 */

#include <mint/basepage.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "xpad.h"

#define PROVIDER "COMpad 0.1"

#define BAUD_9600 1
#define UCR_8N1 0x88

/*
 * Bconmap device numbers. On a Mega STE the port that matches a plain
 * ST's modem port is "Serial 2", the 9-pin socket below the VME slot,
 * driven by the same MFP 68901 at $FFFA00 and so register and
 * interrupt compatible with ST/STE code. It is device 6 and the
 * default at boot, but the machine also has two SCC 85C30 ports that
 * can be mapped over device 1: Serial 1 is channel B (device 7) and
 * the LAN socket is channel A (device 9).
 *
 * That matters here because Iorec(0) follows the mapping. Reading a
 * ring that belongs to an SCC port looks exactly like a dead cable, so
 * the mapping is forced rather than assumed.
 */
#define BCONMAP_MFP 6
#define TOS_WITH_BCONMAP 0x0200

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

static XPAD block;
static COMPAD_DECODER dec;
static _IOREC *aux;

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
        if (p->type == XPAD_TYPE_NONE)
            p->type = XPAD_TYPE_GAMEPAD;
        break;

    case COMPAD_TYPE_COMPACT:
        p->buttons = compad_compact_buttons(f);
        if (p->type == XPAD_TYPE_NONE)
            p->type = XPAD_TYPE_GAMEPAD;
        break;

    case COMPAD_TYPE_DESCRIPTOR:
        p->type = COMPAD_DESC_TYPE(f);
        p->flags = COMPAD_DESC_FLAGS(f);
        caps_shadow = COMPAD_DESC_CAPS(f);
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
    xpad_init(&block, XPAD_MAX_PADS, 0, PROVIDER, 0);

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

static unsigned short tos_version;

/* The OS header pointer lives at 0x4f2, below 0x800, and the ST bus
 * errors on user mode access down there. */
static long read_tos_version(void)
{
    char *sysbase = *(char **)0x4f2L;

    tos_version = *(unsigned short *)(sysbase + 2);

    return 0;
}

/*
 * Point BIOS device 1 at the MFP, and say so if it was pointing
 * somewhere else. Bconmap arrived with the machines that have more
 * than one serial port, so anything older is left alone: it has only
 * the MFP to offer anyway.
 */
static void claim_mfp(void)
{
    long previous;

    Supexec(read_tos_version);

    if (tos_version < TOS_WITH_BCONMAP)
        return;

    previous = Bconmap(BCONMAP_MFP);

    if (previous > 0 && previous != BCONMAP_MFP)
        printf("BIOS device 1 was mapped to %ld; claimed the MFP.\r\n",
               previous);
}

static int install(void)
{
    if (xpad_find())
    {
        printf("An xpad provider is already installed. Leaving it alone.\r\n");
        return 0;
    }

    init_block();

    claim_mfp();
    Rsconf(BAUD_9600, 0, UCR_8N1, -1, -1, -1);
    aux = (_IOREC *)Iorec(0);

    if (!aux)
    {
        printf("No AUX input record; cannot read the serial port.\r\n");
        return 0;
    }

    /* Catch the read index up to the write index, so bytes that
     * arrived before install do not turn up as a burst of half
     * frames. */
    aux->ibufhd = aux->ibuftl;

    if (!xpad_publish(&block))
    {
        printf("Could not install the XPAD cookie. Is the jar full?\r\n");
        return 0;
    }

    /* Read the old vector first, so the trampoline never runs with a
     * garbage chain even for a tick. */
    compad_etv_chain = (void (*)(void))Setexc(ETV_TIMER_VEC, (void (*)())-1L);
    (void)Setexc(ETV_TIMER_VEC, compad_etv_entry);

    printf("%s installed: serial pads on the cookie jar.\r\n", PROVIDER);
    printf("listening on the MFP port (Serial 2 on a Mega STE).\r\n");

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
