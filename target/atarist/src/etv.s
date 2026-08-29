| SPDX-License-Identifier: GPL-3.0-or-later
| SPDX-FileCopyrightText: 2026 Neil Rackett

| etv_timer trampoline.
|
| TOS calls the etv_timer vector ($400) from its Timer C handler at
| about 200 Hz, in supervisor, as a plain subroutine. This saves the
| whole register set around the C tick, then tail-chains into whatever
| the vector held before: chaining is not optional, and the full save is
| cheap insurance inside somebody else's interrupt.
|
| The tick runs with the IPL dropped to 5. Timer C arrives at level 6
| with everything at or below masked, including the keyboard ACIA on
| GPIP4, and the ACIA has one byte of buffer at 7812.5 baud. A handler
| that holds level 6 for too long therefore loses IKBD bytes, and a
| lost byte desyncs the packet stream: mouse deltas replay as phantom
| scancodes, packet headers replay as small negative deltas, and the
| pointer crawls into a corner clicking all the way. Level 5 keeps HBL,
| VBL and traps out while letting the MFP back in; the ACIA outranks
| Timer C inside the MFP, so it is serviced mid-tick. The tas guard
| stops a re-entered Timer C from running the tick twice; the chain
| still runs so TOS never misses its own beat.

        .text
        .even

        .globl  _compad_etv_entry
        .extern _compad_etv_chain       | previous vector, set by install
        .extern _compad_tick            | void (*)(void)

_compad_etv_entry:
        tas     compad_busy
        bne.s   chain
        move.w  %sr,-(%sp)
        move.w  #0x2500,%sr
        movem.l %d0-%d7/%a0-%a6,-(%sp)
        jsr     _compad_tick
        movem.l (%sp)+,%d0-%d7/%a0-%a6
        move.w  (%sp)+,%sr
        clr.b   compad_busy
chain:
        move.l  _compad_etv_chain,-(%sp)
        rts

        .bss
        .even
compad_busy:
        .ds.b   1
