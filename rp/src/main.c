/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Neil Rackett */

/*
 * COMpad adapter firmware: Bluetooth gamepads onto an Atari ST's
 * serial port, as COMpad frames.
 *
 * Phase 3. Bluepad32 over BTstack does the Bluetooth, compad_platform.c
 * turns what it reports into frames, and this file brings up the
 * hardware and hands over to the run loop.
 *
 * Nothing but a MAX3232 on UART0 is required. See config.h for the two
 * optional parts and why they need no build flag to leave out.
 */

#include <btstack_run_loop.h>
#include <hardware/gpio.h>
#include <hardware/uart.h>
#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>
#include <uni.h>

#include "config.h"
#include "sdkconfig.h"

#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Pico W must use BLUEPAD32_PLATFORM_CUSTOM"
#endif

/* Defined in compad_platform.c */
struct uni_platform *get_compad_platform(void);

int main(void)
{
    /*
     * USB only. The wire is on UART0 and stdio must never share it:
     * a single log line would land in the middle of a frame. The
     * CMakeLists disables stdio on UART for the same reason, so this
     * is belt and braces rather than the only guard.
     */
    stdio_init_all();

    /*
     * The serial link, before Bluetooth. If this is the only thing
     * that comes up, a scope on GP0 still shows keepalive frames, and
     * that is the half a builder wants to prove first.
     */
    uart_init(COMPAD_UART, COMPAD_BAUD);

    /* uart_init() already sets 8N1, the FIFO and no flow control. These
     * three restate it deliberately: the protocol depends on 8N1 and
     * the wire is the one thing here with no test behind it, so it is
     * worth spelling out rather than inheriting an SDK default. */
    uart_set_format(COMPAD_UART, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(COMPAD_UART, false, false);
    uart_set_fifo_enabled(COMPAD_UART, true);
    gpio_set_function(COMPAD_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(COMPAD_UART_RX, GPIO_FUNC_UART);

    /* Optional external LED. An unconnected output is harmless. */
    gpio_init(COMPAD_LED_PIN);
    gpio_set_dir(COMPAD_LED_PIN, GPIO_OUT);
    gpio_put(COMPAD_LED_PIN, 0);

    /*
     * Optional button, pulled up internally. With nothing wired it
     * reads high for ever, which is what "not pressed" looks like, so
     * the absent case needs no code of its own.
     */
    gpio_init(COMPAD_BUTTON_PIN);
    gpio_set_dir(COMPAD_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(COMPAD_BUTTON_PIN);

    if (cyw43_arch_init())
    {
        /*
         * The radio failed, and the onboard LED is on the radio, so it
         * cannot say so. The external LED can, which is the one thing
         * it does that the onboard one structurally cannot: solid on
         * means alive but with no Bluetooth. See docs/hardware.md.
         */
        gpio_put(COMPAD_LED_PIN, 1);
        loge("compad: cyw43_arch_init failed\n");
        return -1;
    }

    uni_platform_set_custom(get_compad_platform());
    uni_init(0, NULL);

    /* Does not return. */
    btstack_run_loop_execute();

    return 0;
}
