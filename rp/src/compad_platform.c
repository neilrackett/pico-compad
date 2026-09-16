/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * The Bluepad32 platform: what to do when a pad connects, moves or
 * goes away, and what to put on the wire.
 *
 * The decisions here were settled in docs/roadmap.md's phase 3 before
 * any of this existed, so this file implements them rather than
 * inventing them. In short: no pairing mode, one optional button whose
 * only job is forgetting bonds, and three LED timings that fall out of
 * whether a bond is stored rather than out of a mode.
 *
 * Everything with logic worth getting wrong is in encode.h, which the
 * host build tests. This file is the parts that need a Pico.
 */

#include <btstack_run_loop.h>
#include <ble/le_device_db.h>
#include <gap.h>
#include <hardware/gpio.h>
#include <hardware/uart.h>
#include <pico/cyw43_arch.h>
#include <string.h>
#include <uni.h>

#include "config.h"
#include "encode.h"
#include "sdkconfig.h"

/* The protocol's pad index is two bits, and Xpad carries four pads. */
#define COMPAD_PADS 4

/* Nothing here can tell a DualSense from an Xbox pad without a table of
 * vendor ids, and XPAD_TYPE_GAMEPAD is honest where guessing would not
 * be. The constant comes from xpad.h through encode.h. */

typedef struct
{
    bool present;
    COMPAD_STATE state;
    COMPAD_STATE sent;
    uint8_t since_send;     /* ticks since this pad's last frame     */
    uint8_t since_descriptor;
} COMPAD_SLOT;

static COMPAD_SLOT slots[COMPAD_PADS];
static btstack_timer_source_t tick_timer;

/* Blink phase, ticks since the bond answer was last refreshed, and how
 * long the button has been held. */
static uint16_t blink;
static uint16_t bond_age;
static uint16_t held_ticks;

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

/*
 * Blocking, and deliberately so. A state frame is 12 bytes, the FIFO
 * holds 32, and at 9600 baud a full frame is 12.5 ms against a 20 ms
 * tick, so this only ever waits when the link is already saturated,
 * which is the case docs/protocol.md's send-on-change policy exists to
 * avoid. Queueing instead would add a buffer whose only job is to hide
 * that, and a queue on a link this slow is latency you never get back.
 */
static void wire_write(const uint8_t *f, uint8_t len)
{
    uart_write_blocking(COMPAD_UART, f, len);
}

static void send_state(uint8_t pad)
{
    uint8_t f[CE_STATE_LEN];

    compad_state_frame(pad, &slots[pad].state, f);
    wire_write(f, CE_STATE_LEN);

    slots[pad].sent = slots[pad].state;
    slots[pad].since_send = 0;
}

static void send_descriptor(uint8_t pad)
{
    uint8_t f[CE_DESC_LEN];

    /* No caps claimed. XPAD_CAP_RUMBLE would be a promise this does not
     * keep yet: request frames are phase 4. */
    compad_descriptor_frame(pad, XPAD_TYPE_GAMEPAD, 0, 0, f);
    wire_write(f, CE_DESC_LEN);

    slots[pad].since_descriptor = 0;
}

/* ------------------------------------------------------------------ */
/* Bonds, and therefore the LED                                        */
/* ------------------------------------------------------------------ */

/*
 * Whether anything is bonded decides what the LED means: with no
 * stored keys nothing can reconnect, so the adapter is looking for a
 * new pad; with keys, it is waiting for one it knows. That is the whole
 * of the "no pairing mode" design.
 *
 * Cached, because the answer changes about twice in a device's life and
 * the question is not free to ask: each lookup walks up to 16 TLV tags
 * and each of those rescans the flash bank from the start. It is XIP
 * reads rather than a flash transaction, so this is tidiness rather
 * than a fire, but it is asked 50 times a second while idle.
 *
 * Both databases have to be asked, which is the whole of why this was
 * wrong on hardware first time out. A controller bonds over Classic or
 * over BLE depending on what it is, and the two keep their keys in
 * different places: Classic in the link key database that
 * gap_link_key_iterator walks, BLE in the LE device database. An Xbox
 * Series pad bonds over BLE, so a Classic-only check reported no bonds
 * while the pad was reconnecting on its own in front of us, and the LED
 * claimed to be looking for a new pad for ever.
 *
 * Refreshed at the points that can change it, and then re-asked once a
 * second while nothing is connected, so a cache can never get stuck on
 * a stale no.
 */
static bool bonds;

static bool scan_bonds(void)
{
    btstack_link_key_iterator_t it;
    bd_addr_t addr;
    link_key_t key;
    link_key_type_t type;
    bool any = false;

    if (le_device_db_count() > 0)
        return true;

    if (!gap_link_key_iterator_init(&it))
        return false;

    if (gap_link_key_iterator_get_next(&it, addr, key, &type))
        any = true;

    gap_link_key_iterator_done(&it);

    return any;
}

static void refresh_bonds(void)
{
    bonds = scan_bonds();
}

static bool any_connected(void)
{
    int i;

    for (i = 0; i < COMPAD_PADS; i++)
    {
        if (slots[i].present)
            return true;
    }

    return false;
}

/*
 * Only on a change, which matters more than it looks.
 * cyw43_arch_gpio_put() is not a pin write: the onboard LED hangs off
 * the wireless chip, so it becomes an SDPCM control frame over the gSPI
 * bus and then a busy-poll for the reply, hundreds of microseconds at a
 * time. Called unconditionally from a 20 ms tick that is 50 of those a
 * second, every one of them redundant while a pad is connected and the
 * LED is already solid, competing with live Bluetooth traffic on the
 * same chip and the same run loop. One bool removes all of it.
 */
static void led_set(bool on)
{
    static int shown = -1; /* neither on nor off, so the first call writes */

    if (shown == (int)on)
        return;

    shown = on;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
    gpio_put(COMPAD_LED_PIN, on);
}

/*
 * Solid when connected, rapid flash when looking for a new pad, long
 * blink when waiting for a known one. Plain on and off: the CYW43 LED
 * hangs off the wireless chip and has no PWM, and a fade was never
 * wanted anyway.
 */
static void led_tick(void)
{
    uint16_t period;

    if (any_connected())
    {
        led_set(true);
        blink = 0;
        bond_age = 0;
        return;
    }

    if (++bond_age >= COMPAD_BOND_RESCAN)
    {
        bond_age = 0;
        refresh_bonds();
    }

    period = bonds ? COMPAD_BLINK_SLOW : COMPAD_BLINK_FAST;

    blink++;
    led_set((blink / period) & 1);
}

/* ------------------------------------------------------------------ */
/* The optional button                                                 */
/* ------------------------------------------------------------------ */

/*
 * Held low for long enough, forget every bonded pad. With no button
 * wired the pull-up keeps this high and none of it ever runs.
 *
 * There is no confirmation blink, because none is needed: wiping the
 * keys moves the LED from the slow blink to the fast one by itself,
 * which is the feedback. Only fires once per hold.
 */
static void button_tick(void)
{
    const uint16_t hold = COMPAD_FORGET_HOLD_MS / COMPAD_TICK_MS;

    if (gpio_get(COMPAD_BUTTON_PIN))
    {
        held_ticks = 0;
        return;
    }

    if (held_ticks <= hold)
        held_ticks++;

    if (held_ticks == hold)
    {
        logi("compad: forgetting bonded pads\n");
        uni_bt_del_keys_safe();
        refresh_bonds();
    }
}

/* ------------------------------------------------------------------ */
/* The tick                                                            */
/* ------------------------------------------------------------------ */

/*
 * Send on change plus a keepalive, which is what docs/protocol.md
 * specifies and harness/server.js has done since phase 1. Streaming
 * unconditionally at 50 Hz is 63% of a 9600 link, and at that load a
 * receiver that falls behind never catches up.
 */
static void tick(btstack_timer_source_t *ts)
{
    int i;

    /* Re-armed first. set_timer is relative to now, so re-arming after
     * the sends would make the period 20 ms plus however long they
     * took, and the descriptor and keepalive intervals are counted in
     * ticks: they would stretch with it and stop matching the rates
     * config.h claims. */
    btstack_run_loop_set_timer(ts, COMPAD_TICK_MS);
    btstack_run_loop_add_timer(ts);

    for (i = 0; i < COMPAD_PADS; i++)
    {
        COMPAD_SLOT *s = &slots[i];

        if (!s->present)
            continue;

        /* Neither counter can run away: both are reset below, in this
         * same pass, well before a uint8_t could wrap. */
        s->since_descriptor++;
        s->since_send++;

        /* Repeated, not sent once: the provider discards the serial
         * ring when it installs, so a descriptor that arrived while
         * the ST was still booting is gone. */
        if (s->since_descriptor >= COMPAD_DESCRIPTOR_EVERY)
            send_descriptor((uint8_t)i);

        if (compad_state_differs(&s->state, &s->sent) ||
            s->since_send >= COMPAD_KEEPALIVE_EVERY)
            send_state((uint8_t)i);
    }

    led_tick();
    button_tick();
}

/* ------------------------------------------------------------------ */
/* Platform                                                            */
/* ------------------------------------------------------------------ */

static void compad_platform_init(int argc, const char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    memset(slots, 0, sizeof(slots));
}

static void compad_on_init_complete(void)
{
    /*
     * No pairing mode. Scan and autoconnect from the start: a new pad
     * joins the first time it is put into pairing mode, and a bonded
     * one comes back on its own because incoming connections stay
     * allowed. The ordinary case needs no interface at all.
     *
     * Keys are emphatically not deleted here, which the Bluepad32
     * example does. Deleting them on every boot would mean re-pairing
     * every time the ST is switched on.
     */
    uni_bt_start_scanning_and_autoconnect_unsafe();
    uni_bt_allow_incoming_connections(true);

    btstack_run_loop_set_timer_handler(&tick_timer, tick);
    btstack_run_loop_set_timer(&tick_timer, COMPAD_TICK_MS);
    btstack_run_loop_add_timer(&tick_timer);

    refresh_bonds();
    logi("compad: ready, %s\n",
         bonds ? "waiting for a known pad" : "looking for a new pad");
}

static uni_error_t compad_on_device_discovered(bd_addr_t addr,
                                               const char *name, uint16_t cod,
                                               uint8_t rssi)
{
    /* Not ARG_UNUSED: it is sizeof based, and bd_addr_t is an array
     * parameter, so it warns about measuring the pointer instead. */
    (void)addr;
    ARG_UNUSED(name);
    ARG_UNUSED(cod);
    ARG_UNUSED(rssi);

    /* Everything supported is welcome. Filtering belongs in an
     * allowlist, not in a shim that would reject a pad silently. */
    return UNI_ERROR_SUCCESS;
}

/*
 * Empty, but not optional: Bluepad32 calls this and on_oob_event
 * without a null check (uni_hid_device.c), so leaving them out of the
 * vtable is a jump through NULL. get_property is null-checked there and
 * is therefore absent rather than stubbed. The three look
 * interchangeable and are not.
 */
static void compad_on_device_connected(uni_hid_device_t *d)
{
    ARG_UNUSED(d);
}

static void compad_on_device_disconnected(uni_hid_device_t *d)
{
    int idx = uni_hid_device_get_idx_for_instance(d);

    if (idx < 0 || idx >= COMPAD_PADS)
        return;

    /*
     * Publish the pad at rest before dropping it, so a game does not
     * inherit whatever was held at the moment the batteries died. The
     * ST has no way to hear "gone": full state per frame is the whole
     * recovery model, and all-zero is the honest last word.
     */
    memset(&slots[idx].state, 0, sizeof(slots[idx].state));
    send_state((uint8_t)idx);

    memset(&slots[idx], 0, sizeof(slots[idx]));

    /* A pad that has just been used is a pad that has just bonded, and
     * the LED is about to start caring which. */
    refresh_bonds();
}

static uni_error_t compad_on_device_ready(uni_hid_device_t *d)
{
    int idx = uni_hid_device_get_idx_for_instance(d);

    if (idx < 0 || idx >= COMPAD_PADS)
        return UNI_ERROR_NO_SLOTS;

    memset(&slots[idx], 0, sizeof(slots[idx]));
    slots[idx].present = true;

    send_descriptor((uint8_t)idx);
    send_state((uint8_t)idx);

    /* Phase the repeats apart by slot, after the send that zeroes the
     * counter. Four pads that connected together would otherwise keep
     * their counters in step for ever and put four descriptors on a
     * 9600 link in the same tick, which is 79 ms of wire time inside a
     * 20 ms callback. */
    slots[idx].since_descriptor =
        (uint8_t)(idx * (COMPAD_DESCRIPTOR_EVERY / COMPAD_PADS));

    return UNI_ERROR_SUCCESS;
}

/*
 * Called whenever the pad reports, which is faster than the tick and
 * not on a clock we control. So this only records the latest state and
 * the tick decides what goes on the wire: the link is the scarce thing
 * here, not the radio.
 */
static void compad_on_controller_data(uni_hid_device_t *d,
                                      uni_controller_t *ctl)
{
    int idx = uni_hid_device_get_idx_for_instance(d);
    const uni_gamepad_t *gp;

    if (idx < 0 || idx >= COMPAD_PADS || !slots[idx].present)
        return;

    /* Mice and keyboards reach this too, and have no pad to report. */
    if (ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD)
        return;

    gp = &ctl->gamepad;

    compad_map(gp->dpad, gp->buttons, gp->misc_buttons, gp->axis_x,
               gp->axis_y, gp->axis_rx, gp->axis_ry, gp->brake, gp->throttle,
               &slots[idx].state);
}

static void compad_on_oob_event(uni_platform_oob_event_t event, void *data)
{
    ARG_UNUSED(event);
    ARG_UNUSED(data);
}

struct uni_platform *get_compad_platform(void)
{
    static struct uni_platform plat = {
        .name = "COMpad",
        .init = compad_platform_init,
        .on_init_complete = compad_on_init_complete,
        .on_device_discovered = compad_on_device_discovered,
        .on_device_connected = compad_on_device_connected,
        .on_device_disconnected = compad_on_device_disconnected,
        .on_device_ready = compad_on_device_ready,
        .on_oob_event = compad_on_oob_event,
        .on_controller_data = compad_on_controller_data,
    };

    return &plat;
}
