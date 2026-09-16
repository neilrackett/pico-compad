/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * Phase 0: prove the pipe.
 *
 * Reads the serial port through TOS and reports whether COMpad frames
 * are arriving intact. Deliberately throwaway: no xpad, no interrupts,
 * no residency. If this prints frames, the emulated wiring, baud and
 * framing are all proven, and everything after it is software.
 *
 * The harness watches for the COMPAD-DONE line, exit-status style,
 * since Hatari cannot pass a status out.
 */

#include <mint/osbind.h>
#include <stdio.h>

#include "protocol.h"

#define BAUD_19200 0 /* Rsconf speed code, 0 is its fastest */
#define UCR_8N1 0x88

/*
 * Bconmap devices, for the hunt below. A Mega STE has three serial
 * ports on two different chips, the rear panel labels vary between
 * machines and documentation, and an adapter on the wrong one is
 * indistinguishable from a dead cable: Bconin(1) follows the mapping,
 * so it reads a ring nothing is filling. Rather than argue about which
 * socket is which, ask each in turn and report where the bytes are.
 */
#define BCONMAP_MFP 6   /* the ST-compatible port, the boot default */
#define BCONMAP_SCC_B 7 /* a Mega STE's other DE-9                  */
#define BCONMAP_SCC_A 9 /* the LAN socket                           */
#define TOS_WITH_BCONMAP 0x0200

#define HUNT_TICKS 150 /* ~3 s each, enough for several keepalives */

#define TIMEOUT_TICKS 750 /* ~15 s of Vsync at 50 Hz */
#define ENOUGH_TO_PASS 3 /* a handful proves the framing */
#define ENOUGH_FRAMES 10

/* The OS header pointer lives at 0x4f2, below 0x800, and the ST bus
 * errors on user mode access down there, so this runs under Supexec
 * exactly as compad.c does. */
static unsigned short tos_version;

static long read_tos_version(void)
{
    char *sysbase = *(char **)0x4f2L;

    tos_version = *(unsigned short *)(sysbase + 2);

    return 0;
}

/* Count what arrives on whatever port is mapped now. */
static long drain(long ticks_for, long *frames_out)
{
    COMPAD_DECODER d;
    long bytes = 0, frames = 0, t;

    compad_init(&d);

    for (t = 0; t < ticks_for; t++)
    {
        while (Bconstat(1))
        {
            if (compad_feed(&d, (uint8_t)Bconin(1)))
                frames++;
            bytes++;
        }

        Vsync();
    }

    *frames_out = frames;

    return bytes;
}

/*
 * Nothing came in where we looked. Before blaming the wiring, try the
 * other serial ports: on a Mega STE the adapter may simply be in a
 * different socket from the one the BIOS is pointed at.
 */
static void hunt(void)
{
    /*
     * All three, including the MFP. The first pass above used whatever
     * device 1 happened to be mapped to, which is normally the MFP but
     * is exactly what a resident driver may have changed, so assuming
     * it was already covered is how the interesting case gets skipped.
     * Three seconds each.
     */
    static const struct { int dev; const char *name; } ports[] = {
        {BCONMAP_MFP, "MFP 68901, the ST-compatible port"},
        {BCONMAP_SCC_B, "SCC channel B"},
        {BCONMAP_SCC_A, "SCC channel A, the LAN socket"},
    };
    long was;
    unsigned i;

    Supexec(read_tos_version);

    if (tos_version < TOS_WITH_BCONMAP)
        return; /* no Bconmap on this TOS, so there is nothing to try */

    printf("\r\nnothing on the mapped port. trying all three...\r\n");
    fflush(stdout);

    was = Bconmap(BCONMAP_MFP);

    for (i = 0; i < sizeof(ports) / sizeof(ports[0]); i++)
    {
        long frames = 0, bytes;

        Bconmap(ports[i].dev);
        Rsconf(BAUD_19200, 0, UCR_8N1, -1, -1, -1);
        bytes = drain(HUNT_TICKS, &frames);

        printf("  device %d, %-33s %ld bytes, %ld frames\r\n",
               ports[i].dev, ports[i].name, bytes, frames);

        if (bytes > 0)
            printf("  ^^ the adapter is on this one\r\n");

        fflush(stdout);
    }

    Bconmap(was);
}

int main(void)
{
    COMPAD_DECODER d;
    long bytes = 0;
    long frames = 0;
    long ticks;
    uint32_t buttons = 0;
    int8_t lx = 0, ly = 0;

    Rsconf(BAUD_19200, 0, UCR_8N1, -1, -1, -1);
    compad_init(&d);

    printf("COMpad pipe check: AUX at 19200 8N1\r\n");

    for (ticks = 0; ticks < TIMEOUT_TICKS && frames < ENOUGH_FRAMES; ticks++)
    {
        while (Bconstat(1))
        {
            uint8_t b = (uint8_t)Bconin(1);
            uint8_t got;

            if (bytes < 24)
                printf("%02x ", b);
            else if (bytes == 24)
                printf("...\r\n");

            bytes++;

            got = compad_feed(&d, b);

            if (got == 12)
            {
                if (frames == 0)
                    printf("\r\nfirst frame: pad %d buttons %06lx "
                           "lx %d ly %d lt %u\r\n",
                           COMPAD_HDR_PAD(d.buf[1]),
                           (unsigned long)compad_state_buttons(d.buf),
                           COMPAD_STATE_LX(d.buf), COMPAD_STATE_LY(d.buf),
                           COMPAD_STATE_LT(d.buf));
                frames++;
            }
            else if (got != 0)
            {
                frames++; /* compact or descriptor: still a frame */
            }
        }

        Vsync();
    }

    printf("\r\n%ld bytes, %ld valid frames\r\n", bytes, frames);

    if (frames >= ENOUGH_TO_PASS)
        printf("the pipe works\r\n");
    else if (bytes > 0)
        printf("bytes flow but do not frame: check baud and the writer\r\n");
    else
        printf("nothing arrived on this port\r\n");

    printf("COMPAD-DONE %d\r\n", frames >= ENOUGH_TO_PASS ? 0 : 1);
    fflush(stdout);

    if (bytes == 0)
        hunt();

    /*
     * Then watch, until a key. The verdict above is a total after the
     * fact, which tells you bytes arrived but not that they are yours:
     * on hardware you want to move the stick and see the numbers move
     * with it. That is the difference between a wire that works and a
     * wire that is picking something up.
     *
     * One line, overwritten with a carriage return, four times a
     * second. The TOS console is slow enough that a line per frame at
     * 50 Hz would never keep up, which is the same reason xpadview
     * redraws only what changed.
     *
     * All of this sits after the COMPAD-DONE line, deliberately: the
     * Hatari runner waits for that marker and tears the emulator down,
     * so it never reaches here and never waits on a key that nothing
     * will press.
     */
    printf("\r\nwatching the wire, press any key to stop\r\n");
    fflush(stdout);

    lx = ly = 0;
    buttons = 0;

    for (ticks = 0; !Bconstat(2); ticks++)
    {
        while (Bconstat(1))
        {
            uint8_t b = (uint8_t)Bconin(1);
            uint8_t got = compad_feed(&d, b);

            bytes++;

            if (got == 0)
                continue;

            frames++;

            if (got == 12)
            {
                buttons = compad_state_buttons(d.buf);
                lx = COMPAD_STATE_LX(d.buf);
                ly = COMPAD_STATE_LY(d.buf);
            }
        }

        /* Every 12 vsyncs, so about four times a second. */
        if ((ticks % 12) == 0)
        {
            printf("\rbytes %ld  frames %ld  L %4d,%4d  buttons %06lx   ",
                   bytes, frames, (int)lx, (int)ly, (unsigned long)buttons);
            fflush(stdout);
        }

        Vsync();
    }

    Cconin(); /* swallow the key that stopped us */
    printf("\r\n");

    return frames >= ENOUGH_TO_PASS ? 0 : 1;
}
