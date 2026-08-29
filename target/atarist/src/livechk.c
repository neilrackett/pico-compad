/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * Does the provider follow a CHANGING sender?
 *
 * Every other end-to-end test holds one pattern, which a consumer can
 * pass while re-reading the same stale bytes forever: they decode to
 * the same value and look perfect. That is exactly what compad.c did,
 * having ibufhd and ibuftl the wrong way round, and no held-pattern
 * test could see it. This watches the published block and insists the
 * value actually moves.
 */

#include <stdio.h>
#include <mint/osbind.h>

#include "xpad.h"

#define SAMPLES 40
#define WANT_CHANGES 4

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
    int i;

    if (!x)
    {
        printf("no provider found\r\nLIVE-DONE 1\r\n");
        return 1;
    }

    printf("provider %s\r\n", x->provider ? x->provider : "(unnamed)");

    for (i = 0; i < SAMPLES; i++)
    {
        long t0 = Supexec(read_ticks);

        while (Supexec(read_ticks) - t0 < 60) /* 60 * 5ms = 300ms */
            ;

        if (xpad_read(x, 0, &pad) && (unsigned long)pad.buttons != last)
        {
            last = (unsigned long)pad.buttons;
            changes++;
            printf("change %ld: buttons %08lx\r\n", changes, last);
        }
    }

    printf("changes %ld (want at least %d)\r\n", changes, WANT_CHANGES);
    printf("LIVE-DONE %d\r\n", changes >= WANT_CHANGES ? 0 : 1);
    return 0;
}
