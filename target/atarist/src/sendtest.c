/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * Transmit test: prove the ST can get bytes out of a serial port.
 *
 * Bring-up only. Everything else here tests the adapter talking to the
 * ST, which is the hard direction to diagnose: if nothing arrives you
 * cannot tell a dead wire from a wrong socket from a silent adapter.
 * This does the opposite and much easier thing. The ST sends a pattern,
 * the adapter's USB console says what it received, and between them
 * they name the socket.
 *
 * It sends 0x55 deliberately: alternating bits, so it is legible on a
 * scope and any baud mismatch shows up as something other than 'U'.
 *
 * Tries every Bconmap device in turn, because on a Mega STE the rear
 * panel labels are not a reliable guide to which chip is behind them.
 *
 *   SENDTEST.TOS         cycle through the ports until a key is pressed
 */

#include <mint/osbind.h>
#include <stdint.h>
#include <stdio.h>

#define BAUD_19200 0
#define UCR_8N1 0x88

#define BCONMAP_MFP 6
#define BCONMAP_SCC_B 7
#define BCONMAP_SCC_A 9
#define TOS_WITH_BCONMAP 0x0200

#define BURST 64        /* bytes per burst, about 33 ms at 19200 */
#define TICKS_PER_PORT 150 /* ~3 s of Vsync at 50 Hz             */

static unsigned short tos_version;

/* 0x4f2 is below 0x800, where the ST bus errors in user mode. */
static long read_tos_version(void)
{
    char *sysbase = *(char **)0x4f2L;

    tos_version = *(unsigned short *)(sysbase + 2);

    return 0;
}

static void blast(int dev, const char *name)
{
    long ticks, sent = 0;

    if (dev >= 0)
        Bconmap(dev);

    Rsconf(BAUD_19200, 0, UCR_8N1, -1, -1, -1);

    printf("\r\ndevice %d, %-22s sending 0x55 ", dev, name);
    fflush(stdout);

    for (ticks = 0; ticks < TICKS_PER_PORT; ticks++)
    {
        int i;

        /* Bcostat tells us there is room, so a dead port cannot wedge
         * this the way a blocking write would. */
        for (i = 0; i < BURST && Bcostat(1); i++)
        {
            Bconout(1, 0x55);
            sent++;
        }

        if ((ticks % 25) == 0)
        {
            printf(".");
            fflush(stdout);
        }

        Vsync();
    }

    printf(" %ld bytes\r\n", sent);
    fflush(stdout);
}

int main(void)
{
    long was = -1;

    printf("COMpad transmit test\r\n");
    printf("watch the adapter's USB console for what arrives\r\n");

    Supexec(read_tos_version);

    if (tos_version < TOS_WITH_BCONMAP)
    {
        /* One port, and no way to ask for another. */
        printf("TOS %x: no Bconmap, so the MFP is all there is\r\n",
               tos_version);

        while (!Bconstat(2))
            blast(-1, "the only serial port");
    }
    else
    {
        was = Bconmap(BCONMAP_MFP);

        while (!Bconstat(2))
        {
            blast(BCONMAP_MFP, "MFP, ST compatible");
            if (Bconstat(2))
                break;
            blast(BCONMAP_SCC_B, "SCC channel B");
            if (Bconstat(2))
                break;
            blast(BCONMAP_SCC_A, "SCC channel A, LAN");
        }

        Bconmap(was);
    }

    Cconin();
    printf("\r\n");

    return 0;
}
