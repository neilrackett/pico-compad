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

/*
 * What the link carries in one tick, and the queue that smooths it.
 *
 * 8N1 is ten bits a byte, so 19200 is 1920 bytes a second and a 20 ms
 * tick is 38 of them. Three 12-byte state frames fit and four do not:
 * four pads all moving at once want 48 bytes a tick, which is 125% of
 * the link. So the tick spends a budget and remembers which pad it ran
 * out on, and that pad goes first next time. Every frame is full
 * state, so a pad that loses the race is a frame later, never wrong.
 *
 * The queue is there because the budget is larger than the UART's 32
 * byte FIFO, not to hide a saturated link. Below three pads nothing
 * ever queues: a frame goes straight into the FIFO and the ring stays
 * empty. 128 bytes is several ticks of slack, which is far more than
 * the budget can put in.
 */
#define COMPAD_TX_BUDGET ((COMPAD_BAUD / 10) / COMPAD_TICKS_PER_SEC)
#define COMPAD_TXBUF 128

/* How many times a departing pad's XPAD_TYPE_NONE descriptor is
 * repeated, at the descriptor interval: about two seconds, which is
 * far longer than any burst of noise this link has seen and short
 * enough that an empty slot goes quiet. */
#define COMPAD_GONE_NOTICES 10

/* How often to re-ask whether anything is bonded, while no pad is
 * connected. The events that change the answer mark it due instead of
 * answering it, so there is one path and it is this one. Cheap: the
 * BLE count is a cached integer and the Classic walk is a few dozen
 * XIP reads. */
#define COMPAD_BOND_RESCAN COMPAD_TICKS_PER_SEC

/*
 * Rumble. xpad's request area carries magnitudes and no duration, so a
 * consumer that sets them means "until I set something else", while
 * Bluepad32's play_dual_rumble wants a duration. So the firmware arms
 * a slightly longer burst than the interval it re-arms at: the motor
 * never gaps, and if the ST stops asking, or is switched off mid
 * rumble, the pad falls silent on its own within one burst rather than
 * buzzing for ever.
 */
#define COMPAD_RUMBLE_MS 250
#define COMPAD_RUMBLE_REARM (200 / COMPAD_TICK_MS)

/* Blink periods in ticks. They only have to be obviously different
 * from each other, per docs/roadmap.md: plain on and off, no fade. */
#define COMPAD_BLINK_FAST 5  /* 100 ms: looking for a new pad    */
#define COMPAD_BLINK_SLOW 40 /* 800 ms: waiting for a known one  */

#endif /* COMPAD_CONFIG_H */
