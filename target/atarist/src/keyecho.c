/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * One line per change, and nothing else.
 *
 * XPADVIEW redraws its whole screen every frame through the TOS
 * console, which is slow enough on an 8 MHz machine to be worth ruling
 * out before blaming the link. This prints a single short line only
 * when the published buttons actually change, so what you are watching
 * is the pipeline and not the redraw.
 *
 * Read it against the harness's --verbose output: that timestamps the
 * key arriving and the frame going on the wire, this timestamps the ST
 * seeing it. The gap between them is the delay, and which side it
 * falls on says where to look.
 *
 * Runs until Esc.
 */

#include <stdio.h>
#include <mint/osbind.h>

#include "xpad.h"

/* _hz_200 ticks 200 times a second, so this is centiseconds since the
 * machine booted: enough resolution to see a five second gap. */
static long read_ticks(void)
{
    return *(volatile long *)0x4baL;
}

int main(void)
{
    XPAD *x = (XPAD *)xpad_find();
    XPAD_PAD pad;
    unsigned long last = 0xffffffffUL;
    long changes = 0;

    if (!x)
    {
        printf("No xpad provider. Is COMPAD.PRG in AUTO?\r\n");
        printf("Press a key.\r\n");
        (void)Cconin();
        return 1;
    }

    printf("Key echo: %s\r\n", x->provider ? x->provider : "(unnamed)");
    printf("One line per change. Esc quits.\r\n\r\n");

    for (;;)
    {
        if (Cconis())
        {
            if ((Cconin() & 0xff) == 27)
                break;
        }

        if (xpad_read(x, 0, &pad) && (unsigned long)pad.buttons != last)
        {
            long t = Supexec(read_ticks);

            last = (unsigned long)pad.buttons;
            printf("%3ld  t=%ld.%02lds  buttons %08lx\r\n",
                   ++changes, t / 200, (t % 200) / 2, last);
        }
    }

    printf("\r\n%ld changes seen.\r\n", changes);
    return 0;
}
