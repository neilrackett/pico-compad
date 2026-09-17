/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * Pipe check: is anything reaching the serial port, and does it frame?
 *
 * Phase 0's throwaway, and still the first thing to run on new
 * hardware. Three passes: a short sample that ends in a verdict, a hunt
 * across the other serial ports if that sample saw nothing, and then a
 * live watch until a key is pressed.
 *
 * The harness watches for the COMPAD-DONE line, exit-status style, and
 * tears the emulator down when it sees it. Everything after that line
 * is for a person at a real machine and never runs under Hatari.
 */

#include <mint/osbind.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "stport.h"

#define TIMEOUT_TICKS 750 /* ~15 s of Vsync at 50 Hz               */
#define ENOUGH_TO_PASS 3  /* a handful proves the framing           */
#define ENOUGH_FRAMES 10  /* stop the sample early once it has them */
#define HUNT_TICKS 150    /* ~3 s per port, several keepalives      */
#define DUMP_BYTES 24     /* hex of the first few, then "..."       */

/* Everything one pass over the port learns. */
typedef struct
{
    long bytes;
    long frames;
    uint32_t buttons; /* from the latest state frame */
    int8_t lx, ly;
    int dumped;       /* bytes printed as hex so far */
} SEEN;

/*
 * Drain whatever is waiting on device 1 through the decoder, once.
 * Every pass here is this in a loop with a different stop condition,
 * so the byte and frame accounting lives in one place.
 *
 * With dump set, the first DUMP_BYTES bytes go to the screen as hex,
 * which is what tells you a baud mismatch from a dead wire: garbage is
 * a rate problem, nothing is a wiring one.
 */
static void poll_port(COMPAD_DECODER *d, SEEN *s, int dump)
{
    while (Bconstat(1))
    {
        uint8_t b = (uint8_t)Bconin(1);
        uint8_t got;

        if (dump && s->dumped < DUMP_BYTES)
        {
            printf("%02x ", b);
            if (++s->dumped == DUMP_BYTES)
                printf("...\r\n");
        }

        s->bytes++;
        got = compad_feed(d, b);

        if (!got)
            continue;

        s->frames++;

        if (COMPAD_HDR_TYPE(d->buf[1]) == COMPAD_TYPE_STATE)
        {
            s->buttons = compad_state_buttons(d->buf);
            s->lx = COMPAD_STATE_LX(d->buf);
            s->ly = COMPAD_STATE_LY(d->buf);
        }
    }
}

/*
 * Nothing came in where we looked. Before blaming the wiring, try every
 * serial port in turn: on a Mega STE the adapter may simply be in a
 * different socket from the one the BIOS is pointed at, and the MFP is
 * included because "the one we already tried" is exactly what a
 * resident driver may have remapped.
 */
static int hunt(void)
{
    long was;
    int found = 0;
    unsigned i;

    if (!stport_has_bconmap())
        return 0; /* no Bconmap on this TOS, so there is nothing to try */

    printf("\r\nnothing on the mapped port. trying all three...\r\n");
    fflush(stdout);

    was = Bconmap(-1); /* remember the mapping without changing it */

    for (i = 0; i < STPORT_COUNT; i++)
    {
        COMPAD_DECODER d;
        SEEN s;
        long t;

        /* A plain ST or STE on TOS 2 has Bconmap but only the MFP, and
         * a device it lacks leaves the mapping where it was: sample it
         * anyway and the MFP's bytes get reported under a port that
         * does not exist. */
        if (!stport_select(stports[i].dev))
        {
            printf("  device %d, %-28s not on this machine\r\n",
                   stports[i].dev, stports[i].name);
            continue;
        }

        memset(&s, 0, sizeof(s));
        compad_init(&d);

        for (t = 0; t < HUNT_TICKS; t++)
        {
            poll_port(&d, &s, 0);
            Vsync();
        }

        printf("  device %d, %-28s %ld bytes, %ld frames\r\n",
               stports[i].dev, stports[i].name, s.bytes, s.frames);

        if (s.bytes > 0 && !found)
        {
            printf("  ^^ the adapter is on this one\r\n");
            found = stports[i].dev;
        }

        fflush(stdout);
    }

    Bconmap(was);

    return found;
}

int main(void)
{
    COMPAD_DECODER d;
    SEEN s;
    SEEN shown;
    long ticks;
    int passed;
    int port = 0;

    memset(&s, 0, sizeof(s));
    stport_configure();
    compad_init(&d);

    printf("COMpad pipe check: AUX at %d 8N1\r\n", COMPAD_BAUD);

    /* A short sample, ending in a verdict. */
    for (ticks = 0; ticks < TIMEOUT_TICKS && s.frames < ENOUGH_FRAMES; ticks++)
    {
        poll_port(&d, &s, 1);
        Vsync();
    }

    passed = s.frames >= ENOUGH_TO_PASS;

    printf("\r\n%ld bytes, %ld valid frames\r\n", s.bytes, s.frames);

    if (passed)
        printf("the pipe works\r\n");
    else if (s.bytes > 0)
        printf("bytes flow but do not frame: check baud and the writer\r\n");
    else
        printf("nothing arrived on this port\r\n");

    printf("COMPAD-DONE %d\r\n", passed ? 0 : 1);
    fflush(stdout);

    if (s.bytes == 0)
        port = hunt();

    /* If the hunt found the adapter somewhere else, watch there:
     * watching the port that already produced nothing would show zeros
     * for ever while the user waggled a stick that is being heard. */
    if (port)
    {
        stport_select(port);
        compad_init(&d);
        printf("\rwatching device %d\r\n", port);
    }

    /*
     * Then watch, until a key. The verdict above is a total after the
     * fact, which tells you bytes arrived but not that they are yours:
     * on hardware you want to move the stick and see the numbers move
     * with it. That is the difference between a wire that works and a
     * wire that is picking something up.
     *
     * One short line, overwritten with a carriage return, twice a
     * second. The TOS console is slow: a line per frame at 50 Hz would
     * never keep up, and even this line takes long enough to print that
     * the AUX ring can fill behind it on a slow console, which is why
     * it is short and infrequent. xpadview redraws only what changed
     * for the same reason.
     */
    printf("\r\nwatching the wire, press any key to stop\r\n");
    fflush(stdout);

    shown = s;
    shown.frames = -1; /* so the first line prints */

    for (ticks = 0; !Bconstat(2); ticks++)
    {
        poll_port(&d, &s, 0);

        /* Only when something on the line has changed, and at most
         * twice a second: an idle pad then costs the console nothing,
         * and a moving one cannot outrun it. */
        if ((ticks % 25) == 0 &&
            (s.frames != shown.frames || s.lx != shown.lx ||
             s.ly != shown.ly || s.buttons != shown.buttons))
        {
            printf("\rframes %ld  L %4d,%4d  buttons %06lx   ",
                   s.frames, (int)s.lx, (int)s.ly, (unsigned long)s.buttons);
            fflush(stdout);
            shown = s;
        }

        Vsync();
    }

    Cconin(); /* swallow the key that stopped us */
    printf("\r\n");

    return passed ? 0 : 1;
}
