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
 * Each port sends its own byte, so the adapter's console names the
 * socket rather than leaving you to correlate two screens by eye: 0x66
 * for the MFP, 0x77 for SCC channel B, 0x99 for channel A, chosen to
 * match the Bconmap device numbers. All three alternate bits enough to
 * be legible on a scope, and a baud mismatch shows up as something
 * other than the byte that was sent.
 *
 * Tries every Bconmap device in turn, because on a Mega STE the rear
 * panel labels are not a reliable guide to which chip is behind them.
 *
 *   SENDTEST.TOS         cycle through the ports until a key is pressed
 *
 * It counts what comes back as well as what it sends, which turns it
 * into a loopback test for the ST itself. Short pins 2 and 3 on the
 * ST's own socket, with nothing else attached, and a port that works
 * should report roughly as many bytes received as sent. A port that
 * sends but never receives has its receive path broken or disabled,
 * and that is a fault in the machine rather than in any adapter.
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
    long ticks, sent = 0, got = 0, wrong = 0;
    uint8_t mark = (dev < 0) ? 0x55 : (uint8_t)((dev << 4) | dev);

    if (dev >= 0)
        Bconmap(dev);

    Rsconf(BAUD_19200, 0, UCR_8N1, -1, -1, -1);

    printf("\r\ndevice %d, %-22s sending %02x ", dev, name, mark);
    fflush(stdout);

    for (ticks = 0; ticks < TICKS_PER_PORT; ticks++)
    {
        int i;

        /* Bcostat tells us there is room, so a dead port cannot wedge
         * this the way a blocking write would. */
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
        printf(", %ld not 0x55", wrong);

    printf("\r\n");
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
