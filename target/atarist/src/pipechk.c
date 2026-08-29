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

#define BAUD_9600 1 /* Rsconf speed code */
#define UCR_8N1 0x88

#define TIMEOUT_TICKS 750 /* ~15 s of Vsync at 50 Hz */
#define ENOUGH_TO_PASS 3 /* a handful proves the framing */
#define ENOUGH_FRAMES 10

int main(void)
{
    COMPAD_DECODER d;
    long bytes = 0;
    long frames = 0;
    long ticks;

    Rsconf(BAUD_9600, 0, UCR_8N1, -1, -1, -1);
    compad_init(&d);

    printf("COMpad pipe check: AUX at 9600 8N1\r\n");

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
        printf("nothing arrived: check --rs232-in and the FIFO\r\n");

    printf("COMPAD-DONE %d\r\n", frames >= ENOUGH_TO_PASS ? 0 : 1);
    fflush(stdout);

    return frames >= ENOUGH_TO_PASS ? 0 : 1;
}
