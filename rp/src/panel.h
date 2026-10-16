/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * The board's two optional components: the status LED and the forget
 * button.
 *
 * A sibling to encode.h rather than part of it. That header turns
 * Bluepad32 controller state into COMpad frames, and neither of these
 * touches a frame, a pad or the wire; they are here for the same
 * reason encode.h exists, which is that the host build compiles any
 * header free of the Pico SDK and BTstack, and logic the host can
 * compile is logic the host can test.
 *
 * Both are reachable only by soldering something on and watching it,
 * which without this would put the least testable code in the
 * repository in the one place nobody's bench necessarily has.
 */

#ifndef COMPAD_PANEL_H
#define COMPAD_PANEL_H

#include <stdint.h>

#include <protocol.h> /* COMPAD_UNUSED */

/*
 * The status LED, as an Xbox controller's light behaves: solid when a
 * pad is connected, a fast blink while looking for a new one, a slow
 * blink while waiting for a pad it already knows.
 *
 * The phases only have to be obviously different from each other, so
 * the test is that they are, not what they measure.
 */
static COMPAD_UNUSED int ce_led_on(int connected, int bonded,
                                   uint16_t blink, uint16_t fast,
                                   uint16_t slow)
{
    if (connected)
        return 1;

    return (blink / (bonded ? slow : fast)) & 1;
}

/*
 * The forget button, which is held rather than pressed.
 *
 * Returns true on the single tick the hold completes, never again
 * until the button comes back up, because wiping bonds repeatedly for
 * as long as a finger rests on a switch is not what anyone means. The
 * counter stops at the threshold rather than running on, so a long
 * hold cannot wrap it back round to the firing value.
 *
 * pressed is the logical sense, not the pin: the pin has a pull-up and
 * reads low when pressed, and with no button wired it reads high for
 * ever, which is exactly "not pressed".
 */
static COMPAD_UNUSED int ce_forget_due(int pressed, uint16_t hold,
                                       uint16_t *held)
{
    if (!pressed)
    {
        *held = 0;
        return 0;
    }

    if (*held <= hold)
        (*held)++;

    return *held == hold;
}

/*
 * Whether a pad's motors want re-arming.
 *
 * xpad's request area carries magnitudes and no duration, so "until
 * replaced" is expressed by arming a burst slightly longer than the
 * interval this fires at, and the motor never gaps.
 *
 * It stops when the host does. A host holding a level resends it at
 * least every COMPAD_RUMBLE_KEEPALIVE_MS, so quiet counts the ticks
 * since this pad was last asked, and a pad quiet for timeout ticks has
 * a host that stopped asking, or was switched off mid rumble: nothing
 * is re-armed and the burst already running expires by itself. The
 * stored level alone cannot tell those apart from a game still asking,
 * which is how a reset mid rumble left a pad buzzing until it
 * disconnected.
 *
 * quiet advances here, every tick and whether or not anything is due,
 * and only a request resets it, which is the caller's to report. It
 * stops at the timeout rather than running on, so a long silence
 * cannot wrap it back round to "just asked".
 *
 * Here rather than beside the Bluepad32 call because the decision has
 * no SDK in it. Arming does.
 */
static COMPAD_UNUSED int ce_rumble_due(int present, uint8_t lo, uint8_t hi,
                                       uint16_t rearm, uint16_t *age,
                                       uint16_t timeout, uint16_t *quiet)
{
    if (!present || (!lo && !hi))
        return 0;

    if (*quiet < timeout)
        (*quiet)++;

    if (*quiet >= timeout)
        return 0;

    return ++(*age) >= rearm;
}

#endif /* COMPAD_PANEL_H */
