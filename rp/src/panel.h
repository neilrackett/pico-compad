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
 * interval this fires at: the motor never gaps, and a host that stops
 * asking, or is switched off mid rumble, leaves the pad falling silent
 * on its own rather than buzzing for ever.
 *
 * Here rather than beside the Bluepad32 call because the decision has
 * no SDK in it. Arming does.
 */
static COMPAD_UNUSED int ce_rumble_due(int present, uint8_t lo, uint8_t hi,
                                       uint16_t rearm, uint16_t *age)
{
    if (!present || (!lo && !hi))
        return 0;

    return ++(*age) >= rearm;
}

#endif /* COMPAD_PANEL_H */
