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
#include <stdio.h>
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

/* Which pad the tick's byte budget ran out on, and therefore which one
 * goes first next time. */
static uint8_t next_pad;

/*
 * The host to adapter direction. The ST pings to find which serial
 * port the adapter is on, and sends rumble requests when a consumer
 * writes xpad's request area.
 */
static COMPAD_DECODER rx_dec;

/* What the ST last asked each pad to do, and how long since it was
 * armed. Kept per pad so a re-arm does not need the frame again. */
static struct
{
    uint8_t lo, hi;
    uint16_t age;
} rumble[COMPAD_PADS];

/* Bytes seen on the wire's receive side, and when they were last
 * reported. */
static uint32_t rx_bytes;
static uint8_t rx_last[8];
static uint8_t rx_have;
static uint16_t rx_age;

/* Blink phase, ticks since the bond answer was last refreshed, and how
 * long the button has been held. bond_age starts due, so the first
 * disconnected tick asks rather than blinking a guess for a second. */
static uint16_t blink;
static uint16_t bond_age = COMPAD_BOND_RESCAN;
static uint16_t held_ticks;

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

/*
 * Frames queue here rather than going out under a spin loop.
 *
 * uart_write_blocking was called from the run loop's timer, which was
 * fine for one pad and not for three: a 12 byte frame is 6.25 ms at
 * 19200, so three pads moving at once spent 18.75 ms of a 20 ms tick
 * waiting on hardware that had already taken the bytes. Bluetooth and
 * the receive drain got whatever was left, and since the RX FIFO holds
 * less than one tick of this link, what was left mattered.
 *
 * Draining after each write keeps the wire timing identical to the
 * blocking version at any load the FIFO absorbs, which is everything
 * below three pads: the first byte leaves at the same instant and the
 * UART clocks out the rest on its own. What changes is only that the
 * CPU stops watching it happen.
 */
static uint8_t txbuf[COMPAD_TXBUF];
static uint8_t txhead, txtail;
static uint16_t tx_dropped;

static uint8_t tx_used(void)
{
    return (uint8_t)((txtail + COMPAD_TXBUF - txhead) % COMPAD_TXBUF);
}

/* As far into the FIFO as it will take, never waiting for it. */
static void tx_drain(void)
{
    while (txhead != txtail && uart_is_writable(COMPAD_UART))
    {
        uart_get_hw(COMPAD_UART)->dr = txbuf[txhead];
        txhead = (uint8_t)((txhead + 1) % COMPAD_TXBUF);
    }
}

static void wire_write(const uint8_t *f, uint8_t len)
{
    uint8_t i;

    tx_drain();

    /* Whole frames or nothing. A truncated one is bytes the ST has to
     * resync past, while a dropped one costs nothing: every frame is
     * full state, so the next is complete and correct on its own. */
    if (COMPAD_TXBUF - 1 - tx_used() < len)
    {
        tx_dropped++;
        return;
    }

    for (i = 0; i < len; i++)
    {
        txbuf[txtail] = f[i];
        txtail = (uint8_t)((txtail + 1) % COMPAD_TXBUF);
    }

    tx_drain();
}

static void send_state(uint8_t pad)
{
    uint8_t f[CE_STATE_LEN];

    compad_state_frame(pad, &slots[pad].state, f);
    wire_write(f, CE_STATE_LEN);

    slots[pad].sent = slots[pad].state;
    slots[pad].since_send = 0;
}

/*
 * Buttons only, five bytes instead of twelve, sent when the tick is
 * over budget and the axes have not moved.
 *
 * since_send is deliberately not reset. The keepalive's second job is
 * self healing: a receiver that lost bytes to noise gets its axes back
 * from a full state frame and from nothing else, so sustained button
 * mashing must not be able to hold one off indefinitely. Leaving the
 * counter alone means full state still goes out every 250 ms, and
 * because ce_compact_ok() requires the axes to match what was last
 * sent, nothing is lost in between.
 */
static void send_compact(uint8_t pad)
{
    uint8_t f[CE_COMPACT_LEN];

    compad_compact_frame(pad, slots[pad].state.buttons, f);
    wire_write(f, CE_COMPACT_LEN);

    slots[pad].sent = slots[pad].state;
}

/* Whether this pad's driver can actually drive its motors. */
static bool pad_can_rumble(int idx)
{
    uni_hid_device_t *d = uni_hid_device_get_instance_for_idx(idx);

    return d && d->report_parser.play_dual_rumble != NULL;
}

/*
 * caps is adapter scoped, per docs/protocol.md, so it says what the
 * adapter honours rather than what one pad can do. Rumble is claimed
 * only while a pad that can actually rumble is connected: xpad's rule
 * is never to claim a capability that is not honoured, and a pad with
 * no motors would make it a lie. The ST takes the newest value, so it
 * follows what is plugged in.
 */
static uint16_t caps_now(void)
{
    uint16_t caps = XPAD_CAP_ANALOG;
    int i;

    for (i = 0; i < COMPAD_PADS; i++)
    {
        if (slots[i].present && pad_can_rumble(i))
            return (uint16_t)(caps | XPAD_CAP_RUMBLE);
    }

    return caps;
}

static void send_descriptor(uint8_t pad)
{
    uint8_t f[CE_DESC_LEN];

    /* Every pad here arrives over Bluetooth and reports axes. */
    compad_descriptor_frame(pad, XPAD_TYPE_GAMEPAD,
                            XPAD_PAD_ANALOG | XPAD_PAD_WIRELESS, caps_now(),
                            f);
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
 * Both databases have to be asked. A controller bonds over Classic or
 * over BLE depending on what it is, and the two keep their keys in
 * different places: Classic in the link key database that
 * gap_link_key_iterator walks, BLE in the LE device database. An Xbox
 * Series pad bonds over BLE, so a Classic-only check reported no bonds
 * while the pad was reconnecting on its own in front of us.
 *
 * Cached, because it picks the blink rate on every tick, and asked
 * once a second while nothing is connected. One mechanism: the events
 * that change the answer just mark it due, so the next tick asks.
 */
static bool bonds;

static bool scan_bonds(void)
{
    btstack_link_key_iterator_t it;
    bd_addr_t addr;
    link_key_t key;
    link_key_type_t type;
    bool any;

    if (le_device_db_count() > 0)
        return true;

    if (!gap_link_key_iterator_init(&it))
        return false;

    any = gap_link_key_iterator_get_next(&it, addr, key, &type);
    gap_link_key_iterator_done(&it);

    return any;
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
        bond_age = COMPAD_BOND_RESCAN; /* due: the first disconnected tick asks */
        return;
    }

    if (++bond_age >= COMPAD_BOND_RESCAN)
    {
        bond_age = 0;
        bonds = scan_bonds();
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

        /* The _unsafe form, deliberately: this runs inside the BTstack
         * run loop (the tick is one of its timers), where it is legal,
         * and it deletes now rather than posting a request that lands
         * later. Marking the answer due then flips the LED on the next
         * tick, which is the confirmation the wipe worked. */
        uni_bt_del_keys_unsafe();
        bond_age = COMPAD_BOND_RESCAN;
    }
}

/* ------------------------------------------------------------------ */
/* What the ST asks for                                                */
/* ------------------------------------------------------------------ */

/* Arm a pad's motors for one burst. Silent for a pad that cannot. */
static void arm_rumble(int idx)
{
    uni_hid_device_t *d = uni_hid_device_get_instance_for_idx(idx);

    if (!d || !d->report_parser.play_dual_rumble)
        return;

    /*
     * Bluepad32 calls them weak and strong; xpad calls them high and
     * low frequency, which is the same pair the other way round: the
     * small fast motor is the weak one.
     */
    d->report_parser.play_dual_rumble(d, 0, COMPAD_RUMBLE_MS,
                                      rumble[idx].hi, rumble[idx].lo);
    rumble[idx].age = 0;
}

/*
 * A ping asks "is a COMpad on this port?", and the answer is a
 * descriptor, which is a frame the ST already decodes and which
 * carries the pad type and caps it wants anyway. No new reply type,
 * and noise cannot fake a valid checksum.
 *
 * It answers whether or not a pad is connected: that is the whole
 * point of it. With nothing paired the adapter is otherwise silent,
 * and an idle adapter would be indistinguishable from an absent one,
 * which is exactly how an evening got lost during bring-up.
 */
static void answer_ping(void)
{
    int i, replied = 0;

    for (i = 0; i < COMPAD_PADS; i++)
    {
        if (slots[i].present)
        {
            send_descriptor((uint8_t)i);
            replied = 1;
        }
    }

    if (!replied)
    {
        uint8_t f[CE_DESC_LEN];

        /* Here, but with nothing plugged in: XPAD_TYPE_NONE says so,
         * and xpad_connected() on the ST counts it as no pad. */
        compad_descriptor_frame(0, XPAD_TYPE_NONE, 0, caps_now(), f);
        wire_write(f, CE_DESC_LEN);
    }
}

static void handle_request(const uint8_t *f)
{
    uint8_t pad = COMPAD_HDR_PAD(f[1]);

    switch (COMPAD_HDR_TYPE(f[1]))
    {
    case COMPAD_TYPE_PING:
        answer_ping();
        break;

    case COMPAD_TYPE_REQUEST:
        if (pad >= COMPAD_PADS)
            return;

        rumble[pad].lo = COMPAD_REQ_RUMBLE_LO(f);
        rumble[pad].hi = COMPAD_REQ_RUMBLE_HI(f);

        /* Arm at once, whether that starts it or stops it: a consumer
         * writing zeroes means stop, and waiting for the burst to
         * expire would leave the pad buzzing after the trigger. */
        arm_rumble(pad);
        break;

    default:
        break;
    }
}

/* Keep a held rumble held. xpad's request area has no duration, so
 * "until replaced" is expressed by re-arming just inside the burst. */
static void rumble_tick(void)
{
    int i;

    for (i = 0; i < COMPAD_PADS; i++)
    {
        if (!slots[i].present || (!rumble[i].lo && !rumble[i].hi))
            continue;

        if (++rumble[i].age >= COMPAD_RUMBLE_REARM)
            arm_rumble(i);
    }
}

/*
 * Say what arrives on GP1.
 *
 * Bring-up, and the answer to a question the ST cannot answer for
 * itself: when nothing reaches the ST, it is impossible to tell a bad
 * wire from a wrong socket from a machine that never transmitted. Run
 * SENDTEST.TOS on the ST and watch this console, and the question
 * becomes which port the bytes came from.
 *
 * It also proves the receive half of the link, which nothing has ever
 * exercised: the UART is configured for it and until now not one byte
 * has been read. Draining it is the first piece of phase 4's request
 * path, so this is groundwork rather than scaffolding.
 */
static void rx_tick(void)
{
    while (uart_is_readable(COMPAD_UART))
    {
        uint8_t b = uart_getc(COMPAD_UART);

        rx_bytes++;

        if (rx_have < sizeof(rx_last))
            rx_last[rx_have++] = b;

        if (compad_feed_req(&rx_dec, b))
            handle_request(rx_dec.buf);
    }

    /* Once a second, and only when there is something to say, so a
     * quiet link does not fill the console. Info level, not debug: the
     * shipped log level is 2 and this line is the whole point of
     * SENDTEST.TOS, so demoting it once made that tool print nothing.
     * One call, not nine, because each goes over USB CDC and can block
     * on a host that is not draining. The bytes shown are the first of
     * the second, which is what says whether the rate is right. */
    if (++rx_age < COMPAD_TICKS_PER_SEC)
        return;

    rx_age = 0;

    if (rx_have)
    {
        char line[sizeof(rx_last) * 3 + 1];
        int n = 0;
        unsigned i;

        for (i = 0; i < rx_have; i++)
            n += snprintf(line + n, sizeof(line) - n, " %02x", rx_last[i]);

        logi("compad: rx %lu bytes, first:%s\n", (unsigned long)rx_bytes,
             line);
        rx_have = 0;
    }

    /* Only ever printed when it has happened, because it should not.
     * A drop means the ring filled, which means the budget above let
     * more out than the link carries: the number is the evidence, not
     * a statistic. */
    if (tx_dropped)
    {
        logi("compad: tx dropped %u frames\n", tx_dropped);
        tx_dropped = 0;
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
    uint8_t want[COMPAD_PADS], plan[COMPAD_PADS];

    /* Re-armed first. set_timer is relative to now, so re-arming after
     * the sends would make the period 20 ms plus however long they
     * took, and the descriptor and keepalive intervals are counted in
     * ticks: they would stretch with it and stop matching the rates
     * config.h claims. */
    btstack_run_loop_set_timer(ts, COMPAD_TICK_MS);
    btstack_run_loop_add_timer(ts);

    /* First, before the sends: they block on a full TX FIFO, and the
     * 32 byte RX FIFO holds less than one tick of 19200, so draining
     * after them is exactly when bytes would be lost. */
    rx_tick();
    tx_drain();

    /*
     * What each pad would send on an unlimited link.
     *
     * The counters advance for every pad, whatever the budget lets
     * out: a keepalive the link had no room for is still due, and goes
     * as soon as there is room. Saturating rather than wrapping,
     * because a wrapped counter would read as "just sent" and hold the
     * frame back further still.
     */
    for (i = 0; i < COMPAD_PADS; i++)
    {
        COMPAD_SLOT *s = &slots[i];

        want[i] = 0;

        if (!s->present)
            continue;

        if (s->since_descriptor < 255)
            s->since_descriptor++;
        if (s->since_send < 255)
            s->since_send++;

        /* Repeated, not sent once: the provider discards the serial
         * ring when it installs, so a descriptor that arrived while
         * the ST was still booting is gone. */
        if (s->since_descriptor >= COMPAD_DESCRIPTOR_EVERY)
            want[i] |= CE_SEND_DESCRIPTOR;

        if (s->since_send >= COMPAD_KEEPALIVE_EVERY)
        {
            /* A keepalive is full state or it is not a keepalive. */
            want[i] |= CE_SEND_STATE;
        }
        else if (compad_state_differs(&s->state, &s->sent))
        {
            want[i] |= CE_SEND_STATE;

            if (ce_compact_ok(&s->state, &s->sent))
                want[i] |= CE_SEND_COMPACT;
        }
    }

    /* What the link can carry of it, and who goes first next time. */
    ce_schedule(want, COMPAD_PADS, COMPAD_TX_BUDGET, &next_pad, plan);

    for (i = 0; i < COMPAD_PADS; i++)
    {
        if (plan[i] & CE_SEND_DESCRIPTOR)
            send_descriptor((uint8_t)i);

        if (plan[i] & CE_SEND_STATE)
            send_state((uint8_t)i);
        else if (plan[i] & CE_SEND_COMPACT)
            send_compact((uint8_t)i);
    }

    led_tick();
    button_tick();
    rumble_tick();
}

/* ------------------------------------------------------------------ */
/* Platform                                                            */
/* ------------------------------------------------------------------ */

static void compad_platform_init(int argc, const char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    memset(slots, 0, sizeof(slots));
    memset(rumble, 0, sizeof(rumble));
    compad_init(&rx_dec);
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

    logi("compad %s: ready\n", COMPAD_VERSION);
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
    memset(&rumble[idx], 0, sizeof(rumble[idx]));
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
