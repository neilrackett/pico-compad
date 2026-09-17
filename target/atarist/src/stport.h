/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * The ST's serial port, as the three programs here see it.
 *
 * One home for the facts that must agree between the provider and the
 * two bring-up tools: the line settings, the Bconmap device numbers,
 * and the TOS version that decides whether Bconmap exists at all. They
 * were copied into all three, and the baud in particular is a
 * two-ended contract: change it in one program and the diagnostics you
 * reach for when the link is flaky quietly disagree with the driver,
 * which reads as "bytes flow but do not frame".
 *
 * TOS-specific by design, which is why none of this is in protocol.h:
 * that file is the wire contract and builds on the host.
 */

#ifndef COMPAD_STPORT_H
#define COMPAD_STPORT_H

#include <mint/osbind.h>

#include "protocol.h" /* COMPAD_BAUD */

/*
 * Rsconf speed codes count down from the fastest, so 0 is 19200 and 1
 * is 9600. Derived from the one constant both ends share, and an error
 * rather than a guess for any rate Rsconf cannot produce.
 */
#if COMPAD_BAUD == 19200
#define STPORT_BAUD 0
#elif COMPAD_BAUD == 9600
#define STPORT_BAUD 1
#else
#error "COMPAD_BAUD is not a rate Rsconf offers"
#endif

#define UCR_8N1 0x88

/*
 * Bconmap devices. A Mega STE has three serial ports on two chips, and
 * the rear panel labels are not a reliable guide to which is which:
 * the MFP port, the one a plain ST's modem port corresponds to, is
 * Modem 1, measured on hardware. SENDTEST.TOS names the socket for you.
 */
#define BCONMAP_MFP 6   /* Modem 1: the ST-compatible port, boot default */
#define BCONMAP_SCC_B 7 /* Modem 2                                        */
#define BCONMAP_SCC_A 9 /* LAN                                            */

/* Bconmap arrived with TOS 2.00; anything older has only the MFP. */
#define TOS_WITH_BCONMAP 0x0200

typedef struct
{
    int dev;
    const char *name;
} STPORT;

static const STPORT stports[] __attribute__((unused)) = {
    {BCONMAP_MFP, "MFP, Modem 1 on a Mega STE"},
    {BCONMAP_SCC_B, "SCC channel B, Modem 2"},
    {BCONMAP_SCC_A, "SCC channel A, LAN"},
};

#define STPORT_COUNT (sizeof(stports) / sizeof(stports[0]))

/* The OS header pointer lives at 0x4f2, below 0x800, where the ST bus
 * errors on user mode access, so the read runs under Supexec. */
static unsigned short stport_tos_version;

static inline long stport_read_tos_version(void)
{
    char *sysbase = *(char **)0x4f2L;

    stport_tos_version = *(unsigned short *)(sysbase + 2);

    return 0;
}

static inline unsigned short tos_version(void)
{
    Supexec(stport_read_tos_version);

    return stport_tos_version;
}

/* COMPAD_BAUD 8N1, no handshake, on whatever device 1 is mapped to. */
static inline void stport_configure(void)
{
    Rsconf(STPORT_BAUD, 0, UCR_8N1, -1, -1, -1);
}

static inline int stport_has_bconmap(void)
{
    return tos_version() >= TOS_WITH_BCONMAP;
}

/*
 * Map device 1 to a port and configure it, as one step, because Rsconf
 * acts on whatever is mapped at that moment and the two must not be
 * separated. Returns the previous device, or 0 if this machine has no
 * such port: Bconmap leaves the mapping alone then, so the caller must
 * not believe bytes on a port it never reached. Bconmap(-1) is the
 * query form for remembering a mapping without changing it.
 */
static inline long stport_select(int dev)
{
    long was = Bconmap(dev);

    if (was > 0)
        stport_configure();

    return was;
}

#endif /* COMPAD_STPORT_H */
