/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * Transmit test: prove the ST can get bytes out of a serial port, and
 * back in again.
 *
 * Bring-up only. Everything else here tests the adapter talking to the
 * ST, which is the hard direction to diagnose: if nothing arrives you
 * cannot tell a dead wire from a wrong socket from a silent adapter.
 * This does the opposite and much easier thing. The ST sends, the
 * adapter's USB console says what it received, and between them they
 * name the socket.
 *
 * Each port sends its own byte, matching its Bconmap device number so
 * the adapter's console names the socket rather than leaving you to
 * correlate two screens by eye: 0x66 for the MFP, 0x77 for SCC channel
 * B, 0x99 for channel A.
 *
 * It also counts what comes back, which makes it a loopback test for
 * the ST itself. Short pins 2 and 3 on the ST's own socket, with
 * nothing else attached, and a working port reports roughly as many
 * bytes received as sent. A port that sends but never receives has its
 * receive path broken or disabled, which is a fault in the machine and
 * not in any adapter.
 *
 *   SENDTEST.TOS         cycle through the ports until a key is pressed
 */

#include <mint/osbind.h>
#include <stdint.h>
#include <stdio.h>

#include "stport.h"

#define TICKS_PER_PORT 150 /* ~3 s of Vsync at 50 Hz */

/*
 * Bcostat goes false as soon as the MFP's transmit register is full,
 * which is after a byte or two, so a burst that stops at the first
 * not-ready sends a couple of bytes per vsync: about a hundred a
 * second, not the wire's nineteen hundred. That is still plenty to
 * name a socket, and it keeps a dead port from wedging the loop the
 * way a blocking write would, so it is left that way deliberately.
 */
#define BURST 64

/* Send one port's byte for a while, on whatever device 1 is mapped to
 * now, and count what comes back. The byte is derived from the device
 * number here, once, so every path sends the byte the header promises. */
static void blast(int dev, const char *name)
{
    uint8_t mark = (uint8_t)((dev << 4) | dev);
    long ticks, sent = 0, got = 0, wrong = 0;

    printf("\r\n%-28s sending %02x ", name, mark);
    fflush(stdout);

    for (ticks = 0; ticks < TICKS_PER_PORT; ticks++)
    {
        int i;

        for (i = 0; i < BURST && Bcostat(1); i++)
        {
            Bconout(1, mark);
            sent++;
        }

        /* Anything coming back, whether from a loopback at the shell
         * or from something on the other end that echoes. */
        while (Bconstat(1))
        {
            if ((Bconin(1) & 0xff) == mark)
                got++;
            else
                wrong++;
        }

        if ((ticks % 25) == 0)
        {
            printf(".");
            fflush(stdout);
        }

        Vsync();
    }

    printf(" sent %ld, back %ld", sent, got);

    if (wrong)
        printf(", %ld not %02x", wrong, mark);

    printf("\r\n");
    fflush(stdout);
}

int main(void)
{
    printf("COMpad %s transmit test\r\n", COMPAD_VERSION);
    printf("watch the adapter's USB console for what arrives\r\n");

    if (!stport_has_bconmap())
    {
        /* One port, and no way to ask for another. Said in a branch
         * rather than folded into the table, because that is the
         * fact: this TOS cannot be asked. */
        printf("no Bconmap on this TOS, so the MFP is all there is\r\n");
        stport_configure();

        while (!Bconstat(2))
            blast(BCONMAP_MFP, stports[0].name);
    }
    else
    {
        long was = Bconmap(-1); /* remember it without changing it */
        unsigned i;

        for (i = 0; !Bconstat(2); i = (i + 1) % STPORT_COUNT)
        {
            /* A machine without this port leaves the mapping alone, and
             * blasting anyway would send this port's byte out of the
             * previous one, naming a socket that does not exist. */
            if (!stport_select(stports[i].dev))
            {
                printf("\r\n%-28s not on this machine\r\n", stports[i].name);
                continue;
            }

            blast(stports[i].dev, stports[i].name);
        }

        Bconmap(was);
    }

    Cconin();
    printf("\r\n");

    return 0;
}
