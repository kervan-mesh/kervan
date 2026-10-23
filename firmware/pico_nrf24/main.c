/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Raspberry Pi Pico (and Pico W) + nRF24L01+ firmware for ksim live sessions
 * (docs/hardware.md). Pins are build options (KFW_SPI, KFW_PIN_*); the defaults are the wiring
 * `ksim wiring rpi-pico nrf24l01p` shows: SPI0 on GP16-GP19, CE on GP20 (IRQ is not needed).
 * Talks JSON lines over USB (115200 is ignored by USB).
 */

#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#ifdef KFW_PICO_W
#include "pico/cyw43_arch.h"
#endif

#include "kfw/app.h"

#ifndef KFW_SPI
#define KFW_SPI 0
#define KFW_PIN_SCK 18
#define KFW_PIN_MOSI 19
#define KFW_PIN_MISO 16
#define KFW_PIN_CSN 17
#define KFW_PIN_CE 20
#endif
#define PIN_MISO KFW_PIN_MISO
#define PIN_CSN KFW_PIN_CSN
#define PIN_SCK KFW_PIN_SCK
#define PIN_MOSI KFW_PIN_MOSI
#define PIN_CE KFW_PIN_CE
#define SPI (KFW_SPI == 1 ? spi1 : spi0)
#define STR_(x) #x
#define STR(x) STR_(x)
/* as ksim writes it (FirmwareSpec::wiring) */
#define WIRING                                                                                     \
    "SPI" STR(KFW_SPI) " SCK=GP" STR(KFW_PIN_SCK) " MOSI=GP" STR(KFW_PIN_MOSI) " MISO=GP" STR(     \
        KFW_PIN_MISO) " CSN=GP" STR(KFW_PIN_CSN) " CE=GP" STR(KFW_PIN_CE)
#define SPI_HZ (4u * 1000u * 1000u) /* the chip takes 10 MHz; 4 MHz is safe on jumper wires */
#define LED_ON_US 20000u

static uint64_t led_off_at;

static void spi_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t n)
{
    (void)ctx;
    gpio_put(PIN_CSN, 0);
    if (rx != NULL) {
        spi_write_read_blocking(SPI, tx, rx, n);
    } else {
        spi_write_blocking(SPI, tx, n);
    }
    gpio_put(PIN_CSN, 1);
}

static void ce(void *ctx, bool high)
{
    (void)ctx;
    gpio_put(PIN_CE, high);
}

static void delay(void *ctx, uint32_t us)
{
    (void)ctx;
    sleep_us(us);
}

static void led(bool on)
{
#ifdef KFW_PICO_W
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
#elif defined(PICO_DEFAULT_LED_PIN)
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#else
    (void)on;
#endif
}

static void write_line(void *ctx, const char *line)
{
    (void)ctx;
    printf("%s\n", line);
}

static void activity(void *ctx)
{
    (void)ctx;
    led(true);
    led_off_at = time_us_64() + LED_ON_US;
}

int main(void)
{
    static kfw_app_t app;
    static char line[KFW_LINE_MAX];
    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    size_t len = 0;
    bool too_long = false;
    bool connected = false;
    kfw_app_host_t host;
    kfw_nrf24_io_t io;

    stdio_init_all();
#ifdef KFW_PICO_W
    cyw43_arch_init();
#elif defined(PICO_DEFAULT_LED_PIN)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

    spi_init(SPI, SPI_HZ);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(PIN_CSN);
    gpio_set_dir(PIN_CSN, GPIO_OUT);
    gpio_put(PIN_CSN, 1);
    gpio_init(PIN_CE);
    gpio_set_dir(PIN_CE, GPIO_OUT);
    gpio_put(PIN_CE, 0);
    sleep_ms(110); /* the radio's power-on reset takes 100 ms */

    pico_get_unique_board_id_string(id, sizeof(id));
    host.write_line = write_line;
    host.activity = activity;
    host.ctx = NULL;
#ifdef KFW_PICO_W
    host.board = "rpi-pico-w";
#else
    host.board = "rpi-pico";
#endif
    host.board_id = id;
    host.wiring = WIRING;
    io.spi = spi_xfer;
    io.ce = ce;
    io.delay_us = delay;
    io.ctx = NULL;
    kfw_app_init(&app, &host, &io);

    led(true); /* a short blink: running */
    led_off_at = time_us_64() + 300000u;

    while (true) {
        int c;
        const bool now_connected = stdio_usb_connected();

        if (connected && !now_connected) {
            kfw_app_disconnected(&app); /* nobody listens: stop sending */
        }
        connected = now_connected;

        while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
            if (c == '\n' || c == '\r') {
                if (too_long) {
                    printf("{\"ev\":\"error\",\"msg\":\"line too long\"}\n");
                } else if (len > 0u) {
                    kfw_app_line(&app, line, len, time_us_64());
                }
                len = 0;
                too_long = false;
            } else if (len < sizeof(line)) {
                line[len++] = (char)c;
            } else {
                too_long = true;
            }
        }

        kfw_app_poll(&app, time_us_64());
        if (led_off_at != 0u && time_us_64() >= led_off_at) {
            led(false);
            led_off_at = 0;
        }
    }
}
