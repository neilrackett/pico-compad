/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * Pin assignments and timings, in one place because they are what a
 * builder changes and nothing else here should be.
 *
 * The only component this firmware requires is the MAX3232 on UART0.
 * The button and the external LED are optional and need no build flag
 * to leave out: see the comments on each.
 */

#ifndef COMPAD_CONFIG_H
#define COMPAD_CONFIG_H

/* The wire. GP0 and GP1 are UART0's default pins, and docs/hardware.md
 * wires them to the MAX3232's T1IN and R1OUT. */
#define COMPAD_UART uart0
#define COMPAD_UART_TX 0
#define COMPAD_UART_RX 1

/* The line rate is COMPAD_BAUD in target/atarist/src/protocol.h, the
 * one header both ends compile, so it is not repeated here. */

/*
 * Optional, and absent by construction rather than by #ifdef.
 *
 * The button is an input with the internal pull-up enabled, so with
 * nothing attached it reads high, which is exactly "not pressed". No
 * wire, no press, no behaviour.
 *
 * The LED is an output. Driving a pin with nothing on it costs nothing
 * and upsets nothing, so the firmware always drives it and a builder
 * who wants to see it adds an LED and a resistor later. The CYW43
 * onboard LED shows the same states in the meantime.
 *
 * Neither pin is used by UART0 or by the CYW43, which owns GP23, GP24,
 * GP25 and GP29 on a Pico W.
 */
#define COMPAD_BUTTON_PIN 15
#define COMPAD_LED_PIN 16

/* Hold to forget every bonded pad. Long enough not to happen by
 * accident, short enough not to feel broken. */
#define COMPAD_FORGET_HOLD_MS 2000

/*
 * 50 Hz, the rate the harness has sent at since phase 1. The two
 * intervals below are in ticks, and match harness/server.js exactly so
 * that the adapter and the development rig behave the same way.
 *
 * These three are the exception to this file: they are protocol policy
 * from docs/protocol.md, not things a builder should change. The pins
 * above are yours; these are the spec's.
 */
#define COMPAD_TICK_MS 20
#define COMPAD_DESCRIPTOR_EVERY 10 /* five times a second        */
#define COMPAD_KEEPALIVE_EVERY 12  /* about every 250 ms         */

#define COMPAD_TICKS_PER_SEC (1000 / COMPAD_TICK_MS)

/* How often to re-ask whether anything is bonded, while no pad is
 * connected. The events that change the answer mark it due instead of
 * answering it, so there is one path and it is this one. Cheap: the
 * BLE count is a cached integer and the Classic walk is a few dozen
 * XIP reads. */
#define COMPAD_BOND_RESCAN COMPAD_TICKS_PER_SEC

/* Blink periods in ticks. They only have to be obviously different
 * from each other, per docs/roadmap.md: plain on and off, no fade. */
#define COMPAD_BLINK_FAST 5  /* 100 ms: looking for a new pad    */
#define COMPAD_BLINK_SLOW 40 /* 800 ms: waiting for a known one  */

#endif /* COMPAD_CONFIG_H */
